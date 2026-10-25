#include "ncc.h"

struct ncc_block {
    ncc_block *next;
    size_t capacity;
    size_t used;
    unsigned char data[];
};

const char *ncc_status_string(ncc_status status) {
    switch (status) {
    case NCC_OK: return "success";
    case NCC_INVALID: return "invalid argument";
    case NCC_NOMEM: return "out of memory";
    case NCC_IO: return "input/output error";
    case NCC_NOT_FOUND: return "file not found";
    case NCC_OVERFLOW: return "size exceeds implementation limit";
    }
    return "unknown error";
}

size_t ncc_strlen(const char *s) {
    size_t n = 0;
    while (s[n]) ++n;
    return n;
}

int ncc_memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b;
    for (size_t i = 0; i < n; ++i)
        if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    return 0;
}

void *ncc_memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    for (size_t i = 0; i < n; ++i) d[i] = s[i];
    return dst;
}

void *ncc_memmove(void *dst, const void *src, size_t n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    if ((uintptr_t)d <= (uintptr_t)s) return ncc_memcpy(dst, src, n);
    while (n) { --n; d[n] = s[n]; }
    return dst;
}

void *ncc_memset(void *dst, int value, size_t n) {
    unsigned char *d = dst;
    for (size_t i = 0; i < n; ++i) d[i] = (unsigned char)value;
    return dst;
}

void ncc_arena_init(ncc_arena *a, const ncc_platform *p) {
    a->platform = p;
    a->blocks = NULL;
}

ncc_status ncc_arena_alloc(ncc_arena *a, size_t n, size_t alignment, void **out) {
    ncc_block *b;
    size_t padding, available, capacity, extra;
    void *allocation;
    ncc_status status;
    if (!a || !out || !a->platform || !alignment ||
        (alignment & (alignment - 1))) return NCC_INVALID;
    *out = NULL;
    if (!n) n = 1;
    b = a->blocks;
    if (b) {
        padding = (size_t)(-(uintptr_t)(b->data + b->used)) & (alignment - 1);
        available = b->capacity - b->used;
        if (padding <= available && n <= available - padding) {
            *out = b->data + b->used + padding;
            b->used += padding + n;
            return NCC_OK;
        }
    }
    extra = alignment - 1;
    if (n > SIZE_MAX - extra || n + extra > SIZE_MAX - sizeof(*b))
        return NCC_OVERFLOW;
    capacity = n + extra;
    if (capacity < 4096) capacity = 4096;
    if (capacity > SIZE_MAX - sizeof(*b)) return NCC_OVERFLOW;
    status = a->platform->allocate(a->platform->context, sizeof(*b) + capacity, &allocation);
    if (status != NCC_OK) return status;
    b = allocation;
    b->next = a->blocks;
    b->capacity = capacity;
    padding = (size_t)(-(uintptr_t)b->data) & (alignment - 1);
    b->used = padding + n;
    a->blocks = b;
    *out = b->data + padding;
    return NCC_OK;
}

void ncc_arena_destroy(ncc_arena *a) {
    ncc_block *b = a->blocks;
    while (b) {
        ncc_block *next = b->next;
        a->platform->release(a->platform->context, b, sizeof(*b) + b->capacity);
        b = next;
    }
    a->blocks = NULL;
}

void ncc_buffer_init(ncc_buffer *b, const ncc_platform *p) {
    b->platform = p;
    b->data = NULL;
    b->size = b->capacity = 0;
}

ncc_status ncc_buffer_append(ncc_buffer *b, const void *data, size_t n) {
    size_t need, capacity, offset = 0;
    int internal = 0;
    void *allocation;
    ncc_status status;
    if (!b || !b->platform || (!data && n)) return NCC_INVALID;
    if (!n) return NCC_OK;
    if (n > SIZE_MAX - b->size) return NCC_OVERFLOW;
    if (b->data && (uintptr_t)data >= (uintptr_t)b->data &&
        (uintptr_t)data - (uintptr_t)b->data < b->capacity) {
        offset = (size_t)((uintptr_t)data - (uintptr_t)b->data);
        if (offset > b->size || n > b->size - offset) return NCC_INVALID;
        internal = 1;
    }
    need = b->size + n;
    if (need > b->capacity) {
        capacity = b->capacity ? b->capacity : 256;
        while (capacity < need) {
            if (capacity > SIZE_MAX / 2) { capacity = need; break; }
            capacity *= 2;
        }
        status = b->platform->allocate(b->platform->context, capacity, &allocation);
        if (status != NCC_OK) return status;
        ncc_memcpy(allocation, b->data, b->size);
        if (internal) data = (unsigned char *)allocation + offset;
        if (b->data) b->platform->release(b->platform->context, b->data, b->capacity);
        b->data = allocation;
        b->capacity = capacity;
    }
    ncc_memmove(b->data + b->size, data, n);
    b->size = need;
    return NCC_OK;
}

void ncc_buffer_destroy(ncc_buffer *b) {
    if (b->data) b->platform->release(b->platform->context, b->data, b->capacity);
    b->data = NULL;
    b->size = b->capacity = 0;
}

ncc_status ncc_read_file(const ncc_platform *p, const char *path, ncc_buffer *out) {
    intptr_t fd;
    unsigned char chunk[8192];
    size_t count;
    ncc_status status, closing;
    ncc_buffer result;
    if (!p || !path || !out || out->platform != p) return NCC_INVALID;
    status = p->open(p->context, path, 0, &fd);
    if (status != NCC_OK) return status;
    ncc_buffer_init(&result, p);
    for (;;) {
        status = p->read(p->context, fd, chunk, sizeof(chunk), &count);
        if (status != NCC_OK || !count) break;
        if (count > sizeof(chunk)) { status = NCC_IO; break; }
        status = ncc_buffer_append(&result, chunk, count);
        if (status != NCC_OK) break;
    }
    closing = p->close(p->context, fd);
    if (status == NCC_OK) status = closing;
    if (status != NCC_OK) { ncc_buffer_destroy(&result); return status; }
    ncc_buffer_destroy(out);
    *out = result;
    return NCC_OK;
}

ncc_status ncc_write_all(const ncc_platform *p, intptr_t fd, const void *data, size_t n) {
    const unsigned char *bytes = data;
    if (!p || (!data && n)) return NCC_INVALID;
    while (n) {
        size_t count;
        ncc_status status = p->write(p->context, fd, bytes, n, &count);
        if (status != NCC_OK) return status;
        if (!count || count > n) return NCC_IO;
        bytes += count;
        n -= count;
    }
    return NCC_OK;
}
