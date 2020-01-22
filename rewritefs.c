/* rewritefs.c - mod_rewrite-like FUSE filesystem
 * Copyright 2010 Simon Lipp
 *
 * This program can be distributed under the terms of the GNU GPL.
 * See the file COPYING.
 *
 * Based on Miklos Szeredi's fuserewrite_fh.c:
 * FUSE: Filesystem in Userspace
 * Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>
 */

#define FUSE_USE_VERSION 26

#define _GNU_SOURCE

#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <sys/time.h>
#include <pthread.h>
#ifdef HAVE_SETXATTR
#include <sys/xattr.h>
#endif

#include "rewrite.h"

/* Lock for process EUID/EGID/umask */
static pthread_rwlock_t rwlock = PTHREAD_RWLOCK_INITIALIZER;

#define RLOCK(expr) { \
    pthread_rwlock_rdlock(&rwlock);\
    expr; \
    pthread_rwlock_unlock(&rwlock); \
}

#define WLOCK(expr) { \
    pthread_rwlock_wrlock(&rwlock); \
    uid_t _euid = geteuid(); gid_t _egid = getegid(); mode_t _umask = umask(fuse_get_context()->umask); \
    seteuid(fuse_get_context()->uid); setegid(fuse_get_context()->gid); \
    expr; \
    seteuid(_euid); setegid(_egid); umask(_umask); \
    pthread_rwlock_unlock(&rwlock); \
}

static int rewrite_getattr(const char *path, struct stat *stbuf) {
    int res;
    char *new_path = rewrite(path);
    if (new_path == NULL)
        return -ENOMEM;

    RLOCK(res = fstatat(orig_fd(), new_path, stbuf, AT_SYMLINK_NOFOLLOW));
    free(new_path);
    if (res == -1)
        return -errno;

    return 0;
}

static int rewrite_fgetattr(const char *path, struct stat *stbuf,
        struct fuse_file_info *fi) {
    int res;

    (void) path;

    RLOCK(res = fstat(fi->fh, stbuf));
    if (res == -1)
        return -errno;

    return 0;
}

static int rewrite_access(const char *path, int mask) {
    int res;
    char *new_path = rewrite(path);
    if (new_path == NULL)
        return -ENOMEM;

    RLOCK(res = faccessat(orig_fd(), new_path, mask, 0));
    free(new_path);
    if (res == -1)
        return -errno;

    return 0;
}

static int rewrite_readlink(const char *path, char *buf, size_t size) {
    int res;
    char *new_path = rewrite(path);
    if (new_path == NULL)
        return -ENOMEM;

    RLOCK(res = readlinkat(orig_fd(), new_path, buf, size - 1));
    free(new_path);
    if (res == -1)
        return -errno;

    buf[res] = '\0';
    return 0;
}

struct rewrite_dirp {
    DIR *dp;
    struct dirent *entry;
    off_t offset;
};

static int rewrite_opendir(const char *path, struct fuse_file_info *fi) {
    int fd, res;
    char *new_path;
    struct rewrite_dirp *d = malloc(sizeof(struct rewrite_dirp));
    if (d == NULL)
        return -ENOMEM;

    new_path = rewrite(path);
    if (new_path == NULL)
        return -ENOMEM;

    RLOCK(fd = openat(orig_fd(), new_path, O_RDONLY));
    free(new_path);
    if(fd == -1) {
        res = -errno;
        free(d);
        return res;
    }

    RLOCK(d->dp = fdopendir(fd));

    if (d->dp == NULL) {
        res = -errno;
        close(fd);
        free(d);
        return res;
    }
    d->offset = 0;
    d->entry = NULL;

    fi->fh = (unsigned long) d;
    return 0;
}

static inline struct rewrite_dirp *get_dirp(struct fuse_file_info *fi) {
    return (struct rewrite_dirp *) (uintptr_t) fi->fh;
}

static int rewrite_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
        off_t offset, struct fuse_file_info *fi) {
    struct rewrite_dirp *d = get_dirp(fi);

    (void) path;
    if (offset != d->offset) {
        RLOCK(seekdir(d->dp, offset));
        d->entry = NULL;
        d->offset = offset;
    }
    while (1) {
        struct stat st;
        off_t nextoff;

        if (!d->entry) {
            RLOCK(d->entry = readdir(d->dp));
            if (!d->entry)
                break;
        }

        memset(&st, 0, sizeof(st));
        st.st_ino = d->entry->d_ino;
        st.st_mode = d->entry->d_type << 12;
        RLOCK(nextoff = telldir(d->dp));
        if (filler(buf, d->entry->d_name, &st, nextoff))
            break;

        d->entry = NULL;
        d->offset = nextoff;
    }

    return 0;
}

static int rewrite_releasedir(const char *path, struct fuse_file_info *fi) {
    struct rewrite_dirp *d = get_dirp(fi);
    (void) path;
    RLOCK(closedir(d->dp));
    free(d);
    return 0;
}

static int rewrite_mknod(const char *path, mode_t mode, dev_t rdev) {
    int res;
    char *new_path = rewrite(path);
    if (new_path == NULL)
        return -ENOMEM;

    WLOCK(res = mknodat(orig_fd(), new_path, mode, rdev));
    free(new_path);
    if (res == -1)
        return -errno;

    return 0;
}

static int rewrite_mkdir(const char *path, mode_t mode) {
    int res;
    char *new_path = rewrite(path);
    if (new_path == NULL)
        return -ENOMEM;

    WLOCK(res = mkdirat(orig_fd(), new_path, mode));
    free(new_path);
    if (res == -1)
        return -errno;

    return 0;
}

static int rewrite_unlink(const char *path) {
    int res;
    char *new_path = rewrite(path);
    if (new_path == NULL)
        return -ENOMEM;

    RLOCK(res = unlinkat(orig_fd(), new_path, 0));
    if (res == -1)
        return -errno;
    free(new_path);

    return 0;
}

static int rewrite_rmdir(const char *path) {
    int res;
    char *new_path = rewrite(path);
    if (new_path == NULL)
        return -ENOMEM;

    RLOCK(res = unlinkat(orig_fd(), new_path, AT_REMOVEDIR));
    if (res == -1)
        return -errno;
    free(new_path);

    return 0;
}

static int rewrite_symlink(const char *from, const char *to) {
    int res;
    char *new_to;
    if ((new_to = rewrite(to)) == NULL)
        return -ENOMEM;

    WLOCK(res = symlinkat(from, orig_fd(), new_to));
    free(new_to);
    if (res == -1)
        return -errno;

    return 0;
}

static int rewrite_rename(const char *from, const char *to) {
    int res;
    char *new_from, *new_to;
    if ((new_from = rewrite(from)) == NULL)
        return -ENOMEM;
    if ((new_to = rewrite(to)) == NULL) {
        free(new_from);
        return -ENOMEM;
    }

    RLOCK(res = renameat(orig_fd(), new_from, orig_fd(), new_to));
    free(new_from);
    free(new_to);
    if (res == -1)
        return -errno;

    return 0;
}

static int rewrite_link(const char *from, const char *to) {
    int res;
    char *new_from, *new_to;
    if ((new_from = rewrite(from)) == NULL)
        return -ENOMEM;
    if ((new_to = rewrite(to)) == NULL) {
        free(new_from);
        return -ENOMEM;
    }

    RLOCK(res = linkat(orig_fd(), new_from, orig_fd(), new_to, 0));
    free(new_from);
    free(new_to);
    if (res == -1)
        return -errno;

    return 0;
}

static int rewrite_chmod(const char *path, mode_t mode) {
    int res;
    char *new_path = rewrite(path);
    if (new_path == NULL)
        return -ENOMEM;

    RLOCK(res = fchmodat(orig_fd(), new_path, mode, 0));
    if (res == -1)
        return -errno;

    return 0;
}

static int rewrite_chown(const char *path, uid_t uid, gid_t gid) {
    int res;
    char *new_path = rewrite(path);
    if (new_path == NULL)
        return -ENOMEM;

    RLOCK(res = fchownat(orig_fd(), new_path, uid, gid, 0));
    free(new_path);
    if (res == -1)
        return -errno;

    return 0;
}

static int rewrite_truncate(const char *path, off_t size) {
    int fd, res;
    char *new_path = rewrite(path);
    if (new_path == NULL)
        return -ENOMEM;

    RLOCK(fd = openat(orig_fd(), new_path, O_WRONLY));
    free(new_path);
    if (fd == -1)
        return -errno;

    RLOCK(res = ftruncate(fd, size));
    close(fd);
    if (res == -1)
        return -errno;

    return 0;
}

static int rewrite_ftruncate(const char *path, off_t size,
        struct fuse_file_info *fi) {
    int res;

    (void) path;

    RLOCK(res = ftruncate(fi->fh, size));
    if (res == -1)
        return -errno;

    return 0;
}

static int rewrite_utimens(const char *path, const struct timespec ts[2]) {
    int res;
    char *new_path = rewrite(path);
    if (new_path == NULL)
        return -ENOMEM;

    RLOCK(res = utimensat(orig_fd(), new_path, ts, 0));
    free(new_path);
    if (res == -1)
        return -errno;

    return 0;
}

static int rewrite_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
    int fd;
    char *new_path = rewrite(path);
    if (new_path == NULL)
        return -ENOMEM;

    WLOCK(fd = openat(orig_fd(), new_path, fi->flags | O_CREAT, mode));
    free(new_path);
    if (fd == -1)
        return -errno;

    fi->fh = fd;
    return 0;
}

static int rewrite_open(const char *path, struct fuse_file_info *fi) {
    int fd;
    char *new_path = rewrite(path);
    if (new_path == NULL)
        return -ENOMEM;

    if (fi->flags & O_CREAT) {
        WLOCK(fd = openat(orig_fd(), new_path, fi->flags));
    } else {
        RLOCK(fd = openat(orig_fd(), new_path, fi->flags));
    }
    free(new_path);
    if (fd == -1)
        return -errno;

    fi->fh = fd;
    return 0;
}

static int rewrite_read(const char *path, char *buf, size_t size, off_t offset,
        struct fuse_file_info *fi) {
    int res;

    (void) path;
    RLOCK(res = pread(fi->fh, buf, size, offset));
    if (res == -1)
        res = -errno;

    return res;
}

static int rewrite_write(const char *path, const char *buf, size_t size,
        off_t offset, struct fuse_file_info *fi) {
    int res;

    (void) path;
    RLOCK(res = pwrite(fi->fh, buf, size, offset));
    if (res == -1)
        res = -errno;

    return res;
}

static int rewrite_statfs(const char *path, struct statvfs *stbuf) {
    int res, fd;
    char *new_path = rewrite(path);
    if (new_path == NULL)
        return -ENOMEM;

    RLOCK(fd = openat(orig_fd(), new_path, O_RDONLY));
    free(new_path);
    if (fd == -1)
        return -errno;

    RLOCK(res = fstatvfs(fd, stbuf));
    close(fd);
    if (res == -1)
        return -errno;

    return 0;
}

static int rewrite_flush(const char *path, struct fuse_file_info *fi) {
    int res;

    (void) path;
    RLOCK(res = close(dup(fi->fh)));
    if (res == -1)
        return -errno;

    return 0;
}

static int rewrite_release(const char *path, struct fuse_file_info *fi) {
    (void) path;
    RLOCK(close(fi->fh));

    return 0;
}

static int rewrite_fsync(const char *path, int isdatasync,
        struct fuse_file_info *fi) {
    int res;
    (void) path;

#ifndef HAVE_FDATASYNC
    (void) isdatasync;
#else
    if (isdatasync) {
        RLOCK(res = fdatasync(fi->fh));
    } else
#endif
    {
        RLOCK(res = fsync(fi->fh));
    }
    if (res == -1)
        return -errno;

    return 0;
}

#ifdef HAVE_SETXATTR
static int rewrite_setxattr(const char *path, const char *name, const char *value,
        size_t size, int flags) {
    int res, fd;
    char *new_path = rewrite(path);
    if (new_path == NULL)
        return -ENOMEM;

    RLOCK(fd = openat(orig_fd(), new_path, O_RDONLY));
    free(new_path);
    if (fd == -1)
        return -errno;

    RLOCK(res = fsetxattr(fd, name, value, size, flags));
    close(fd);
    if (res == -1)
        return -errno;
    return 0;
}

static int rewrite_getxattr(const char *path, const char *name, char *value,
        size_t size) {
    int res, fd;
    char *new_path = rewrite(path);
    if (new_path == NULL)
        return -ENOMEM;

    RLOCK(fd = openat(orig_fd(), new_path, O_RDONLY));
    free(new_path);
    if (fd == -1)
        return -errno;

    RLOCK(res = fgetxattr(fd, name, value, size));
    close(fd);
    if (res == -1)
        return -errno;
    return res;
}

static int rewrite_listxattr(const char *path, char *list, size_t size) {
    int res, fd;
    char *new_path = rewrite(path);
    if (new_path == NULL)
        return -ENOMEM;

    RLOCK(fd = openat(orig_fd(), new_path, O_RDONLY));
    free(new_path);
    if (fd == -1)
        return -errno;

    RLOCK(res = flistxattr(fd, list, size));
    close(fd);
    if (res == -1)
        return -errno;
    return res;
}

static int rewrite_removexattr(const char *path, const char *name) {
    int res, fd;
    char *new_path = rewrite(path);
    if (new_path == NULL)
        return -ENOMEM;

    RLOCK(fd = openat(orig_fd(), new_path, O_RDONLY));
    free(new_path);
    if (fd == -1)
        return -errno;

    RLOCK(res = fremovexattr(fd, name));
    close(fd);
    if (res == -1)
        return -errno;
    return 0;
}
#endif /* HAVE_SETXATTR */

static int rewrite_lock(const char *path, struct fuse_file_info *fi, int cmd,
        struct flock *lock) {
    int res;

    (void) path;
    RLOCK(res = fcntl(fi->fh, cmd, lock));
    if (res == -1)
        return -errno;

    return res;
}

static struct fuse_operations rewrite_oper = {
    .getattr     = rewrite_getattr,
    .fgetattr    = rewrite_fgetattr,
    .access      = rewrite_access,
    .readlink    = rewrite_readlink,
    .opendir     = rewrite_opendir,
    .readdir     = rewrite_readdir,
    .releasedir  = rewrite_releasedir,
    .mknod       = rewrite_mknod,
    .mkdir       = rewrite_mkdir,
    .symlink     = rewrite_symlink,
    .unlink      = rewrite_unlink,
    .rmdir       = rewrite_rmdir,
    .rename      = rewrite_rename,
    .link        = rewrite_link,
    .chmod       = rewrite_chmod,
    .chown       = rewrite_chown,
    .truncate    = rewrite_truncate,
    .ftruncate   = rewrite_ftruncate,
    .utimens     = rewrite_utimens,
    .create      = rewrite_create,
    .open        = rewrite_open,
    .read        = rewrite_read,
    .write       = rewrite_write,
    .statfs      = rewrite_statfs,
    .flush       = rewrite_flush,
    .release     = rewrite_release,
    .fsync       = rewrite_fsync,
#ifdef HAVE_SETXATTR
    .setxattr    = rewrite_setxattr,
    .getxattr    = rewrite_getxattr,
    .listxattr   = rewrite_listxattr,
    .removexattr = rewrite_removexattr,
#endif
    .lock        = rewrite_lock,

    .flag_nullpath_ok = 1,
};

int main(int argc, char *argv[]) {
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

    umask(0);
    parse_args(argc, argv, &args);
    return fuse_main(args.argc, args.argv, &rewrite_oper, NULL);
}
