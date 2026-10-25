#include "front.h"

typedef struct Macro Macro;
struct Macro {
    Macro *next;
    const char *name;
    Token *body;
    const char **parameters;
    size_t count;
    int function, variadic, busy;
};
typedef struct { Context *c; Macro *macros; unsigned depth; } Preprocessor;
typedef struct Conditional Conditional;
struct Conditional { Conditional *next; int parent, active, taken, otherwise; };
static Token *expand(Preprocessor *, Token *, unsigned);
static Token *process(Preprocessor *, const char *, const char *);
static Token *clone(Context *c, Token *t) {
    Token *n = tool_alloc(c,sizeof(*n));
    if (n) { *n = *t; n->next = NULL; }
    return n;
}
static void append_token(Context *c, Token **tail, Token *t) {
    Token *n = clone(c,t);
    if (n) *tail = (*tail)->next = n;
}
static Macro *find_macro(Preprocessor *p, Token *t) {
    for (Macro *m = p->macros; m; m = m->next) if (token_is(t,m->name)) return m;
    return NULL;
}
static Token *synthetic(Context *c, Token *at, const char *s, int kind) {
    Token *t = clone(c,at);
    if (t) { t->text = s; t->length = ncc_strlen(s); t->kind = kind; }
    return t;
}
static int parameter(Macro *m, Token *t) {
    for (size_t i = 0; i < m->count; ++i) if (token_is(t,m->parameters[i])) return (int)i;
    return -1;
}
static Token *stringize(Preprocessor *p, Token *at, Token *argument) {
    ncc_buffer b;
    char *s;
    ncc_buffer_init(&b,p->c->platform);
    tool_byte(p->c,&b,'"');
    for (Token *t = argument; t; t = t->next) {
        if (t != argument && t->space) tool_byte(p->c,&b,' ');
        for (size_t i = 0; i < t->length; ++i) {
            if (t->text[i] == '\\' || t->text[i] == '"') tool_byte(p->c,&b,'\\');
            tool_byte(p->c,&b,(unsigned char)t->text[i]);
        }
    }
    tool_byte(p->c,&b,'"');
    s = tool_string(p->c,(char *)b.data,b.size);
    ncc_buffer_destroy(&b);
    return s ? synthetic(p->c,at,s,TK_STRING) : NULL;
}
static int hidden(Token *t, const char *name) {
    for (Hide *h = t->hides; h; h = h->next) if (tool_equal(h->name,name)) return 1;
    return 0;
}
static void hide_macro(Context *c, Token *t, const char *name) {
    if (hidden(t,name)) return;
    Hide *h = tool_alloc(c,sizeof(*h));
    if (!h) return;
    h->name = name; h->next = t->hides; t->hides = h;
}
static Token *expand(Preprocessor *p, Token *input, unsigned depth) {
    Token head = {0}, *tail = &head;
    Context *c = p->c;
    if (depth > 128) { tool_error(c,"macro expansion nesting exceeds 128"); return NULL; }
    for (Token *t = input; t && t->kind && !c->failed;) {
        Macro *m = t->kind == TK_IDENT ? find_macro(p,t) : NULL;
        Token *after = t->next, *replacement;
        Token **arguments = NULL;
        if (token_is(t,"__LINE__")) {
            ncc_buffer b;
            char *s;
            ncc_buffer_init(&b,c->platform); tool_number(c,&b,t->line);
            s = tool_string(c,(char *)b.data,b.size); ncc_buffer_destroy(&b);
            if (!s) return NULL;
            replacement = synthetic(c,t,s,TK_NUMBER);
            if (!replacement) return NULL;
            append_token(c,&tail,replacement); t = after; continue;
        }
        if (token_is(t,"__FILE__")) {
            Token file = *t;
            file.text = t->path; file.length = ncc_strlen(t->path); file.next = NULL;
            replacement = stringize(p,t,&file);
            if (!replacement) return NULL;
            append_token(c,&tail,replacement); t = after; continue;
        }
        if (!m || hidden(t,m->name) || (m->function && !token_is(after,"("))) {
            append_token(c,&tail,t); t = after; continue;
        }
        if (m->function) {
            Token *scan = after->next;
            size_t index = 0;
            unsigned nesting = 0;
            Token arghead = {0}, *argtail = &arghead;
            arguments = tool_alloc(c,(m->count+1)*sizeof(*arguments));
            if (!arguments) return NULL;
            while (scan && scan->kind && !c->failed) {
                if ((!nesting && token_is(scan,")")) || (!nesting && token_is(scan,",") && !(m->variadic && index+1 == m->count))) {
                    if (index >= m->count && (arghead.next || index || m->count)) { tool_error(c,"too many macro arguments"); return NULL; }
                    if (index < m->count) arguments[index++] = arghead.next;
                    arghead.next = NULL; argtail = &arghead;
                    if (token_is(scan,")")) break;
                } else {
                    if (token_is(scan,"(")) ++nesting;
                    if (token_is(scan,")")) --nesting;
                    append_token(c,&argtail,scan);
                }
                scan = scan->next;
            }
            if (!token_is(scan,")")) { tool_error(c,"unterminated macro invocation"); return NULL; }
            if (index != m->count) { tool_error(c,"incorrect number of macro arguments"); return NULL; }
            after = scan->next;
        }
        {
            Token body = {0}, *bt = &body;
            for (Token *r = m->body; r && !c->failed; r = r->next) {
                int param;
                if (m->function && token_is(r,"#")) {
                    param = parameter(m,r->next);
                    if (param < 0) { tool_error(c,"'#' must precede a macro parameter"); return NULL; }
                    replacement = stringize(p,t,arguments[param]);
                    if (!replacement) return NULL;
                    append_token(c,&bt,replacement); r = r->next; continue;
                }
                param = parameter(m,r);
                if (param >= 0) {
                    Token *arg = arguments[param];
                    if (!token_is(r->next,"##") && !token_is(bt,"##")) arg = expand(p,arg,depth+1);
                    if (!arg && (token_is(r->next,"##") || token_is(bt,"##"))) {
                        Token *empty = synthetic(c,t,"",TK_PUNCT);
                        if (!empty) return NULL;
                        append_token(c,&bt,empty);
                    }
                    for (; arg; arg = arg->next) append_token(c,&bt,arg);
                } else append_token(c,&bt,r);
            }
            {
                Token *prev = &body;
                while (prev->next && !c->failed) {
                    Token *left = prev->next, *hash = left->next;
                    if (!token_is(hash,"##")) { prev = left; continue; }
                    if (!hash->next) { tool_error(c,"'##' cannot end a replacement list"); return NULL; }
                    Token *right = hash->next;
                    ncc_buffer b;
                    char *s;
                    Token *joined;
                    ncc_buffer_init(&b,c->platform);
                    tool_append(c,&b,left->text,left->length); tool_append(c,&b,right->text,right->length);
                    s = tool_string(c,(char *)b.data,b.size); ncc_buffer_destroy(&b);
                    if (!s) return NULL;
                    if (!*s) { prev->next = right->next; continue; }
                    joined = front_lex(c,s,t->path);
                    if (!joined || !joined->kind || (joined->next && joined->next->kind)) { tool_error(c,"token paste does not form one preprocessing token"); return NULL; }
                    joined->next = right->next; joined->line = t->line;
                    prev->next = joined;
                }
            }
            Token *last = NULL;
            for (Token *r = body.next; r; r = r->next) {
                r->line = t->line; r->path = t->path;
                for (Hide *h = t->hides; h; h = h->next) hide_macro(c,r,h->name);
                hide_macro(c,r,m->name);
                last = r;
            }
            if (last) { last->next = after; t = body.next; }
            else t = after;
            continue;
        }
        t = after;
    }
    return head.next;
}

static int precedence(Token *t) {
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
typedef struct { uint64_t value; int unsign; } ConditionValue;
static ConditionValue condition_expression(Preprocessor *, Token **, int, int);
static unsigned condition_escape(Preprocessor *p, const char **cursor, const char *end) {
    const char *s = *cursor;
    unsigned value = (unsigned char)*s++;
    if (value == '\\' && s < end) {
        value = (unsigned char)*s++;
        if (value == 'n') value = 10;
        else if (value == 'r') value = 13;
        else if (value == 't') value = 9;
        else if (value == 'a') value = 7;
        else if (value == 'b') value = 8;
        else if (value == 'f') value = 12;
        else if (value == 'v') value = 11;
        else if (value >= '0' && value <= '7') {
            value -= '0';
            for (unsigned i = 1; i < 3 && s < end && *s >= '0' && *s <= '7'; ++i) value = value*8+(unsigned)(*s++-'0');
        } else if (value == 'x') {
            unsigned count = 0; value = 0;
            while (s < end) {
                unsigned ch = (unsigned char)*s, d = ch >= '0' && ch <= '9' ? ch-'0' : ch >= 'a' && ch <= 'f' ? ch-'a'+10 : ch >= 'A' && ch <= 'F' ? ch-'A'+10 : 99;
                if (d >= 16) break;
                value = value*16+d; ++s; ++count;
            }
            if (!count) tool_error(p->c,"hex escape requires a digit");
        } else if (value != '\\' && value != '\'' && value != '"' && value != '?') tool_error(p->c,"invalid character escape");
    }
    *cursor = s; return value;
}
static ConditionValue condition_atom(Preprocessor *p, Token **cursor, int eval) {
    Token *t = *cursor;
    ConditionValue result = {0,0};
    if (!t) { tool_error(p->c,"expected an expression after #if"); return result; }
    *cursor = t->next;
    if (token_is(t,"(")) {
        result = condition_expression(p,cursor,1,eval);
        if (!token_is(*cursor,")")) tool_error(p->c,"expected ')' in preprocessor expression");
        else *cursor = (*cursor)->next;
    } else if (token_is(t,"!") || token_is(t,"~") || token_is(t,"+") || token_is(t,"-")) {
        result = condition_atom(p,cursor,eval);
        if (token_is(t,"!")) { result.value = !result.value; result.unsign = 0; }
        if (token_is(t,"~")) result.value = ~result.value;
        if (token_is(t,"-")) result.value = 0-result.value;
    } else if (t->kind == TK_CHAR) {
        const char *s = t->text+1, *end = t->text+t->length-1;
        if (s == end) tool_error(p->c,"empty character constant");
        while (s < end && !p->c->failed) result.value = (result.value<<8) | condition_escape(p,&s,end);
    } else if (t->kind == TK_NUMBER) {
        unsigned base = 10, longs = 0;
        size_t i = 0;
        if (t->length > 1 && t->text[0] == '0') { base = 8; i = 1; }
        if (t->length > 2 && t->text[0] == '0' && (t->text[1] == 'x' || t->text[1] == 'X')) { base = 16; i = 2; }
        for (; i < t->length; ++i) {
            unsigned ch = (unsigned char)t->text[i], d = ch >= '0' && ch <= '9' ? ch-'0' : ch >= 'a' && ch <= 'f' ? ch-'a'+10 : ch >= 'A' && ch <= 'F' ? ch-'A'+10 : 99;
            if (d >= base) break;
            if (result.value > (UINT64_MAX-d)/base) { tool_error(p->c,"preprocessor integer exceeds 64 bits"); return result; }
            result.value = result.value*base+d;
        }
        for (; i < t->length; ++i) {
            int ch = t->text[i];
            if ((ch == 'u' || ch == 'U') && !result.unsign) result.unsign = 1;
            else if ((ch == 'l' || ch == 'L') && longs < 2) ++longs;
            else { tool_error(p->c,"invalid integer in preprocessor expression"); return result; }
        }
        if (result.value > INT64_MAX) result.unsign = 1;
    } else if (t->kind != TK_IDENT) tool_error(p->c,"invalid preprocessor expression operand");
    return result;
}
static ConditionValue condition_expression(Preprocessor *p, Token **cursor, int min, int eval) {
    ConditionValue left = condition_atom(p,cursor,eval);
    while (*cursor && !p->c->failed) {
        Token *op = *cursor;
        int prec = precedence(op), unsign;
        uint64_t l = left.value, r;
        ConditionValue right;
        if (prec < min) break;
        *cursor = op->next;
        right = condition_expression(p,cursor,prec+1,eval && !(token_is(op,"&&") && !l) && !(token_is(op,"||") && l));
        r = right.value; unsign = left.unsign || right.unsign;
        if (token_is(op,"+")) l += r;
        else if (token_is(op,"-")) l -= r;
        else if (token_is(op,"*")) l *= r;
        else if (token_is(op,"/") || token_is(op,"%")) {
            if (!r) { if (eval) tool_error(p->c,"division by zero in preprocessor expression"); l = 0; }
            else if (!unsign && l == (1ull<<63) && r == UINT64_MAX) { if (eval) tool_error(p->c,"signed division overflow in preprocessor expression"); l = 0; }
            else if (unsign) l = token_is(op,"/") ? l/r : l%r;
            else l = (uint64_t)(token_is(op,"/") ? (int64_t)l/(int64_t)r : (int64_t)l%(int64_t)r);
        } else if (token_is(op,"<<") || token_is(op,">>")) {
            unsign = left.unsign;
            if (r >= 64) { if (eval) tool_error(p->c,"shift count is out of range in preprocessor expression"); l = 0; }
            else l = token_is(op,"<<") ? l << r : unsign ? l >> r : (uint64_t)((int64_t)l >> r);
        } else if (token_is(op,"&")) l &= r;
        else if (token_is(op,"^")) l ^= r;
        else if (token_is(op,"|")) l |= r;
        else {
            if (token_is(op,"<")) l = unsign ? l < r : (int64_t)l < (int64_t)r;
            else if (token_is(op,">")) l = unsign ? l > r : (int64_t)l > (int64_t)r;
            else if (token_is(op,"<=")) l = unsign ? l <= r : (int64_t)l <= (int64_t)r;
            else if (token_is(op,">=")) l = unsign ? l >= r : (int64_t)l >= (int64_t)r;
            else if (token_is(op,"==")) l = l == r;
            else if (token_is(op,"!=")) l = l != r;
            else if (token_is(op,"&&")) l = l && r;
            else if (token_is(op,"||")) l = l || r;
            unsign = 0;
        }
        left.value = l; left.unsign = unsign;
    }
    if (min == 1 && token_is(*cursor,"?")) {
        ConditionValue yes, no;
        *cursor = (*cursor)->next;
        yes = condition_expression(p,cursor,1,eval && left.value);
        if (!token_is(*cursor,":")) { tool_error(p->c,"expected ':' in conditional expression"); return left; }
        *cursor = (*cursor)->next;
        no = condition_expression(p,cursor,1,eval && !left.value);
        left.value = left.value ? yes.value : no.value; left.unsign = yes.unsign || no.unsign;
    }
    return left;
}
static int condition(Preprocessor *p, Token *tokens) {
    Token head = {0}, *tail = &head, *expanded;
    uint64_t value;
    for (Token *t = tokens; t; t = t->next) {
        if (token_is(t,"defined")) {
            int paren = token_is(t->next,"(");
            Token *name = paren ? t->next->next : t->next, *n;
            if (!name || name->kind != TK_IDENT) { tool_error(p->c,"defined requires a macro name"); return 0; }
            n = synthetic(p->c,t,find_macro(p,name) ? "1" : "0",TK_NUMBER);
            if (!n) return 0;
            append_token(p->c,&tail,n);
            t = name;
            if (paren) {
                if (!token_is(t->next,")")) { tool_error(p->c,"expected ')' after defined"); return 0; }
                t = t->next;
            }
        } else append_token(p->c,&tail,t);
    }
    expanded = expand(p,head.next,0);
    value = condition_expression(p,&expanded,1,1).value;
    if (expanded) tool_error(p->c,"unexpected token in preprocessor condition");
    return value != 0;
}

static void define_macro(Preprocessor *p, Token *t) {
    Macro *m;
    Token *name = t;
    if (!t || t->kind != TK_IDENT) { tool_error(p->c,"#define requires a macro name"); return; }
    m = tool_alloc(p->c,sizeof(*m));
    if (!m) return;
    m->name = token_string(p->c,t);
    if (!m->name) return;
    t = t->next;
    if (token_is(t,"(") && !t->space) {
        Token *scan;
        m->function = 1;
        t = t->next;
        for (scan = t; scan && !token_is(scan,")"); scan = scan->next) ++m->count;
        m->parameters = tool_alloc(p->c,(m->count+1)*sizeof(*m->parameters));
        if (!m->parameters) return;
        m->count = 0;
        while (t && !token_is(t,")")) {
            if (token_is(t,"...")) { m->parameters[m->count++] = "__VA_ARGS__"; m->variadic = 1; t = t->next; break; }
            if (t->kind != TK_IDENT) { tool_error(p->c,"expected a macro parameter"); return; }
            for (size_t i = 0; i < m->count; ++i) if (token_is(t,m->parameters[i])) { tool_error(p->c,"duplicate macro parameter"); return; }
            m->parameters[m->count++] = token_string(p->c,t);
            t = t->next;
            if (!token_is(t,",")) break;
            t = t->next;
        }
        if (!token_is(t,")")) { tool_error(p->c,"expected ')' after macro parameters"); return; }
        t = t->next;
    }
    m->body = t;
    if (token_is(t,"##")) { tool_error(p->c,"'##' cannot start a replacement list"); return; }
    for (Token *r = t; r; r = r->next)
        if (token_is(r,"##") && !r->next) { tool_error(p->c,"'##' cannot end a replacement list"); return; }
    {
        Macro *old = find_macro(p,name);
        if (old) {
            old->body = m->body; old->parameters = m->parameters; old->count = m->count;
            old->function = m->function; old->variadic = m->variadic;
            return;
        }
    }
    m->next = p->macros; p->macros = m;
}
static Token *include_file(Preprocessor *p, Token *tokens, const char *path) {
    Token *t = tokens;
    ncc_buffer name, bytes, candidate;
    char *full = NULL;
    Token *result = NULL;
    int quoted;
    ncc_status status = NCC_NOT_FOUND;
    if (!t || (t->kind != TK_STRING && !token_is(t,"<"))) t = expand(p,tokens,0);
    if (!t) { tool_error(p->c,"#include requires a header name"); return NULL; }
    ncc_buffer_init(&name,p->c->platform); ncc_buffer_init(&bytes,p->c->platform); ncc_buffer_init(&candidate,p->c->platform);
    quoted = t->kind == TK_STRING;
    if (quoted && !t->next) tool_append(p->c,&name,t->text+1,t->length-2);
    else if (token_is(t,"<")) {
        for (t = t->next; t && !token_is(t,">"); t = t->next) tool_append(p->c,&name,t->text,t->length);
        if (!t || t->next) { tool_error(p->c,"invalid #include header name"); goto done; }
    } else { tool_error(p->c,"invalid #include operand"); goto done; }
    for (size_t attempt = 0; attempt <= p->c->include_count && status == NCC_NOT_FOUND; ++attempt) {
        candidate.size = 0;
        if (!attempt) {
            size_t slash = 0;
            if (!quoted) continue;
            for (size_t i = 0; path[i]; ++i) if (path[i] == '/') slash = i+1;
            if (name.size && name.data[0] == '/') slash = 0;
            tool_append(p->c,&candidate,path,slash);
        } else {
            const char *directory = p->c->include_paths[attempt-1];
            tool_append(p->c,&candidate,directory,ncc_strlen(directory)); tool_byte(p->c,&candidate,'/');
        }
        tool_append(p->c,&candidate,name.data,name.size);
        full = tool_string(p->c,(char *)candidate.data,candidate.size);
        if (!full) goto done;
        status = ncc_read_file(p->c->platform,full,&bytes);
    }
    if (status != NCC_OK) { if (full) p->c->path = full; tool_error(p->c,ncc_status_string(status)); goto done; }
    if (!tool_byte(p->c,&bytes,0)) goto done;
    if (++p->depth > 128) { tool_error(p->c,"include nesting exceeds 128"); --p->depth; goto done; }
    result = process(p,(char *)bytes.data,full);
    --p->depth;
done:
    ncc_buffer_destroy(&name); ncc_buffer_destroy(&bytes); ncc_buffer_destroy(&candidate);
    return result;
}
static Token *process(Preprocessor *p, const char *source, const char *path) {
    Token *all = front_lex(p->c,source,path), head = {0}, *tail = &head;
    Token pending = {0}, *pending_tail = &pending;
    Conditional *stack = NULL;
    const char *logical_path = path;
    int64_t line_delta = 0;
    Context *c = p->c;
    for (Token *t = all; t && t->kind && !c->failed;) {
        Token line = {0}, *lt = &line, *end = t;
        int active = !stack || stack->active;
        while (end && end->kind && end->kind != TK_NEWLINE) { append_token(c,&lt,end); end = end->next; }
        for (Token *x = line.next; x; x = x->next) { x->path = logical_path; x->line = (size_t)((int64_t)x->line+line_delta); }
        c->path = logical_path; c->line = (size_t)((int64_t)t->line+line_delta);
        t = end && end->kind == TK_NEWLINE ? end->next : end;
        if (!token_is(line.next,"#")) {
            if (pending.next && line.next) line.next->space = 1;
            if (active) for (Token *x = line.next; x; x = x->next) append_token(c,&pending_tail,x);
            continue;
        }
        for (Token *x = expand(p,pending.next,0); x; x = x->next) append_token(c,&tail,x);
        pending.next = NULL; pending_tail = &pending;
        Token *directive = line.next->next, *args = directive ? directive->next : NULL;
        if (!directive) continue;
        if (token_is(directive,"if") || token_is(directive,"ifdef") || token_is(directive,"ifndef")) {
            Conditional *s = tool_alloc(c,sizeof(*s));
            int truth = 0;
            if (!s) break;
            if (token_is(directive,"if")) { if (active) truth = condition(p,args); }
            else {
                if (!args || args->kind != TK_IDENT || args->next) { tool_error(c,"conditional directive requires one identifier"); break; }
                truth = find_macro(p,args) != NULL;
                if (token_is(directive,"ifndef")) truth = !truth;
            }
            s->parent = active; s->taken = s->active = active && truth;
            s->next = stack; stack = s;
        } else if (token_is(directive,"else") || token_is(directive,"elif")) {
            if (!stack || stack->otherwise) { tool_error(c,"unmatched or repeated conditional branch"); break; }
            if (token_is(directive,"else")) {
                if (args) { tool_error(c,"unexpected token after #else"); break; }
                stack->otherwise = 1;
                stack->active = stack->parent && !stack->taken;
            } else stack->active = stack->parent && !stack->taken && condition(p,args);
            stack->taken |= stack->active;
        } else if (token_is(directive,"endif")) {
            if (!stack || args) { tool_error(c,"unmatched #endif or unexpected trailing tokens"); break; }
            stack = stack->next;
        } else if (!active) continue;
        else if (token_is(directive,"define")) define_macro(p,args);
        else if (token_is(directive,"undef")) {
            Macro **m = &p->macros;
            if (!args || args->kind != TK_IDENT || args->next) { tool_error(c,"#undef requires one identifier"); break; }
            while (*m) { if (token_is(args,(*m)->name)) { *m = (*m)->next; break; } m = &(*m)->next; }
        } else if (token_is(directive,"include")) {
            for (Token *x = include_file(p,args,path); x; x = x->next) append_token(c,&tail,x);
        } else if (token_is(directive,"line")) {
            Token *values = expand(p,args,0);
            uint64_t value = 0;
            if (!values || values->kind != TK_NUMBER) { tool_error(c,"#line requires a line number"); break; }
            for (size_t i = 0; i < values->length; ++i) {
                unsigned ch = (unsigned char)values->text[i];
                if (ch < '0' || ch > '9' || value > 214748364u) { tool_error(c,"invalid #line number"); break; }
                value = value*10+ch-'0';
            }
            if (!value || value > 2147483647u) { tool_error(c,"#line number is out of range"); break; }
            if (values->next) {
                if (values->next->kind != TK_STRING || values->next->next) { tool_error(c,"invalid filename after #line"); break; }
                logical_path = tool_string(c,values->next->text+1,values->next->length-2);
            }
            line_delta = (int64_t)value-(int64_t)(t ? t->line : value);
        } else if (token_is(directive,"error")) tool_error(c,"#error directive encountered");
        else if (token_is(directive,"pragma")) {
            if (args && token_is(args,"STDC")) continue;
            tool_error(c,"unsupported #pragma directive");
        } else tool_error(c,"unknown preprocessing directive");
    }
    if (stack && !c->failed) tool_error(c,"unterminated preprocessor conditional");
    for (Token *x = expand(p,pending.next,0); x; x = x->next) append_token(c,&tail,x);
    return head.next;
}
Token *front_preprocess(Context *c, const char *source, const char *path) {
    Preprocessor p = {c,NULL,0};
    Token *builtins = front_lex(c,"__STDC__ 1\n__STDC_VERSION__ 199901L\n__STDC_HOSTED__ 0\n__x86_64__ 1\n__linux__ 1\n",path);
    for (Token *t = builtins; t && t->kind;) {
        Token *next = t;
        while (next && next->kind && next->kind != TK_NEWLINE) next = next->next;
        Token *after = next ? next->next : NULL;
        if (next) {
            Token *last = t;
            while (last->next != next) last = last->next;
            last->next = NULL;
        }
        define_macro(&p,t);
        t = after;
    }
    if (c->predefines) process(&p,c->predefines,"<command-line>");
    Token *result = c->failed ? NULL : process(&p,source,path), *tail = result;
    Token *end = tool_alloc(c,sizeof(*end));
    if (!end) return NULL;
    end->text = ""; end->path = path; end->line = 1;
    if (!tail) return end;
    while (tail->next) tail = tail->next;
    tail->next = end;
    return c->failed ? NULL : result;
}
