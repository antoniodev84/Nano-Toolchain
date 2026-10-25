#include "tool.h"

void tool_init(Context *c) {
    c->platform = ncc_linux_platform();
    ncc_arena_init(&c->arena, c->platform);
    c->failed = 0;
    c->path = NULL;
    c->line = 0;
    c->include_paths = NULL; c->include_count = 0; c->predefines = NULL;
}

void tool_destroy(Context *c) { ncc_arena_destroy(&c->arena); }

int tool_equal(const char *a, const char *b) {
    size_t n = ncc_strlen(a);
    return n == ncc_strlen(b) && !ncc_memcmp(a, b, n);
}

void tool_error(Context *c, const char *message) {
    ncc_buffer b;
    if (c->failed) return;
    c->failed = 1;
    ncc_buffer_init(&b, c->platform);
    if (c->path) {
        ncc_buffer_append(&b, c->path, ncc_strlen(c->path));
        ncc_buffer_append(&b, ":", 1);
        if (c->line) {
            tool_number(c, &b, c->line);
            ncc_buffer_append(&b, ":", 1);
        }
        ncc_buffer_append(&b, " ", 1);
    }
    ncc_buffer_append(&b, "error: ", 7);
    ncc_buffer_append(&b, message, ncc_strlen(message));
    ncc_buffer_append(&b, "\n", 1);
    ncc_write_all(c->platform, 2, b.data, b.size);
    ncc_buffer_destroy(&b);
}

void *tool_alloc(Context *c, size_t n) {
    void *p = NULL;
    ncc_status s = ncc_arena_alloc(&c->arena, n, 16, &p);
    if (s != NCC_OK) { tool_error(c, ncc_status_string(s)); c->platform->terminate(c->platform->context,1); return NULL; }
    ncc_memset(p, 0, n);
    return p;
}

char *tool_string(Context *c, const char *s, size_t n) {
    char *p;
    if (n == SIZE_MAX) { tool_error(c, "string is too large"); return NULL; }
    p = tool_alloc(c, n + 1);
    if (p) ncc_memcpy(p, s, n);
    return p;
}

void tool_number(Context *c, ncc_buffer *b, uint64_t value) {
    char digits[20];
    size_t n = 0;
    do { digits[n++] = (char)('0' + value % 10); value /= 10; } while (value);
    while (n) tool_byte(c, b, (unsigned)digits[--n]);
}

int tool_append(Context *c, ncc_buffer *b, const void *p, size_t n) {
    ncc_status s = ncc_buffer_append(b, p, n);
    if (s != NCC_OK) tool_error(c, ncc_status_string(s));
    return s == NCC_OK;
}

int tool_byte(Context *c, ncc_buffer *b, unsigned value) {
    unsigned char v = (unsigned char)value;
    return tool_append(c, b, &v, 1);
}

int tool_zeros(Context *c, ncc_buffer *b, size_t n) {
    static const unsigned char zero[256] = {0};
    while (n) {
        size_t k = n < sizeof(zero) ? n : sizeof(zero);
        if (!tool_append(c, b, zero, k)) return 0;
        n -= k;
    }
    return 1;
}

int tool_save(Context *c, const char *path, ncc_buffer *b, int executable) {
    intptr_t fd;
    ncc_status s = c->platform->open(c->platform->context, path, 1, &fd), closing;
    if (s != NCC_OK) { tool_error(c, ncc_status_string(s)); return 0; }
    s = ncc_write_all(c->platform, fd, b->data, b->size);
    if (s == NCC_OK && executable) s = c->platform->executable ? c->platform->executable(c->platform->context, fd) : NCC_INVALID;
    closing = c->platform->close(c->platform->context, fd);
    if (s == NCC_OK) s = closing;
    if (s != NCC_OK) tool_error(c, ncc_status_string(s));
    return s == NCC_OK;
}

uint64_t elf_get(const unsigned char *p, unsigned n) {
    uint64_t result = 0;
    for (unsigned i = 0; i < n; ++i) result |= (uint64_t)p[i] << (8 * i);
    return result;
}

void elf_set(unsigned char *p, uint64_t v, unsigned n) {
    for (unsigned i = 0; i < n; ++i) { p[i] = (unsigned char)v; v >>= 8; }
}
