#include <pthread.h>

/* Lock for process EUID/EGID/umask */
extern pthread_rwlock_t rwlock;

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

void parse_args(int argc, char **argv, struct fuse_args *outargs);
char *rewrite(const char *path);
int orig_fd();
