/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* GIO - GLib Input, Output and Streaming Library
 * 
 * Copyright (C) 2006-2007 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: David Zeuthen <davidz@redhat.com>
 */

#include <glib.h>
#include <gmodule.h>
#include <gio/gio.h>

#include "ghalvolumemonitor.h"

void
g_io_module_load (GIOModule *module)
{
  g_hal_volume_monitor_register (module);
  g_warning ("loaded");

  /* TODO: Since dynamic types need some fixing we want to make ourselves resident; I couldn't
   *       figure out how to get a GModule from GIOModule so do this hack until then
   */
  g_type_module_use (G_TYPE_MODULE (module));
}

void
g_io_module_unload (GIOModule *module)
{
  g_warning ("unloaded");
}