#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <inttypes.h>
#include <pthread.h>

// Defaults
static off_t small_file_cutoff = 1 << 20; // 1 MiB
static int do_open_hint = 1; // apply POSIX_FADV_NOREUSE at open
static int do_close_drop  = 1; // apply POSIX_FADV_DONTNEED at close

// Real functions
static int   (*real_open)(const char *pathname, int flags, ...) = NULL;
static int   (*real_open64)(const char *pathname, int flags, ...) = NULL;
static FILE* (*real_fopen)(const char *pathname, const char *mode) = NULL;
static ssize_t (*real_read)(int fd, void *buf, size_t count) = NULL;
static ssize_t (*real_pread)(int fd, void *buf, size_t count, off_t offset) = NULL;
static ssize_t (*real_readv)(int fd, const struct iovec *iov, int iovcnt) = NULL;
static int   (*real_close)(int fd) = NULL;
static int   (*real_fclose)(FILE *stream) = NULL;

// once init
static pthread_once_t init_once = PTHREAD_ONCE_INIT;

static void load_config_from_env(void) {
    char *env;

    env = getenv("FADV_SMALL_CUTOFF");
    if (env) {
        off_t val = (off_t)strtoll(env, NULL, 10);
        if (val > 0) small_file_cutoff = val;
    }

    env = getenv("FADV_OPEN_HINT");
    if (env) {
        if (strcmp(env, "none") == 0) do_open_hint = 0;
        else if (strcmp(env, "noreuse") == 0) do_open_hint = 1;
    }

    env = getenv("FADV_CLOSE_DROP");
    if (env) {
        if (strcmp(env, "0") == 0) do_close_drop = 0;
        else do_close_drop = 1;
    }
}

static void init_real_fns(void) {
    real_open   = dlsym(RTLD_NEXT, "open");
    real_open64 = dlsym(RTLD_NEXT, "open64");
    real_fopen  = dlsym(RTLD_NEXT, "fopen");
    real_read   = dlsym(RTLD_NEXT, "read");
    real_pread  = dlsym(RTLD_NEXT, "pread");
    real_readv  = dlsym(RTLD_NEXT, "readv");
    real_close  = dlsym(RTLD_NEXT, "close");
    real_fclose = dlsym(RTLD_NEXT, "fclose");

    // load env config
    load_config_from_env();
}

// Try to call posix_fadvise DONTNEED on fd if it's a regular file and <= cutoff
static void try_drop_fd(int fd) {
    if (fd < 0) return;
    struct stat st;
    if (fstat(fd, &st) != 0) return;
    if (!S_ISREG(st.st_mode)) return; // only regular files
    if (st.st_size > small_file_cutoff) return;

    // Best-effort; ignore errors
    posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
}

// Try to call posix_fadvise NOREUSE at open time
static void try_open_hint(int fd) {
    if (!do_open_hint) return;
    if (fd < 0) return;
    struct stat st;
    if (fstat(fd, &st) != 0) return;
    if (!S_ISREG(st.st_mode)) return;
    if (st.st_size > small_file_cutoff) return;

    // Hint kernel not to retain in page cache (NOREUSE)
    posix_fadvise(fd, 0, 0, POSIX_FADV_NOREUSE);
}

/* ----- Interposed functions ----- */

int open(const char *pathname, int flags, ...) {
    pthread_once(&init_once, init_real_fns);

    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }

    int fd = -1;
    if (real_open) fd = real_open(pathname, flags, mode);
    else fd = syscall(SYS_open, pathname, flags, mode);

    try_open_hint(fd);
    return fd;
}

int open64(const char *pathname, int flags, ...) {
    pthread_once(&init_once, init_real_fns);

    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }

    int fd = -1;
    if (real_open64) fd = real_open64(pathname, flags, mode);
    else fd = syscall(SYS_open, pathname, flags, mode);

    try_open_hint(fd);
    return fd;
}

FILE *fopen(const char *pathname, const char *mode) {
    pthread_once(&init_once, init_real_fns);
    FILE *f = NULL;
    if (real_fopen) f = real_fopen(pathname, mode);
    else f = fdopen(open(pathname, O_RDONLY), "r");

    if (f && do_open_hint) {
        int fd = fileno(f);
        try_open_hint(fd);
    }
    return f;
}

ssize_t read(int fd, void *buf, size_t count) {
    pthread_once(&init_once, init_real_fns);
    ssize_t r = -1;
    if (real_read) r = real_read(fd, buf, count);
    else r = syscall(SYS_read, fd, buf, count);

    // If we hit EOF (read returns 0), drop pages for small regular files
    if (r == 0) try_drop_fd(fd);
    return r;
}

ssize_t pread(int fd, void *buf, size_t count, off_t offset) {
    pthread_once(&init_once, init_real_fns);
    ssize_t r = -1;
    if (real_pread) r = real_pread(fd, buf, count, offset);
    else r = syscall(SYS_pread64, fd, buf, count, offset);

    // pread returning 0 -> EOF at that offset for regular reads; drop as hint
    if (r == 0) try_drop_fd(fd);
    return r;
}

ssize_t readv(int fd, const struct iovec *iov, int iovcnt) {
    pthread_once(&init_once, init_real_fns);
    ssize_t r = -1;
    if (real_readv) r = real_readv(fd, iov, iovcnt);
    else r = syscall(SYS_readv, fd, iov, iovcnt);

    if (r == 0) try_drop_fd(fd);
    return r;
}

int close(int fd) {
    pthread_once(&init_once, init_real_fns);
    if (do_close_drop) try_drop_fd(fd);
    if (real_close) return real_close(fd);
    return syscall(SYS_close, fd);
}

int fclose(FILE *stream) {
    pthread_once(&init_once, init_real_fns);
    if (stream) {
        if (do_close_drop) {
            int fd = fileno(stream);
            if (fd >= 0) try_drop_fd(fd);
        }
    }
    if (real_fclose) return real_fclose(stream);
    return 0;
}
