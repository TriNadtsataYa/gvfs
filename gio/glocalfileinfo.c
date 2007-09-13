#include <config.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#ifdef HAVE_SELINUX
#include <selinux/selinux.h>
#endif

#include <sys/types.h>
#ifdef HAVE_XATTR

#if defined HAVE_SYS_XATTR_H
  #include <sys/xattr.h>
#elif defined HAVE_ATTR_XATTR_H
  #include <attr/xattr.h>
#else
  #error "Neither <sys/xattr.h> nor <attr/xattr.h> is present but extended attribute support is enabled."
#endif /* defined HAVE_SYS_XATTR_H || HAVE_ATTR_XATTR_H */

#endif /* HAVE_XATTR */

#include <glib/gstdio.h>
#include <glib/gi18n-lib.h>

#include "glocalfileinfo.h"
#include "gioerror.h"
#include "gcontenttype.h"
#include "gcontenttypeprivate.h"

char *
_g_local_file_info_create_etag (struct stat *statbuf)
{
  GTimeVal tv;
  
  tv.tv_sec = statbuf->st_mtime;
#if defined (HAVE_STRUCT_STAT_ST_MTIMENSEC)
  tv.tv_usec = statbuf->st_mtimensec / 1000;
#elif defined (HAVE_STRUCT_STAT_ST_MTIM_TV_NSEC)
  tv.tv_usec = statbuf->st_mtim.tv_nsec / 1000;
#else
  tv.tv_usec = 0;
#endif

  return g_strdup_printf ("%ld:%ld", tv.tv_sec, tv.tv_usec);
}

static gchar *
read_link (const gchar *full_name)
{
#ifdef HAVE_READLINK
  gchar *buffer;
  guint size;
  
  size = 256;
  buffer = g_malloc (size);
  
  while (1)
    {
      int read_size;
      
      read_size = readlink (full_name, buffer, size);
      if (read_size < 0)
	{
	  g_free (buffer);
	  return NULL;
	}
      if (read_size < size)
	{
	  buffer[read_size] = 0;
	  return buffer;
	}
      size *= 2;
      buffer = g_realloc (buffer, size);
    }
#else
  return NULL;
#endif
}

/* Get the SELinux security context */
static void
get_selinux_context (const char *path,
		     GFileInfo *info,
		     GFileAttributeMatcher *attribute_matcher,
		     gboolean follow_symlinks)
{
#ifdef HAVE_SELINUX
  char *context;

  if (!g_file_attribute_matcher_matches (attribute_matcher, "selinux:context"))
    return;
  
  if (is_selinux_enabled ())
    {
      if (follow_symlinks)
	{
	  if (lgetfilecon_raw (path, &context) < 0)
	    return;
	}
      else
	{
	  if (getfilecon_raw (path, &context) < 0)
	    return;
	}

      if (context)
	{
	  g_file_info_set_attribute_string (info, "selinux:context", context);
	  freecon(context);
	}
    }
#endif
}

#ifdef HAVE_XATTR

static gboolean
valid_char (char c)
{
  return c >= 32 && c <= 126 && c != '\\';
}

static char *
hex_escape_string (const char *str, gboolean *free_return)
{
  int num_invalid, i;
  char *escaped_str, *p;
  unsigned char c;
  static char *hex_digits = "0123456789abcdef";
  int len;

  len = strlen (str);
  
  num_invalid = 0;
  for (i = 0; i < len; i++)
    {
      if (!valid_char (str[i]))
	num_invalid++;
    }

  if (num_invalid == 0)
    {
      *free_return = FALSE;
      return (char *)str;
    }

  escaped_str = g_malloc (len + num_invalid*3 + 1);

  p = escaped_str;
  for (i = 0; i < len; i++)
    {
      if (valid_char (str[i]))
	*p++ = str[i];
      else
	{
	  c = str[i];
	  *p++ = '\\';
	  *p++ = 'x';
	  *p++ = hex_digits[(c >> 8) & 0xf];
	  *p++ = hex_digits[c & 0xf];
	}
    }
  *p++ = 0;

  *free_return = TRUE;
  return escaped_str;
}

static void
escape_xattr (GFileInfo *info,
	      const char *attr, /* not escaped */
	      const char *value, /* Is zero terminated */
	      size_t len /* not including zero termination */)
{
  char *full_attr;
  char *escaped_val, *escaped_attr;
  gboolean free_escaped_val, free_escaped_attr;
  
  escaped_attr = hex_escape_string (attr, &free_escaped_attr);
  full_attr = g_strconcat ("xattr:", escaped_attr, NULL);
  if (free_escaped_attr)
    g_free (escaped_attr);

  escaped_val = hex_escape_string (value, &free_escaped_val);
  g_file_info_set_attribute_string (info, full_attr, escaped_val);
  if (free_escaped_val)
    g_free (escaped_val);
  
  g_free (full_attr);
}

static void
get_one_xattr (const char *path,
	       GFileInfo *info,
	       const char *attr,
	       gboolean follow_symlinks)
{
  char value[64];
  char *value_p;
  ssize_t len;

  if (follow_symlinks)  
    len = getxattr (path, attr, value, sizeof (value)-1);
  else
    len = lgetxattr (path, attr,value, sizeof (value)-1);

  value_p = NULL;
  if (len >= 0)
    value_p = value;
  else if (len == -1 && errno == ERANGE)
    {
      if (follow_symlinks)  
	len = getxattr (path, attr, NULL, 0);
      else
	len = lgetxattr (path, attr, NULL, 0);

      if (len < 0)
	return;

      value_p = g_malloc (len+1);

      if (follow_symlinks)  
	len = getxattr (path, attr, value_p, len);
      else
	len = lgetxattr (path, attr, value_p, len);

      if (len < 0)
	{
	  g_free (value_p);
	  return;
	}
    }
  else
    return;
  
  /* Null terminate */
  value_p[len] = 0;

  escape_xattr (info, attr, value_p, len);
  
  if (value_p != value)
    g_free (value_p);
}

#endif /* defined HAVE_XATTR */

static void
get_xattrs (const char *path,
	    GFileInfo *info,
	    GFileAttributeMatcher *matcher,
	    gboolean follow_symlinks)
{
#ifdef HAVE_XATTR
  gboolean all;
  gsize list_size;
  ssize_t list_res_size;
  size_t len;
  char *list;
  const char *attr;

  all = g_file_attribute_matcher_enumerate_namespace (matcher, "xattr");
  if (all)
    {
      if (follow_symlinks)
	list_res_size = listxattr (path, NULL, 0);
      else
	list_res_size = llistxattr (path, NULL, 0);

      if (list_res_size == -1 ||
	  list_res_size == 0)
	return;

      list_size = list_res_size;
      list = g_malloc (list_size);

    retry:
      
      if (follow_symlinks)
	list_res_size = listxattr (path, list, list_size);
      else
	list_res_size = llistxattr (path, list, list_size);
      
      if (list_res_size == -1 && errno == ERANGE)
	{
	  list_size = list_size * 2;
	  list = g_realloc (list, list_size);
	  goto retry;
	}

      if (list_res_size == -1)
	return;

      attr = list;
      while (list_res_size > 0)
	{
	  get_one_xattr (path, info, attr, follow_symlinks);
	  len = strlen (attr) + 1;
	  attr += len;
	  list_res_size -= len;
	}

      g_free (list);
    }
  else
    {
      while ((attr = g_file_attribute_matcher_enumerate_next (matcher)) != NULL)
	get_one_xattr (path, info, attr, follow_symlinks);
    }
#endif /* defined HAVE_XATTR */
}

#ifdef HAVE_XATTR
static void
get_one_xattr_from_fd (int fd,
		       GFileInfo *info,
		       const char *attr)
{
  char value[64];
  char *value_p;
  ssize_t len;

  len = fgetxattr (fd, attr, value, sizeof (value)-1);

  value_p = NULL;
  if (len >= 0)
    value_p = value;
  else if (len == -1 && errno == ERANGE)
    {
      len = fgetxattr (fd, attr, NULL, 0);

      if (len < 0)
	return;

      value_p = g_malloc (len+1);

      len = fgetxattr (fd, attr, value_p, len);

      if (len < 0)
	{
	  g_free (value_p);
	  return;
	}
    }
  else
    return;
  
  /* Null terminate */
  value_p[len] = 0;

  escape_xattr (info, attr, value_p, len);
  
  if (value_p != value)
    g_free (value_p);
}
#endif /* defined HAVE_XATTR */

static void
get_xattrs_from_fd (int fd,
		    GFileInfo *info,
		    GFileAttributeMatcher *matcher)
{
#ifdef HAVE_XATTR
  gboolean all;
  gsize list_size;
  ssize_t list_res_size;
  size_t len;
  char *list;
  const char *attr;

  all = g_file_attribute_matcher_enumerate_namespace (matcher, "xattr");

  if (all)
    {
      list_res_size = flistxattr (fd, NULL, 0);

      if (list_res_size == -1 ||
	  list_res_size == 0)
	return;

      list_size = list_res_size;
      list = g_malloc (list_size);

    retry:
      
      list_res_size = flistxattr (fd, list, list_size);
      
      if (list_res_size == -1 && errno == ERANGE)
	{
	  list_size = list_size * 2;
	  list = g_realloc (list, list_size);
	  goto retry;
	}

      if (list_res_size == -1)
	return;

      attr = list;
      while (list_res_size > 0)
	{
	  get_one_xattr_from_fd (fd, info, attr);
	  len = strlen (attr) + 1;
	  attr += len;
	  list_res_size -= len;
	}

      g_free (list);
    }
  else
    {
      while ((attr = g_file_attribute_matcher_enumerate_next (matcher)) != NULL)
	get_one_xattr_from_fd (fd, info, attr);
    }
#endif /* defined HAVE_XATTR */
}

void
_g_local_file_info_get_parent_info (const char             *dir,
				    GFileAttributeMatcher  *attribute_matcher,
				    GLocalParentFileInfo   *parent_info)
{
  struct stat statbuf;
  int res;
  
  parent_info->writable = FALSE;
  parent_info->is_sticky = FALSE;

  if (g_file_attribute_matcher_matches (attribute_matcher, G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME) ||
      g_file_attribute_matcher_matches (attribute_matcher, G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE))
    {
      parent_info->writable = (g_access (dir, W_OK) == 0);
      
      if (parent_info->writable)
	{
	  res = g_stat (dir, &statbuf);

	  /*
	   * The sticky bit (S_ISVTX) on a directory means that a file in that directory can be
	   * renamed or deleted only by the owner of the file, by the owner of the directory, and
	   * by a privileged process.
	   */
	  if (res == 0)
	    {
	      parent_info->is_sticky = (statbuf.st_mode & S_ISVTX) != 0;
	      parent_info->owner = statbuf.st_uid;
	    }
	}
    }
}

static void
get_access_rights (GFileAttributeMatcher *attribute_matcher,
		   GFileInfo *info,
		   const gchar *path,
		   struct stat *statbuf,
		   GLocalParentFileInfo *parent_info)
{
  if (g_file_attribute_matcher_matches (attribute_matcher,
					G_FILE_ATTRIBUTE_ACCESS_CAN_READ))
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ,
				       g_access (path, R_OK) == 0);
  
  if (g_file_attribute_matcher_matches (attribute_matcher,
					G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE))
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE,
				       g_access (path, W_OK) == 0);
  
  if (g_file_attribute_matcher_matches (attribute_matcher,
					G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE))
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE,
				       g_access (path, X_OK) == 0);


  if (parent_info)
    {
      gboolean writable;

      writable = FALSE;
      if (parent_info->writable)
	{
	  if (parent_info->is_sticky)
	    {
	      uid_t uid = geteuid ();

	      if (uid == statbuf->st_uid ||
		  uid == parent_info->owner ||
		  uid == 0)
		writable = TRUE;
	    }
	  else
	    writable = TRUE;
	}

      if (g_file_attribute_matcher_matches (attribute_matcher, G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME))
	g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME,
					   writable);
      
      if (g_file_attribute_matcher_matches (attribute_matcher, G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE))
	g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE,
					   writable);
    }
}

static void
set_info_from_stat (GFileInfo *info, struct stat *statbuf,
		    GFileAttributeMatcher *attribute_matcher)
{
  GFileType file_type;

  file_type = G_FILE_TYPE_UNKNOWN;

  if (S_ISREG (statbuf->st_mode))
    file_type = G_FILE_TYPE_REGULAR;
  else if (S_ISDIR (statbuf->st_mode))
    file_type = G_FILE_TYPE_DIRECTORY;
  else if (S_ISCHR (statbuf->st_mode) ||
	   S_ISBLK (statbuf->st_mode) ||
	   S_ISFIFO (statbuf->st_mode)
#ifdef S_ISSOCK
	   || S_ISSOCK (statbuf->st_mode)
#endif
	   )
    file_type = G_FILE_TYPE_SPECIAL;
#ifdef S_ISLNK
  else if (S_ISLNK (statbuf->st_mode))
    file_type = G_FILE_TYPE_SYMBOLIC_LINK;
#endif

  g_file_info_set_file_type (info, file_type);
  g_file_info_set_size (info, statbuf->st_size);

  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_DEVICE, statbuf->st_dev);
  g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_UNIX_INODE, statbuf->st_ino);
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_MODE, statbuf->st_mode);
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_NLINK, statbuf->st_nlink);
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_UID, statbuf->st_uid);
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_GID, statbuf->st_uid);
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_RDEV, statbuf->st_rdev);
#if defined (HAVE_STRUCT_STAT_BLKSIZE)
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_BLOCK_SIZE, statbuf->st_blksize);
#endif
#if defined (HAVE_STRUCT_STAT_BLOCKS)
  g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_UNIX_BLOCKS, statbuf->st_blocks);
#endif
  
  g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED, statbuf->st_mtime);
#if defined (HAVE_STRUCT_STAT_ST_MTIMENSEC)
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC, statbuf->st_mtimensec / 1000);
#elif defined (HAVE_STRUCT_STAT_ST_MTIM_TV_NSEC)
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC, statbuf->st_mtim.tv_nsec / 1000);
#endif
  
  g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_ACCESS, statbuf->st_atime);
#if defined (HAVE_STRUCT_STAT_ST_ATIMENSEC)
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_TIME_ACCESS_USEC, statbuf->st_atimensec / 1000);
#elif defined (HAVE_STRUCT_STAT_ST_ATIM_TV_NSEC)
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_TIME_ACCESS_USEC, statbuf->st_atim.tv_nsec / 1000);
#endif
  
  g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_CHANGED, statbuf->st_ctime);
#if defined (HAVE_STRUCT_STAT_ST_CTIMENSEC)
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_TIME_CHANGED_USEC, statbuf->st_ctimensec / 1000);
#elif defined (HAVE_STRUCT_STAT_ST_CTIM_TV_NSEC)
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_TIME_CHANGED_USEC, statbuf->st_ctim.tv_nsec / 1000);
#endif

  if (g_file_attribute_matcher_matches (attribute_matcher,
					G_FILE_ATTRIBUTE_ETAG_VALUE))
    {
      char *etag = _g_local_file_info_create_etag (statbuf);
      g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_ETAG_VALUE, etag);
      g_free (etag);
    }

}

GFileInfo *
_g_local_file_info_get (const char *basename,
			const char *path,
			GFileAttributeMatcher *attribute_matcher,
			GFileGetInfoFlags flags,
			GLocalParentFileInfo *parent_info,
			GError **error)
{
  GFileInfo *info;
  struct stat statbuf;
  struct stat statbuf2;
  int res;
  gboolean is_symlink, symlink_broken;

  info = g_file_info_new ();

  g_file_info_set_name (info, basename);

  /* Avoid stat in trivial case */
  if (attribute_matcher == NULL)
    return info;

  res = g_lstat (path, &statbuf);
  if (res == -1)
    {
      g_object_unref (info);
      g_set_error (error, G_IO_ERROR,
		   g_io_error_from_errno (errno),
		   _("Error stating file '%s': %s"),
		   path, g_strerror (errno));
      return NULL;
    }

#ifdef S_ISLNK
  is_symlink = S_ISLNK (statbuf.st_mode);
#else
  is_symlink = FALSE;
#endif
  symlink_broken = FALSE;
  
  if (is_symlink)
    {
      g_file_info_set_is_symlink (info, TRUE);

      /* Unless NOFOLLOW was set we default to following symlinks */
      if (!(flags & G_FILE_GET_INFO_NOFOLLOW_SYMLINKS))
	{
	  res = stat (path, &statbuf2);

	    /* Report broken links as symlinks */
	  if (res != -1)
	    {
	      statbuf = statbuf2;
	      symlink_broken = TRUE;
	    }
	}
    }

  set_info_from_stat (info, &statbuf, attribute_matcher);
  
  if (basename != NULL && basename[0] == '.')
    g_file_info_set_is_hidden (info, TRUE);

  if (basename != NULL && basename[strlen (basename) -1] == '~')
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_STD_IS_BACKUP, TRUE);

  if (is_symlink &&
      g_file_attribute_matcher_matches (attribute_matcher,
					G_FILE_ATTRIBUTE_STD_SYMLINK_TARGET))
    {
      char *link = read_link (path);
      g_file_info_set_symlink_target (info, link);
      g_free (link);
    }

  if (g_file_attribute_matcher_matches (attribute_matcher,
					G_FILE_ATTRIBUTE_STD_DISPLAY_NAME))
    {
      char *display_name = g_filename_display_basename (path);
      
      if (strstr (display_name, "\357\277\275") != NULL)
	{
	  char *p = display_name;
	  display_name = g_strconcat (display_name, _(" (invalid encoding)"), NULL);
	  g_free (p);
	}
      g_file_info_set_display_name (info, display_name);
      g_free (display_name);
    }
  
  if (g_file_attribute_matcher_matches (attribute_matcher,
					G_FILE_ATTRIBUTE_STD_EDIT_NAME))
    {
      char *edit_name = g_filename_display_basename (path);
      g_file_info_set_edit_name (info, edit_name);
      g_free (edit_name);
    }

  if (g_file_attribute_matcher_matches (attribute_matcher,
					G_FILE_ATTRIBUTE_STD_CONTENT_TYPE))
    {
      /* TODO: Add windows specific code */

      if (is_symlink &&
	  (symlink_broken || (flags & G_FILE_GET_INFO_NOFOLLOW_SYMLINKS)))
	g_file_info_set_content_type (info, "inode/symlink");
      else if (S_ISDIR(statbuf.st_mode))
	g_file_info_set_content_type (info, "inode/directory");
      else if (S_ISCHR(statbuf.st_mode))
	g_file_info_set_content_type (info, "inode/chardevice");
      else if (S_ISBLK(statbuf.st_mode))
	g_file_info_set_content_type (info, "inode/blockdevice");
      else if (S_ISFIFO(statbuf.st_mode))
	g_file_info_set_content_type (info, "inode/fifo");
#ifdef S_ISSOCK
      else if (S_ISSOCK(statbuf.st_mode))
	g_file_info_set_content_type (info, "inode/socket");
#endif
      else
	{
	  char *content_type;
	  
	  content_type = g_content_type_guess (basename, NULL, 0);

#ifndef G_OS_WIN32
	  if (g_content_type_is_unknown (content_type) && path != NULL)
	    {
	      guchar sniff_buffer[4096];
	      gsize sniff_length;
	      int fd;
	      
	      sniff_length = _g_unix_content_type_get_sniff_len ();
	      if (sniff_length > 4096)
		sniff_length = 4096;

	      fd = open (path, O_RDONLY);
	      if (fd != -1)
		{
		  ssize_t res;

		  res = read (fd, sniff_buffer, sniff_length);
		  close (fd);
		  if (res != -1)
		    {
		      g_free (content_type);
		      content_type = g_content_type_guess (basename, sniff_buffer, sniff_length);
		    }
		  
		}
	    }
#endif

	  g_file_info_set_content_type (info, content_type);
	  g_free (content_type);
	}
            
    }
  
  if (g_file_attribute_matcher_matches (attribute_matcher,
					G_FILE_ATTRIBUTE_STD_ICON))
    {
      /* TODO */
    }

  get_access_rights (attribute_matcher, info, path, &statbuf, parent_info);
  
  get_selinux_context (path, info, attribute_matcher, (flags & G_FILE_GET_INFO_NOFOLLOW_SYMLINKS) == 0);
  get_xattrs (path, info, attribute_matcher, (flags & G_FILE_GET_INFO_NOFOLLOW_SYMLINKS) == 0);
  
  return info;
}

GFileInfo *
_g_local_file_info_get_from_fd (int fd,
				char *attributes,
				GError **error)
{
  struct stat stat_buf;
  GFileAttributeMatcher *matcher;
  GFileInfo *info;
  
  if (fstat (fd, &stat_buf) == -1)
    {
      g_set_error (error, G_IO_ERROR,
		   g_io_error_from_errno (errno),
		   _("Error stating file descriptor: %s"),
		   g_strerror (errno));
      return NULL;
    }

  info = g_file_info_new ();

  matcher = g_file_attribute_matcher_new (attributes);
  
  set_info_from_stat (info, &stat_buf, matcher);
  
#ifdef HAVE_SELINUX
  if (g_file_attribute_matcher_matches (matcher, "selinux:context") &&
      is_selinux_enabled ())
    {
      char *context;
      if (fgetfilecon_raw (fd, &context) >= 0)
	{
	  g_file_info_set_attribute_string (info, "selinux:context", context);
	  freecon(context);
	}
    }
#endif

  get_xattrs_from_fd (fd, info, matcher);
  
  g_file_attribute_matcher_free (matcher);
  
  return info;
}

static gboolean
get_uint32 (const GFileAttributeValue *value,
	    guint32 *val_out,
	    GError **error)
{
  if (value->type != G_FILE_ATTRIBUTE_TYPE_UINT32)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		   _("Invalid attribute type (uint32 expected)"));
      return FALSE;
    }

  *val_out = value->u.uint32;
  
  return TRUE;
}

#if defined(HAVE_SYMLINK)
static gboolean
get_byte_string (const GFileAttributeValue *value,
		 const char **val_out,
		 GError **error)
{
  if (value->type != G_FILE_ATTRIBUTE_TYPE_BYTE_STRING)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		   _("Invalid attribute type (byte string expected)"));
      return FALSE;
    }

  *val_out = value->u.string;
  
  return TRUE;
}
#endif

gboolean
_g_local_file_info_set_attribute (char *filename,
				  const char *attribute,
				  const GFileAttributeValue *value,
				  GFileGetInfoFlags flags,
				  GCancellable *cancellable,
				  GError **error)
{
  if (strcmp (attribute, G_FILE_ATTRIBUTE_UNIX_MODE) == 0)
    {
      guint32 val;

      if (!get_uint32 (value, &val, error))
	  return FALSE;

      if (g_chmod (filename, val) == -1)
	{
	  g_set_error (error, G_IO_ERROR,
		       g_io_error_from_errno (errno),
		       _("Error setting permissions: %s"),
		       g_strerror (errno));
	  return FALSE;
	}
      return TRUE;
    }
#ifdef HAVE_CHOWN
  else if (strcmp (attribute, G_FILE_ATTRIBUTE_UNIX_UID) == 0)
    {
      int res;
      guint32 val;

      if (!get_uint32 (value, &val, error))
	  return FALSE;

      if (flags & G_FILE_GET_INFO_NOFOLLOW_SYMLINKS)
	res = lchown (filename, val, -1);
      else
	res = chown (filename, val, -1);
	
      if (res == -1)
	{
	  g_set_error (error, G_IO_ERROR,
		       g_io_error_from_errno (errno),
		       _("Error setting owner: %s"),
		       g_strerror (errno));
	  return FALSE;
	}
      return TRUE;
    }
#endif
#ifdef HAVE_CHOWN
  else if (strcmp (attribute, G_FILE_ATTRIBUTE_UNIX_GID) == 0)
    {
      int res;
      guint32 val;

      if (!get_uint32 (value, &val, error))
	  return FALSE;

      if (flags & G_FILE_GET_INFO_NOFOLLOW_SYMLINKS)
	res = lchown (filename, -1, val);
      else
	res = chown (filename, -1, val);
	
      if (res == -1)
	{
	  g_set_error (error, G_IO_ERROR,
		       g_io_error_from_errno (errno),
		       _("Error setting owner: %s"),
		       g_strerror (errno));
	  return FALSE;
	}
      return TRUE;
    }
#endif
#ifdef HAVE_SYMLINK
  else if (strcmp (attribute, G_FILE_ATTRIBUTE_STD_SYMLINK_TARGET) == 0)
    {
      const char *val;
      struct stat statbuf;

      if (!get_byte_string (value, &val, error))
	  return FALSE;
      
      
      if (val == NULL)
	{
	  g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		       _("symlink must be non-NULL"));
	  return FALSE;
	}

      if (g_lstat (filename, &statbuf))
	{
	  g_set_error (error, G_IO_ERROR,
		       g_io_error_from_errno (errno),
		       _("Error setting symlink: %s"),
		       g_strerror (errno));
	  return FALSE;
	}

	if (!S_ISLNK (statbuf.st_mode))
	  {
	  g_set_error (error, G_IO_ERROR,
		       G_IO_ERROR_NOT_SYMBOLIC_LINK,
		       _("Error setting symlink: file is not a symlink"));
	  return FALSE;
	}

	if (g_unlink (filename))
	  {
	    g_set_error (error, G_IO_ERROR,
			 g_io_error_from_errno (errno),
			 _("Error setting symlink: %s"),
			 g_strerror (errno));
	    return FALSE;
	  }

	if (symlink (filename, val) != 0)
	  {
	    g_set_error (error, G_IO_ERROR,
			 g_io_error_from_errno (errno),
			 _("Error setting symlink: %s"),
			 g_strerror (errno));
	    return FALSE;
	  }
	
	return TRUE;
    }
  #endif
  
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
	       _("Setting attribute %s not supported"), attribute);
  return FALSE;
}
