/* rewritefs.c - mod_rewrite-like FUSE filesystem
 * Copyright 2010-2017 Simon Lipp
 *
 * This program can be distributed under the terms of the GNU GPL.
 * See the file COPYING.
 *
 * Based on FUSE's passthrough_fh.c:
 * Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>
 * Copyright (C) 2011       Sebastian Pipping <sebastian@pipping.org>
 */

#define FUSE_USE_VERSION 31

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
#ifdef HAVE_SETXATTR
#include <sys/xattr.h>
#endif

#include "rewrite.h"

pthread_rwlock_t rwlock = PTHREAD_RWLOCK_INITIALIZER;

static void *rewrite_init(struct fuse_conn_info *conn,
                          struct fuse_config *cfg) {
    (void)conn;
    cfg->use_ino = 1;
    cfg->nullpath_ok = 1;
    cfg->entry_timeout = 0;
    cfg->attr_timeout = 0;
    cfg->negative_timeout = 0;
    cfg->hard_remove = 1;

    return NULL;
}

static int rewrite_getattr(const char *path, struct stat *stbuf,
                           struct fuse_file_info *fi) {
    int res;

    if(fi == NULL) {
        char *new_path = rewrite(path);
        if (new_path == NULL)
            return -ENOMEM;

        RLOCK(res = fstatat(orig_fd(), new_path, stbuf, AT_SYMLINK_NOFOLLOW));
        free(new_path);
    } else {
        RLOCK(res = fstat(fi->fh, stbuf));
    }

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
                           off_t offset, struct fuse_file_info *fi,
                           enum fuse_readdir_flags flags) {
    struct rewrite_dirp *d = get_dirp(fi);

    (void) path;
    (void) flags;
    if (offset != d->offset) {
        RLOCK(seekdir(d->dp, offset));
        d->entry = NULL;
        d->offset = offset;
    }
    for (;;) {
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
        if (filler(buf, d->entry->d_name, &st, nextoff, 0))
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
    char *new_to = rewrite(to);
    if (new_to == NULL)
        return -ENOMEM;

    WLOCK(res = symlinkat(from, orig_fd(), new_to));
    free(new_to);
    if (res == -1)
        return -errno;

    return 0;
}

static int rewrite_rename(const char *from, const char *to, unsigned int flags) {
    int res;
    char *new_from, *new_to;

    if (flags != 0)
        return -EINVAL;

    new_from = rewrite(from);
    new_to = rewrite(to);
    if (new_from == NULL || new_to == NULL) {
        free(new_from);
        free(new_to);
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
    char *new_from = rewrite(from),
         *new_to = rewrite(to);
    if (new_from == NULL)
        return -ENOMEM;
    if (new_to == NULL) {
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

static int rewrite_chmod(const char *path, mode_t mode,
                         struct fuse_file_info *fi) {
    int res;

    if(fi == NULL) {
        char *new_path = rewrite(path);
        if (new_path == NULL)
            return -ENOMEM;

        RLOCK(res = fchmodat(orig_fd(), new_path, mode, AT_SYMLINK_NOFOLLOW));
        free(new_path);
    } else {
        RLOCK(res = fchmod(fi->fh, mode));
    }

    if (res == -1)
        return -errno;

    return 0;
}

static int rewrite_chown(const char *path, uid_t uid, gid_t gid,
                         struct fuse_file_info *fi) {
    int res;

    if(fi == NULL) {
        char *new_path = rewrite(path);
        if (new_path == NULL)
            return -ENOMEM;

        RLOCK(res = fchownat(orig_fd(), new_path, uid, gid, AT_SYMLINK_NOFOLLOW));
        free(new_path);
    } else {
        RLOCK(res = fchown(fi->fh, uid, gid));
    }

    if (res == -1)
        return -errno;

    return 0;
}

static int rewrite_truncate(const char *path, off_t size,
                            struct fuse_file_info *fi) {
    int fd, res;
    if(fi == NULL) {
        char *new_path = rewrite(path);
        if (new_path == NULL)
            return -ENOMEM;

        RLOCK(fd = openat(orig_fd(), new_path, O_WRONLY));
        free(new_path);
        if (fd == -1)
            return -errno;

        RLOCK(res = ftruncate(fd, size));
        close(fd);
    } else {
        RLOCK(res = ftruncate(fi->fh, size));
    }

    if (res == -1)
        return -errno;

    return res;
}

static int rewrite_utimens(const char *path, const struct timespec ts[2],
                           struct fuse_file_info *fi) {
    int res;
    if (fi == NULL) {
        char *new_path = rewrite(path);
        if (new_path == NULL)
            return -ENOMEM;
        RLOCK(res = utimensat(orig_fd(), new_path, ts, AT_SYMLINK_NOFOLLOW));
        free(new_path);
    } else {
        RLOCK(res = futimens(fi->fh, ts));
    }

    if (res == -1)
        return -errno;

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

static int rewrite_read_buf(const char *path, struct fuse_bufvec **bufp,
                            size_t size, off_t offset, struct fuse_file_info *fi) {
    struct fuse_bufvec *src;

    (void) path;

    src = malloc(sizeof(struct fuse_bufvec));
    if (src == NULL)
        return -ENOMEM;

    *src = FUSE_BUFVEC_INIT(size);

    src->buf[0].flags = FUSE_BUF_IS_FD | FUSE_BUF_FD_SEEK;
    src->buf[0].fd = fi->fh;
    src->buf[0].pos = offset;

    *bufp = src;

    return 0;
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

static int rewrite_write_buf(const char *path, struct fuse_bufvec *buf,
                             off_t offset, struct fuse_file_info *fi) {
    struct fuse_bufvec dst = FUSE_BUFVEC_INIT(fuse_buf_size(buf));

    (void) path;

    dst.buf[0].flags = FUSE_BUF_IS_FD | FUSE_BUF_FD_SEEK;
    dst.buf[0].fd = fi->fh;
    dst.buf[0].pos = offset;

    return fuse_buf_copy(&dst, buf, FUSE_BUF_SPLICE_NONBLOCK);
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

static int rewrite_fallocate(const char *path, int mode,
                             off_t offset, off_t length, struct fuse_file_info *fi) {
    (void) path;
    if (mode)
        return -EOPNOTSUPP;
    return -posix_fallocate(fi->fh, offset, length);
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

static struct fuse_operations rewrite_oper = {
    .init        = rewrite_init,

    .getattr     = rewrite_getattr,
    .readlink    = rewrite_readlink,
    .mknod       = rewrite_mknod,
    .mkdir       = rewrite_mkdir,
    .unlink      = rewrite_unlink,
    .rmdir       = rewrite_rmdir,
    .symlink     = rewrite_symlink,
    .rename      = rewrite_rename,
    .link        = rewrite_link,
    .chmod       = rewrite_chmod,
    .chown       = rewrite_chown,
    .truncate    = rewrite_truncate,
    .open        = rewrite_open,
    .read        = rewrite_read,
    .write       = rewrite_write,
    .statfs      = rewrite_statfs,
    .flush       = rewrite_flush,
    .release     = rewrite_release,
    .fsync       = rewrite_fsync,
    .opendir     = rewrite_opendir,
    .readdir     = rewrite_readdir,
    .releasedir  = rewrite_releasedir,
    .access      = rewrite_access,
    .utimens     = rewrite_utimens,
    .read_buf    = rewrite_read_buf,
    .write_buf   = rewrite_write_buf,
    .fallocate   = rewrite_fallocate,

#ifdef HAVE_SETXATTR
    .setxattr    = rewrite_setxattr,
    .getxattr    = rewrite_getxattr,
    .listxattr   = rewrite_listxattr,
    .removexattr = rewrite_removexattr,
#endif
};

int main(int argc, char *argv[]) {
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

    umask(0);
    parse_args(argc, argv, &args);
    return fuse_main(args.argc, args.argv, &rewrite_oper, NULL);
}
