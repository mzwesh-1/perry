/*
 * lexer.c - the Perry lexer (also called a scanner or tokenizer).
 *
 * It walks through the source text one character at a time and hands
 * back one token each time lexer_next() is called. Like Python, Perry
 * uses indentation for blocks, so the lexer also produces INDENT and
 * DEDENT tokens when a line is indented more or less than the one
 * before it.
 */
#include <stdio.h>
#include <string.h>
#include "lexer.h"

/* ------------------------------------------------------------------ */
/* Names and keyword table                                            */
/* ------------------------------------------------------------------ */

static const char *kind_names[TOK_KIND_COUNT] = {
    "EOF", "NEWLINE", "INDENT", "DEDENT",
    "IDENT", "NUMBER", "MONEY", "PERCENT", "DATE", "STRING",
    "ASSIGN", "PLUS", "MINUS", "STAR", "SLASH",
    "LPAREN", "RPAREN", "LBRACKET", "RBRACKET",
    "COMMA", "COLON", "DOT",
    "ERROR",
#define X(name, text) #name,
    PERRY_KEYWORDS(X)
#undef X
};

typedef struct {
    const char *text;
    TokenKind   kind;
} Keyword;

static const Keyword keywords[] = {
#define X(name, text) { text, TOK_##name },
    PERRY_KEYWORDS(X)
#undef X
};

#define KEYWORD_COUNT ((int)(sizeof keywords / sizeof keywords[0]))

const char *token_kind_name(TokenKind kind)
{
    if (kind < 0 || kind >= TOK_KIND_COUNT) return "?";
    return kind_names[kind];
}

const char *token_keyword_text(TokenKind kind)
{
    int i;
    for (i = 0; i < KEYWORD_COUNT; i++)
        if (keywords[i].kind == kind) return keywords[i].text;
    return token_kind_name(kind);
}

int token_is_keyword(TokenKind kind)
{
    return kind > TOK_ERROR && kind < TOK_KIND_COUNT;
}

/* ------------------------------------------------------------------ */
/* Small helpers                                                      */
/* ------------------------------------------------------------------ */

static int is_digit(char c) { return c >= '0' && c <= '9'; }
static int is_upper(char c) { return c >= 'A' && c <= 'Z'; }
static int is_alpha(char c)
{
    return (c >= 'a' && c <= 'z') || is_upper(c) || c == '_';
}
static int is_alnum(char c) { return is_alpha(c) || is_digit(c); }

static Token make_token(Lexer *lx, TokenKind kind)
{
    Token t;
    t.kind   = kind;
    t.start  = lx->start;
    t.length = (int)(lx->current - lx->start);
    t.line   = lx->line;
    return t;
}

static Token error_token(Lexer *lx, const char *message)
{
    lx->error = message;
    return make_token(lx, TOK_ERROR);
}

static TokenKind keyword_or_ident(const char *start, int length)
{
    int i;
    for (i = 0; i < KEYWORD_COUNT; i++) {
        if ((int)strlen(keywords[i].text) == length &&
            memcmp(keywords[i].text, start, (size_t)length) == 0)
            return keywords[i].kind;
    }
    return TOK_IDENT;
}

static int at_triple_quote(const char *p)
{
    return p[0] == '"' && p[1] == '"' && p[2] == '"';
}

/* Skips a """ ... """ comment. Returns 0 if it never closes. */
static int skip_block_comment(Lexer *lx)
{
    int start_line = lx->line;
    lx->current += 3;
    while (!at_triple_quote(lx->current)) {
        if (*lx->current == '\0') {
            lx->line = start_line;   /* report where the comment began */
            return 0;
        }
        if (*lx->current == '\n') lx->line++;
        lx->current++;
    }
    lx->current += 3;
    return 1;
}

static void skip_to_end_of_line(Lexer *lx)
{
    while (*lx->current != '\n' && *lx->current != '\0')
        lx->current++;
}

/* ------------------------------------------------------------------ */
/* Indentation                                                        */
/* ------------------------------------------------------------------ */

/*
 * Called at the start of each line. Skips blank lines and comment-only
 * lines, measures the indentation of the next real line, and produces
 * an INDENT or DEDENT token if the level changed. Returns 1 if it
 * produced a token (stored in *out), 0 if normal scanning should go on.
 */
static int handle_line_start(Lexer *lx, Token *out)
{
    for (;;) {
        int column = 0;
        int top;

        while (*lx->current == ' ' || *lx->current == '\t') {
            if (*lx->current == '\t') column += 4 - (column % 4);
            else                      column += 1;
            lx->current++;
        }
        lx->start = lx->current;

        if (*lx->current == '\r') { lx->current++; continue; }
        if (*lx->current == '\n') {            /* blank line */
            lx->current++;
            lx->line++;
            continue;
        }
        if (*lx->current == '#') {             /* comment-only line */
            skip_to_end_of_line(lx);
            continue;
        }
        if (at_triple_quote(lx->current)) {    /* """ comment block */
            if (!skip_block_comment(lx)) {
                *out = error_token(lx, "this \"\"\" comment is never closed");
                return 1;
            }
            while (*lx->current == ' ' || *lx->current == '\t' || *lx->current == '\r')
                lx->current++;
            if (*lx->current != '\n' && *lx->current != '\0' && *lx->current != '#') {
                lx->start = lx->current;
                *out = error_token(lx, "put code on a new line after a \"\"\" comment");
                return 1;
            }
            continue;
        }
        if (*lx->current == '\0') {            /* end of file */
            lx->at_line_start = 0;
            return 0;
        }

        /* A real line of code. Compare its indentation. */
        lx->at_line_start = 0;
        top = lx->indent_stack[lx->indent_depth - 1];

        if (column > top) {
            if (lx->indent_depth >= PERRY_MAX_INDENT) {
                *out = error_token(lx, "blocks are nested too deeply");
                return 1;
            }
            lx->indent_stack[lx->indent_depth++] = column;
            *out = make_token(lx, TOK_INDENT);
            return 1;
        }
        if (column < top) {
            while (lx->indent_depth > 1 && lx->indent_stack[lx->indent_depth - 1] > column) {
                lx->indent_depth--;
                lx->pending_dedents++;
            }
            if (lx->indent_stack[lx->indent_depth - 1] != column) {
                lx->pending_dedents = 0;
                *out = error_token(lx, "this line's indentation doesn't line up with any block above it");
                return 1;
            }
            lx->pending_dedents--;
            *out = make_token(lx, TOK_DEDENT);
            return 1;
        }
        return 0;
    }
}

/* ------------------------------------------------------------------ */
/* Scanning the different kinds of token                              */
/* ------------------------------------------------------------------ */

/* Numbers, money (199.99 ZAR), percentages (15%) and dates (2026-12-31). */
static Token scan_number(Lexer *lx)
{
    const char *p = lx->start;
    int i;

    /* A date: exactly dddd-dd-dd */
    if (is_digit(p[0]) && is_digit(p[1]) && is_digit(p[2]) && is_digit(p[3]) &&
        p[4] == '-' && is_digit(p[5]) && is_digit(p[6]) &&
        p[7] == '-' && is_digit(p[8]) && is_digit(p[9]) && !is_alnum(p[10])) {
        lx->current = p + 10;
        return make_token(lx, TOK_DATE);
    }

    while (is_digit(*lx->current)) lx->current++;
    if (*lx->current == '.' && is_digit(lx->current[1])) {
        lx->current++;
        while (is_digit(*lx->current)) lx->current++;
    }

    if (*lx->current == '%') {
        lx->current++;
        return make_token(lx, TOK_PERCENT);
    }

    /* Money: the number is followed by a 3-letter currency code. */
    p = lx->current;
    while (*p == ' ' || *p == '\t') p++;
    if (p != lx->current) {
        for (i = 0; i < 3 && is_upper(p[i]); i++)
            ;
        if (i == 3 && !is_alnum(p[3])) {
            lx->current = p + 3;
            return make_token(lx, TOK_MONEY);
        }
    }

    if (is_alpha(*lx->current)) {
        while (is_alnum(*lx->current)) lx->current++;
        return error_token(lx, "a number can't run straight into a word; names can't start with a digit");
    }
    return make_token(lx, TOK_NUMBER);
}

static Token scan_word(Lexer *lx)
{
    while (is_alnum(*lx->current)) lx->current++;
    return make_token(lx, keyword_or_ident(lx->start, (int)(lx->current - lx->start)));
}

static Token scan_string(Lexer *lx)
{
    lx->current++;                              /* opening quote */
    while (*lx->current != '"') {
        if (*lx->current == '\n' || *lx->current == '\r' || *lx->current == '\0')
            return error_token(lx, "this text is missing its closing \"");
        if (*lx->current == '\\' && lx->current[1] != '\0' && lx->current[1] != '\n')
            lx->current++;                      /* skip escaped char */
        lx->current++;
    }
    lx->current++;                              /* closing quote */
    return make_token(lx, TOK_STRING);
}

/* ------------------------------------------------------------------ */
/* The main entry points                                              */
/* ------------------------------------------------------------------ */

void lexer_init(Lexer *lx, const char *source)
{
    memset(lx, 0, sizeof *lx);
    lx->source  = source;
    lx->start   = source;
    lx->current = source;
    lx->line    = 1;
    lx->indent_stack[0] = 0;
    lx->indent_depth    = 1;
    lx->at_line_start   = 1;
    lx->last_was_newline = 1;
}

static Token scan(Lexer *lx)
{
    static char unknown_msg[80];
    Token t;
    char c;

    if (lx->pending_dedents > 0) {
        lx->pending_dedents--;
        lx->start = lx->current;
        return make_token(lx, TOK_DEDENT);
    }
    if (lx->at_line_start && handle_line_start(lx, &t))
        return t;

    for (;;) {
        /* skip spaces, tabs, Windows '\r', comments */
        while (*lx->current == ' ' || *lx->current == '\t' || *lx->current == '\r')
            lx->current++;
        if (*lx->current == '#') skip_to_end_of_line(lx);
        if (at_triple_quote(lx->current)) {
            lx->start = lx->current;
            if (!skip_block_comment(lx))
                return error_token(lx, "this \"\"\" comment is never closed");
            continue;
        }

        lx->start = lx->current;
        c = *lx->current;

        if (c == '\0') {
            /* End of file: finish the last line, close open blocks. */
            if (!lx->finished) {
                lx->finished = 1;
                if (!lx->last_was_newline) return make_token(lx, TOK_NEWLINE);
            }
            if (lx->indent_depth > 1) {
                lx->indent_depth--;
                return make_token(lx, TOK_DEDENT);
            }
            return make_token(lx, TOK_EOF);
        }

        if (c == '\n') {
            lx->current++;
            if (lx->paren_depth > 0) { lx->line++; continue; }
            t = make_token(lx, TOK_NEWLINE);
            lx->line++;
            lx->at_line_start = 1;
            return t;
        }
        break;
    }

    if (is_digit(c)) return scan_number(lx);
    if (is_alpha(c)) return scan_word(lx);
    if (c == '"')    return scan_string(lx);

    lx->current++;
    switch (c) {
    case '=':
        if (*lx->current == '=') {
            lx->current++;
            return error_token(lx, "Perry doesn't use '==' - compare with is_equal_to_");
        }
        return make_token(lx, TOK_ASSIGN);
    case '+': return make_token(lx, TOK_PLUS);
    case '-': return make_token(lx, TOK_MINUS);
    case '*': return make_token(lx, TOK_STAR);
    case '/': return make_token(lx, TOK_SLASH);
    case ',': return make_token(lx, TOK_COMMA);
    case ':': return make_token(lx, TOK_COLON);
    case '.': return make_token(lx, TOK_DOT);
    case '(': lx->paren_depth++; return make_token(lx, TOK_LPAREN);
    case '[': lx->paren_depth++; return make_token(lx, TOK_LBRACKET);
    case ')': if (lx->paren_depth > 0) lx->paren_depth--; return make_token(lx, TOK_RPAREN);
    case ']': if (lx->paren_depth > 0) lx->paren_depth--; return make_token(lx, TOK_RBRACKET);
    case '<': return error_token(lx, "Perry doesn't use '<' - use is_less_than_");
    case '>': return error_token(lx, "Perry doesn't use '>' - use is_greater_than_");
    case '!': return error_token(lx, "Perry doesn't use '!' - use not_ or is_not_equal_to_");
    default:
        if ((unsigned char)c >= 32 && (unsigned char)c < 127)
            sprintf(unknown_msg, "Perry doesn't know the character '%c'", c);
        else
            sprintf(unknown_msg, "Perry doesn't know this character (byte %d)", (unsigned char)c);
        return error_token(lx, unknown_msg);
    }
}

Token lexer_next(Lexer *lx)
{
    Token t = scan(lx);
    lx->last_was_newline = (t.kind == TOK_NEWLINE);
    return t;
}
