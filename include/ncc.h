#ifndef NCC_H
#define NCC_H
#include <stddef.h>
#include <stdint.h>

typedef enum {
    NCC_OK, NCC_INVALID, NCC_NOMEM, NCC_IO, NCC_NOT_FOUND, NCC_OVERFLOW
} ncc_status;

typedef struct {
    void *context;
    ncc_status (*allocate)(void *, size_t, void **);
    void (*release)(void *, void *, size_t);
    ncc_status (*open)(void *, const char *, int, intptr_t *);
    ncc_status (*read)(void *, intptr_t, void *, size_t, size_t *);
    ncc_status (*write)(void *, intptr_t, const void *, size_t, size_t *);
    ncc_status (*close)(void *, intptr_t);
    ncc_status (*executable)(void *, intptr_t);
    void (*terminate)(void *, int);
} ncc_platform;

typedef struct ncc_block ncc_block;
typedef struct {
    const ncc_platform *platform;
    ncc_block *blocks;
} ncc_arena;

typedef struct {
    const ncc_platform *platform;
    unsigned char *data;
    size_t size;
    size_t capacity;
} ncc_buffer;

const ncc_platform *ncc_linux_platform(void);
const char *ncc_status_string(ncc_status);
size_t ncc_strlen(const char *);
int ncc_memcmp(const void *, const void *, size_t);
void *ncc_memcpy(void *, const void *, size_t);
void *ncc_memmove(void *, const void *, size_t);
void *ncc_memset(void *, int, size_t);
void ncc_arena_init(ncc_arena *, const ncc_platform *);
ncc_status ncc_arena_alloc(ncc_arena *, size_t, size_t, void **);
void ncc_arena_destroy(ncc_arena *);
void ncc_buffer_init(ncc_buffer *, const ncc_platform *);
ncc_status ncc_buffer_append(ncc_buffer *, const void *, size_t);
void ncc_buffer_destroy(ncc_buffer *);
ncc_status ncc_read_file(const ncc_platform *, const char *, ncc_buffer *);
ncc_status ncc_write_all(const ncc_platform *, intptr_t, const void *, size_t);
#endif
