/* fuse-overlayfs: Overlay Filesystem in Userspace

   Copyright (C) 2018 Giuseppe Scrivano <giuseppe@scrivano.org>
   Copyright (C) 2018-2019 Red Hat Inc.
   Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

#include <config.h>

#include "fuse-overlayfs.h"

#include "limits.h"
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <sys/xattr.h>

#include "utils.h"

#define XATTR_OVERRIDE_STAT "user.fuseoverlayfs.override_stat"
#define XATTR_PRIVILEGED_OVERRIDE_STAT "security.fuseoverlayfs.override_stat"

static int
direct_file_exists (struct ovl_layer *l, const char *pathname)
{
  return file_exists_at (l->fd, pathname);
}

static int
direct_listxattr (struct ovl_layer *l, const char *path, char *buf, size_t size)
{
  cleanup_close int fd = -1;
  char full_path[PATH_MAX];
  int ret;

  full_path[0] = '\0';
  ret = open_fd_or_get_path (l, path, full_path, &fd, O_RDONLY);
  if (ret < 0)
    return ret;

  if (fd >= 0)
    return flistxattr (fd, buf, size);

  return llistxattr (full_path, buf, size);
}

static int
direct_getxattr (struct ovl_layer *l, const char *path, const char *name, char *buf, size_t size)
{
  cleanup_close int fd = -1;
  char full_path[PATH_MAX];
  int ret;

  full_path[0] = '\0';
  ret = open_fd_or_get_path (l, path, full_path, &fd, O_RDONLY);
  if (ret < 0)
    return ret;

  if (fd >= 0)
    return fgetxattr (fd, name, buf, size);

  return lgetxattr (full_path, name, buf, size);
}

static int
override_mode (struct ovl_layer *l, int fd, const char *path, struct stat *st)
{
  int ret;
  uid_t uid;
  gid_t gid;
  mode_t mode;
  char buf[64];
  cleanup_close int cleanup_fd = -1;
  const char *xattr_name;

  if (l->has_stat_override == 0 && l->has_privileged_stat_override == 0)
    return 0;

  xattr_name = l->has_privileged_stat_override ? XATTR_PRIVILEGED_OVERRIDE_STAT : XATTR_OVERRIDE_STAT;

  if (fd >= 0)
    {
      ret = fgetxattr (fd, xattr_name, buf, sizeof (buf) - 1);
      if (ret < 0)
        return ret;
    }
  else
    {
      char full_path[PATH_MAX];

      full_path[0] = '\0';
      ret = open_fd_or_get_path (l, path, full_path, &cleanup_fd, O_RDONLY);
      if (ret < 0)
        return ret;
      fd = cleanup_fd;

      if (fd >= 0)
        ret = fgetxattr (fd, xattr_name, buf, sizeof (buf) - 1);
      else
          ret = lgetxattr (full_path, xattr_name, buf, sizeof (buf) - 1);

      if (ret < 0)
        return ret;
    }

  buf[ret] = '\0';

  ret = sscanf (buf, "%d:%d:%o", &uid, &gid, &mode);
  if (ret != 3)
    {
      errno = EINVAL;
      return -1;
    }

  st->st_uid = uid;
  st->st_gid = gid;
  st->st_mode = (st->st_mode & S_IFMT) | mode;

  return 0;
}


static int
direct_fstat (struct ovl_layer *l, int fd, const char *path, unsigned int mask, struct stat *st)
{
  int ret;
#ifdef HAVE_STATX
  struct statx stx;

  ret = statx (fd, "", AT_STATX_DONT_SYNC|AT_EMPTY_PATH, mask, &stx);

  if (ret < 0 && errno == ENOSYS)
    goto fallback;
  if (ret == 0)
    {
      statx_to_stat (&stx, st);
      return override_mode (l, fd, path, st);
    }

  return ret;
#endif

 fallback:
  ret = fstat (fd, st);
  if (ret != 0)
    return ret;

  return override_mode (l, fd, path, st);
}

static int
direct_statat (struct ovl_layer *l, const char *path, struct stat *st, int flags, unsigned int mask)
{
  int ret;
#ifdef HAVE_STATX
  struct statx stx;

  ret = statx (l->fd, path, AT_STATX_DONT_SYNC|flags, mask, &stx);

  if (ret < 0 && errno == ENOSYS)
    goto fallback;
  if (ret == 0)
    {
      statx_to_stat (&stx, st);
      return override_mode (l, -1, path, st);
    }

  return ret;
#endif
 fallback:
  ret = fstatat (l->fd, path, st, flags);
  if (ret != 0)
    return ret;

  return override_mode (l, -1, path, st);
}

static struct dirent *
direct_readdir (void *dirp)
{
  return readdir (dirp);
}

static void *
direct_opendir (struct ovl_layer *l, const char *path)
{
  cleanup_close int cleanup_fd = -1;
  DIR *dp = NULL;

  cleanup_fd = TEMP_FAILURE_RETRY (safe_openat (l->fd, path, O_DIRECTORY, 0));
  if (cleanup_fd < 0)
    return NULL;

  dp = fdopendir (cleanup_fd);
  if (dp == NULL)
    return NULL;

  cleanup_fd = -1;

  return dp;
}

static int
direct_closedir (void *dirp)
{
  return closedir (dirp);
}

static int
direct_openat (struct ovl_layer *l, const char *path, int flags, mode_t mode)
{
  return TEMP_FAILURE_RETRY (safe_openat (l->fd, path, flags, mode));
}

static ssize_t
direct_readlinkat (struct ovl_layer *l, const char *path, char *buf, size_t bufsiz)
{
  return TEMP_FAILURE_RETRY (readlinkat (l->fd, path, buf, bufsiz));
}

static int
direct_load_data_source (struct ovl_layer *l, const char *opaque, const char *path, int n_layer)
{
  char tmp[64];
  l->path = realpath (path, NULL);
  if (l->path == NULL)
    {
      fprintf (stderr, "cannot resolve path %s\n", path);
      return -1;
    }

  l->fd = open (path, O_DIRECTORY);
  if (l->fd < 0)
    {
      free (l->path);
      l->path = NULL;
      return l->fd;
    }

  if (fgetxattr (l->fd, XATTR_PRIVILEGED_OVERRIDE_STAT, tmp, sizeof (tmp)) >= 0)
    l->has_privileged_stat_override = 1;
  else if (fgetxattr (l->fd, XATTR_OVERRIDE_STAT, tmp, sizeof (tmp)) >= 0)
    l->has_stat_override = 1;

  return 0;
}

static int
direct_cleanup (struct ovl_layer *l)
{
  return 0;
}

static int
direct_num_of_layers (const char *opaque, const char *path)
{
  return 1;
}

static bool
direct_must_be_remapped (struct ovl_layer *l)
{
  return l->has_privileged_stat_override == 0 && l->has_stat_override == 0;
}

struct data_source direct_access_ds =
  {
   .num_of_layers = direct_num_of_layers,
   .load_data_source = direct_load_data_source,
   .cleanup = direct_cleanup,
   .file_exists = direct_file_exists,
   .statat = direct_statat,
   .fstat = direct_fstat,
   .opendir = direct_opendir,
   .readdir = direct_readdir,
   .closedir = direct_closedir,
   .openat = direct_openat,
   .getxattr = direct_getxattr,
   .listxattr = direct_listxattr,
   .readlinkat = direct_readlinkat,
   .must_be_remapped = direct_must_be_remapped,
  };
