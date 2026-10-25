#include "front.h"

typedef struct Type Type;
typedef struct Member Member;
typedef struct Parameter Parameter;
typedef struct Symbol Symbol;
typedef struct Node Node;
typedef struct Function Function;
typedef struct InitialReloc InitialReloc;
struct InitialReloc { InitialReloc *next; size_t offset; Symbol *target; uint64_t addend; };
typedef struct Tag Tag;
typedef struct JumpLabel JumpLabel;
struct JumpLabel { JumpLabel *next; Token *name; unsigned id; int defined; };
struct Parameter { Parameter *next; Type *type; Token *name; };
struct Member { Member *next; Type *type; Token *name; size_t offset; };
struct Type {
    int kind, is_unsigned, is_bool, is_union, is_const;
    size_t size, alignment, length;
    Type *base;
    Parameter *parameters;
    Member *members;
    int variadic;
};
enum { TY_VOID, TY_INT, TY_POINTER, TY_ARRAY, TY_FUNCTION, TY_STRUCT };
struct Symbol {
    Symbol *next, *global_next;
    Token *name;
    const char *assembly_name;
    Type *type;
    size_t offset;
    int local, depth, is_typedef, constant, defined, internal, external, has_initializer;
    uint64_t value;
    ncc_buffer initial;
    Symbol *target;
    InitialReloc *relocations;
    uint64_t addend;
};
struct Tag { Tag *next; Token *name; Type *type; int depth; };
struct Node {
    int kind;
    Token *token;
    Type *type;
    Node *left, *right, *condition, *then, *otherwise, *init, *step, *body, *next, *arguments, *cases, *case_next;
    Symbol *symbol;
    uint64_t value;
    size_t member_offset, argument_offset, callee_offset;
    int argument_register;
    size_t argument_stack, argument_slots;
    const char *op;
};
enum { ND_NUMBER, ND_VARIABLE, ND_BINARY, ND_ASSIGN, ND_ADDRESS, ND_DEREF, ND_NEGATE, ND_NOT, ND_BITNOT, ND_CAST, ND_CALL, ND_CONDITIONAL, ND_COMMA, ND_MEMBER, ND_PREINC, ND_POSTINC, ND_BLOCK, ND_EXPR, ND_RETURN, ND_IF, ND_FOR, ND_DO, ND_BREAK, ND_CONTINUE, ND_EMPTY, ND_SWITCH, ND_CASE, ND_GOTO, ND_LABEL, ND_ZERO, ND_VASTART, ND_VAARG, ND_LITERAL };
struct Function { Function *next; Symbol *symbol; Symbol **parameters; size_t parameter_count, stack_size, argument_spill, named_gp, named_stack; Node *body; Symbol *function_name; };
typedef struct {
    Context *c;
    Token *token;
    Symbol *symbols, *globals;
    Tag *tags;
    Function *functions, *current;
    Type *t_void, *t_char, *t_short, *t_int, *t_long;
    int depth, loops, breakable, optimize;
    Node *switch_current;
    JumpLabel *jump_labels;
    unsigned label;
    ncc_buffer *output;
    size_t stack_depth;
    unsigned return_label, break_label, continue_label;
} Compiler;
static Node *expression(Compiler *);
static Node *assignment(Compiler *);
static Node *statement(Compiler *);
static Type *declaration_specifiers(Compiler *, int *, int *, int *);
static Type *declarator(Compiler *, Type *, Token **, int);
static void generate_expression(Compiler *, Node *);
static void generate_statement(Compiler *, Node *);
static void initialize_object(Compiler *, Symbol *, Type *, size_t, Node **, int);
static void allocate_local(Compiler *, Symbol *);

static void location(Compiler *p, Token *t) { if (t) { p->c->path = t->path; p->c->line = t->line; } }
static void error(Compiler *p, const char *s) { location(p,p->token); tool_error(p->c,s); }
static int take(Compiler *p, const char *s) {
    if (!token_is(p->token,s)) return 0;
    p->token = p->token->next;
    return 1;
}
static void expect(Compiler *p, const char *s) {
    if (!take(p,s)) {
        ncc_buffer b;
        ncc_buffer_init(&b,p->c->platform);
        tool_append(p->c,&b,"expected '",10); tool_append(p->c,&b,s,ncc_strlen(s));
        tool_append(p->c,&b,"'",2);
        error(p,(char *)b.data); ncc_buffer_destroy(&b);
    }
}
static Type *type_new(Compiler *p, int kind, size_t size, size_t alignment) {
    Type *t = tool_alloc(p->c,sizeof(*t));
    if (t) { t->kind = kind; t->size = size; t->alignment = alignment; }
    return t;
}
static Type *pointer_type(Compiler *p, Type *base) {
    Type *t = type_new(p,TY_POINTER,8,8);
    if (t) t->base = base;
    return t;
}
static int pointer(Type *t) { return t && (t->kind == TY_POINTER || t->kind == TY_ARRAY); }
static int integer_type(Type *t) { return t && t->kind == TY_INT; }
static size_t aligned(size_t n, size_t a) { return (n+a-1)&(0-a); }
static int compatible(Type *a, Type *b) {
    if (a == b) return 1;
    if (!a || !b || a->kind != b->kind) return 0;
    if (a->kind == TY_VOID) return 1;
    if (a->kind == TY_INT) return a->size == b->size && a->is_unsigned == b->is_unsigned && a->is_bool == b->is_bool;
    if (a->kind == TY_STRUCT) return a->members == b->members && a->size == b->size && a->is_union == b->is_union;
    if (a->kind == TY_POINTER) return compatible(a->base,b->base);
    if (a->kind == TY_ARRAY) return (!a->length || !b->length || a->length == b->length) && compatible(a->base,b->base);
    if (a->kind == TY_FUNCTION) {
        Parameter *x = a->parameters, *y = b->parameters;
        if (!compatible(a->base,b->base) || a->variadic != b->variadic) return 0;
        while (x && y) { if (!compatible(x->type,y->type)) return 0; x = x->next; y = y->next; }
        return !x && !y;
    }
    return 0;
}
static Symbol *lookup(Compiler *p, Token *name) {
    for (Symbol *s = p->symbols; s; s = s->next)
        if (s->name->length == name->length && !ncc_memcmp(s->name->text,name->text,name->length)) return s;
    return NULL;
}
static Symbol *new_symbol(Compiler *p, Token *name, Type *type, int local) {
    Symbol *s = tool_alloc(p->c,sizeof(*s));
    if (!s) return NULL;
    s->name = name; s->type = type; s->local = local; s->depth = p->depth;
    s->assembly_name = token_string(p->c,name);
    s->next = p->symbols; p->symbols = s;
    ncc_buffer_init(&s->initial,p->c->platform);
    if (!local) { s->global_next = p->globals; p->globals = s; }
    return s;
}
static Node *node(Compiler *p, int kind, Token *at) {
    Node *n = tool_alloc(p->c,sizeof(*n));
    if (n) { n->kind = kind; n->token = at; n->type = p->t_int; }
    return n;
}
static Node *number_node(Compiler *p, uint64_t value, Type *type, Token *at) {
    Node *n = node(p,ND_NUMBER,at);
    if (n) { n->value = value; n->type = type; }
    return n;
}
static Node *variable_node(Compiler *p, Symbol *s, Token *at) {
    Node *n = node(p,ND_VARIABLE,at);
    if (n) { n->symbol = s; n->type = s->type; }
    return n;
}
static int type_start(Compiler *p) {
    static const char *const words[] = {"void","char","short","int","long","signed","unsigned","const","volatile","restrict","struct","enum","typedef","static","extern","auto","register","inline","_Bool","float","double","union",NULL};
    Symbol *s;
    for (unsigned i = 0; words[i]; ++i) if (token_is(p->token,words[i])) return 1;
    s = p->token && p->token->kind == TK_IDENT ? lookup(p,p->token) : NULL;
    return s && s->is_typedef;
}
static uint64_t literal_integer(Compiler *p, Token *t, Type **type) {
    uint64_t value = 0;
    unsigned base = 10;
    size_t i = 0;
    int unsigned_suffix = 0, long_suffix = 0;
    *type = p->t_int;
    if (t->length > 1 && t->text[0] == '0') { base = 8; i = 1; }
    if (t->length > 2 && t->text[0] == '0' && (t->text[1] == 'x' || t->text[1] == 'X')) { base = 16; i = 2; }
    for (; i < t->length; ++i) {
        unsigned ch = (unsigned char)t->text[i], d = ch >= '0' && ch <= '9' ? ch-'0' : ch >= 'a' && ch <= 'f' ? ch-'a'+10 : ch >= 'A' && ch <= 'F' ? ch-'A'+10 : 99;
        if (d >= base) break;
        if (value > (UINT64_MAX-d)/base) { error(p,"integer literal exceeds 64 bits"); return 0; }
        value = value*base+d;
    }
    for (; i < t->length; ++i) {
        char ch = t->text[i];
        if ((ch == 'u' || ch == 'U') && !unsigned_suffix) unsigned_suffix = 1;
        else if ((ch == 'l' || ch == 'L') && long_suffix < 2) ++long_suffix;
        else { error(p,"invalid integer literal or unsupported floating-point literal"); return 0; }
    }
    if (long_suffix || value > UINT32_MAX || (value > INT32_MAX && base == 10 && !unsigned_suffix)) *type = p->t_long;
    if (unsigned_suffix || (value > INT32_MAX && base != 10 && !long_suffix) || value > INT64_MAX) {
        Type *tcopy = tool_alloc(p->c,sizeof(**type));
        if (tcopy) { *tcopy = **type; tcopy->is_unsigned = 1; *type = tcopy; }
    }
    return value;
}
static unsigned decoded(Compiler *p, const char **cursor, const char *end) {
    const char *s = *cursor;
    unsigned v;
    if (s >= end) { error(p,"empty character literal"); return 0; }
    v = (unsigned char)*s++;
    if (v == '\\') {
        if (s >= end) { error(p,"incomplete escape sequence"); return 0; }
        v = (unsigned char)*s++;
        if (v == 'n') v = 10;
        else if (v == 't') v = 9;
        else if (v == 'r') v = 13;
        else if (v == 'a') v = 7;
        else if (v == 'b') v = 8;
        else if (v == 'f') v = 12;
        else if (v == 'v') v = 11;
        else if (v >= '0' && v <= '7') {
            v -= '0';
            for (unsigned n = 1; n < 3 && s < end && *s >= '0' && *s <= '7'; ++n) v = v*8+(unsigned)(*s++-'0');
        } else if (v == 'x') {
            unsigned count = 0; v = 0;
            while (s < end) {
                unsigned ch = (unsigned char)*s, d = ch >= '0' && ch <= '9' ? ch-'0' : ch >= 'a' && ch <= 'f' ? ch-'a'+10 : ch >= 'A' && ch <= 'F' ? ch-'A'+10 : 99;
                if (d >= 16) break;
                v = v*16+d; ++s; ++count;
            }
            if (!count) error(p,"hex escape requires a digit");
        } else if (v != '\\' && v != '\'' && v != '"' && v != '?') error(p,"invalid escape sequence");
    }
    *cursor = s;
    return v;
}
static uint64_t normalized_value(uint64_t value, Type *type) {
    if (type->is_bool) return value != 0;
    if (type->kind == TY_INT && type->size < 8) {
        unsigned bits = (unsigned)type->size*8;
        value &= (1ull<<bits)-1;
        if (!type->is_unsigned && (value & (1ull<<(bits-1)))) value |= UINT64_MAX << bits;
    }
    return value;
}
static int constant(Node *n, uint64_t *value) {
    uint64_t l, r;
    if (!n) return 0;
    if (n->kind == ND_NUMBER) { *value = normalized_value(n->value,n->type); return 1; }
    if (n->kind == ND_CAST && constant(n->left,&l)) {
        *value = normalized_value(l,n->type); return 1;
    }
    if ((n->kind == ND_NEGATE || n->kind == ND_NOT || n->kind == ND_BITNOT) && constant(n->left,&l)) {
        *value = normalized_value(n->kind == ND_NEGATE ? 0-l : n->kind == ND_NOT ? !l : ~l,n->type); return 1;
    }
    if (n->kind == ND_CONDITIONAL && constant(n->condition,&l)) return constant(l ? n->then : n->otherwise,value);
    if (n->kind != ND_BINARY || !constant(n->left,&l)) return 0;
    if (tool_equal(n->op,"&&") && !l) { *value = 0; return 1; }
    if (tool_equal(n->op,"||") && l) { *value = 1; return 1; }
    if (!constant(n->right,&r)) return 0;
    if (tool_equal(n->op,"+")) l += r;
    else if (tool_equal(n->op,"-")) l -= r;
    else if (tool_equal(n->op,"*")) l *= r;
    else if (tool_equal(n->op,"/") || tool_equal(n->op,"%")) {
        if (!r || (l == (1ull<<63) && r == UINT64_MAX)) return 0;
        if (n->left->type->is_unsigned) l = tool_equal(n->op,"/") ? l/r : l%r;
        else l = (uint64_t)(tool_equal(n->op,"/") ? (int64_t)l/(int64_t)r : (int64_t)l%(int64_t)r);
    } else if (tool_equal(n->op,"&")) l &= r;
    else if (tool_equal(n->op,"|")) l |= r;
    else if (tool_equal(n->op,"^")) l ^= r;
    else if (tool_equal(n->op,"<<")) { if (r >= 64) return 0; l <<= r; }
    else if (tool_equal(n->op,">>")) { if (r >= 64) return 0; l = n->left->type->is_unsigned ? l >> r : (uint64_t)((int64_t)l >> r); }
    else if (tool_equal(n->op,"==")) l = l == r;
    else if (tool_equal(n->op,"!=")) l = l != r;
    else if (tool_equal(n->op,"<")) l = n->left->type->is_unsigned ? l < r : (int64_t)l < (int64_t)r;
    else if (tool_equal(n->op,">")) l = n->left->type->is_unsigned ? l > r : (int64_t)l > (int64_t)r;
    else if (tool_equal(n->op,"<=")) l = n->left->type->is_unsigned ? l <= r : (int64_t)l <= (int64_t)r;
    else if (tool_equal(n->op,">=")) l = n->left->type->is_unsigned ? l >= r : (int64_t)l >= (int64_t)r;
    else if (tool_equal(n->op,"&&")) l = l && r;
    else if (tool_equal(n->op,"||")) l = l || r;
    else return 0;
    *value = normalized_value(l,n->type); return 1;
}
static Node *cast_node(Compiler *p, Node *n, Type *type) {
    Node *r = node(p,ND_CAST,n->token);
    if (r) { r->left = n; r->type = type; }
    return r;
}
static Node *binary_node(Compiler *p, const char *op, Node *left, Node *right, Token *at) {
    Node *n;
    if (!left || !right) return NULL;
    n = node(p,ND_BINARY,at);
    if (!n) return NULL;
    n->op = op; n->left = left; n->right = right;
    if ((tool_equal(op,"+") || tool_equal(op,"-")) && pointer(left->type) && integer_type(right->type)) {
        Node *scale = binary_node(p,"*",right,number_node(p,left->type->base->size,p->t_long,at),at);
        n->right = scale; n->type = pointer_type(p,left->type->base); return n;
    }
    if (tool_equal(op,"+") && integer_type(left->type) && pointer(right->type)) return binary_node(p,op,right,left,at);
    if (tool_equal(op,"-") && pointer(left->type) && pointer(right->type)) {
        n->type = p->t_long;
        return binary_node(p,"/",n,number_node(p,left->type->base->size,p->t_long,at),at);
    }
    if (!integer_type(left->type) && !pointer(left->type)) error(p,"invalid binary operand type");
    if (!integer_type(right->type) && !pointer(right->type)) error(p,"invalid binary operand type");
    if (tool_equal(op,"<<") || tool_equal(op,">>")) {
        if (!integer_type(left->type) || !integer_type(right->type)) error(p,"shift operands must have integer types");
        n->type = left->type->size < 4 ? p->t_int : left->type;
        n->left = cast_node(p,left,n->type); return n;
    }
    n->type = left->type->size > 4 || right->type->size > 4 ? p->t_long : p->t_int;
    if (pointer(left->type) || pointer(right->type) ||
        ((left->type->is_unsigned && left->type->size >= 4 && left->type->size >= right->type->size) ||
        (right->type->is_unsigned && right->type->size >= 4 && right->type->size >= left->type->size))) {
        Type *t = tool_alloc(p->c,sizeof(*t));
        if (t) { *t = *n->type; t->is_unsigned = 1; n->type = t; }
    }
    n->left = cast_node(p,left,n->type); n->right = cast_node(p,right,n->type);
    if (tool_equal(op,"==") || tool_equal(op,"!=") || tool_equal(op,"<") || tool_equal(op,">") || tool_equal(op,"<=") || tool_equal(op,">=") || tool_equal(op,"&&") || tool_equal(op,"||")) n->type = p->t_int;
    return n;
}

static Type *aggregate(Compiler *p, int enumeration) {
    Token *name = NULL;
    Tag *tag = NULL;
    Type *t;
    if (p->token->kind == TK_IDENT) { name = p->token; p->token = p->token->next; }
    if (name) for (Tag *s = p->tags; s; s = s->next)
        if (s->depth <= p->depth && s->name->length == name->length && !ncc_memcmp(s->name->text,name->text,name->length)) { tag = s; break; }
    if (!token_is(p->token,"{")) {
        if (tag) return tag->type;
        if (!name || enumeration == 1) { error(p,"unknown type tag"); return p->t_int; }
    }
    t = enumeration == 1 ? p->t_int : type_new(p,TY_STRUCT,0,1);
    if (!tag && name) {
        tag = tool_alloc(p->c,sizeof(*tag));
        if (!tag) return t;
        tag->name = name; tag->type = t; tag->depth = p->depth; tag->next = p->tags; p->tags = tag;
    } else if (tag) {
        if (tag->type->size) { error(p,"type tag is already defined"); return tag->type; }
        t = tag->type;
    }
    if (!take(p,"{")) return t;
    t->is_union = enumeration == 2;
    if (enumeration == 1) {
        uint64_t value = 0;
        while (!p->c->failed && !take(p,"}")) {
            Token *identifier = p->token;
            Symbol *s;
            if (identifier->kind != TK_IDENT) { error(p,"expected an enumerator name"); break; }
            p->token = identifier->next;
            if (take(p,"=")) {
                Node *n = assignment(p);
                if (!constant(n,&value)) error(p,"enumerator is not an integer constant expression");
            }
            s = new_symbol(p,identifier,p->t_int,1);
            if (!s) return t;
            s->constant = 1; s->value = value++;
            if (take(p,"}")) break;
            expect(p,",");
        }
    } else {
        Member head = {0}, *tail = &head;
        size_t size = 0, alignment = 1;
        while (!p->c->failed && !take(p,"}")) {
            int td = 0, st = 0, ex = 0;
            Type *base = declaration_specifiers(p,&td,&st,&ex);
            do {
                Token *identifier = NULL;
                Type *member_type = declarator(p,base,&identifier,0);
                Member *m = tool_alloc(p->c,sizeof(*m));
                if (!m) return t;
                if (!identifier || (!member_type->size && member_type->kind != TY_ARRAY)) { error(p,"structure member needs a name and complete type"); break; }
                for (Member *old = head.next; old; old = old->next)
                    if (old->name->length == identifier->length && !ncc_memcmp(old->name->text,identifier->text,identifier->length)) error(p,"duplicate structure member");
                if (tail != &head && tail->type->kind == TY_ARRAY && !tail->type->size) error(p,"flexible array member must be last");
                if (!t->is_union) size = aligned(size,member_type->alignment);
                m->type = member_type; m->name = identifier; m->offset = t->is_union ? 0 : size;
                if (size > SIZE_MAX-member_type->size) { error(p,"structure size overflow"); break; }
                if (t->is_union) { if (member_type->size > size) size = member_type->size; }
                else size += member_type->size;
                if (member_type->alignment > alignment) alignment = member_type->alignment;
                tail = tail->next = m;
            } while (take(p,","));
            expect(p,";");
        }
        t->members = head.next; t->alignment = alignment; t->size = aligned(size,alignment);
    }
    return t;
}
static Type *declaration_specifiers(Compiler *p, int *td, int *st, int *ex) {
    Type *type = NULL;
    int sign = 0, longs = 0, shorts = 0, chars = 0, ints = 0, seen = 0, qual_const = 0;
    for (;;) {
        if (take(p,"typedef")) { *td = 1; continue; }
        if (take(p,"static")) { *st = 1; continue; }
        if (take(p,"extern")) { *ex = 1; continue; }
        if (take(p,"const")) { qual_const = 1; continue; }
        if (take(p,"auto") || take(p,"register") || take(p,"inline") || take(p,"volatile") || take(p,"restrict")) continue;
        if (type) break;
        if (take(p,"unsigned")) { sign = 1; seen = 1; continue; }
        if (take(p,"signed")) { sign = -1; seen = 1; continue; }
        if (take(p,"long")) { ++longs; seen = 1; continue; }
        if (take(p,"short")) { ++shorts; seen = 1; continue; }
        if (take(p,"char")) { ++chars; seen = 1; continue; }
        if (take(p,"int")) { ++ints; seen = 1; continue; }
        if (take(p,"void")) { type = p->t_void; continue; }
        if (take(p,"_Bool")) { type = type_new(p,TY_INT,1,1); type->is_unsigned = type->is_bool = 1; continue; }
        if (take(p,"struct")) { type = aggregate(p,0); continue; }
        if (take(p,"union")) { type = aggregate(p,2); continue; }
        if (take(p,"enum")) { type = aggregate(p,1); continue; }
        if (token_is(p->token,"float") || token_is(p->token,"double")) { error(p,"floating-point types are not implemented yet"); return p->t_int; }
        if (!seen && p->token->kind == TK_IDENT) {
            Symbol *s = lookup(p,p->token);
            if (s && s->is_typedef) { type = s->type; p->token = p->token->next; continue; }
        }
        break;
    }
    if (!seen && !type) { error(p,"expected a type specifier"); return p->t_int; }
    if (!type) {
        if (longs > 2 || shorts > 1 || chars > 1 || ints > 1 || (shorts && longs) || (chars && (shorts || longs || ints))) error(p,"invalid combination of type specifiers");
        type = chars ? p->t_char : shorts ? p->t_short : longs ? p->t_long : p->t_int;
        if (sign > 0) {
            Type *copy = tool_alloc(p->c,sizeof(*copy));
            if (copy) { *copy = *type; copy->is_unsigned = 1; type = copy; }
        }
    }
    if (qual_const) { Type *copy = tool_alloc(p->c,sizeof(*copy)); *copy = *type; copy->is_const = 1; type = copy; }
    if ((*td && (*st || *ex)) || (*st && *ex)) error(p,"conflicting storage class specifiers");
    return type;
}
static Type *suffix(Compiler *p, Type *base) {
    if (take(p,"[")) {
        uint64_t length = 0;
        Type *element, *t;
        if (!take(p,"]")) {
            Node *n = assignment(p);
            if (!constant(n,&length)) error(p,"variable length arrays are not implemented yet");
            expect(p,"]");
        }
        element = suffix(p,base);
        if (element->size && length > SIZE_MAX/element->size) { error(p,"array size overflow"); length = 0; }
        t = type_new(p,TY_ARRAY,(size_t)length*element->size,element->alignment);
        if (t) { t->base = element; t->length = (size_t)length; }
        return t;
    }
    if (take(p,"(")) {
        Type *t = type_new(p,TY_FUNCTION,1,1);
        Parameter head = {0}, *tail = &head;
        if (!t) return base;
        t->base = base;
        if (token_is(p->token,"void") && token_is(p->token->next,")")) p->token = p->token->next;
        while (!p->c->failed && !take(p,")")) {
            int td = 0, st = 0, ex = 0;
            Type *pt;
            Token *name = NULL;
            Parameter *param;
            if (take(p,"...")) { t->variadic = 1; expect(p,")"); break; }
            pt = declaration_specifiers(p,&td,&st,&ex);
            pt = declarator(p,pt,&name,1);
            if (pt->kind == TY_ARRAY) pt = pointer_type(p,pt->base);
            if (pt->kind == TY_FUNCTION) pt = pointer_type(p,pt);
            param = tool_alloc(p->c,sizeof(*param));
            if (!param) return t;
            param->type = pt; param->name = name; tail = tail->next = param;
            if (take(p,")")) break;
            expect(p,",");
        }
        t->parameters = head.next;
        return t;
    }
    return base;
}
static Type *declarator(Compiler *p, Type *base, Token **name, int abstract) {
    while (take(p,"*")) {
        base = pointer_type(p,base);
        while (token_is(p->token,"const") || token_is(p->token,"volatile") || token_is(p->token,"restrict")) { if (take(p,"const")) base->is_const = 1; else p->token = p->token->next; }
    }
    if (take(p,"(")) {
        Type *placeholder = type_new(p,TY_VOID,0,1);
        Type *inner = declarator(p,placeholder,name,abstract), *outer;
        expect(p,")");
        outer = suffix(p,base);
        if (placeholder && outer) *placeholder = *outer;
        return inner;
    }
    if (p->token->kind == TK_IDENT) { *name = p->token; p->token = p->token->next; }
    else if (!abstract) error(p,"expected a declarator name");
    return suffix(p,base);
}

static Node *primary(Compiler *p) {
    Token *t = p->token;
    if (take(p,"(")) {
        Node *n = expression(p); expect(p,")"); return n;
    }
    if (token_is(t,"__ncc_va_start") || token_is(t,"__ncc_va_arg")) {
        int start = token_is(t,"__ncc_va_start");
        Node *n = node(p,start ? ND_VASTART : ND_VAARG,t);
        p->token = t->next; expect(p,"("); n->left = assignment(p); expect(p,",");
        if (start) {
            if (!p->current || !p->current->symbol->type->variadic) error(p,"va_start requires a variadic function");
            assignment(p); n->type = p->t_void;
        } else {
            int td = 0, st = 0, ex = 0;
            Token *name = NULL;
            n->type = declaration_specifiers(p,&td,&st,&ex); n->type = declarator(p,n->type,&name,1);
            if (n->type->kind == TY_ARRAY || n->type->kind == TY_FUNCTION || n->type->kind == TY_VOID || !n->type->size) error(p,"invalid va_arg type");
        }
        expect(p,")"); return n;
    }
    if (token_is(t,"__func__") && p->current) {
        if (p->current->function_name) { p->token = t->next; return variable_node(p,p->current->function_name,t); }
        ncc_buffer literal;
        ncc_buffer_init(&literal,p->c->platform); tool_byte(p->c,&literal,'"');
        tool_append(p->c,&literal,p->current->symbol->name->text,p->current->symbol->name->length); tool_byte(p->c,&literal,'"');
        Token *replacement = tool_alloc(p->c,sizeof(*replacement)); *replacement = *t;
        replacement->text = tool_string(p->c,(char *)literal.data,literal.size); replacement->length = literal.size; replacement->kind = TK_STRING;
        ncc_buffer_destroy(&literal); p->token = replacement;
        Node *value = primary(p); p->current->function_name = value->symbol; return value;
    }
    if (t->kind == TK_NUMBER) {
        Type *type;
        uint64_t value = literal_integer(p,t,&type);
        p->token = t->next;
        return number_node(p,value,type,t);
    }
    if (t->kind == TK_CHAR) {
        const char *cursor = t->text+1;
        unsigned value = decoded(p,&cursor,t->text+t->length-1);
        if (cursor != t->text+t->length-1) error(p,"multi-character constants are not implemented");
        p->token = t->next;
        return number_node(p,value,p->t_int,t);
    }
    if (t->kind == TK_STRING) {
        ncc_buffer bytes, name;
        Symbol *s;
        Type *array;
        Token *label;
        char *label_text;
        ncc_buffer_init(&bytes,p->c->platform); ncc_buffer_init(&name,p->c->platform);
        while (p->token->kind == TK_STRING) {
            Token *str = p->token;
            const char *cursor = str->text+1;
            while (cursor < str->text+str->length-1 && !p->c->failed) tool_byte(p->c,&bytes,decoded(p,&cursor,str->text+str->length-1));
            p->token = str->next;
        }
        tool_byte(p->c,&bytes,0);
        tool_append(p->c,&name,".LC",3); tool_number(p->c,&name,p->label++);
        label_text = tool_string(p->c,(char *)name.data,name.size);
        ncc_buffer_destroy(&name);
        label = tool_alloc(p->c,sizeof(*label));
        if (!label || !label_text) { ncc_buffer_destroy(&bytes); return NULL; }
        *label = *t; label->text = label_text; label->length = ncc_strlen(label_text);
        array = type_new(p,TY_ARRAY,bytes.size,1); array->base = p->t_char; array->length = bytes.size;
        s = new_symbol(p,label,array,0);
        if (!s) { ncc_buffer_destroy(&bytes); return NULL; }
        s->defined = 1; s->internal = 1; s->initial = bytes;
        return variable_node(p,s,t);
    }
    if (t->kind == TK_IDENT) {
        Symbol *s = lookup(p,t);
        p->token = t->next;
        if (!s || s->is_typedef) { location(p,t); tool_error(p->c,"use of an undeclared identifier"); return number_node(p,0,p->t_int,t); }
        if (s->constant) return number_node(p,s->value,s->type,t);
        return variable_node(p,s,t);
    }
    error(p,"expected an expression");
    return number_node(p,0,p->t_int,t);
}
static Node *postfix(Compiler *p) {
    Node *n = primary(p);
    while (!p->c->failed && n) {
        Token *at = p->token;
        if (take(p,"(")) {
            Node head = {0}, *tail = &head, *call = node(p,ND_CALL,at);
            Type *function_type = n->type;
            Parameter *parameter_type;
            if (function_type->kind == TY_POINTER) function_type = function_type->base;
            if (function_type->kind != TY_FUNCTION) { error(p,"called expression is not a function"); return n; }
            parameter_type = function_type->parameters;
            while (!p->c->failed && !take(p,")")) {
                Node *arg = assignment(p);
                if (parameter_type) { arg = cast_node(p,arg,parameter_type->type); parameter_type = parameter_type->next; }
                else if (!function_type->variadic) error(p,"too many function arguments");
                if (!arg) return n;
                tail = tail->next = arg;
                if (take(p,")")) break;
                expect(p,",");
            }
            if (parameter_type) error(p,"too few function arguments");
            call->left = n; call->arguments = head.next; call->type = function_type->base;
            if (p->current) {
                for (Node *arg = head.next; arg; arg = arg->next) {
                    size_t size = arg->type->kind == TY_STRUCT ? aligned(arg->type->size,8) : 8;
                    p->current->stack_size = aligned(p->current->stack_size+size,8); arg->argument_offset = p->current->stack_size;
                }
                p->current->stack_size = aligned(p->current->stack_size+8,8); call->callee_offset = p->current->stack_size;
                if (call->type->kind == TY_STRUCT) { p->current->stack_size += aligned(call->type->size,8); call->member_offset = p->current->stack_size; }
            }
            n = call;
        } else if (take(p,"[")) {
            Node *index = expression(p), *add;
            expect(p,"]");
            add = binary_node(p,"+",n,index,at);
            if (!add || !pointer(add->type)) { error(p,"subscript requires a pointer and an integer"); return n; }
            n = node(p,ND_DEREF,at); n->left = add; n->type = add->type->base;
        } else if (token_is(p->token,".") || token_is(p->token,"->")) {
            int indirect = take(p,"->");
            Member *member;
            if (!indirect) expect(p,".");
            if (indirect) {
                Node *deref;
                if (!pointer(n->type)) { error(p,"'->' requires a pointer"); return n; }
                deref = node(p,ND_DEREF,at); deref->left = n; deref->type = n->type->base; n = deref;
            }
            if (n->type->kind != TY_STRUCT || p->token->kind != TK_IDENT) { error(p,"expected a structure member name"); return n; }
            for (member = n->type->members; member; member = member->next)
                if (member->name->length == p->token->length && !ncc_memcmp(member->name->text,p->token->text,p->token->length)) break;
            if (!member) { error(p,"structure has no such member"); return n; }
            p->token = p->token->next;
            Node *access = node(p,ND_MEMBER,at); access->left = n; access->type = member->type; access->member_offset = member->offset; n = access;
        } else if (take(p,"++") || take(p,"--")) {
            Node *inc = node(p,ND_POSTINC,at); inc->left = n; inc->type = n->type; inc->value = token_is(at,"++") ? 1 : UINT64_MAX; n = inc;
        } else break;
    }
    return n;
}
static Node *unary(Compiler *p) {
    Token *at = p->token;
    Node *n;
    if (take(p,"sizeof")) {
        Type *t;
        Token *save = p->token;
        if (take(p,"(") && type_start(p)) {
            int td = 0, st = 0, ex = 0;
            Token *name = NULL;
            t = declaration_specifiers(p,&td,&st,&ex); t = declarator(p,t,&name,1); expect(p,")");
        } else { p->token = save; n = unary(p); t = n->type; }
        if (!t->size || t->kind == TY_VOID || t->kind == TY_FUNCTION) error(p,"sizeof requires a complete object type");
        Type *size_type = tool_alloc(p->c,sizeof(*size_type)); *size_type = *p->t_long; size_type->is_unsigned = 1;
        return number_node(p,t->size,size_type,at);
    }
    if (token_is(p->token,"(") && p->token->next) {
        Token *save = p->token;
        p->token = p->token->next;
        if (type_start(p)) {
            int td = 0, st = 0, ex = 0;
            Token *name = NULL;
            Type *t = declaration_specifiers(p,&td,&st,&ex);
            t = declarator(p,t,&name,1); expect(p,")");
            if (token_is(p->token,"{")) {
                ncc_buffer label;
                Token *identifier = tool_alloc(p->c,sizeof(*identifier));
                ncc_buffer_init(&label,p->c->platform); tool_append(p->c,&label,".LL",3); tool_number(p->c,&label,p->label++);
                *identifier = *at; identifier->text = tool_string(p->c,(char *)label.data,label.size); identifier->length = label.size; identifier->kind = TK_IDENT;
                ncc_buffer_destroy(&label);
                Symbol *s = new_symbol(p,identifier,t,p->current != NULL);
                Node *literal = node(p,ND_LITERAL,at), head = {0}, *tail = &head;
                literal->symbol = s; literal->type = t;
                if (s->local) {
                    if (t->kind == TY_STRUCT || t->kind == TY_ARRAY) { Node *zero = node(p,ND_ZERO,at); zero->symbol = s; tail = tail->next = zero; }
                    initialize_object(p,s,t,0,&tail,0); allocate_local(p,s); literal->body = head.next;
                } else { s->internal = s->defined = 1; initialize_object(p,s,t,0,&tail,1); if (s->initial.size < t->size) tool_zeros(p->c,&s->initial,t->size-s->initial.size); }
                return literal;
            }
            return cast_node(p,unary(p),t);
        }
        p->token = save;
    }
    if (take(p,"+")) return unary(p);
    if (take(p,"-") || take(p,"!") || take(p,"~") || take(p,"&") || take(p,"*") || take(p,"++") || take(p,"--")) {
        int kind = token_is(at,"-") ? ND_NEGATE : token_is(at,"!") ? ND_NOT : token_is(at,"~") ? ND_BITNOT : token_is(at,"&") ? ND_ADDRESS : token_is(at,"*") ? ND_DEREF : ND_PREINC;
        n = node(p,kind,at); n->left = unary(p); n->type = n->left->type;
        if (kind == ND_ADDRESS) n->type = pointer_type(p,n->left->type);
        if (kind == ND_DEREF) {
            if (!pointer(n->left->type)) error(p,"dereference requires a pointer");
            else n->type = n->left->type->base;
        }
        if (kind == ND_NOT) n->type = p->t_int;
        if (kind == ND_PREINC) n->value = token_is(at,"++") ? 1 : UINT64_MAX;
        return n;
    }
    return postfix(p);
}
static int binary_precedence(Token *t) {
    if (token_is(t,"||")) return 1;
    if (token_is(t,"&&")) return 2;
    if (token_is(t,"|")) return 3;
    if (token_is(t,"^")) return 4;
    if (token_is(t,"&")) return 5;
    if (token_is(t,"==") || token_is(t,"!=")) return 6;
    if (token_is(t,"<") || token_is(t,">") || token_is(t,"<=") || token_is(t,">=")) return 7;
    if (token_is(t,"<<") || token_is(t,">>")) return 8;
    if (token_is(t,"+") || token_is(t,"-")) return 9;
    if (token_is(t,"*") || token_is(t,"/") || token_is(t,"%")) return 10;
    return 0;
}
static Node *binary_expression(Compiler *p, int min) {
    Node *left = unary(p);
    while (!p->c->failed) {
        Token *op = p->token;
        int prec = binary_precedence(op);
        if (prec < min) break;
        p->token = op->next;
        left = binary_node(p,token_string(p->c,op),left,binary_expression(p,prec+1),op);
    }
    return left;
}
static Node *conditional_expression(Compiler *p) {
    Node *left = binary_expression(p,1);
    if (take(p,"?")) {
        Node *n = node(p,ND_CONDITIONAL,left->token);
        n->condition = left; n->then = expression(p); expect(p,":"); n->otherwise = conditional_expression(p); n->type = n->then->type;
        if (integer_type(n->then->type) && integer_type(n->otherwise->type)) {
            Node *conversion = binary_node(p,"+",n->then,n->otherwise,n->token);
            n->type = conversion->type;
        } else if (pointer(n->then->type)) n->type = pointer_type(p,n->then->type->base);
        else if (pointer(n->otherwise->type)) n->type = pointer_type(p,n->otherwise->type->base);
        n->then = cast_node(p,n->then,n->type); n->otherwise = cast_node(p,n->otherwise,n->type);
        return n;
    }
    return left;
}
static Node *assignment(Compiler *p) {
    Node *left = conditional_expression(p);
    Token *op = p->token;
    if (take(p,"=")) {
        if (left->type->is_const) error(p,"assignment to a const-qualified object");
        Node *n = node(p,ND_ASSIGN,op); n->left = left; n->right = assignment(p); n->type = left->type; return n;
    }
    if (token_is(op,"+=") || token_is(op,"-=") || token_is(op,"*=") || token_is(op,"/=") || token_is(op,"%=") || token_is(op,"&=") || token_is(op,"|=") || token_is(op,"^=") || token_is(op,"<<=") || token_is(op,">>=")) {
        if (left->type->is_const) error(p,"assignment to a const-qualified object");
        Node *n = node(p,ND_ASSIGN,op);
        p->token = op->next;
        n->left = left; n->right = assignment(p); n->type = left->type;
        n->op = tool_string(p->c,op->text,op->length-1);
        return n;
    }
    return left;
}
static Node *expression(Compiler *p) {
    Node *left = assignment(p);
    while (take(p,",")) {
        Node *n = node(p,ND_COMMA,left->token); n->left = left; n->right = assignment(p); n->type = n->right->type; left = n;
    }
    return left;
}

static void allocate_local(Compiler *p, Symbol *s) {
    Type *t = s->type;
    if (!t->size || t->kind == TY_VOID || t->kind == TY_FUNCTION) { error(p,"variable has incomplete object type"); return; }
    if (t->size > (1u<<26) || p->current->stack_size > (1u<<26)-t->size) { error(p,"stack frame exceeds 64 MiB"); return; }
    p->current->stack_size = aligned(p->current->stack_size+(t->kind == TY_STRUCT ? aligned(t->size,8) : t->size),t->alignment);
    s->offset = p->current->stack_size;
}
static void initialize_object(Compiler *, Symbol *, Type *, size_t, Node **, int);
static void global_initializer(Compiler *, Symbol *);
static Node *local_declaration(Compiler *p) {
    int td = 0, st = 0, ex = 0;
    Type *base = declaration_specifiers(p,&td,&st,&ex);
    Node *block = node(p,ND_BLOCK,p->token), head = {0}, *tail = &head;
    if (take(p,";")) return block;
    do {
        Token *name = NULL;
        Type *type = declarator(p,base,&name,0);
        Symbol *s, *old;
        if (!name || p->c->failed) break;
        old = lookup(p,name);
        if (old && old->depth == p->depth) { error(p,"duplicate declaration in the same scope"); break; }
        s = new_symbol(p,name,type,!(st || ex || type->kind == TY_FUNCTION));
        if (!s) break;
        s->is_typedef = td;
        if (st || ex || type->kind == TY_FUNCTION) {
            if (st) {
                ncc_buffer label;
                ncc_buffer_init(&label,p->c->platform);
                tool_append(p->c,&label,".LS",3); tool_number(p->c,&label,p->label++);
                s->assembly_name = tool_string(p->c,(char *)label.data,label.size);
                ncc_buffer_destroy(&label); s->internal = s->defined = 1;
            } else s->external = 1;
            if (take(p,"=")) {
                if (!st) { error(p,"block-scope extern declarations cannot have initializers"); break; }
                global_initializer(p,s);
            }
            continue;
        }
        if (take(p,"=")) {
            if (td) { error(p,"a typedef cannot have an initializer"); break; }
            if (type->kind == TY_ARRAY || type->kind == TY_STRUCT) {
                Node *zero = node(p,ND_ZERO,name); zero->symbol = s; tail = tail->next = zero;
            }
            initialize_object(p,s,type,0,&tail,0);
        }
        if (!td) allocate_local(p,s);
    } while (!p->c->failed && take(p,","));
    expect(p,";"); block->body = head.next; return block;
}
static Node *compound(Compiler *p) {
    Node *block = node(p,ND_BLOCK,p->token), head = {0}, *tail = &head;
    Symbol *saved = p->symbols;
    Tag *tags = p->tags;
    ++p->depth;
    expect(p,"{");
    while (!p->c->failed && !take(p,"}")) {
        Node *n;
        if (!p->token->kind) { error(p,"expected '}' before end of input"); break; }
        n = type_start(p) ? local_declaration(p) : statement(p);
        if (!n) break;
        tail = tail->next = n;
    }
    --p->depth; p->symbols = saved; p->tags = tags; block->body = head.next;
    return block;
}
static JumpLabel *jump_label(Compiler *p, Token *name) {
    for (JumpLabel *l = p->jump_labels; l; l = l->next)
        if (l->name->length == name->length && !ncc_memcmp(l->name->text,name->text,name->length)) return l;
    JumpLabel *l = tool_alloc(p->c,sizeof(*l));
    if (!l) return NULL;
    l->name = name; l->id = p->label++; l->next = p->jump_labels; p->jump_labels = l; return l;
}
static Node *statement(Compiler *p) {
    Token *at = p->token;
    Node *n;
    if (token_is(at,"{")) return compound(p);
    if (take(p,"return")) {
        n = node(p,ND_RETURN,at);
        if (!take(p,";")) { n->left = expression(p); expect(p,";"); }
        if (p->current->symbol->type->base->kind != TY_VOID && !n->left) error(p,"non-void function must return a value");
        if (p->current->symbol->type->base->kind == TY_VOID && n->left) error(p,"void function cannot return a value");
        return n;
    }
    if (take(p,"if")) {
        n = node(p,ND_IF,at); expect(p,"("); n->condition = expression(p); expect(p,")"); n->then = statement(p);
        if (take(p,"else")) n->otherwise = statement(p);
        return n;
    }
    if (take(p,"while")) {
        n = node(p,ND_FOR,at); expect(p,"("); n->condition = expression(p); expect(p,")"); ++p->loops; ++p->breakable; n->body = statement(p); --p->loops; --p->breakable; return n;
    }
    if (take(p,"for")) {
        Symbol *saved = p->symbols;
        n = node(p,ND_FOR,at); expect(p,"("); ++p->depth;
        if (type_start(p)) n->init = local_declaration(p);
        else if (!take(p,";")) { n->init = node(p,ND_EXPR,at); n->init->left = expression(p); expect(p,";"); }
        if (!take(p,";")) { n->condition = expression(p); expect(p,";"); }
        if (!take(p,")")) { n->step = expression(p); expect(p,")"); }
        ++p->loops; ++p->breakable; n->body = statement(p); --p->loops; --p->breakable;
        --p->depth; p->symbols = saved;
        return n;
    }
    if (take(p,"do")) {
        n = node(p,ND_DO,at); ++p->loops; ++p->breakable; n->body = statement(p); --p->loops; --p->breakable;
        expect(p,"while"); expect(p,"("); n->condition = expression(p); expect(p,")"); expect(p,";"); return n;
    }
    if (take(p,"break") || take(p,"continue")) {
        if (token_is(at,"break") ? !p->breakable : !p->loops) error(p,"break/continue is not valid in this context");
        n = node(p,token_is(at,"break") ? ND_BREAK : ND_CONTINUE,at); expect(p,";"); return n;
    }
    if (take(p,"switch")) {
        Node *saved = p->switch_current;
        n = node(p,ND_SWITCH,at); expect(p,"("); n->condition = expression(p); expect(p,")");
        if (!integer_type(n->condition->type)) error(p,"switch requires an integer expression");
        p->switch_current = n; ++p->breakable; n->body = statement(p); --p->breakable; p->switch_current = saved;
        return n;
    }
    if (take(p,"case") || take(p,"default")) {
        int is_default = token_is(at,"default");
        n = node(p,ND_CASE,at); n->member_offset = p->label++;
        if (!p->switch_current) { error(p,"case/default requires an enclosing switch"); return n; }
        if (!is_default) {
            Node *value = conditional_expression(p);
            if (!constant(value,&n->value)) error(p,"case label requires an integer constant expression");
            n->value = normalized_value(n->value,p->switch_current->condition->type);
            for (Node *old = p->switch_current->cases; old; old = old->case_next)
                if (old->value == n->value) error(p,"duplicate case value");
            n->case_next = p->switch_current->cases; p->switch_current->cases = n;
        } else {
            if (p->switch_current->otherwise) error(p,"duplicate default label");
            p->switch_current->otherwise = n;
        }
        expect(p,":"); n->body = statement(p); return n;
    }
    if (take(p,"goto")) {
        n = node(p,ND_GOTO,at);
        if (p->token->kind != TK_IDENT) { error(p,"goto requires a label name"); return n; }
        JumpLabel *l = jump_label(p,p->token);
        n->member_offset = l->id; p->token = p->token->next; expect(p,";"); return n;
    }
    if (at->kind == TK_IDENT && token_is(at->next,":")) {
        JumpLabel *l = jump_label(p,at);
        n = node(p,ND_LABEL,at);
        if (l->defined) error(p,"duplicate statement label");
        l->defined = 1; n->member_offset = l->id;
        p->token = at->next->next; n->body = statement(p); return n;
    }
    if (take(p,";")) return node(p,ND_EMPTY,at);
    n = node(p,ND_EXPR,at); n->left = expression(p); expect(p,";"); return n;
}
static int initializer_address(Node *n, Symbol **target, uint64_t *addend) {
    uint64_t value;
    if (n->kind == ND_CAST) return initializer_address(n->left,target,addend);
    if (n->kind == ND_ADDRESS) {
        n = n->left;
        while (n->kind == ND_MEMBER) { *addend += n->member_offset; n = n->left; }
        if (n->kind == ND_DEREF) return initializer_address(n->left,target,addend);
        if (n->kind == ND_VARIABLE && !n->symbol->local) { *target = n->symbol; return 1; }
        return 0;
    }
    if (n->kind == ND_VARIABLE && !n->symbol->local && (n->type->kind == TY_ARRAY || n->type->kind == TY_FUNCTION)) { *target = n->symbol; return 1; }
    if (n->kind == ND_BINARY && (tool_equal(n->op,"+") || tool_equal(n->op,"-")) && constant(n->right,&value)) {
        *addend += tool_equal(n->op,"+") ? value : 0-value;
        return initializer_address(n->left,target,addend);
    }
    return 0;
}
static void initializer_store(Compiler *p, Symbol *s, Type *type, size_t offset, Node *value, Node **tail, int global) {
    if (global) {
        uint64_t integer;
        if (offset > (1u<<26) || type->size > (1u<<26)-offset) { error(p,"initializer exceeds 64 MiB"); return; }
        if (s->initial.size < offset+type->size) tool_zeros(p->c,&s->initial,offset+type->size-s->initial.size);
        if (constant(value,&integer)) elf_set(s->initial.data+offset,normalized_value(integer,type),(unsigned)type->size);
        else {
            Symbol *target = NULL;
            uint64_t addend = 0;
            if (type->size != 8 || !initializer_address(value,&target,&addend)) { error(p,"initializer is not a supported constant expression"); return; }
            InitialReloc *r = tool_alloc(p->c,sizeof(*r));
            r->offset = offset; r->target = target; r->addend = addend; r->next = s->relocations; s->relocations = r;
        }
    } else {
        Node *destination = node(p,ND_MEMBER,s->name), *assign = node(p,ND_ASSIGN,s->name), *expr = node(p,ND_EXPR,s->name);
        destination->left = variable_node(p,s,s->name); destination->member_offset = offset; destination->type = type; destination->value = 1;
        assign->left = destination; assign->right = value; assign->type = type; expr->left = assign;
        *tail = (*tail)->next = expr;
    }
}
static void initialize_object(Compiler *p, Symbol *s, Type *type, size_t offset, Node **tail, int global) {
    if (type->kind == TY_ARRAY && type->base->size == 1 && p->token->kind == TK_STRING) {
        Node *str = primary(p);
        size_t length = str->symbol->initial.size;
        if (!type->length) { type->length = length; type->size = length; }
        if (length > type->length && length != type->length+1) { error(p,"string initializer is too long"); return; }
        if (length > type->length) length = type->length;
        for (size_t i = 0; i < length; ++i) initializer_store(p,s,type->base,offset+i,number_node(p,str->symbol->initial.data[i],p->t_int,s->name),tail,global);
        return;
    }
    if (type->kind == TY_ARRAY || type->kind == TY_STRUCT) {
        size_t index = 0, maximum = 0;
        Member *member = type->members;
        if (!take(p,"{")) {
            if (type->kind == TY_STRUCT && !global) { initializer_store(p,s,type,offset,assignment(p),tail,0); return; }
            error(p,"aggregate initializer requires braces"); return;
        }
        while (!p->c->failed && !take(p,"}")) {
            Type *element;
            size_t element_offset;
            if (type->kind == TY_ARRAY) {
                if (take(p,"[")) {
                    uint64_t value;
                    Node *n = conditional_expression(p);
                    if (!constant(n,&value) || value > (1u<<26)) { error(p,"array designator requires a bounded integer constant"); return; }
                    index = (size_t)value; expect(p,"]"); expect(p,"=");
                }
                if (type->length && index >= type->length) { error(p,"array initializer index is out of bounds"); return; }
                element = type->base;
                if (element->size && index > (SIZE_MAX-offset)/element->size) { error(p,"array initializer offset overflow"); return; }
                element_offset = offset+index*element->size;
                ++index; if (index > maximum) maximum = index;
            } else {
                if (take(p,".")) {
                    for (member = type->members; member; member = member->next)
                        if (member->name->length == p->token->length && !ncc_memcmp(member->name->text,p->token->text,p->token->length)) break;
                    if (!member) { error(p,"unknown member in designated initializer"); return; }
                    p->token = p->token->next; expect(p,"=");
                }
                if (!member) { error(p,"too many structure initializer elements"); return; }
                element = member->type; element_offset = offset+member->offset;
                member = type->is_union ? NULL : member->next;
            }
            initialize_object(p,s,element,element_offset,tail,global);
            if (take(p,"}")) break;
            expect(p,",");
        }
        if (type->kind == TY_ARRAY && !type->length) {
            type->length = maximum;
            if (type->base->size && maximum > SIZE_MAX/type->base->size) { error(p,"array size overflow"); return; }
            type->size = maximum*type->base->size;
        }
        return;
    }
    int braces = take(p,"{");
    Node *value = assignment(p);
    initializer_store(p,s,type,offset,value,tail,global);
    if (braces) { take(p,","); expect(p,"}"); }
}
static void global_initializer(Compiler *p, Symbol *s) {
    Node dummy = {0}, *tail = &dummy;
    initialize_object(p,s,s->type,0,&tail,1);
    if (s->initial.size < s->type->size) tool_zeros(p->c,&s->initial,s->type->size-s->initial.size);
}
static void translation_unit(Compiler *p) {
    while (p->token->kind && !p->c->failed) {
        int td = 0, st = 0, ex = 0;
        Type *base = declaration_specifiers(p,&td,&st,&ex);
        if (take(p,";")) continue;
        do {
            Token *name = NULL;
            Type *type = declarator(p,base,&name,0);
            Symbol *s;
            if (!name || p->c->failed) return;
            s = lookup(p,name);
            if (s && (s->is_typedef || td || !compatible(s->type,type))) { error(p,"conflicting global declarations"); return; }
            if (!s) s = new_symbol(p,name,type,0);
            if (!s) return;
            s->type = type; s->internal = st; s->external = ex; s->is_typedef = td;
            if (type->kind == TY_FUNCTION && token_is(p->token,"{")) {
                Function *f;
                Symbol *saved = p->symbols;
                size_t count = 0, index = 0;
                if (s->defined || td) { error(p,"function is already defined or declared as a typedef"); return; }
                s->defined = 1;
                f = tool_alloc(p->c,sizeof(*f)); if (!f) return;
                f->stack_size = f->argument_spill = 48;
                f->symbol = s; f->next = p->functions; p->functions = f; p->current = f;
                for (Parameter *q = type->parameters; q; q = q->next) ++count;
                f->parameters = tool_alloc(p->c,(count+1)*sizeof(*f->parameters)); f->parameter_count = count;
                ++p->depth;
                for (Parameter *q = type->parameters; q; q = q->next) {
                    Symbol *param;
                    if (!q->name) { error(p,"function definition requires parameter names"); break; }
                    param = new_symbol(p,q->name,q->type,1);
                    if (!param) return;
                    allocate_local(p,param); f->parameters[index++] = param;
                }
                p->jump_labels = NULL;
                f->body = compound(p);
                for (JumpLabel *l = p->jump_labels; l; l = l->next) if (!l->defined) error(p,"goto refers to an undefined label");
                --p->depth; p->symbols = saved; p->current = NULL;
                break;
            }
            if (take(p,"=")) {
                if (td || s->has_initializer || type->kind == TY_FUNCTION) { error(p,"invalid or duplicate global initializer"); return; }
                global_initializer(p,s); s->defined = s->has_initializer = 1;
            } else if (!td && !ex && type->kind != TY_FUNCTION) s->defined = 1;
            if (!take(p,",")) { expect(p,";"); break; }
        } while (!p->c->failed);
    }
}

static void output(Compiler *p, const char *s) { tool_append(p->c,p->output,s,ncc_strlen(s)); }
static void output_number(Compiler *p, uint64_t v) { tool_number(p->c,p->output,v); }
static void numbered(Compiler *p, const char *before, uint64_t v, const char *after) { output(p,before); output_number(p,v); output(p,after); }
static void label(Compiler *p, unsigned id) { numbered(p,".L",id,":\n"); }
static void jump(Compiler *p, const char *op, unsigned id) { output(p,op); numbered(p," .L",id,"\n"); }
static void push(Compiler *p) { output(p,"push rax\n"); ++p->stack_depth; }
static void pop(Compiler *p, const char *reg) { output(p,"pop "); output(p,reg); output(p,"\n"); --p->stack_depth; }
static void normalize(Compiler *p, Type *t) {
    if (t->kind != TY_INT) return;
    if (t->is_bool) { output(p,"cmp rax, 0\nsetne al\nmovzx eax, al\n"); return; }
    if (t->size == 1) output(p,t->is_unsigned ? "movzx eax, al\n" : "movsx rax, al\n");
    if (t->size == 2) output(p,t->is_unsigned ? "movzx eax, ax\n" : "movsx rax, ax\n");
    if (t->size == 4) output(p,t->is_unsigned ? "mov eax, eax\n" : "movsxd rax, eax\n");
}
static void load(Compiler *p, Type *t) {
    if (t->kind == TY_ARRAY || t->kind == TY_FUNCTION) return;
    if (t->kind == TY_STRUCT) return;
    if (t->kind == TY_VOID) { error(p,"aggregate and void values cannot be loaded here"); return; }
    if (t->size == 1) output(p,t->is_unsigned ? "movzx eax, byte [rax]\n" : "movsx rax, byte [rax]\n");
    else if (t->size == 2) output(p,t->is_unsigned ? "movzx eax, word [rax]\n" : "movsx rax, word [rax]\n");
    else if (t->size == 4) { output(p,"mov eax, dword [rax]\n"); normalize(p,t); }
    else output(p,"mov rax, qword [rax]\n");
}
static void store(Compiler *p, Type *t) {
    if (t->kind == TY_ARRAY || t->kind == TY_FUNCTION || t->kind == TY_STRUCT || t->kind == TY_VOID) { error(p,"assignment requires a scalar modifiable lvalue"); return; }
    output(p,t->size == 1 ? "mov byte [rdi], al\n" : t->size == 2 ? "mov word [rdi], ax\n" : t->size == 4 ? "mov dword [rdi], eax\n" : "mov qword [rdi], rax\n");
}
static void address(Compiler *p, Node *n) {
    location(p,n->token);
    if (n->kind == ND_LITERAL) { for (Node *s = n->body; s; s = s->next) generate_statement(p,s); }
    if (n->kind == ND_VARIABLE || n->kind == ND_LITERAL) {
        if (n->symbol->local) numbered(p,"lea rax, [rbp-",n->symbol->offset,"]\n");
        else { output(p,"lea rax, [rip+"); output(p,n->symbol->assembly_name); output(p,"]\n"); }
    } else if (n->kind == ND_DEREF) generate_expression(p,n->left);
    else if (n->kind == ND_MEMBER) { if (n->value) address(p,n->left); else generate_expression(p,n->left); numbered(p,"add rax, ",n->member_offset,"\n"); }
    else error(p,"expression is not an lvalue");
}
static void binary_operation(Compiler *p, const char *op, Type *t) {
    int unsign = t->is_unsigned || pointer(t);
    if (tool_equal(op,"+")) output(p,"add rax, rcx\n");
    else if (tool_equal(op,"-")) output(p,"sub rax, rcx\n");
    else if (tool_equal(op,"*")) output(p,"imul rax, rcx\n");
    else if (tool_equal(op,"/") || tool_equal(op,"%")) {
        output(p,unsign ? "xor edx, edx\ndiv rcx\n" : "cqo\nidiv rcx\n");
        if (tool_equal(op,"%")) output(p,"mov rax, rdx\n");
    } else if (tool_equal(op,"&")) output(p,"and rax, rcx\n");
    else if (tool_equal(op,"|")) output(p,"or rax, rcx\n");
    else if (tool_equal(op,"^")) output(p,"xor rax, rcx\n");
    else if (tool_equal(op,"<<") || tool_equal(op,">>")) {
        if (t->size <= 4) output(p,"and ecx, 31\n");
        output(p,tool_equal(op,"<<") ? "shl rax, cl\n" : unsign ? "shr rax, cl\n" : "sar rax, cl\n");
    } else {
        output(p,"cmp rax, rcx\n");
        if (tool_equal(op,"==")) output(p,"sete al\n");
        else if (tool_equal(op,"!=")) output(p,"setne al\n");
        else if (tool_equal(op,"<")) output(p,unsign ? "setb al\n" : "setl al\n");
        else if (tool_equal(op,">")) output(p,unsign ? "seta al\n" : "setg al\n");
        else if (tool_equal(op,"<=")) output(p,unsign ? "setbe al\n" : "setle al\n");
        else if (tool_equal(op,">=")) output(p,unsign ? "setae al\n" : "setge al\n");
        else { error(p,"unknown binary operator"); return; }
        output(p,"movzx eax, al\n");
    }
}
static void pack_return(Compiler *p, Type *type) {
    output(p,"mov rsi, rax\n");
    for (size_t chunk = 0; chunk < 2 && chunk*8 < type->size; ++chunk) {
        const char *reg = chunk ? "rdx" : "rax";
        output(p,"xor "); output(p,reg); output(p,", "); output(p,reg); output(p,"\n");
        for (size_t i = 0; i < 8 && chunk*8+i < type->size; ++i) {
            numbered(p,"movzx r10d, byte [rsi+",chunk*8+i,"]\n");
            if (i) numbered(p,"shl r10, ",i*8,"\n");
            output(p,"or "); output(p,reg); output(p,", r10\n");
        }
    }
}
static void immediate(Compiler *p, uint64_t value) {
    if (p->optimize == 3 && !value) output(p,"xor eax, eax\n");
    else numbered(p,p->optimize == 3 && value <= UINT32_MAX ? "mov eax, " : "mov rax, ",value,"\n");
}
static void generate_expression(Compiler *p, Node *n) {
    uint64_t value;
    if (!n || p->c->failed) return;
    location(p,n->token);
    if (p->optimize && constant(n,&value)) { immediate(p,value); normalize(p,n->type); return; }
    switch (n->kind) {
    case ND_VASTART:
        generate_expression(p,n->left);
        numbered(p,"mov dword [rax], ",p->current->named_gp*8,"\nmov dword [rax+4], 48\n");
        numbered(p,"lea rcx, [rbp+",p->current->named_stack,"]\nmov qword [rax+8], rcx\n");
        numbered(p,"lea rcx, [rbp-",p->current->argument_spill,"]\nmov qword [rax+16], rcx\n"); return;
    case ND_VAARG: {
        unsigned overflow = p->label++, done = p->label++;
        size_t size = aligned(n->type->size,8), slots = size/8;
        generate_expression(p,n->left); output(p,"mov rdi, rax\n");
        if (n->type->kind != TY_STRUCT || n->type->size <= 16) {
            output(p,"mov ecx, dword [rdi]\n"); numbered(p,"cmp ecx, ",48-slots*8,"\n"); jump(p,"ja",overflow);
            output(p,"mov rax, qword [rdi+16]\nadd rax, rcx\n"); numbered(p,"add dword [rdi], ",slots*8,"\n"); jump(p,"jmp",done);
        }
        label(p,overflow); output(p,"mov rax, qword [rdi+8]\n"); numbered(p,"add qword [rdi+8], ",size,"\n");
        label(p,done); load(p,n->type); return;
    }
    case ND_NUMBER: immediate(p,n->value); normalize(p,n->type); return;
    case ND_VARIABLE: case ND_MEMBER: case ND_LITERAL: address(p,n); load(p,n->type); return;
    case ND_ADDRESS: address(p,n->left); return;
    case ND_DEREF: generate_expression(p,n->left); load(p,n->type); return;
    case ND_CAST: generate_expression(p,n->left); normalize(p,n->type); return;
    case ND_NEGATE: case ND_NOT: case ND_BITNOT:
        generate_expression(p,n->left);
        if (n->kind == ND_NOT) output(p,"cmp rax, 0\nsete al\nmovzx eax, al\n");
        else output(p,n->kind == ND_NEGATE ? "neg rax\n" : "not rax\n");
        normalize(p,n->type); return;
    case ND_COMMA: generate_expression(p,n->left); generate_expression(p,n->right); return;
    case ND_ASSIGN:
        address(p,n->left); push(p);
        if (n->type->kind == TY_STRUCT && !n->op) {
            generate_expression(p,n->right); output(p,"mov rsi, rax\n"); pop(p,"rdi");
            output(p,"mov rax, rdi\n"); numbered(p,"mov rcx, ",n->type->size,"\nrep movsb\n"); return;
        }
        if (n->op) { load(p,n->left->type); push(p); }
        generate_expression(p,n->right);
        if (n->op) {
            output(p,"mov rcx, rax\n"); pop(p,"rax");
            if (pointer(n->type) && (tool_equal(n->op,"+") || tool_equal(n->op,"-"))) { numbered(p,"mov rdx, ",n->type->base->size,"\nimul rcx, rdx\n"); }
            binary_operation(p,n->op,n->type);
        }
        normalize(p,n->type); pop(p,"rdi"); store(p,n->type); return;
    case ND_PREINC: case ND_POSTINC:
        if (n->type->is_const) { error(p,"increment/decrement of a const-qualified object"); return; }
        address(p,n->left); output(p,"mov rdi, rax\n"); load(p,n->type);
        if (n->kind == ND_POSTINC) output(p,"mov rdx, rax\n");
        numbered(p,n->value == 1 ? "add rax, " : "sub rax, ",pointer(n->type) ? n->type->base->size : 1,"\n");
        normalize(p,n->type); store(p,n->type);
        if (n->kind == ND_POSTINC) output(p,"mov rax, rdx\n");
        return;
    case ND_BINARY:
        if (tool_equal(n->op,"&&") || tool_equal(n->op,"||")) {
            unsigned short_label = p->label++, done = p->label++;
            generate_expression(p,n->left); output(p,"cmp rax, 0\n"); jump(p,tool_equal(n->op,"&&") ? "je" : "jne",short_label);
            generate_expression(p,n->right); output(p,"cmp rax, 0\nsetne al\nmovzx eax, al\n"); jump(p,"jmp",done);
            label(p,short_label); numbered(p,"mov eax, ",tool_equal(n->op,"||") ? 1 : 0,"\n"); label(p,done); return;
        }
        generate_expression(p,n->left); push(p); generate_expression(p,n->right); output(p,"mov rcx, rax\n"); pop(p,"rax");
        binary_operation(p,n->op,n->left->type); normalize(p,n->type); return;
    case ND_CONDITIONAL: {
        unsigned no = p->label++, done = p->label++;
        if (p->optimize > 1 && constant(n->condition,&value)) { generate_expression(p,value ? n->then : n->otherwise); return; }
        generate_expression(p,n->condition); output(p,"cmp rax, 0\n"); jump(p,"je",no);
        generate_expression(p,n->then); jump(p,"jmp",done); label(p,no); generate_expression(p,n->otherwise); label(p,done); return;
    }
    case ND_CALL: {
        static const char *const registers[] = {"rdi","rsi","rdx","rcx","r8","r9"};
        size_t reg = n->type->kind == TY_STRUCT && n->type->size > 16 ? 1 : 0, stack = 0, padding;
        int direct = n->left->kind == ND_VARIABLE && n->left->symbol->type->kind == TY_FUNCTION;
        if (!direct) { generate_expression(p,n->left); numbered(p,"mov qword [rbp-",n->callee_offset,"], rax\n"); }
        for (Node *arg = n->arguments; arg; arg = arg->next) {
            generate_expression(p,arg);
            if (arg->type->kind == TY_STRUCT) {
                output(p,"mov rsi, rax\n"); numbered(p,"lea rdi, [rbp-",arg->argument_offset,"]\n");
                numbered(p,"mov rcx, ",arg->type->size,"\nrep movsb\n");
                arg->argument_slots = aligned(arg->type->size,8)/8;
            } else {
                numbered(p,"mov qword [rbp-",arg->argument_offset,"], rax\n"); arg->argument_slots = 1;
            }
            if ((arg->type->kind != TY_STRUCT || arg->type->size <= 16) && reg+arg->argument_slots <= 6) {
                arg->argument_register = (int)reg; reg += arg->argument_slots;
            } else { arg->argument_register = -1; arg->argument_stack = stack; stack += arg->argument_slots*8; }
        }
        padding = ((p->stack_depth+stack/8)&1)*8;
        if (stack+padding) { numbered(p,"sub rsp, ",stack+padding,"\n"); p->stack_depth += (stack+padding)/8; }
        for (Node *arg = n->arguments; arg; arg = arg->next) if (arg->argument_register < 0) {
            numbered(p,"lea rsi, [rbp-",arg->argument_offset,"]\n"); numbered(p,"lea rdi, [rsp+",arg->argument_stack,"]\n");
            numbered(p,"mov rcx, ",arg->type->kind == TY_STRUCT ? arg->type->size : 8,"\nrep movsb\n");
        }
        for (Node *arg = n->arguments; arg; arg = arg->next) if (arg->argument_register >= 0) {
            for (size_t chunk = 0; chunk < arg->argument_slots; ++chunk) {
                output(p,"mov "); output(p,registers[(size_t)arg->argument_register+chunk]);
                numbered(p,", qword [rbp-",arg->argument_offset-chunk*8,"]\n");
            }
        }
        if (n->type->kind == TY_STRUCT && n->type->size > 16) numbered(p,"lea rdi, [rbp-",n->member_offset,"]\n");
        output(p,"mov eax, 0\n");
        if (direct) { output(p,"call "); output(p,n->left->symbol->assembly_name); output(p,"\n"); }
        else numbered(p,"call qword [rbp-",n->callee_offset,"]\n");
        if (stack+padding) { numbered(p,"add rsp, ",stack+padding,"\n"); p->stack_depth -= (stack+padding)/8; }
        if (n->type->kind == TY_STRUCT) {
            if (n->type->size <= 16) {
                numbered(p,"mov qword [rbp-",n->member_offset,"], rax\n");
                if (n->type->size > 8) numbered(p,"mov qword [rbp-",n->member_offset-8,"], rdx\n");
            }
            numbered(p,"lea rax, [rbp-",n->member_offset,"]\n");
        } else normalize(p,n->type);
        return;
    }
    default: error(p,"invalid expression node"); return;
    }
}
static void generate_statement(Compiler *p, Node *n) {
    uint64_t value;
    if (!n || p->c->failed) return;
    location(p,n->token);
    switch (n->kind) {
    case ND_EMPTY: return;
    case ND_ZERO:
        numbered(p,"lea rdi, [rbp-",n->symbol->offset,"]\n");
        numbered(p,"mov rcx, ",n->symbol->type->size,"\nxor eax, eax\nrep stosb\n"); return;
    case ND_BLOCK:
        for (Node *s = n->body; s; s = s->next) {
            generate_statement(p,s);
            if (p->optimize > 1 && (s->kind == ND_RETURN || s->kind == ND_BREAK || s->kind == ND_CONTINUE || s->kind == ND_GOTO)) {
                while (s->next && s->next->kind != ND_LABEL && s->next->kind != ND_CASE) s = s->next;
            }
        }
        return;
    case ND_EXPR: generate_expression(p,n->left); return;
    case ND_RETURN:
        if (n->left) {
            Type *result = p->current->symbol->type->base;
            generate_expression(p,n->left);
            if (result->kind == TY_STRUCT && result->size <= 16) pack_return(p,result);
            else if (result->kind == TY_STRUCT) {
                output(p,"mov rsi, rax\n"); numbered(p,"mov rdi, qword [rbp-",p->current->argument_spill,"]\n");
                numbered(p,"mov rcx, ",result->size,"\nrep movsb\n");
                numbered(p,"mov rax, qword [rbp-",p->current->argument_spill,"]\n");
            } else normalize(p,result);
        }
        jump(p,"jmp",p->return_label); return;
    case ND_IF: {
        unsigned no = p->label++, done = p->label++;
        if (p->optimize > 1 && constant(n->condition,&value)) { generate_statement(p,value ? n->then : n->otherwise); return; }
        generate_expression(p,n->condition); output(p,"cmp rax, 0\n"); jump(p,"je",no);
        generate_statement(p,n->then); if (n->otherwise) jump(p,"jmp",done);
        label(p,no); if (n->otherwise) { generate_statement(p,n->otherwise); label(p,done); }
        return;
    }
    case ND_FOR: case ND_DO: {
        unsigned begin = p->label++, end = p->label++, step = p->label++;
        unsigned saved_break = p->break_label, saved_continue = p->continue_label;
        p->break_label = end; p->continue_label = step;
        generate_statement(p,n->init);
        if (p->optimize > 1 && n->kind == ND_FOR && n->condition && constant(n->condition,&value) && !value) { p->break_label = saved_break; p->continue_label = saved_continue; return; }
        label(p,begin);
        if (n->kind == ND_FOR && n->condition) { generate_expression(p,n->condition); output(p,"cmp rax, 0\n"); jump(p,"je",end); }
        generate_statement(p,n->body); label(p,step); generate_expression(p,n->step);
        if (n->kind == ND_DO) { generate_expression(p,n->condition); output(p,"cmp rax, 0\n"); jump(p,"jne",begin); }
        else jump(p,"jmp",begin);
        label(p,end); p->break_label = saved_break; p->continue_label = saved_continue; return;
    }
    case ND_SWITCH: {
        unsigned end = p->label++, saved = p->break_label;
        p->break_label = end;
        generate_expression(p,n->condition);
        for (Node *s = n->cases; s; s = s->case_next) {
            numbered(p,"mov rcx, ",s->value,"\ncmp rax, rcx\n"); jump(p,"je",(unsigned)s->member_offset);
        }
        jump(p,"jmp",n->otherwise ? (unsigned)n->otherwise->member_offset : end);
        generate_statement(p,n->body); label(p,end); p->break_label = saved; return;
    }
    case ND_LABEL: case ND_CASE: label(p,(unsigned)n->member_offset); generate_statement(p,n->body); return;
    case ND_GOTO: jump(p,"jmp",(unsigned)n->member_offset); return;
    case ND_BREAK: jump(p,"jmp",p->break_label); return;
    case ND_CONTINUE: jump(p,"jmp",p->continue_label); return;
    default: error(p,"invalid statement node"); return;
    }
}
static void generate_unit(Compiler *p) {
    for (Symbol *s = p->globals; s && !p->c->failed; s = s->global_next) {
        if (s->is_typedef || s->type->kind == TY_FUNCTION || !s->defined) continue;
        if (!s->type->size) { error(p,"global has an incomplete type"); break; }
        output(p,s->initial.size || s->target ? ".data\n" : ".bss\n");
        numbered(p,".align ",s->type->alignment,"\n");
        if (!s->internal) { output(p,".global "); output(p,s->assembly_name); output(p,"\n"); }
        output(p,s->assembly_name); output(p,":\n");
        if (!s->initial.size) numbered(p,".zero ",s->type->size,"\n");
        else for (size_t i = 0; i < s->initial.size;) {
            InitialReloc *relocation = NULL;
            for (InitialReloc *r = s->relocations; r; r = r->next) if (r->offset == i) { relocation = r; break; }
            if (relocation) {
                output(p,".quad "); output(p,relocation->target->assembly_name);
                numbered(p,"+",relocation->addend,"\n"); i += 8;
            } else numbered(p,".byte ",s->initial.data[i++],"\n");
        }
    }
    for (Function *f = p->functions; f && !p->c->failed; f = f->next) {
        static const char *const registers[] = {"rdi","rsi","rdx","rcx","r8","r9"};
        p->current = f; p->stack_depth = 0; p->return_label = p->label++;
        output(p,".text\n");
        if (!f->symbol->internal) { output(p,".global "); output(p,f->symbol->assembly_name); output(p,"\n"); }
        output(p,f->symbol->assembly_name); output(p,":\npush rbp\nmov rbp, rsp\n");
        if (f->stack_size) numbered(p,"sub rsp, ",aligned(f->stack_size,16),"\n");
        for (size_t i = 0; i < 6; ++i) {
            numbered(p,"mov qword [rbp-",f->argument_spill-i*8,"], "); output(p,registers[i]); output(p,"\n");
        }
        size_t reg = f->symbol->type->base->kind == TY_STRUCT && f->symbol->type->base->size > 16 ? 1 : 0, stack = 16;
        for (size_t i = 0; i < f->parameter_count; ++i) {
            Symbol *param = f->parameters[i];
            size_t slots = param->type->kind == TY_STRUCT ? aligned(param->type->size,8)/8 : 1;
            if ((param->type->kind != TY_STRUCT || param->type->size <= 16) && reg+slots <= 6) {
                for (size_t chunk = 0; chunk < slots; ++chunk) {
                    numbered(p,"mov rax, qword [rbp-",f->argument_spill-(reg+chunk)*8,"]\n");
                    if (param->type->kind == TY_STRUCT) numbered(p,"mov qword [rbp-",param->offset-chunk*8,"], rax\n");
                    else { numbered(p,"lea rdi, [rbp-",param->offset,"]\n"); store(p,param->type); }
                }
                reg += slots;
            } else {
                if (param->type->kind == TY_STRUCT) {
                    numbered(p,"lea rsi, [rbp+",stack,"]\n"); numbered(p,"lea rdi, [rbp-",param->offset,"]\n");
                    numbered(p,"mov rcx, ",param->type->size,"\nrep movsb\n");
                } else { numbered(p,"mov rax, qword [rbp+",stack,"]\n"); numbered(p,"lea rdi, [rbp-",param->offset,"]\n"); store(p,param->type); }
                stack += slots*8;
            }
        }
        f->named_gp = reg; f->named_stack = stack;
        generate_statement(p,f->body);
        if (tool_equal(f->symbol->assembly_name,"main")) output(p,"mov eax, 0\n");
        label(p,p->return_label); output(p,"leave\nret\n");
        if (p->stack_depth) error(p,"internal error: unbalanced expression stack");
    }
}
int ncc_compile(Context *c, const char *source, const char *path, int optimization, ncc_buffer *out) {
    Compiler p;
    int result;
    ncc_memset(&p,0,sizeof(p)); p.c = c; p.output = out; p.optimize = optimization;
    p.t_void = type_new(&p,TY_VOID,0,1); p.t_char = type_new(&p,TY_INT,1,1); p.t_short = type_new(&p,TY_INT,2,2); p.t_int = type_new(&p,TY_INT,4,4); p.t_long = type_new(&p,TY_INT,8,8);
    if (c->failed) return 0;
    p.token = front_preprocess(c,source,path);
    if (!p.token || c->failed) return 0;
    translation_unit(&p);
    if (!c->failed) generate_unit(&p);
    result = !c->failed;
    for (Symbol *s = p.globals; s; s = s->global_next) ncc_buffer_destroy(&s->initial);
    return result;
}
