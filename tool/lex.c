#include "front.h"

int token_is(Token *t, const char *s) {
    return t && t->length == ncc_strlen(s) && !ncc_memcmp(t->text,s,t->length);
}
char *token_string(Context *c, Token *t) { return tool_string(c,t->text,t->length); }
static int alpha(int ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_'; }
static int decimal(int ch) { return ch >= '0' && ch <= '9'; }
Token *front_lex(Context *c, const char *source, const char *path) {
    static const char *const punct[] = {"%:%:","...",">>=","<<=","##","->","++","--","<<",">>","<=",">=","==","!=","&&","||","*=","/=","%=","+=","-=","&=","^=","|=","<:",":>","<%","%>","%:",NULL};
    Token head = {0}, *tail = &head;
    ncc_buffer normalized;
    const char *p, *base;
    size_t line = 1, original_line = 1, source_length = ncc_strlen(source);
    size_t *positions;
    int space = 0;
    ncc_buffer_init(&normalized,c->platform);
    if (source_length > SIZE_MAX/sizeof(*positions)-1) { tool_error(c,"source file is too large"); return NULL; }
    positions = tool_alloc(c,(source_length+1)*sizeof(*positions));
    if (!positions) return NULL;
    for (size_t i = 0; source[i]; ++i) {
        unsigned ch = (unsigned char)source[i];
        if (ch == '?' && source[i+1] == '?' && source[i+2]) {
            static const char keys[] = "=/'()!<>-";
            static const char values[] = "#\\^[]|{}~";
            for (unsigned k = 0; keys[k]; ++k) if (source[i+2] == keys[k]) { ch = (unsigned char)values[k]; i += 2; break; }
        }
        if (ch == '\\' && source[i+1] == '\n') { ++i; ++original_line; continue; }
        if (ch == '\\' && source[i+1] == '\r' && source[i+2] == '\n') { i += 2; ++original_line; continue; }
        if (ch == '\r') { if (source[i+1] == '\n') ++i; ch = '\n'; }
        positions[normalized.size] = original_line;
        if (!tool_byte(c,&normalized,ch)) goto done;
        if (ch == '\n') ++original_line;
    }
    positions[normalized.size] = original_line;
    if (!tool_byte(c,&normalized,0)) goto done;
    p = tool_string(c,(char *)normalized.data,normalized.size-1);
    if (!p) goto done;
    base = p;
    while (*p && !c->failed) {
        const char *start;
        int kind;
        Token *t;
        line = positions[(size_t)(p-base)];
        c->path = path; c->line = line;
        if (*p == ' ' || *p == '\t' || *p == '\v' || *p == '\f') { space = 1; ++p; continue; }
        if (p[0] == '/' && p[1] == '/') { while (*p && *p != '\n') ++p; space = 1; continue; }
        if (p[0] == '/' && p[1] == '*') {
            p += 2;
            while (*p && !(p[0] == '*' && p[1] == '/')) { if (*p == '\n') ++line; ++p; }
            if (!*p) { tool_error(c,"unterminated block comment"); break; }
            p += 2; space = 1; continue;
        }
        start = p;
        if (*p == '\n') { kind = TK_NEWLINE; ++p; }
        else if (alpha(*p)) { kind = TK_IDENT; do { ++p; } while (alpha(*p) || decimal(*p)); }
        else if (decimal(*p) || (*p == '.' && decimal(p[1]))) {
            kind = TK_NUMBER;
            do {
                int prev = *p++;
                if ((prev == 'e' || prev == 'E' || prev == 'p' || prev == 'P') && (*p == '+' || *p == '-')) ++p;
            } while (alpha(*p) || decimal(*p) || *p == '.');
        } else if (*p == '"' || *p == '\'') {
            int quote = *p++;
            kind = quote == '"' ? TK_STRING : TK_CHAR;
            while (*p && *p != quote && *p != '\n') {
                if (*p == '\\' && p[1]) ++p;
                ++p;
            }
            if (*p != quote) { tool_error(c,"unterminated literal"); break; }
            ++p;
        } else {
            unsigned i;
            kind = TK_PUNCT;
            for (i = 0; punct[i]; ++i) {
                size_t n = ncc_strlen(punct[i]), j = 0;
                while (j < n && p[j] && p[j] == punct[i][j]) ++j;
                if (j == n) { p += n; break; }
            }
            if (!punct[i]) {
                if (!*p || (unsigned char)*p < 33 || (unsigned char)*p > 126) { tool_error(c,"invalid source character"); break; }
                ++p;
            }
        }
        t = tool_alloc(c,sizeof(*t));
        if (!t) break;
        t->text = start; t->length = (size_t)(p-start); t->kind = kind; t->line = line; t->path = path; t->space = space;
        if (token_is(t,"<:")) { t->text = "["; t->length = 1; }
        else if (token_is(t,":>")) { t->text = "]"; t->length = 1; }
        else if (token_is(t,"<%")) { t->text = "{"; t->length = 1; }
        else if (token_is(t,"%>")) { t->text = "}"; t->length = 1; }
        else if (token_is(t,"%:")) { t->text = "#"; t->length = 1; }
        else if (token_is(t,"%:%:")) { t->text = "##"; t->length = 2; }
        tail = tail->next = t;
        space = 0;
        if (kind == TK_NEWLINE) ++line;
    }
    if (!c->failed) {
        Token *end = tool_alloc(c,sizeof(*end));
        if (end) { end->text = ""; end->path = path; end->line = line; tail->next = end; }
    }
done:
    ncc_buffer_destroy(&normalized);
    return c->failed ? NULL : head.next;
}
