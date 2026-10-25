#include "tool.h"

typedef struct AsmSymbol AsmSymbol;
typedef struct AsmReloc AsmReloc;
struct AsmSymbol {
    AsmSymbol *next;
    char *name;
    uint64_t value;
    unsigned section, global, defined, index;
};
struct AsmReloc {
    AsmReloc *next;
    AsmSymbol *symbol;
    uint64_t offset, addend;
    unsigned section, type;
};
typedef struct {
    Context *c;
    const char *p;
    ncc_buffer sections[4];
    uint64_t alignments[4];
    unsigned section;
    AsmSymbol *symbols;
    AsmReloc *relocations;
} Assembler;
typedef struct { AsmSymbol *symbol; uint64_t value; } Expression;
typedef struct {
    int kind, reg, base, width, rip;
    Expression value;
} Operand;

static int letter(int ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_' || ch == '.' || ch == '$';
}
static int digit(int ch) { return ch >= '0' && ch <= '9'; }
static void spaces(Assembler *a) { while (*a->p == ' ' || *a->p == '\t' || *a->p == '\r') ++a->p; }
static char *word(Assembler *a) {
    const char *start;
    spaces(a);
    start = a->p;
    if (!letter(*a->p)) { tool_error(a->c, "expected an identifier"); return NULL; }
    while (letter(*a->p) || digit(*a->p)) ++a->p;
    return tool_string(a->c, start, (size_t)(a->p - start));
}
static AsmSymbol *asm_symbol(Assembler *a, const char *name) {
    AsmSymbol *s;
    for (s = a->symbols; s; s = s->next) if (tool_equal(s->name, name)) return s;
    s = tool_alloc(a->c, sizeof(*s));
    if (!s) return NULL;
    s->name = tool_string(a->c, name, ncc_strlen(name));
    if (!s->name) return NULL;
    s->next = a->symbols;
    a->symbols = s;
    return s;
}
static int number(Assembler *a, uint64_t *value) {
    unsigned base = 10;
    uint64_t n = 0;
    int any = 0;
    spaces(a);
    if (a->p[0] == '0' && (a->p[1] == 'x' || a->p[1] == 'X')) { a->p += 2; base = 16; }
    for (;;) {
        unsigned d;
        int ch = *a->p;
        if (digit(ch)) d = (unsigned)(ch - '0');
        else if (ch >= 'a' && ch <= 'f') d = (unsigned)(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') d = (unsigned)(ch - 'A' + 10);
        else break;
        if (d >= base) break;
        if (n > (UINT64_MAX - d) / base) { tool_error(a->c, "integer literal exceeds 64 bits"); return 0; }
        any = 1;
        n = n * base + d;
        ++a->p;
    }
    if (!any) { tool_error(a->c, "expected an integer literal"); return 0; }
    *value = n;
    return 1;
}
static Expression expression(Assembler *a) {
    Expression e = {NULL, 0};
    int negative = 0;
    spaces(a);
    if (*a->p == '-' || *a->p == '+') negative = *a->p++ == '-';
    spaces(a);
    if (letter(*a->p)) {
        char *name = word(a);
        if (!name) return e;
        if (negative) { tool_error(a->c, "a symbol cannot be negated"); return e; }
        e.symbol = asm_symbol(a, name);
    } else {
        if (!number(a, &e.value)) return e;
        if (negative) e.value = 0 - e.value;
    }
    spaces(a);
    if (*a->p == '+' || *a->p == '-') {
        uint64_t value;
        negative = *a->p++ == '-';
        if (number(a, &value)) e.value += negative ? 0 - value : value;
    }
    return e;
}
static int register_id(const char *name, int *width) {
    static const char *const names[4][16] = {
        {"al","cl","dl","bl","spl","bpl","sil","dil","r8b","r9b","r10b","r11b","r12b","r13b","r14b","r15b"},
        {"ax","cx","dx","bx","sp","bp","si","di","r8w","r9w","r10w","r11w","r12w","r13w","r14w","r15w"},
        {"eax","ecx","edx","ebx","esp","ebp","esi","edi","r8d","r9d","r10d","r11d","r12d","r13d","r14d","r15d"},
        {"rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi","r8","r9","r10","r11","r12","r13","r14","r15"}
    };
    for (int w = 0; w < 4; ++w) for (int r = 0; r < 16; ++r)
        if (tool_equal(name, names[w][r])) { *width = 1 << w; return r; }
    return -1;
}
static Operand operand(Assembler *a) {
    Operand o;
    const char *save;
    char *name;
    ncc_memset(&o, 0, sizeof(o));
    o.base = -1;
    spaces(a);
    save = a->p;
    if (letter(*a->p)) {
        name = word(a);
        if (!name) return o;
        if (tool_equal(name, "byte")) o.width = 1;
        else if (tool_equal(name, "word")) o.width = 2;
        else if (tool_equal(name, "dword")) o.width = 4;
        else if (tool_equal(name, "qword")) o.width = 8;
        if (o.width) {
            spaces(a);
            if (a->p[0] == 'p' && a->p[1] == 't' && a->p[2] == 'r' && !letter(a->p[3])) a->p += 3;
        } else a->p = save;
    }
    spaces(a);
    if (*a->p == '[') {
        o.kind = 2;
        ++a->p;
        spaces(a);
        save = a->p;
        if (letter(*a->p)) {
            int width = 0;
            name = word(a);
            if (!name) return o;
            o.base = register_id(name, &width);
            if (tool_equal(name, "rip")) { o.rip = 1; o.base = -1; }
            else if (o.base >= 0 && width != 8) tool_error(a->c, "memory addresses require 64-bit registers");
            if (o.base < 0 && !o.rip) a->p = save;
        }
        spaces(a);
        if (o.base >= 0 || o.rip) {
            if (*a->p == '+' || *a->p == '-') o.value = expression(a);
        } else o.value = expression(a);
        spaces(a);
        if (*a->p != ']') tool_error(a->c, "expected ']' after memory address");
        else ++a->p;
        return o;
    }
    if (o.width) { o.width = 0; a->p = save; }
    save = a->p;
    if (letter(*a->p)) {
        name = word(a);
        if (!name) return o;
        o.reg = register_id(name, &o.width);
        if (o.reg >= 0) { o.kind = 1; return o; }
        a->p = save;
    }
    o.kind = 3;
    o.value = expression(a);
    return o;
}
static void emit(Assembler *a, unsigned v) { tool_byte(a->c, &a->sections[a->section], v); }
static void integer(Assembler *a, uint64_t v, unsigned width) {
    for (unsigned i = 0; i < width; ++i) { emit(a, (unsigned)v); v >>= 8; }
}
static void reference(Assembler *a, Expression e, unsigned width, unsigned type, uint64_t adjustment) {
    if (!e.symbol && width < 8) {
        unsigned bits = width*8;
        uint64_t value = e.value+adjustment;
        if (value > ((1ull<<bits)-1) && value < UINT64_MAX-((1ull<<(bits-1))-1)) {
            tool_error(a->c,"immediate does not fit its encoded operand"); return;
        }
    }
    if (e.symbol && width < 4) { tool_error(a->c,"symbolic immediates require at least 32 bits"); return; }
    if (e.symbol) {
        AsmReloc *r = tool_alloc(a->c, sizeof(*r));
        if (!r) return;
        r->symbol = e.symbol;
        r->section = a->section;
        r->offset = a->sections[a->section].size;
        r->addend = e.value + adjustment;
        r->type = type;
        r->next = a->relocations;
        a->relocations = r;
        integer(a, 0, width);
    } else integer(a, e.value + adjustment, width);
}
static void prefix(Assembler *a, int width, int reg, Operand rm, int force) {
    unsigned rex = 0x40 | (width == 8 ? 8 : 0) | (reg >= 8 ? 4 : 0);
    int base = rm.kind == 1 ? rm.reg : rm.base;
    if (base >= 8) rex |= 1;
    if (width == 2) emit(a, 0x66);
    if (rex != 0x40 || force || (width == 1 && (reg >= 4 || (rm.kind == 1 && rm.reg >= 4)))) emit(a, rex);
}
static void modrm(Assembler *a, int reg, Operand rm, unsigned tail) {
    uint64_t disp = rm.value.value;
    if (rm.kind == 1) { emit(a, 0xc0 | ((unsigned)reg & 7) * 8 | ((unsigned)rm.reg & 7)); return; }
    if (rm.kind != 2) { tool_error(a->c, "expected a register or memory operand"); return; }
    if (rm.rip) {
        emit(a, ((unsigned)reg & 7) * 8 | 5);
        reference(a, rm.value, 4, 2, rm.value.symbol ? 0 - (uint64_t)(4 + tail) : 0);
    } else if (rm.base < 0) {
        emit(a, ((unsigned)reg & 7) * 8 | 4);
        emit(a, 0x25);
        reference(a, rm.value, 4, 11, 0);
    } else {
        unsigned mode;
        if (rm.value.symbol) { tool_error(a->c, "symbolic base displacements require RIP-relative addressing"); return; }
        if (!disp && (rm.base & 7) != 5) mode = 0;
        else if (disp <= 127 || disp >= UINT64_MAX - 127) mode = 1;
        else if (disp <= INT32_MAX || disp >= UINT64_MAX - INT32_MAX) mode = 2;
        else { tool_error(a->c, "memory displacement exceeds signed 32 bits"); return; }
        emit(a, mode * 64 | ((unsigned)reg & 7) * 8 | ((unsigned)rm.base & 7));
        if ((rm.base & 7) == 4) emit(a, 0x24);
        if (mode) integer(a, disp, mode == 1 ? 1 : 4);
    }
}
static int fits_signed32(uint64_t v) { return v <= INT32_MAX || v >= UINT64_MAX - INT32_MAX; }
static void instruction(Assembler *a, const char *name) {
    static const char *const conditions[] = {"jo","jno","jb","jae","je","jne","jbe","ja","js","jns","jp","jnp","jl","jge","jle","jg"};
    static const char *const binary[] = {"add","or","adc","sbb","and","sub","xor","cmp"};
    if (tool_equal(name,"rep")) {
        char *operation = word(a);
        if (!operation) return;
        if (!tool_equal(operation,"stosb") && !tool_equal(operation,"movsb")) { tool_error(a->c,"unsupported repeat instruction"); return; }
        emit(a,0xf3); emit(a,tool_equal(operation,"stosb") ? 0xaa : 0xa4); return;
    }
    Operand x, y;
    int width, count = 0;
    ncc_memset(&x, 0, sizeof(x));
    ncc_memset(&y, 0, sizeof(y));
    spaces(a);
    if (*a->p && *a->p != '\n' && *a->p != '#' && *a->p != ';') {
        x = operand(a); count = 1;
        spaces(a);
        if (*a->p == ',') { ++a->p; y = operand(a); count = 2; }
    }
    if (a->c->failed) return;
    if (tool_equal(name, "ret") || tool_equal(name, "nop") || tool_equal(name, "leave") || tool_equal(name, "syscall") || tool_equal(name, "cqo") || tool_equal(name, "cdq") || tool_equal(name, "ud2")) {
        if (count) { tool_error(a->c, "instruction takes no operands"); return; }
        if (tool_equal(name, "syscall")) { emit(a, 0x0f); emit(a, 0x05); }
        else if (tool_equal(name, "ud2")) { emit(a, 0x0f); emit(a, 0x0b); }
        else if (tool_equal(name, "cqo")) { emit(a, 0x48); emit(a, 0x99); }
        else if (tool_equal(name, "cdq")) emit(a, 0x99);
        else emit(a, tool_equal(name, "ret") ? 0xc3 : tool_equal(name, "leave") ? 0xc9 : 0x90);
        return;
    }
    if (tool_equal(name, "push") || tool_equal(name, "pop")) {
        if (count != 1 || x.kind != 1 || x.width != 8) { tool_error(a->c, "push/pop require one 64-bit register"); return; }
        if (x.reg >= 8) emit(a, 0x41);
        emit(a, (tool_equal(name, "push") ? 0x50 : 0x58) + (unsigned)(x.reg & 7));
        return;
    }
    if (tool_equal(name, "jmp") || tool_equal(name, "call")) {
        if (count != 1) { tool_error(a->c, "branch requires one operand"); return; }
        if (x.kind == 3) {
            emit(a, tool_equal(name, "call") ? 0xe8 : 0xe9);
            reference(a, x.value, 4, tool_equal(name, "call") ? 4 : 2, x.value.symbol ? UINT64_MAX - 3 : 0);
        } else { prefix(a, 4, 0, x, 0); emit(a, 0xff); modrm(a, tool_equal(name, "call") ? 2 : 4, x, 0); }
        return;
    }
    for (unsigned i = 0; i < 16; ++i) {
        if (tool_equal(name, conditions[i])) {
            if (count != 1 || x.kind != 3) { tool_error(a->c, "conditional branch requires a label"); return; }
            emit(a, 0x0f); emit(a, 0x80 + i);
            reference(a, x.value, 4, 2, x.value.symbol ? UINT64_MAX - 3 : 0);
            return;
        }
        if (name[0] == 's' && name[1] == 'e' && name[2] == 't' && tool_equal(name + 3, conditions[i] + 1)) {
            if (count != 1 || x.kind != 1 || x.width != 1) { tool_error(a->c, "setcc requires one byte register"); return; }
            prefix(a, 1, 0, x, 0); emit(a, 0x0f); emit(a, 0x90 + i); modrm(a, 0, x, 0); return;
        }
    }
    width = x.width ? x.width : y.width ? y.width : 8;
    if (x.width && y.width && x.width != y.width && !tool_equal(name, "movzx") && !tool_equal(name, "movsx") && !tool_equal(name, "movsxd") && !tool_equal(name, "shl") && !tool_equal(name, "shr") && !tool_equal(name, "sar")) {
        tool_error(a->c, "operand sizes do not match"); return;
    }
    if (tool_equal(name, "neg") || tool_equal(name, "not") || tool_equal(name, "idiv") || tool_equal(name, "div") || tool_equal(name, "inc") || tool_equal(name, "dec")) {
        int group = tool_equal(name,"not") ? 2 : tool_equal(name,"neg") ? 3 : tool_equal(name,"div") ? 6 : 7;
        int increment = tool_equal(name,"inc") || tool_equal(name,"dec");
        if (count != 1 || x.kind == 3) { tool_error(a->c, "expected one register or memory operand"); return; }
        if (increment) group = tool_equal(name,"inc") ? 0 : 1;
        prefix(a, width, 0, x, 0); emit(a, increment ? (width == 1 ? 0xfe : 0xff) : (width == 1 ? 0xf6 : 0xf7)); modrm(a, group, x, 0); return;
    }
    if (count != 2) { tool_error(a->c, "unknown instruction or incorrect operand count"); return; }
    if (tool_equal(name, "mov")) {
        if (x.kind == 1 && y.kind == 3) {
            prefix(a, width, 0, x, width == 1 && x.reg >= 4);
            emit(a, (width == 1 ? 0xb0 : 0xb8) + (unsigned)(x.reg & 7));
            reference(a, y.value, (unsigned)width, width == 8 ? 1 : 10, 0);
        } else if (x.kind == 2 && y.kind == 3) {
            unsigned n = width == 8 ? 4 : (unsigned)width;
            if (width == 8 && !y.value.symbol && !fits_signed32(y.value.value)) { tool_error(a->c, "memory immediate exceeds signed 32 bits"); return; }
            prefix(a, width, 0, x, 0); emit(a, width == 1 ? 0xc6 : 0xc7); modrm(a, 0, x, n); reference(a, y.value, n, width == 8 ? 11 : 10, 0);
        } else if (x.kind == 1 && (y.kind == 1 || y.kind == 2)) {
            prefix(a, width, x.reg, y, 0); emit(a, width == 1 ? 0x8a : 0x8b); modrm(a, x.reg, y, 0);
        } else if (x.kind == 2 && y.kind == 1) {
            prefix(a, width, y.reg, x, 0); emit(a, width == 1 ? 0x88 : 0x89); modrm(a, y.reg, x, 0);
        } else tool_error(a->c, "invalid mov operands");
        return;
    }
    if (tool_equal(name, "lea")) {
        if (x.kind != 1 || y.kind != 2 || width == 1) { tool_error(a->c, "lea requires a register and memory address"); return; }
        prefix(a, width, x.reg, y, 0); emit(a, 0x8d); modrm(a, x.reg, y, 0); return;
    }
    if (tool_equal(name,"movzx") || tool_equal(name,"movsx") || tool_equal(name,"movsxd")) {
        if (x.kind != 1 || (y.kind != 1 && y.kind != 2) || !y.width || x.width <= y.width) { tool_error(a->c, "invalid extending move operands"); return; }
        if (tool_equal(name,"movsxd")) {
            if (x.width != 8 || y.width != 4) { tool_error(a->c,"movsxd requires 64-bit and 32-bit operands"); return; }
            prefix(a, 8, x.reg, y, 0); emit(a,0x63);
        } else {
            if (y.width != 1 && y.width != 2) { tool_error(a->c,"source must be byte or word"); return; }
            prefix(a, x.width, x.reg, y, y.kind == 1 && y.width == 1 && y.reg >= 4);
            emit(a,0x0f); emit(a,(tool_equal(name,"movzx") ? 0xb6 : 0xbe) + (y.width == 2));
        }
        modrm(a,x.reg,y,0); return;
    }
    if (tool_equal(name,"imul")) {
        if (x.kind != 1 || y.kind == 3 || width == 1) { tool_error(a->c,"imul requires a register and register/memory source"); return; }
        prefix(a,width,x.reg,y,0); emit(a,0x0f); emit(a,0xaf); modrm(a,x.reg,y,0); return;
    }
    if (tool_equal(name,"shl") || tool_equal(name,"shr") || tool_equal(name,"sar")) {
        int group = tool_equal(name,"shl") ? 4 : tool_equal(name,"shr") ? 5 : 7;
        if ((x.kind != 1 && x.kind != 2) || (y.kind != 3 && !(y.kind == 1 && y.reg == 1 && y.width == 1))) { tool_error(a->c,"shift requires immediate count or cl"); return; }
        if (y.kind == 3 && (y.value.symbol || y.value.value > 255)) { tool_error(a->c,"shift count must fit one byte"); return; }
        prefix(a,width,0,x,0); emit(a,y.kind == 3 ? (width == 1 ? 0xc0 : 0xc1) : (width == 1 ? 0xd2 : 0xd3));
        modrm(a,group,x,y.kind == 3 ? 1 : 0); if (y.kind == 3) integer(a,y.value.value,1); return;
    }
    for (unsigned i = 0; i < 8; ++i) if (tool_equal(name, binary[i])) {
        if (y.kind == 3 && (x.kind == 1 || x.kind == 2)) {
            unsigned n = width == 8 ? 4 : (unsigned)width;
            if (width == 8 && !y.value.symbol && !fits_signed32(y.value.value)) { tool_error(a->c,"arithmetic immediate exceeds signed 32 bits"); return; }
            prefix(a,width,0,x,0); emit(a,width == 1 ? 0x80 : 0x81); modrm(a,(int)i,x,n); reference(a,y.value,n,width == 8 ? 11 : 10,0);
        } else if (x.kind == 1 && (y.kind == 1 || y.kind == 2)) {
            prefix(a,width,x.reg,y,0); emit(a,i * 8 + (width == 1 ? 2 : 3)); modrm(a,x.reg,y,0);
        } else if (x.kind == 2 && y.kind == 1) {
            prefix(a,width,y.reg,x,0); emit(a,i * 8 + (width == 1 ? 0 : 1)); modrm(a,y.reg,x,0);
        } else tool_error(a->c,"invalid arithmetic operands");
        return;
    }
    if (tool_equal(name,"test")) {
        if (y.kind != 1 || (x.kind != 1 && x.kind != 2)) { tool_error(a->c,"test requires a register source"); return; }
        prefix(a,width,y.reg,x,0); emit(a,width == 1 ? 0x84 : 0x85); modrm(a,y.reg,x,0); return;
    }
    tool_error(a->c, "unknown instruction");
}

static int escape(Assembler *a, unsigned *out) {
    unsigned value;
    if (!*a->p || *a->p == '\n') { tool_error(a->c,"unterminated string literal"); return 0; }
    value = (unsigned char)*a->p++;
    if (value == '\\') {
        value = (unsigned char)*a->p++;
        if (!value || value == '\n') { tool_error(a->c,"unterminated escape sequence"); return 0; }
        if (value == 'n') value = 10;
        else if (value == 'r') value = 13;
        else if (value == 't') value = 9;
        else if (value == '0') value = 0;
        else if (value != '\\' && value != '"' && value != '\'') { tool_error(a->c,"unsupported escape sequence"); return 0; }
    }
    *out = value;
    return 1;
}
static void directive(Assembler *a, const char *name) {
    if (tool_equal(name,".intel_syntax")) {
        char *syntax = word(a);
        if (!syntax || !tool_equal(syntax,"noprefix")) tool_error(a->c,"only Intel syntax without register prefixes is supported");
    }
    else if (tool_equal(name,".text")) a->section = 0;
    else if (tool_equal(name,".data")) a->section = 1;
    else if (tool_equal(name,".bss")) a->section = 2;
    else if (tool_equal(name,".rodata")) a->section = 3;
    else if (tool_equal(name,".section")) {
        char *s = word(a);
        if (!s) return;
        if (tool_equal(s,".text")) a->section = 0;
        else if (tool_equal(s,".data")) a->section = 1;
        else if (tool_equal(s,".bss")) a->section = 2;
        else if (tool_equal(s,".rodata")) a->section = 3;
        else tool_error(a->c,"unsupported section name");
    } else if (tool_equal(name,".global") || tool_equal(name,".globl") || tool_equal(name,".extern")) {
        for (;;) {
            char *s = word(a);
            AsmSymbol *symbol;
            if (!s) return;
            symbol = asm_symbol(a,s);
            if (!symbol) return;
            symbol->global = 1;
            spaces(a);
            if (*a->p != ',') break;
            ++a->p;
        }
    } else if (tool_equal(name,".byte") || tool_equal(name,".word") || tool_equal(name,".long") || tool_equal(name,".quad")) {
        unsigned width = tool_equal(name,".byte") ? 1 : tool_equal(name,".word") ? 2 : tool_equal(name,".long") ? 4 : 8;
        for (;;) {
            Expression e = expression(a);
            if (a->c->failed) return;
                if (e.symbol && width < 4) { tool_error(a->c,"symbol relocations require .long or .quad"); return; }
            reference(a,e,width,width == 8 ? 1 : 10,0);
            spaces(a);
            if (*a->p != ',') break;
            ++a->p;
        }
    } else if (tool_equal(name,".zero") || tool_equal(name,".align")) {
        uint64_t value;
        if (!number(a,&value)) return;
        if (value > (1u << 26)) { tool_error(a->c,"directive exceeds the 64 MiB section limit"); return; }
        if (tool_equal(name,".align")) {
            if (!value || (value & (value - 1))) { tool_error(a->c,"alignment must be a power of two"); return; }
            if (value > a->alignments[a->section]) a->alignments[a->section] = value;
            value = (0 - a->sections[a->section].size) & (value - 1);
        }
        tool_zeros(a->c,&a->sections[a->section],(size_t)value);
    } else if (tool_equal(name,".ascii") || tool_equal(name,".asciz")) {
        spaces(a);
        if (*a->p != '"') { tool_error(a->c,"expected a quoted string"); return; }
        ++a->p;
        while (*a->p != '"' && !a->c->failed) {
            unsigned v;
            if (!escape(a,&v)) return;
            emit(a,v);
        }
        if (*a->p == '"') ++a->p;
        if (tool_equal(name,".asciz")) emit(a,0);
    } else tool_error(a->c,"unknown assembler directive");
}

static void section_header(ncc_buffer *out, unsigned index, uint64_t table, uint32_t name, uint32_t type, uint64_t flags, uint64_t offset, uint64_t size, unsigned link, unsigned info, uint64_t alignment, unsigned entry) {
    unsigned char *h = out->data + table + index * 64;
    elf_set(h,name,4); elf_set(h+4,type,4); elf_set(h+8,flags,8);
    elf_set(h+24,offset,8); elf_set(h+32,size,8); elf_set(h+40,link,4); elf_set(h+44,info,4);
    elf_set(h+48,alignment,8); elf_set(h+56,entry,8);
}
static int object_output(Assembler *a, ncc_buffer *out) {
    static const char names[] = "\0.text\0.data\0.bss\0.rodata\0.rela.text\0.rela.data\0.rela.bss\0.rela.rodata\0.symtab\0.strtab\0.shstrtab\0";
    static const unsigned name_offsets[] = {0,1,7,13,18,26,37,48,58,71,79,87};
    uint64_t offsets[12] = {0}, sizes[12] = {0}, table;
    unsigned local_count = 1, index = 1;
    ncc_buffer strings, symbols, relocs[4];
    int result = 0;
    Context *c = a->c;
    ncc_buffer_init(&strings,c->platform);
    ncc_buffer_init(&symbols,c->platform);
    for (unsigned i = 0; i < 4; ++i) ncc_buffer_init(&relocs[i],c->platform);
    tool_byte(c,&strings,0);
    tool_zeros(c,&symbols,24);
    for (unsigned pass = 0; pass < 2; ++pass) for (AsmSymbol *s = a->symbols; s; s = s->next) {
        unsigned char entry[24] = {0};
        unsigned global = s->global || !s->defined;
        if (global != pass) continue;
        elf_set(entry,strings.size,4);
        elf_set(entry+4,global ? 16 : 0,1);
        elf_set(entry+6,s->defined ? s->section+1 : 0,2);
        elf_set(entry+8,s->value,8);
        tool_append(c,&strings,s->name,ncc_strlen(s->name)+1);
        tool_append(c,&symbols,entry,24);
        s->index = index++;
        if (!global) ++local_count;
    }
    for (AsmReloc *r = a->relocations; r; r = r->next) {
        unsigned char entry[24];
        elf_set(entry,r->offset,8);
        elf_set(entry+8,(uint64_t)r->symbol->index*0x100000000ull+r->type,8);
        elf_set(entry+16,r->addend,8);
        tool_append(c,&relocs[r->section],entry,24);
    }
    if (!tool_zeros(c,out,64)) goto done;
    for (unsigned i = 0; i < 4; ++i) {
        uint64_t alignment = a->alignments[i];
        if (!tool_zeros(c,out,(size_t)((0-out->size)&(alignment-1)))) goto done;
        offsets[i+1] = out->size;
        sizes[i+1] = a->sections[i].size;
        if (i != 2 && !tool_append(c,out,a->sections[i].data,a->sections[i].size)) goto done;
    }
    if (!tool_zeros(c,out,(0-out->size)&7)) goto done;
    for (unsigned i = 0; i < 4; ++i) {
        offsets[i+5] = out->size;
        sizes[i+5] = relocs[i].size;
        if (!tool_append(c,out,relocs[i].data,relocs[i].size)) goto done;
    }
    offsets[9] = out->size; sizes[9] = symbols.size;
    if (!tool_append(c,out,symbols.data,symbols.size)) goto done;
    offsets[10] = out->size; sizes[10] = strings.size;
    if (!tool_append(c,out,strings.data,strings.size)) goto done;
    offsets[11] = out->size; sizes[11] = sizeof(names);
    if (!tool_append(c,out,names,sizeof(names)) || !tool_zeros(c,out,(0-out->size)&7)) goto done;
    table = out->size;
    if (!tool_zeros(c,out,12*64)) goto done;
    ncc_memcpy(out->data,"\177ELF\2\1\1",7);
    elf_set(out->data+16,1,2); elf_set(out->data+18,62,2); elf_set(out->data+20,1,4);
    elf_set(out->data+40,table,8); elf_set(out->data+52,64,2); elf_set(out->data+58,64,2);
    elf_set(out->data+60,12,2); elf_set(out->data+62,11,2);
    for (unsigned i = 1; i < 12; ++i) {
        unsigned type = i == 3 ? 8 : i < 5 ? 1 : i < 9 ? 4 : i == 9 ? 2 : 3;
        unsigned flags = i == 1 ? 6 : i == 2 || i == 3 ? 3 : i == 4 ? 2 : 0;
        section_header(out,i,table,name_offsets[i],type,flags,offsets[i],sizes[i],i < 9 && i >= 5 ? 9 : i == 9 ? 10 : 0,i < 9 && i >= 5 ? i-4 : i == 9 ? local_count : 0,i < 5 ? a->alignments[i-1] : i < 10 ? 8 : 1,i >= 5 && i <= 9 ? 24 : 0);
    }
    result = !c->failed;
done:
    ncc_buffer_destroy(&strings); ncc_buffer_destroy(&symbols);
    for (unsigned i = 0; i < 4; ++i) ncc_buffer_destroy(&relocs[i]);
    return result;
}

int nas_assemble(Context *c, const char *source, ncc_buffer *out) {
    Assembler a;
    int result = 0;
    ncc_memset(&a,0,sizeof(a));
    a.c = c; a.p = source;
    c->line = 1;
    for (unsigned i = 0; i < 4; ++i) { ncc_buffer_init(&a.sections[i],c->platform); a.alignments[i] = i ? 8 : 16; }
    while (*a.p && !c->failed) {
        char *name;
        spaces(&a);
        if (*a.p == '\n') { ++a.p; ++c->line; continue; }
        if (*a.p == '#' || *a.p == ';') { while (*a.p && *a.p != '\n') ++a.p; continue; }
        if (!*a.p) break;
        name = word(&a);
        if (!name) break;
        spaces(&a);
        if (*a.p == ':') {
            AsmSymbol *s = asm_symbol(&a,name);
            if (!s) break;
            if (s->defined) { tool_error(c,"duplicate label definition"); break; }
            s->defined = 1; s->section = a.section; s->value = a.sections[a.section].size;
            ++a.p;
            continue;
        }
        if (*name == '.') directive(&a,name);
        else if (a.section == 2) tool_error(c,"instructions are not permitted in .bss");
        else instruction(&a,name);
        spaces(&a);
        if (*a.p && *a.p != '\n' && *a.p != '#' && *a.p != ';') tool_error(c,"unexpected text after instruction or directive");
        if (a.sections[a.section].size > (1u << 26)) tool_error(c,"section exceeds 64 MiB");
    }
    if (!c->failed) {
        for (size_t i = 0; i < a.sections[2].size; ++i)
            if (a.sections[2].data[i]) { tool_error(c,"nonzero data is not permitted in .bss"); break; }
        for (AsmReloc *r = a.relocations; r; r = r->next)
            if (r->section == 2) { tool_error(c,"relocations are not permitted in .bss"); break; }
    }
    if (!c->failed) result = object_output(&a,out);
    for (unsigned i = 0; i < 4; ++i) ncc_buffer_destroy(&a.sections[i]);
    return result;
}
