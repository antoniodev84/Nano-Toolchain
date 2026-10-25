#ifndef NCC_TOOL_H
#define NCC_TOOL_H
#include "ncc.h"

typedef struct {
    const ncc_platform *platform;
    ncc_arena arena;
    int failed;
    const char *path;
    size_t line;
    const char **include_paths;
    size_t include_count;
    const char *predefines;
} Context;

void tool_init(Context *);
void tool_destroy(Context *);
void *tool_alloc(Context *, size_t);
char *tool_string(Context *, const char *, size_t);
int tool_equal(const char *, const char *);
void tool_error(Context *, const char *);
void tool_number(Context *, ncc_buffer *, uint64_t);
int tool_append(Context *, ncc_buffer *, const void *, size_t);
int tool_byte(Context *, ncc_buffer *, unsigned);
int tool_zeros(Context *, ncc_buffer *, size_t);
int tool_save(Context *, const char *, ncc_buffer *, int);
uint64_t elf_get(const unsigned char *, unsigned);
void elf_set(unsigned char *, uint64_t, unsigned);
int nas_assemble(Context *, const char *, ncc_buffer *);
int nld_link(Context *, const char **, size_t, const char *, ncc_buffer *);
int nld_link_memory(Context *, const char **, const ncc_buffer *, size_t, const char *, ncc_buffer *);
int ncc_compile(Context *, const char *, const char *, int, ncc_buffer *);
#endif
