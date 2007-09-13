#ifndef __G_FILE_INFO_SIMPLE_H__
#define __G_FILE_INFO_SIMPLE_H__

#include <gfileinfo.h>

G_BEGIN_DECLS

void g_file_info_simple_get (const char *path,
			     GFileInfo *info,
			     GFileInfoRequestFlags requested,
			     const char *attributes,
			     gboolean follow_symlinks);

G_END_DECLS

#endif /* __G_FILE_FILE_INFO_SIMPLE_H__ */


