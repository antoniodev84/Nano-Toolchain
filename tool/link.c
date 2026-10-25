#include "tool.h"

typedef struct LinkObject LinkObject;
typedef struct LinkSection LinkSection;
typedef struct Definition Definition;

struct LinkSection {
    uint32_t type;
    uint64_t flags, offset, size, alignment, entry_size, address, output;
    uint32_t link, info;
    int allocated;
};

struct LinkObject {
    LinkObject *next;
    const char *path;
    ncc_buffer bytes;
    LinkSection *sections;
    size_t count;
    unsigned group;
    int active;
};

struct Definition {
    Definition *next;
    const char *name;
    uint64_t value, size;
    unsigned binding;
    int common;
};

typedef struct {
    Context *context;
    LinkObject *objects;
    Definition *definitions;
    ncc_buffer *output;
} Linker;

static int bounded(uint64_t start, uint64_t size, size_t total) {
    return start <= total && size <= total - start;
}

static const char *symbol_name(Linker *l, LinkObject *o, LinkSection *symbols, const unsigned char *symbol) {
    LinkSection *strings;
    uint64_t offset = elf_get(symbol, 4);
    if (symbols->link >= o->count) { tool_error(l->context, "invalid symbol string table index"); return NULL; }
    strings = &o->sections[symbols->link];
    if (strings->type != 3 || offset >= strings->size) {
        tool_error(l->context, "invalid symbol name offset"); return NULL;
    }
    for (uint64_t i = offset; i < strings->size; ++i)
        if (!o->bytes.data[strings->offset + i])
            return (const char *)o->bytes.data + strings->offset + offset;
    tool_error(l->context, "unterminated symbol name");
    return NULL;
}

static Definition *definition(Linker *l, const char *name) {
    for (Definition *d = l->definitions; d; d = d->next)
        if (tool_equal(d->name, name)) return d;
    return NULL;
}

static int load_object(Linker *l, const char *path, const ncc_buffer *memory) {
    Context *c = l->context;
    LinkObject *o = tool_alloc(c, sizeof(*o));
    unsigned char *h;
    uint64_t table;
    ncc_status status;
    if (!o) return 0;
    c->path = path;
    o->path = path; o->active = 1;
    ncc_buffer_init(&o->bytes, c->platform);
    o->next = l->objects;
    l->objects = o;
    status = memory ? ncc_buffer_append(&o->bytes,memory->data,memory->size) : ncc_read_file(c->platform, path, &o->bytes);
    if (status != NCC_OK) { tool_error(c, ncc_status_string(status)); return 0; }
    h = o->bytes.data;
    if (o->bytes.size < 64 || ncc_memcmp(h, "\177ELF\2\1\1", 7) ||
        elf_get(h + 16, 2) != 1 || elf_get(h + 18, 2) != 62 ||
        elf_get(h + 20, 4) != 1 || elf_get(h + 52, 2) != 64 || elf_get(h + 58, 2) != 64) {
        tool_error(c, "expected an ELF64 little-endian x86_64 relocatable object"); return 0;
    }
    table = elf_get(h + 40, 8);
    o->count = (size_t)elf_get(h + 60, 2);
    if (!o->count || !bounded(table, o->count * 64, o->bytes.size)) {
        tool_error(c, "invalid or unsupported extended section header table"); return 0;
    }
    o->sections = tool_alloc(c, o->count * sizeof(*o->sections));
    if (!o->sections) return 0;
    for (size_t i = 0; i < o->count; ++i) {
        unsigned char *s = h + table + i * 64;
        LinkSection *section = &o->sections[i];
        section->type = (uint32_t)elf_get(s + 4, 4);
        section->flags = elf_get(s + 8, 8);
        section->offset = elf_get(s + 24, 8);
        section->size = elf_get(s + 32, 8);
        section->link = (uint32_t)elf_get(s + 40, 4);
        section->info = (uint32_t)elf_get(s + 44, 4);
        section->alignment = elf_get(s + 48, 8);
        section->entry_size = elf_get(s + 56, 8);
        if (section->type != 8 && !bounded(section->offset, section->size, o->bytes.size)) {
            tool_error(c, "section extends beyond the input file"); return 0;
        }
        if (section->alignment && (section->alignment & (section->alignment - 1))) {
            tool_error(c, "section alignment is not a power of two"); return 0;
        }
        if (section->flags & (0x400u | 0x800u)) {
            tool_error(c, "TLS and compressed sections are not supported"); return 0;
        }
        section->allocated = (section->flags & 2) && section->size;
        if (section->allocated && ((section->flags & 5) == 5)) {
            tool_error(c, "writable executable sections are rejected"); return 0;
        }
        if (section->type == 2 && (section->entry_size != 24 || section->size % 24)) {
            tool_error(c, "invalid symbol table entry size"); return 0;
        }
    }
    return 1;
}

static int symbol_value(Linker *l, LinkObject *o, LinkSection *table, const unsigned char *s, uint64_t *out) {
    unsigned index = (unsigned)elf_get(s + 6, 2);
    uint64_t value = elf_get(s + 8, 8);
    const char *name;
    Definition *d;
    if ((s[4] >> 4) && index) {
        name = symbol_name(l,o,table,s);
        if (!name) return 0;
        d = definition(l,name);
        if (d) { *out = d->value; return 1; }
    }
    if (index == 0 || index == 0xfff2) {
        name = symbol_name(l, o, table, s);
        if (!name) return 0;
        d = definition(l, name);
        if (!d) {
            if (!name[0] && !(s[4] >> 4)) { *out = 0; return 1; }
            if ((s[4] >> 4) == 2) { *out = 0; return 1; }
            tool_error(l->context, name[0] ? name : "undefined symbol"); return 0;
        }
        *out = d->value;
        return 1;
    }
    if (index == 0xfff1) { *out = value; return 1; }
    if (index >= o->count || value > o->sections[index].size || !(o->sections[index].flags & 2)) {
        tool_error(l->context, "symbol refers to an invalid or non-allocated section"); return 0;
    }
    *out = o->sections[index].address + value;
    return 1;
}

static int collect_definitions(Linker *l) {
    Context *c = l->context;
    for (LinkObject *o = l->objects; o; o = o->next) {
        if (!o->active) continue;
        c->path = o->path;
        for (size_t i = 1; i < o->count; ++i) {
            LinkSection *table = &o->sections[i];
            if (table->type != 2) continue;
            for (uint64_t j = 24; j < table->size; j += 24) {
                unsigned char *s = o->bytes.data + table->offset + j;
                unsigned binding = s[4] >> 4, index = (unsigned)elf_get(s + 6, 2);
                uint64_t value;
                const char *name;
                Definition *old, *d;
                if (!binding || !index) continue;
                if (binding != 1 && binding != 2) { tool_error(c, "unsupported symbol binding"); return 0; }
                if (index == 0xfff2) { tool_error(c, "COMMON symbols require compilation with -fno-common"); return 0; }
                if (index < o->count && !(o->sections[index].flags & 2)) continue;
                name = symbol_name(l, o, table, s);
                if (!name) return 0;
                value = elf_get(s+8,8);
                if (index != 0xfff1) {
                    if (index >= o->count || value > o->sections[index].size) {
                        tool_error(c,"symbol has an invalid section index or offset"); return 0;
                    }
                    value += o->sections[index].address;
                }
                old = definition(l, name);
                if (old) {
                    if (old->binding == 1 && binding == 1) { tool_error(c, "duplicate strong symbol definition"); return 0; }
                    if (old->binding == 2 && binding == 1) { old->binding = binding; old->value = value; }
                    continue;
                }
                d = tool_alloc(c, sizeof(*d));
                if (!d) return 0;
                d->name = name;
                d->value = value;
                d->binding = binding;
                d->next = l->definitions;
                l->definitions = d;
            }
        }
    }
    return 1;
}

static int relocate(Linker *l) {
    Context *c = l->context;
    for (LinkObject *o = l->objects; o; o = o->next) {
        if (!o->active) continue;
        c->path = o->path;
        for (size_t i = 1; i < o->count; ++i) {
            LinkSection *r = &o->sections[i], *target, *table;
            if (r->type != 4 && r->type != 9) continue;
            if (r->info >= o->count || r->link >= o->count) { tool_error(c, "invalid relocation section indices"); return 0; }
            target = &o->sections[r->info];
            if (!(target->flags & 2)) continue;
            table = &o->sections[r->link];
            if (r->type != 4 || r->entry_size != 24 || r->size % 24 || table->type != 2) {
                tool_error(c, "expected ELF64 RELA relocations and a symbol table"); return 0;
            }
            for (uint64_t j = 0; j < r->size; j += 24) {
                unsigned char *entry = o->bytes.data + r->offset + j;
                uint64_t offset = elf_get(entry, 8), info = elf_get(entry + 8, 8), value;
                uint64_t addend = elf_get(entry + 16, 8), result, index = info >> 32;
                unsigned type = (unsigned)(info & 0xffffffffu), width;
                if (!type) continue;
                if (type == 1) width = 8;
                else if (type == 2 || type == 4 || type == 10 || type == 11) width = 4;
                else { tool_error(c, "unsupported x86_64 relocation type"); return 0; }
                if (offset > target->size || width > target->size - offset || index >= table->size / 24) {
                    tool_error(c, "relocation is outside its section or symbol table"); return 0;
                }
                if (!symbol_value(l, o, table, o->bytes.data + table->offset + index * 24, &value)) return 0;
                result = value + addend;
                if (type == 2 || type == 4) result -= target->address + offset;
                if (width == 4 && (type == 10 ? result > UINT32_MAX :
                    (result > INT32_MAX && result < UINT64_MAX - INT32_MAX))) {
                    tool_error(c, "relocation value does not fit its destination"); return 0;
                }
                elf_set(l->output->data + target->output + offset, result, width);
            }
        }
    }
    return 1;
}

static int object_defines(Linker *l, LinkObject *o, const char *name) {
    for (size_t i = 1; i < o->count; ++i) {
        LinkSection *table = &o->sections[i];
        if (table->type != 2) continue;
        for (uint64_t j = 24; j < table->size; j += 24) {
            unsigned char *s = o->bytes.data+table->offset+j;
            if (!(s[4] >> 4) || !elf_get(s+6,2)) continue;
            const char *candidate = symbol_name(l,o,table,s);
            if (!candidate) return 0;
            if (tool_equal(name,candidate)) return 1;
        }
    }
    return 0;
}
static int member_needed(Linker *l, LinkObject *candidate) {
    for (LinkObject *o = l->objects; o; o = o->next) {
        if (!o->active) continue;
        for (size_t i = 1; i < o->count; ++i) {
            LinkSection *table = &o->sections[i];
            if (table->type != 2) continue;
            for (uint64_t j = 24; j < table->size; j += 24) {
                unsigned char *s = o->bytes.data+table->offset+j;
                if ((s[4] >> 4) != 1 || elf_get(s+6,2)) continue;
                const char *name = symbol_name(l,o,table,s);
                int resolved = 0;
                if (!name) return 0;
                for (LinkObject *other = l->objects; other; other = other->next)
                    if (other->active && object_defines(l,other,name)) { resolved = 1; break; }
                if (!resolved && object_defines(l,candidate,name)) return 1;
            }
        }
    }
    return 0;
}
static int archive_decimal(Context *c, const unsigned char *bytes, size_t n, uint64_t *value) {
    size_t i = 0;
    uint64_t result = 0;
    int any = 0;
    while (i < n && bytes[i] == ' ') ++i;
    for (; i < n && bytes[i] >= '0' && bytes[i] <= '9'; ++i) {
        if (result > (UINT64_MAX-(bytes[i]-'0'))/10) { tool_error(c,"archive member size overflow"); return 0; }
        result = result*10+bytes[i]-'0'; any = 1;
    }
    while (i < n && bytes[i] == ' ') ++i;
    if (!any || i != n) { tool_error(c,"invalid archive decimal field"); return 0; }
    *value = result; return 1;
}
static int load_input(Linker *l, const char *path, const ncc_buffer *memory, unsigned group) {
    ncc_buffer input;
    Context *c = l->context;
    int result = 0;
    ncc_buffer_init(&input,c->platform);
    c->path = path;
    if (!memory) {
        ncc_status status = ncc_read_file(c->platform,path,&input);
        if (status != NCC_OK) { tool_error(c,ncc_status_string(status)); goto done; }
        memory = &input;
    }
    if (memory->size < 8 || ncc_memcmp(memory->data,"!<arch>\n",8)) {
        result = load_object(l,path,memory); goto done;
    }
    for (size_t offset = 8; offset < memory->size;) {
        const unsigned char *h = memory->data+offset;
        uint64_t size;
        size_t length = 16;
        ncc_buffer member, label;
        char *member_path;
        if (memory->size-offset < 60 || h[58] != '`' || h[59] != '\n') { tool_error(c,"truncated or invalid archive member header"); goto done; }
        if (!archive_decimal(c,h+48,10,&size)) goto done;
        offset += 60;
        if (size > memory->size-offset) { tool_error(c,"archive member exceeds input file"); goto done; }
        while (length && h[length-1] == ' ') --length;
        if (length && h[0] != '/') {
            size_t name_length = length;
            uint64_t embedded = 0;
            if (length >= 3 && !ncc_memcmp(h,"#1/",3)) {
                if (!archive_decimal(c,h+3,length-3,&embedded) || embedded > size) { tool_error(c,"invalid BSD archive filename"); goto done; }
                h = memory->data+offset; name_length = (size_t)embedded;
                while (name_length && !h[name_length-1]) --name_length;
            } else if (name_length && h[name_length-1] == '/') --name_length;
            ncc_buffer_init(&label,c->platform);
            tool_append(c,&label,path,ncc_strlen(path)); tool_byte(c,&label,'(');
            tool_append(c,&label,h,name_length); tool_byte(c,&label,')');
            member_path = tool_string(c,(char *)label.data,label.size); ncc_buffer_destroy(&label);
            if (!member_path) goto done;
            ncc_buffer_init(&member,c->platform);
            member.data = memory->data+offset+embedded; member.size = (size_t)(size-embedded);
            if (!load_object(l,member_path,&member)) goto done;
            l->objects->active = 0; l->objects->group = group;
        } else if (length > 1 && h[0] == '/' && h[1] >= '0' && h[1] <= '9') {
            ncc_buffer_init(&member,c->platform);
            member.data = memory->data+offset; member.size = (size_t)size;
            if (!load_object(l,path,&member)) goto done;
            l->objects->active = 0; l->objects->group = group;
        }
        offset += (size_t)size;
        if (offset & 1) {
            if (offset == memory->size) { tool_error(c,"missing archive member padding"); goto done; }
            ++offset;
        }
    }
    for (;;) {
        int changed = 0;
        for (LinkObject *o = l->objects; o; o = o->next)
            if (!o->active && o->group == group && member_needed(l,o)) { o->active = 1; changed = 1; }
        if (c->failed) goto done;
        if (!changed) break;
    }
    result = 1;
done:
    ncc_buffer_destroy(&input); return result;
}

int nld_link_memory(Context *c, const char **paths, const ncc_buffer *memory, size_t count, const char *entry, ncc_buffer *out) {
    Linker l = {c, NULL, NULL, out};
    size_t segments = 1, program = 1;
    int success = 0;
    unsigned char *h;
    Definition *start;
    c->line = 0;
    if (!count) { tool_error(c, "no input object files"); return 0; }
    for (size_t i = 0; i < count; ++i) if (!load_input(&l, paths[i], memory ? &memory[i] : NULL,(unsigned)i+1)) goto done;
    for (LinkObject *o = l.objects; o; o = o->next)
        if (o->active) for (size_t i = 1; i < o->count; ++i) if (o->sections[i].allocated) ++segments;
    if (segments >= 65535) { tool_error(c, "too many loadable sections"); goto done; }
    if (!tool_zeros(c, out, 64 + segments * 56 + 56)) goto done;
    for (LinkObject *o = l.objects; o; o = o->next) {
        if (!o->active) continue;
        for (size_t i = 1; i < o->count; ++i) {
            LinkSection *s = &o->sections[i];
            uint64_t alignment = s->alignment > 4096 ? s->alignment : 4096;
            uint64_t offset;
            if (!(s->flags & 2)) continue;
            if (alignment > (1u << 22) || out->size > (1u << 30) || s->size > (1u << 30)) {
                tool_error(c, "output exceeds the linker's 1 GiB size limit"); goto done;
            }
            offset = (out->size + alignment - 1) & ~(alignment - 1);
            s->output = offset;
            s->address = 0x400000 + offset;
            if (!s->allocated) continue;
            if (!tool_zeros(c, out, (size_t)(offset - out->size))) goto done;
            if (s->type == 8) {
                if (!tool_zeros(c, out, (size_t)s->size)) goto done;
            } else if (!tool_append(c, out, o->bytes.data + s->offset, (size_t)s->size)) goto done;
            h = out->data + 64 + program++ * 56;
            elf_set(h, 1, 4);
            elf_set(h + 4, 4 | ((s->flags & 1) ? 2 : 0) | ((s->flags & 4) ? 1 : 0), 4);
            elf_set(h + 8, offset, 8);
            elf_set(h + 16, s->address, 8);
            elf_set(h + 24, s->address, 8);
            elf_set(h + 32, s->size, 8);
            elf_set(h + 40, s->size, 8);
            elf_set(h + 48, alignment, 8);
        }
    }
    if (!collect_definitions(&l) || !relocate(&l)) goto done;
    c->path = NULL;
    start = definition(&l, entry);
    if (!start) { tool_error(c, "entry symbol is not defined"); goto done; }
    h = out->data;
    ncc_memcpy(h, "\177ELF\2\1\1", 7);
    elf_set(h + 16, 2, 2);
    elf_set(h + 18, 62, 2);
    elf_set(h + 20, 1, 4);
    elf_set(h + 24, start->value, 8);
    elf_set(h + 32, 64, 8);
    elf_set(h + 52, 64, 2);
    elf_set(h + 54, 56, 2);
    elf_set(h + 56, segments + 1, 2);
    h += 64;
    elf_set(h, 1, 4);
    elf_set(h + 4, 4, 4);
    elf_set(h + 16, 0x400000, 8);
    elf_set(h + 24, 0x400000, 8);
    elf_set(h + 32, 64 + (segments + 1) * 56, 8);
    elf_set(h + 40, 64 + (segments + 1) * 56, 8);
    elf_set(h + 48, 4096, 8);
    h = out->data + 64 + segments * 56;
    elf_set(h, 0x6474e551, 4);
    elf_set(h + 4, 6, 4);
    elf_set(h + 48, 16, 8);
    success = 1;
done:
    for (LinkObject *o = l.objects; o; o = o->next) ncc_buffer_destroy(&o->bytes);
    return success;
}

int nld_link(Context *c, const char **paths, size_t count, const char *entry, ncc_buffer *out) {
    return nld_link_memory(c,paths,NULL,count,entry,out);
}
