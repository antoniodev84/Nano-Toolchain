#include "ncc.h"

extern long ncc_linux_syscall(long, long, long, long, long, long, long);

static long invoke(long number, long a, long b, long c, long d, long e, long f) {
    return ncc_linux_syscall(number,a,b,c,d,e,f);
}

static ncc_status error_status(long r) {
    if (r == -2) return NCC_NOT_FOUND;
    if (r == -12) return NCC_NOMEM;
    if (r == -22) return NCC_INVALID;
    return NCC_IO;
}

static ncc_status allocate(void *context, size_t size, void **out) {
    long r;
    (void)context;
    if (!out || !size || size > (size_t)INTPTR_MAX) return NCC_INVALID;
    *out = NULL;
    r = invoke(9, 0, (long)size, 3, 34, -1, 0);
    if ((unsigned long)r >= (unsigned long)-4095) return error_status(r);
    *out = (void *)r;
    return NCC_OK;
}

static void release(void *context, void *p, size_t size) {
    (void)context;
    invoke(11, (long)p, (long)size, 0, 0, 0, 0);
}

static ncc_status open_file(void *context, const char *path, int writing, intptr_t *out) {
    long r;
    (void)context;
    if (!path || !out) return NCC_INVALID;
    do { r = invoke(257, -100, (long)path, writing ? 577 : 0, 0666, 0, 0); } while (r == -4);
    if (r < 0) return error_status(r);
    *out = r;
    return NCC_OK;
}

static ncc_status read_file(void *context, intptr_t fd, void *data, size_t size, size_t *out) {
    long r;
    (void)context;
    if (!out || (!data && size)) return NCC_INVALID;
    if (size > 0x7ffff000u) size = 0x7ffff000u;
    do { r = invoke(0, fd, (long)data, (long)size, 0, 0, 0); } while (r == -4);
    if (r < 0) return error_status(r);
    *out = (size_t)r;
    return NCC_OK;
}

static ncc_status write_file(void *context, intptr_t fd, const void *data, size_t size, size_t *out) {
    long r;
    (void)context;
    if (!out || (!data && size)) return NCC_INVALID;
    if (size > 0x7ffff000u) size = 0x7ffff000u;
    do { r = invoke(1, fd, (long)data, (long)size, 0, 0, 0); } while (r == -4);
    if (r < 0) return error_status(r);
    *out = (size_t)r;
    return NCC_OK;
}

static ncc_status close_file(void *context, intptr_t fd) {
    long r;
    (void)context;
    r = invoke(3, fd, 0, 0, 0, 0, 0);
    return r < 0 ? error_status(r) : NCC_OK;
}

static ncc_status executable(void *context, intptr_t fd) {
    long r;
    (void)context;
    r = invoke(91, fd, 0755, 0, 0, 0, 0);
    return r < 0 ? error_status(r) : NCC_OK;
}

static void terminate(void *context, int status) {
    (void)context;
    invoke(231, status, 0, 0, 0, 0, 0);
    for (;;) {}
}

const ncc_platform *ncc_linux_platform(void) {
    static const ncc_platform p = {
        NULL, allocate, release, open_file, read_file, write_file, close_file, executable, terminate
    };
    return &p;
}
