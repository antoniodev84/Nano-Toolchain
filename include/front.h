#ifndef NCC_FRONT_H
#define NCC_FRONT_H
#include "tool.h"

typedef struct Token Token;
typedef struct Hide Hide;
struct Hide { Hide *next; const char *name; };
struct Token {
    Token *next;
    const char *text, *path;
    size_t length, line;
    int kind, space;
    Hide *hides;
};
enum { TK_END, TK_IDENT, TK_NUMBER, TK_STRING, TK_CHAR, TK_PUNCT, TK_NEWLINE };
Token *front_lex(Context *, const char *, const char *);
Token *front_preprocess(Context *, const char *, const char *);
int token_is(Token *, const char *);
char *token_string(Context *, Token *);
#endif
