/*
 * lexer.h - turns Perry source text into tokens.
 */
#ifndef PERRY_LEXER_H
#define PERRY_LEXER_H

#include "tokens.h"

#define PERRY_MAX_INDENT 64

typedef struct {
    const char *source;
    const char *start;     /* start of the token being scanned        */
    const char *current;   /* next character to look at               */
    int line;

    int indent_stack[PERRY_MAX_INDENT];
    int indent_depth;
    int pending_dedents;
    int at_line_start;
    int paren_depth;       /* newlines inside ( ) or [ ] are ignored  */
    int last_was_newline;
    int finished;

    const char *error;     /* message for the last TOK_ERROR          */
} Lexer;

void  lexer_init(Lexer *lx, const char *source);
Token lexer_next(Lexer *lx);

#endif
