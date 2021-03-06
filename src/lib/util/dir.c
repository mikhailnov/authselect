/*
    Authors:
        Pavel Březina <pbrezina@redhat.com>

    Copyright (C) 2019 Red Hat

    This program is free software; you can redistribute it and/or modify
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

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/common.h"
#include "lib/util/dir.h"
#include "lib/util/file.h"
#include "lib/util/string_array.h"

static int
compare_timespec(struct timespec *a, struct timespec *b)
{
    if (a->tv_sec == b->tv_sec) {
        if (a->tv_nsec < b->tv_nsec) {
            return -1;
        }

        if (a->tv_nsec > b->tv_nsec) {
            return 1;
        }

        return 0;
    }

    if (a->tv_sec < b->tv_sec) {
        return -1;
    }

    if (a->tv_sec > b->tv_sec) {
        return 1;
    }

    return 0;
}

static int
sort_by_ctime(const void *a, const void *b, void *arg)
{
    const char *name_a = *(const char **)a;
    const char *name_b = *(const char **)b;
    int dirfd = *(int *)arg;
    const char *base_a;
    const char *base_b;
    struct stat stat_a;
    struct stat stat_b;
    errno_t ret;

    base_a = file_get_basename(name_a);
    if (base_a == NULL) {
        ERROR("Unable to get basename of [%s]", name_a);
        return 0;
    }

    base_b = file_get_basename(name_b);
    if (base_b == NULL) {
        ERROR("Unable to get basename of [%s]", name_b);
        return 0;
    }

    ret = fstatat(dirfd, base_a, &stat_a, 0);
    if (ret != 0) {
        ret = errno;
        ERROR("Unable to stat [%s] [%d]: %s", name_a, ret, strerror(ret));
        return 0;
    }

    ret = fstatat(dirfd, base_b, &stat_b, 0);
    if (ret != 0) {
        ret = errno;
        ERROR("Unable to stat [%s] [%d]: %s", name_b, ret, strerror(ret));
        return 0;
    }

    return compare_timespec(&stat_a.st_ctim, &stat_b.st_ctim);
}

static bool
is_dot_dir(const char *name)
{
    return strcmp(name, ".") == 0 || strcmp(name, "..") == 0;
}

static bool
is_directory(struct dirent *entry, int dirfd)
{
    struct stat statres;
    errno_t ret;

#ifdef _DIRENT_HAVE_D_TYPE
    if (entry->d_type == DT_DIR) {
        return true;
    } else if (entry->d_type != DT_UNKNOWN) {
        return false;
    }
#endif

    /* We must use stat() if d_type is not available or it couldn't determine
     * the type (which may happen on some filesystems). */

    ret = fstatat(dirfd, entry->d_name, &statres, 0);
    if (ret != 0) {
        ret = errno;
        ERROR("Unable to stat directory [%d]: %s", ret, strerror(ret));
        return false;
    }

    if (S_ISDIR(statres.st_mode)) {
        return true;
    }

    return false;
}

static errno_t
dir_open(const char *path,
         DIR **_dirstream,
         int *_descriptor)
{
    DIR *dirstream;
    int descriptor;
    errno_t ret;

    if (path == NULL) {
        return EINVAL;
    }

    dirstream = opendir(path);
    if (dirstream == NULL) {
        /* To silence static analyzers that expect errno == 0. */
        return errno == EOK ? EINVAL : errno;
    }

    /* Descriptor is closed when closedir() is called. */
    descriptor = dirfd(dirstream);
    if (descriptor == -1) {
        ret = errno;
        closedir(dirstream);
        return ret;
    }

    *_dirstream = dirstream;
    *_descriptor = descriptor;

    return EOK;
}

errno_t
dir_list(const char *path,
         uint32_t flags,
         char ***_items,
         int *_dirfd)
{
    struct dirent *entry;
    DIR *dirstream;
    char *fullpath;
    char **items;
    int dirfd;
    int dupfd;
    errno_t ret;

    ret = dir_open(path, &dirstream, &dirfd);
    if (ret != EOK) {
        return ret;
    }

    items = string_array_create(1);
    if (items == NULL) {
        ret = ENOMEM;
        goto done;
    }

    errno = 0;
    while ((entry = readdir(dirstream)) != NULL) {
        if (is_dot_dir(entry->d_name)) {
            continue;
        }

        if (is_directory(entry, dirfd)) {
            if (!(flags & DIR_LIST_DIRS)) {
                continue;
            }
        } else {
            if (!(flags & DIR_LIST_FILES)) {
                continue;
            }
        }

        if (flags & DIR_LIST_FULL_PATH) {
            fullpath = format("%s/%s", path, entry->d_name);
            if (fullpath == NULL) {
                ret = ENOMEM;
                goto done;
            }

            items = string_array_add_value(items, fullpath, true);
            free(fullpath);
        } else {
            items = string_array_add_value(items, entry->d_name, true);
        }

        if (items == NULL) {
            ret = ENOMEM;
            goto done;
        }
    }

    if (flags & DIR_LIST_SORT_BY_CTIME) {
        qsort_r(items, string_array_count(items), sizeof(char *),
                sort_by_ctime, &dirfd);
    }

    if (_dirfd != NULL) {
        dupfd = dup(dirfd);
        if (dupfd == -1) {
            ret = errno;
            goto done;
        }

        *_dirfd = dupfd;
    }

    *_items = items;

    ret = EOK;

done:
    if (ret == EOK && _dirfd != NULL)  {
        closedir(dirstream);
    }

    return ret;
}

errno_t
dir_remove(const char *path)
{
    char **subdirs = NULL;
    char **files = NULL;
    int dirfd = -1;
    errno_t ret;
    int i;

    ret = dir_list(path, DIR_LIST_DIRS | DIR_LIST_FULL_PATH, &subdirs, NULL);
    if (ret == ENOENT) {
        ret = EOK;
        goto done;
    } else if (ret != EOK) {
        goto done;
    }

    ret = dir_list(path, DIR_LIST_FILES, &files, &dirfd);
    if (ret == ENOENT) {
        ret = EOK;
        goto done;
    } else if (ret != EOK) {
        goto done;
    }

    for (i = 0; subdirs[i] != NULL; i++) {
        ret = dir_remove(subdirs[i]);
        if (ret != EOK) {
            goto done;
        }
    }

    for (i = 0; files[i] != NULL; i++) {
        INFO("Removing file [%s/%s]", path, files[i]);
        ret = unlinkat(dirfd, files[i], 0);
        if (ret != 0) {
            ret = errno;
            goto done;
        }
    }

    INFO("Removing directory [%s]", path);
    ret = rmdir(path);
    if (ret != 0) {
        ret = errno;
        goto done;
    }

    ret = EOK;

done:
    string_array_free(subdirs);
    string_array_free(files);

    if (dirfd != -1) {
        close(dirfd);
    }

    return ret;
}
