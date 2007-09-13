#include <config.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <poll.h>

#include <glib/gi18n-lib.h>

#include <gio/gvfserror.h>
#include "gvfsdaemondbus.h"
#include <gvfsdaemonprotocol.h>
#include "gdbusutils.h"
#include "gsysutils.h"

#define DBUS_TIMEOUT_DEFAULT 30 * 1000 /* 1/2 min */

/* Extra vfs-specific data for DBusConnections */
typedef struct {
  int extra_fd;
  int extra_fd_count;
  
  /* Only used for async connections */
  GHashTable *outstanding_fds;
  GSource *extra_fd_source;
} VfsConnectionData;

static gint32 vfs_data_slot = -1;
static GOnce once_init_dbus = G_ONCE_INIT;

static GStaticPrivate local_connections = G_STATIC_PRIVATE_INIT;

/* dbus id -> async connection */
static GHashTable *async_map = NULL;
G_LOCK_DEFINE_STATIC(async_map);

/* dbus object path -> dbus message filter */
static GHashTable *obj_path_map = NULL;
G_LOCK_DEFINE_STATIC(obj_path_map);

static void setup_async_fd_receive (VfsConnectionData *connection_data);

static gpointer
vfs_dbus_init (gpointer arg)
{
  if (!dbus_connection_allocate_data_slot (&vfs_data_slot))
    g_error ("Unable to allocate data slot");

  return NULL;
}

/**************************************************************************
 *               message filters for vfs dbus connections                 *
 *************************************************************************/

typedef struct {
  DBusHandleMessageFunction callback;
  GObject *data;
} PathMapEntry;

void
_g_dbus_register_vfs_filter (const char *obj_path,
			     DBusHandleMessageFunction callback,
			     GObject *data)
{
  PathMapEntry * entry;
  
  G_LOCK (obj_path_map);
  
  if (obj_path_map == NULL)
    obj_path_map = g_hash_table_new_full (g_str_hash, g_str_equal,
					  g_free, g_free);

  entry = g_new (PathMapEntry,1 );
  entry->callback = callback;
  entry->data = data;

  g_hash_table_insert  (obj_path_map, g_strdup (obj_path), entry);
  
  G_UNLOCK (obj_path_map);
}

void
_g_dbus_unregister_vfs_filter (const char *obj_path)
{
  G_LOCK (obj_path_map);
  
  if (obj_path_map)
      g_hash_table_remove (obj_path_map, obj_path);
  
  G_UNLOCK (obj_path_map);
}

static DBusHandlerResult
vfs_connection_filter (DBusConnection     *connection,
		       DBusMessage        *message,
		       void               *user_data)
{
  PathMapEntry *entry;
  DBusHandlerResult res;
  DBusHandleMessageFunction callback;
  GObject *data;

  callback = NULL;
  data = NULL;
  
  G_LOCK (obj_path_map);
  if (obj_path_map)
    {
      entry = g_hash_table_lookup (obj_path_map,
				   dbus_message_get_path (message));

      if (entry)
	{
	  callback = entry->callback;
	  data = g_object_ref (entry->data);
	}
    }
  G_UNLOCK (obj_path_map);

  res = DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

  if (callback)
    {
      res = callback (connection, message, data);
      g_object_unref (data);
    }

  return res;
}

static void
connection_data_free (gpointer p)
{
  VfsConnectionData *data = p;
  
  close (data->extra_fd);

  if (data->extra_fd_source)
    {
      g_source_destroy (data->extra_fd_source);
      g_source_unref (data->extra_fd_source);
    }

  if (data->outstanding_fds)
    g_hash_table_destroy (data->outstanding_fds);
  
  g_free (data);
}

static void
vfs_connection_setup (DBusConnection *connection,
		      int extra_fd,
		      gboolean async)
{
  VfsConnectionData *connection_data;
  
  connection_data = g_new (VfsConnectionData, 1);
  connection_data->extra_fd = extra_fd;
  connection_data->extra_fd_count = 0;

  if (async)
    setup_async_fd_receive (connection_data);
  
  if (!dbus_connection_set_data (connection, vfs_data_slot, connection_data, connection_data_free))
    _g_dbus_oom ();

  if (!dbus_connection_add_filter (connection, vfs_connection_filter, NULL, NULL))
    _g_dbus_oom ();
}

/**************************************************************************
 *            Functions to get fds from vfs dbus connections              *
 *************************************************************************/

typedef struct {
  int fd;
  GetFdAsyncCallback callback;
  gpointer callback_data;
} OutstandingFD;

static void
outstanding_fd_free (OutstandingFD *outstanding)
{
  if (outstanding->fd != -1)
    close (outstanding->fd);

  g_free (outstanding);
}

static void
async_connection_accept_new_fd (VfsConnectionData *data,
				GIOCondition condition,
				int fd)
{
  int new_fd;
  int fd_id;
  OutstandingFD *outstanding_fd;
  
  fd_id = data->extra_fd_count;
  new_fd = _g_socket_receive_fd (data->extra_fd);
  if (new_fd != -1)
    {
      data->extra_fd_count++;

      outstanding_fd = g_hash_table_lookup (data->outstanding_fds, GINT_TO_POINTER (fd_id));
      
      if (outstanding_fd)
	{
	  outstanding_fd->callback (new_fd, outstanding_fd->callback_data);
	  g_hash_table_remove (data->outstanding_fds, GINT_TO_POINTER (fd_id));
	}
      else
	{
	  outstanding_fd = g_new0 (OutstandingFD, 1);
	  outstanding_fd->fd = new_fd;
	  outstanding_fd->callback = NULL;
	  outstanding_fd->callback_data = NULL;
	  g_hash_table_insert (data->outstanding_fds,
			       GINT_TO_POINTER (fd_id),
			       outstanding_fd);
	}
    }
}

static void
setup_async_fd_receive (VfsConnectionData *connection_data)
{
  connection_data->outstanding_fds =
    g_hash_table_new_full (g_direct_hash,
			   g_direct_equal,
			   NULL,
			   (GDestroyNotify)outstanding_fd_free);
  
  
  connection_data->extra_fd_source =
    __g_fd_source_new (connection_data->extra_fd, POLLIN, NULL);
  g_source_set_callback (connection_data->extra_fd_source,
			 (GSourceFunc)async_connection_accept_new_fd,
			 connection_data, NULL);
  g_source_attach (connection_data->extra_fd_source, NULL);
}

int
_g_dbus_connection_get_fd_sync (DBusConnection *connection,
				int fd_id)
{
  VfsConnectionData *data;
  int fd;

  data = dbus_connection_get_data (connection, vfs_data_slot);
  g_assert (data != NULL);

  /* I don't think we can get reorders here, can we?
   * Its a sync per-thread connection after all
   */
  g_assert (fd_id == data->extra_fd_count);
  
  fd = _g_socket_receive_fd (data->extra_fd);
  if (fd != -1)
    data->extra_fd_count++;

  return fd;
}

void
_g_dbus_connection_get_fd_async (DBusConnection *connection,
				 int fd_id,
				 GetFdAsyncCallback callback,
				 gpointer callback_data)
{
  VfsConnectionData *data;
  OutstandingFD *outstanding_fd;
  int fd;
  
  data = dbus_connection_get_data (connection, vfs_data_slot);
  g_assert (data != NULL);

  outstanding_fd = g_hash_table_lookup (data->outstanding_fds, GINT_TO_POINTER (fd_id));

  if (outstanding_fd)
    {
      fd = outstanding_fd->fd;
      outstanding_fd->fd = -1;
      g_hash_table_remove (data->outstanding_fds, GINT_TO_POINTER (fd_id));
      callback (fd, callback_data);
    }
  else
    {
      outstanding_fd = g_new0 (OutstandingFD, 1);
      outstanding_fd->fd = -1;
      outstanding_fd->callback = callback;
      outstanding_fd->callback_data = callback_data;
      g_hash_table_insert (data->outstanding_fds,
			   GINT_TO_POINTER (fd_id),
			   outstanding_fd);
    }
}

/*******************************************************************
 *                Caching of async connections                     *
 *******************************************************************/


static DBusConnection *
get_connection_for_async (const char *dbus_id)
{
  DBusConnection *connection;

  connection = NULL;
  G_LOCK (async_map);
  if (async_map != NULL)
    connection = g_hash_table_lookup (async_map, dbus_id);
  if (connection)
    dbus_connection_ref (connection);
  G_UNLOCK (async_map);
  
  return connection;
}

static void
close_and_unref_connection (void *data)
{
  DBusConnection *connection = data;
  
  dbus_connection_close (connection);
  dbus_connection_unref (connection);
}

static void
set_connection_for_async (DBusConnection *connection, const char *dbus_id)
{
  G_LOCK (async_map);
  if (async_map == NULL)
    async_map = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, close_and_unref_connection);
      
  g_hash_table_insert (async_map, g_strdup (dbus_id), connection);
  dbus_connection_ref (connection);
  G_UNLOCK (async_map);
}

/**************************************************************************
 *                 Asynchronous daemon calls                              *
 *************************************************************************/

typedef struct {
  const char *dbus_id;

  DBusMessage *message;
  DBusConnection *connection;
  GCancellable *cancellable;

  GVfsAsyncDBusCallback callback;
  gpointer callback_data;
  
  GError *io_error;
  gulong cancelled_tag;
} AsyncDBusCall;

static void
async_call_finish (AsyncDBusCall *async_call,
		   DBusMessage *reply)
{
  async_call->callback (reply, async_call->connection,
			async_call->io_error, 
			async_call->callback_data);

  if (async_call->connection)
    dbus_connection_unref (async_call->connection);
  dbus_message_unref (async_call->message);
  if (async_call->cancellable)
    g_object_unref (async_call->cancellable);
  if (async_call->io_error)
    g_error_free (async_call->io_error);
  g_free (async_call);
}

static gboolean
async_call_finish_at_idle (gpointer data)
{
  AsyncDBusCall *async_call = data;

  async_call_finish (async_call, NULL);
  
  return FALSE;
}

static void
async_dbus_response (DBusPendingCall *pending,
		     void            *data)
{
  AsyncDBusCall *async_call = data;
  DBusMessage *reply;
  DBusError derror;

  if (async_call->cancelled_tag)
    g_signal_handler_disconnect (async_call->cancellable,
				 async_call->cancelled_tag);

  reply = dbus_pending_call_steal_reply (pending);
  dbus_pending_call_unref (pending);

  dbus_error_init (&derror);
  if (dbus_set_error_from_message (&derror, reply))
    {
      _g_error_from_dbus (&derror, &async_call->io_error);
      dbus_error_free (&derror);
      async_call_finish (async_call, NULL);
    }
  else
    async_call_finish (async_call, reply);
  
  dbus_message_unref (reply);
}

typedef struct {
  DBusConnection *connection;
  dbus_uint32_t serial;
} AsyncCallCancelData;

static void
async_call_cancel_data_free (gpointer _data)
{
  AsyncCallCancelData *data = _data;

  dbus_connection_unref (data->connection);
  g_free (data);
}

/* Might be called on another thread */
static void
async_call_cancelled_cb (GCancellable *cancellable,
			 gpointer _data)
{
  AsyncCallCancelData *data = _data;
  DBusMessage *cancel_message;

  /* Send cancellation message, this just queues it, sending
   * will happen in mainloop */
  cancel_message = dbus_message_new_method_call (NULL,
						 G_VFS_DBUS_DAEMON_PATH,
						 G_VFS_DBUS_DAEMON_INTERFACE,
						 G_VFS_DBUS_OP_CANCEL);
  if (cancel_message != NULL)
    {
      if (dbus_message_append_args (cancel_message,
				    DBUS_TYPE_UINT32, &data->serial,
				    DBUS_TYPE_INVALID))
	dbus_connection_send (data->connection,
			      cancel_message, NULL);
      dbus_message_unref (cancel_message);
    }
}

static void
async_call_send (AsyncDBusCall *async_call)
{
  DBusPendingCall *pending;
  AsyncCallCancelData *cancel_data;

  if (!dbus_connection_send_with_reply (async_call->connection,
					async_call->message,
					&pending,
					DBUS_TIMEOUT_DEFAULT))
    _g_dbus_oom ();

  if (pending == NULL)
    {
      g_set_error (&async_call->io_error, G_FILE_ERROR, G_FILE_ERROR_IO,
		   "Error while getting peer-to-peer dbus connection: %s",
		   "Connection is closed");
      async_call_finish (async_call, NULL);
      return;
    }
  
  if (async_call->cancellable)
    {
      cancel_data = g_new0 (AsyncCallCancelData, 1);
      cancel_data->connection = dbus_connection_ref (async_call->connection);
      cancel_data->serial = dbus_message_get_serial (async_call->message);
      async_call->cancelled_tag =
	g_signal_connect_data (async_call->cancellable, "cancelled",
			       (GCallback)async_call_cancelled_cb,
			       cancel_data,
			       (GClosureNotify)async_call_cancel_data_free,
			       0);
    }
      
  if (!dbus_pending_call_set_notify (pending,
				     async_dbus_response,
				     async_call,
				     NULL))
    _g_dbus_oom ();

}

static void
async_get_connection_response (DBusPendingCall *pending,
			       void            *data)
{
  AsyncDBusCall *async_call = data;
  GError *error;
  DBusError derror;
  DBusMessage *reply;
  char *address1, *address2;
  int extra_fd;
  DBusConnection *connection, *existing_connection;

  reply = dbus_pending_call_steal_reply (pending);
  dbus_pending_call_unref (pending);

  dbus_error_init (&derror);
  if (!dbus_message_get_args (reply, &derror,
			      DBUS_TYPE_STRING, &address1,
			      DBUS_TYPE_STRING, &address2,
			      DBUS_TYPE_INVALID))
    {
      _g_error_from_dbus (&derror, &async_call->io_error);
      dbus_error_free (&derror);
      async_call_finish (async_call, NULL);
      return;
    }

  /* I don't know of any way to do an async connect */
  error = NULL;
  extra_fd = _g_socket_connect (address2, &error);
  if (extra_fd == -1)
    {
      g_set_error (&async_call->io_error, G_FILE_ERROR, G_FILE_ERROR_IO,
		   _("Error connecting to daemon: %s"), error->message);
      g_error_free (error);
      dbus_message_unref (reply);
      async_call_finish (async_call, NULL);
      return;
    }

  /* Unfortunately dbus doesn't have an async open */
  dbus_error_init (&derror);
  connection = dbus_connection_open_private (address1, &derror);
  if (!connection)
    {
      close (extra_fd);
      dbus_message_unref (reply);
      
      g_set_error (&async_call->io_error, G_FILE_ERROR, G_FILE_ERROR_IO,
		   "Error while getting peer-to-peer dbus connection: %s",
		   derror.message);
      dbus_error_free (&derror);
      async_call_finish (async_call, NULL);
      return;
    }
  dbus_message_unref (reply);

  vfs_connection_setup (connection, extra_fd, TRUE);
  
  /* Maybe we already had a connection? This happens if we requested
   * the same owner several times in parallel.
   * If so, just drop this connection and use that.
   */
  
  existing_connection = get_connection_for_async (async_call->dbus_id);
  if (existing_connection != NULL)
    {
      async_call->connection = existing_connection;
      dbus_connection_close (connection);
      dbus_connection_unref (connection);
    }
  else
    {  
      _g_dbus_connection_integrate_with_main (connection);
      set_connection_for_async (connection, async_call->dbus_id);
      async_call->connection = connection;
    }

  /* Maybe we were canceled while setting up connection, then
   * avoid doing the operation */
  if (g_cancellable_is_cancelled (async_call->cancellable))
    {
      g_set_error (&async_call->io_error,
		   G_VFS_ERROR,
		   G_VFS_ERROR_CANCELLED,
		   _("Operation was cancelled"));
      async_call_finish (async_call, NULL);
      return;
    }

  async_call_send (async_call);
}

static void
open_connection_async (AsyncDBusCall *async_call)
{
  DBusMessage *get_connection_message;
  DBusPendingCall *pending;
  DBusConnection *session_bus;

  get_connection_message = dbus_message_new_method_call (async_call->dbus_id,
							 G_VFS_DBUS_DAEMON_PATH,
							 G_VFS_DBUS_DAEMON_INTERFACE,
							 G_VFS_DBUS_OP_GET_CONNECTION);
  
  if (get_connection_message == NULL)
    _g_dbus_oom ();

  session_bus = dbus_bus_get (DBUS_BUS_SESSION, NULL);
  if (!dbus_connection_send_with_reply (session_bus,
					get_connection_message, &pending,
					DBUS_TIMEOUT_DEFAULT))
    _g_dbus_oom ();

  dbus_message_unref (get_connection_message);
  dbus_connection_unref (session_bus);
  
  if (pending == NULL)
    {
      g_set_error (&async_call->io_error, G_FILE_ERROR, G_FILE_ERROR_IO,
		   "Error while getting peer-to-peer dbus connection: %s",
		   "Connection is closed");
      g_idle_add (async_call_finish_at_idle, async_call);
      return;
    }
  
  if (!dbus_pending_call_set_notify (pending,
				     async_get_connection_response,
				     async_call,
				     NULL))
    _g_dbus_oom ();
}

void
_g_vfs_daemon_call_async (DBusMessage *message,
			  GVfsAsyncDBusCallback callback,
			  gpointer callback_data,
			  GCancellable *cancellable)
{
  AsyncDBusCall *async_call;

  g_once (&once_init_dbus, vfs_dbus_init, NULL);

  async_call = g_new0 (AsyncDBusCall, 1);
  async_call->dbus_id = dbus_message_get_destination (message);
  async_call->message = dbus_message_ref (message);
  if (cancellable)
    async_call->cancellable = g_object_ref (cancellable);
  async_call->callback = callback;
  async_call->callback_data = callback_data;

  async_call->connection = get_connection_for_async (async_call->dbus_id);
  if (async_call->connection == NULL)
    open_connection_async (async_call);
  else
    async_call_send (async_call);
}

/**************************************************************************
 *                  Synchronous daemon calls                              *
 *************************************************************************/

DBusMessage *
_g_vfs_daemon_call_sync (DBusMessage *message,
			 DBusConnection **connection_out,
			 GCancellable *cancellable,
			 GError **error)
{
  DBusConnection *connection;
  DBusError derror;
  DBusMessage *reply;
  DBusPendingCall *pending;
  int dbus_fd;
  int cancel_fd;
  gboolean sent_cancel;
  DBusMessage *cancel_message;
  dbus_uint32_t serial;
  const char *dbus_id = dbus_message_get_destination (message);

  if (g_cancellable_is_cancelled (cancellable))
    {
      g_set_error (error,
		   G_VFS_ERROR,
		   G_VFS_ERROR_CANCELLED,
		   _("Operation was cancelled"));
      return NULL;
    }
	    
  connection = _g_dbus_connection_get_sync (dbus_id, error);
  if (connection == NULL)
    return NULL;

  /* TODO: Handle errors below due to unmount and invalidate the
     sync connection cache */
  
  if (g_cancellable_is_cancelled (cancellable))
    {
      g_set_error (error,
		   G_VFS_ERROR,
		   G_VFS_ERROR_CANCELLED,
		   _("Operation was cancelled"));
      return NULL;
    }

  cancel_fd = g_cancellable_get_fd (cancellable);
  if (cancel_fd != -1)
    {
      if (!dbus_connection_send_with_reply (connection, message,
					    &pending, DBUS_TIMEOUT_DEFAULT))
	_g_dbus_oom ();
      
      if (pending == NULL)
	{
	  g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_IO,
		       "Error while getting peer-to-peer dbus connection: %s",
		       "Connection is closed");
	  return NULL;
	}

      /* Make sure the message is sent */
      dbus_connection_flush (connection);

      if (!dbus_connection_get_socket (connection, &dbus_fd))
	{
	  dbus_pending_call_unref (pending);
	  g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_IO,
		       "Error while getting peer-to-peer dbus connection: %s",
		       "No fd");
	  return NULL;
	}

      sent_cancel = FALSE;
      while (!dbus_pending_call_get_completed (pending))
	{
	  struct pollfd poll_fds[2];
	  int poll_ret;
	  
	  do
	    {
	      poll_fds[0].events = POLLIN;
	      poll_fds[0].fd = dbus_fd;
	      poll_fds[1].events = POLLIN;
	      poll_fds[1].fd = cancel_fd;
	      poll_ret = poll (poll_fds, sent_cancel?1:2, -1);
	    }
	  while (poll_ret == -1 && errno == EINTR);

	  if (poll_ret == -1)
	    {
	      dbus_pending_call_unref (pending);
	      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_IO,
			   "Error while getting peer-to-peer dbus connection: %s",
			   "poll error");
	      return NULL;
	    }
	  
	  if (!sent_cancel && g_cancellable_is_cancelled (cancellable))
	    {
	      sent_cancel = TRUE;
	      serial = dbus_message_get_serial (message);
	      cancel_message =
		dbus_message_new_method_call (NULL,
					      G_VFS_DBUS_DAEMON_PATH,
					      G_VFS_DBUS_DAEMON_INTERFACE,
					      G_VFS_DBUS_OP_CANCEL);
	      if (cancel_message != NULL)
		{
		  if (dbus_message_append_args (cancel_message,
						DBUS_TYPE_UINT32, &serial,
						DBUS_TYPE_INVALID))
		    {
		      dbus_connection_send (connection, cancel_message, NULL);
		      dbus_connection_flush (connection);
		    }
			    
		  dbus_message_unref (cancel_message);
		}
	    }

	  if (poll_fds[0].revents != 0)
	    {
	      dbus_connection_read_write (connection, DBUS_TIMEOUT_DEFAULT);

	      while (dbus_connection_dispatch (connection) == DBUS_DISPATCH_DATA_REMAINS)
		;
		
	    }
	}

      reply = dbus_pending_call_steal_reply (pending);
      dbus_pending_call_unref (pending);
    }
  else
    {
      dbus_error_init (&derror);
      reply = dbus_connection_send_with_reply_and_block (connection, message,
							 DBUS_TIMEOUT_DEFAULT,
							 &derror);
      if (!reply)
	{
	  _g_error_from_dbus (&derror, error);
	  dbus_error_free (&derror);
	  return NULL;
	}
    }

  if (connection_out)
    *connection_out = connection;

  if (dbus_set_error_from_message (&derror, reply))
    {
      _g_error_from_dbus (&derror, error);
      dbus_error_free (&derror);
      dbus_message_unref (reply);
      return NULL;
    }
  
  return reply;
}

/*************************************************************************
 *               get per-thread synchronous dbus connections             *
 *************************************************************************/

typedef struct {
  GHashTable *connections;
  DBusConnection *session_bus;
} ThreadLocalConnections;

static void
free_mount_connection (DBusConnection *conn)
{
  dbus_connection_close (conn);
  dbus_connection_unref (conn);
}

static void
free_local_connections (ThreadLocalConnections *local)
{
  g_hash_table_destroy (local->connections);
  if (local->session_bus)
    free_mount_connection (local->session_bus);
  g_free (local);
}

DBusConnection *
_g_dbus_connection_get_sync (const char *dbus_id,
			     GError **error)
{
  DBusConnection *bus;
  ThreadLocalConnections *local;
  GError *local_error;
  DBusConnection *connection;
  DBusMessage *message, *reply;
  DBusError derror;
  char *address1, *address2;
  int extra_fd;

  g_once (&once_init_dbus, vfs_dbus_init, NULL);

  local = g_static_private_get (&local_connections);
  if (local == NULL)
    {
      local = g_new0 (ThreadLocalConnections, 1);
      local->connections = g_hash_table_new_full (g_str_hash, g_str_equal,
						  g_free, (GDestroyNotify)free_mount_connection);
      g_static_private_set (&local_connections, local, (GDestroyNotify)free_local_connections);
    }

  if (dbus_id == NULL)
    connection = local->session_bus;
  else
    connection = g_hash_table_lookup (local->connections, dbus_id);
  
  if (connection != NULL)
    return connection;

  dbus_error_init (&derror);
  
  if (local->session_bus == NULL)
    {
      bus = dbus_bus_get_private (DBUS_BUS_SESSION, &derror);
      if (bus == NULL)
	{
	  g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_IO,
		       "Couldn't get main dbus connection: %s\n",
		       derror.message);
	  dbus_error_free (&derror);
	  return NULL;
	}
      
      local->session_bus = bus;

      if (dbus_id == NULL)
	return bus;
    }
  
  message = dbus_message_new_method_call (dbus_id,
					  G_VFS_DBUS_DAEMON_PATH,
					  G_VFS_DBUS_DAEMON_INTERFACE,
					  G_VFS_DBUS_OP_GET_CONNECTION);
  reply = dbus_connection_send_with_reply_and_block (local->session_bus, message, -1,
						     &derror);
  dbus_message_unref (message);

  if (!reply)
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_IO,
		   "Error while getting peer-to-peer dbus connection: %s",
		   derror.message);
      dbus_error_free (&derror);
      return NULL;
    }

  if (dbus_set_error_from_message (&derror, reply))
    {
      _g_error_from_dbus (&derror, error);
      dbus_error_free (&derror);
      return NULL;
    }
  
  dbus_message_get_args (reply, NULL,
			 DBUS_TYPE_STRING, &address1,
			 DBUS_TYPE_STRING, &address2,
			 DBUS_TYPE_INVALID);

  local_error = NULL;
  extra_fd = _g_socket_connect (address2, &local_error);
  if (extra_fd == -1)
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_IO,
		   _("Error connecting to daemon: %s"), local_error->message);
      g_error_free (local_error);
      dbus_message_unref (reply);
      return NULL;
    }

  dbus_error_init (&derror);
  connection = dbus_connection_open_private (address1, &derror);
  if (!connection)
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_IO,
		   "Error while getting peer-to-peer dbus connection: %s",
		   derror.message);
      close (extra_fd);
      dbus_message_unref (reply);
      dbus_error_free (&derror);
      return NULL;
    }
  dbus_message_unref (reply);

  vfs_connection_setup (connection, extra_fd, FALSE);

  g_hash_table_insert (local->connections, g_strdup (dbus_id), connection);

  return connection;
}

/**************************************************************************
 *                 GFileInfo demarshaller                                 *
 *************************************************************************/


GFileInfo *
_g_dbus_get_file_info (DBusMessageIter *iter,
		       GFileInfoRequestFlags requested,
		       GError **error)
{
  GFileInfo *info;
  DBusMessageIter struct_iter, array_iter;

  info = g_file_info_new ();

  if (dbus_message_iter_get_arg_type (iter) != DBUS_TYPE_STRUCT)
    goto error;

  dbus_message_iter_recurse (iter, &struct_iter);

  if (requested & G_FILE_INFO_FILE_TYPE)
    {
      guint16 type;
      
      if (dbus_message_iter_get_arg_type (&struct_iter) != DBUS_TYPE_UINT16)
	goto error;

      dbus_message_iter_get_basic (&struct_iter, &type);

      g_file_info_set_file_type (info, type);

      dbus_message_iter_next (&struct_iter);
    }

  if (requested & G_FILE_INFO_NAME)
    {
      char *str;
      const char *data;
      int len;
      
      if (dbus_message_iter_get_arg_type (&struct_iter) != DBUS_TYPE_ARRAY ||
	  dbus_message_iter_get_element_type (&struct_iter) != DBUS_TYPE_BYTE)
	goto error;

      dbus_message_iter_recurse (&struct_iter, &array_iter);
      dbus_message_iter_get_fixed_array (&array_iter, &data, &len);
      str = g_strndup (data, len);
      g_file_info_set_name (info, str);
      g_free (str);
      
      dbus_message_iter_next (&struct_iter);
    }

  if (requested & G_FILE_INFO_DISPLAY_NAME)
    {
      const char *str;
      
      if (dbus_message_iter_get_arg_type (&struct_iter) != DBUS_TYPE_STRING)
	goto error;

      dbus_message_iter_get_basic (&struct_iter, &str);
      
      g_file_info_set_display_name (info, str);
      dbus_message_iter_next (&struct_iter);
    }

  if (requested & G_FILE_INFO_EDIT_NAME)
    {
      const char *str;
      
      if (dbus_message_iter_get_arg_type (&struct_iter) != DBUS_TYPE_STRING)
	goto error;

      dbus_message_iter_get_basic (&struct_iter, &str);
      
      g_file_info_set_edit_name (info, str);
      dbus_message_iter_next (&struct_iter);
    }

  if (requested & G_FILE_INFO_ICON)
    {
      const char *str;
      
      if (dbus_message_iter_get_arg_type (&struct_iter) != DBUS_TYPE_STRING)
	goto error;

      dbus_message_iter_get_basic (&struct_iter, &str);
      
      g_file_info_set_icon (info, str);
      dbus_message_iter_next (&struct_iter);
    }

  if (requested & G_FILE_INFO_MIME_TYPE)
    {
      const char *str;
      
      if (dbus_message_iter_get_arg_type (&struct_iter) != DBUS_TYPE_STRING)
	goto error;

      dbus_message_iter_get_basic (&struct_iter, &str);
      
      g_file_info_set_mime_type (info, str);
      dbus_message_iter_next (&struct_iter);
    }

  if (requested & G_FILE_INFO_SIZE)
    {
      guint64 size;
      
      if (dbus_message_iter_get_arg_type (&struct_iter) != DBUS_TYPE_UINT64)
	goto error;

      dbus_message_iter_get_basic (&struct_iter, &size);
      
      g_file_info_set_size (info, size);
      dbus_message_iter_next (&struct_iter);
    }

  if (requested & G_FILE_INFO_MODIFICATION_TIME)
    {
      guint64 time;
      
      if (dbus_message_iter_get_arg_type (&struct_iter) != DBUS_TYPE_UINT64)
	goto error;

      dbus_message_iter_get_basic (&struct_iter, &time);
      
      g_file_info_set_modification_time (info, time);
      dbus_message_iter_next (&struct_iter);
    }

  if (requested & G_FILE_INFO_ACCESS_RIGHTS)
    {
      guint32 rights;
      
      if (dbus_message_iter_get_arg_type (&struct_iter) != DBUS_TYPE_UINT32)
	goto error;

      dbus_message_iter_get_basic (&struct_iter, &rights);
      
      g_file_info_set_access_rights (info, rights);
      dbus_message_iter_next (&struct_iter);
    }

  if (requested & G_FILE_INFO_STAT_INFO)
    {
      guint32 tmp;
      
      if (dbus_message_iter_get_arg_type (&struct_iter) != DBUS_TYPE_UINT32)
	goto error;

      dbus_message_iter_get_basic (&struct_iter, &tmp);
      
      /* TODO: implement statinfo */
      dbus_message_iter_next (&struct_iter);
    }

  if (requested & G_FILE_INFO_SYMLINK_TARGET)
    {
      char *str;
      const char *data;
      int len;
      
      if (dbus_message_iter_get_arg_type (&struct_iter) != DBUS_TYPE_ARRAY ||
	  dbus_message_iter_get_element_type (&struct_iter) != DBUS_TYPE_BYTE)
	goto error;

      dbus_message_iter_recurse (&struct_iter, &array_iter);
      dbus_message_iter_get_fixed_array (&array_iter, &data, &len);
      str = g_strndup (data, len);
      g_file_info_set_symlink_target (info, str);
      g_free (str);
      
      dbus_message_iter_next (&struct_iter);
    }

  if (requested & G_FILE_INFO_IS_HIDDEN)
    {
      dbus_bool_t is_hidden;
      
      if (dbus_message_iter_get_arg_type (&struct_iter) != DBUS_TYPE_BOOLEAN)
	goto error;

      dbus_message_iter_get_basic (&struct_iter, &is_hidden);
      
      g_file_info_set_is_hidden (info, is_hidden);
      dbus_message_iter_next (&struct_iter);
    }

  /* TODO: Attributes */

  dbus_message_iter_next (iter);
  return info;

 error:
  g_object_unref (info);
  g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_IO,
	       _("Invalid file info format"));
  return NULL;
}