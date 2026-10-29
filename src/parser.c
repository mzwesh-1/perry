/*
 * parser.c - a hand-written recursive descent parser.
 *
 * Each grammar rule is one C function. A rule looks at the current
 * token, decides what it is looking at, and calls the functions for
 * the smaller pieces. The grammar, from biggest to smallest:
 *
 *   program     -> statement* EOF
 *   statement   -> check_if_ ... | if_then_ ... | exactly_ ... | ...
 *   block       -> NEWLINE INDENT statement+ DEDENT
 *
 *   expression  -> or
 *   or          -> and ( or_ and )*
 *   and         -> not ( and_ not )*
 *   not         -> not_ not | comparison
 *   comparison  -> sum ( is_..._ sum ( within_ sum )? )?
 *   sum         -> product ( ( + | - ) product )*
 *   product     -> unary ( ( * | / | of ) unary )*
 *   unary       -> - unary | primary
 *   primary     -> number | money | percent | text | yes_ | no_
 *                | nothing_ | name | ( expression )
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "parser.h"
#include "lexer.h"
#include "errors.h"
#include "dates.h"

typedef struct {
    Lexer lx;
    Token current;
    Token previous;
    int   block_depth;    /* 0 = top level of the file   */
    int   in_function;    /* inside a define_ block?      */
    int   no_tax_on;      /* inside convert_, on_ means "on this date" */
} Parser;

static Parser P;

/* ------------------------------------------------------------------ */
/* Token handling                                                     */
/* ------------------------------------------------------------------ */

static void describe(Token t, char *buf, size_t size)
{
    switch (t.kind) {
    case TOK_NEWLINE: snprintf(buf, size, "the end of the line"); break;
    case TOK_INDENT:  snprintf(buf, size, "an indented line");    break;
    case TOK_DEDENT:  snprintf(buf, size, "the end of a block");  break;
    case TOK_EOF:     snprintf(buf, size, "the end of the file"); break;
    default:          snprintf(buf, size, "'%.*s'", t.length > 40 ? 40 : t.length, t.start);
    }
}

static void fail_here(const char *message)
{
    char found[64];
    describe(P.current, found, sizeof found);
    perry_fail(EXIT_PARSE_ERROR, P.current.line, "%s (found %s)", message, found);
}

static void advance(void)
{
    P.previous = P.current;
    P.current  = lexer_next(&P.lx);
    if (P.current.kind == TOK_ERROR)
        perry_fail(EXIT_PARSE_ERROR, P.current.line, "%s", P.lx.error);
}

static int check(TokenKind kind) { return P.current.kind == kind; }

static int match(TokenKind kind)
{
    if (!check(kind)) return 0;
    advance();
    return 1;
}

static void expect(TokenKind kind, const char *message)
{
    if (!match(kind)) fail_here(message);
}

static int starts_a_value(TokenKind k)
{
    return k == TOK_NUMBER || k == TOK_MONEY || k == TOK_PERCENT || k == TOK_STRING ||
           k == TOK_DATE || k == TOK_IDENT;
}

static void end_of_line(void)
{
    if (match(TOK_NEWLINE) || check(TOK_EOF)) return;
    if (P.previous.kind == TOK_IDENT && starts_a_value(P.current.kind))
        perry_fail(EXIT_PARSE_ERROR, P.current.line,
                   "to use a function, put what it needs in brackets, like: %.*s(%.*s)",
                   P.previous.length, P.previous.start, P.current.length, P.current.start);
    fail_here("expected the end of the line");
}

/* ------------------------------------------------------------------ */
/* Building nodes                                                     */
/* ------------------------------------------------------------------ */

static Node *new_node(NodeKind kind, int line)
{
    Node *n = (Node *)calloc(1, sizeof(Node));
    if (n == NULL) {
        fprintf(stderr, "PERRY: out of memory\n");
        exit(1);
    }
    n->kind = kind;
    n->line = line;
    return n;
}

static void add_item(Node *list, Node *item)
{
    list->items = (Node **)realloc(list->items, sizeof(Node *) * (size_t)(list->count + 1));
    if (list->items == NULL) {
        fprintf(stderr, "PERRY: out of memory\n");
        exit(1);
    }
    list->items[list->count++] = item;
}

static Node *binary(NodeKind kind, TokenKind op, Node *a, Node *b, int line)
{
    Node *n = new_node(kind, line);
    n->op = op;
    n->a  = a;
    n->b  = b;
    return n;
}

/* ------------------------------------------------------------------ */
/* Literals: turning token text into values                           */
/* ------------------------------------------------------------------ */

static dec_t parse_digits(Token t, int len, int *decimals)
{
    dec_t amount = 0;
    int r = dec_parse(t.start, len, &amount, decimals);
    if (r == 0)  perry_fail(EXIT_PARSE_ERROR, t.line, "Perry numbers can have at most %d decimal places", PERRY_DECIMALS);
    if (r < 0)   perry_fail(EXIT_PARSE_ERROR, t.line, "this number is too big for Perry");
    return amount;
}

static Node *literal_number(Token t)
{
    Node *n = new_node(N_LITERAL, t.line);
    int decimals;
    n->value.kind   = VAL_NUMBER;
    n->value.amount = parse_digits(t, t.length, &decimals);
    return n;
}

static Node *literal_percent(Token t)
{
    Node *n = new_node(N_LITERAL, t.line);
    int decimals;
    dec_t whole = parse_digits(t, t.length - 1, &decimals);  /* without '%' */
    if (decimals > PERRY_DECIMALS - 2)
        perry_fail(EXIT_PARSE_ERROR, t.line, "percentages can have at most %d decimal places", PERRY_DECIMALS - 2);
    n->value.kind   = VAL_PERCENT;
    n->value.amount = whole / 100;                                 /* 15% -> 0.15 */
    return n;
}

static Node *literal_money(Token t)
{
    Node *n = new_node(N_LITERAL, t.line);
    const char *code = t.start + t.length - 3;
    int digits_len = 0, decimals, allowed;

    while (digits_len < t.length &&
           ((t.start[digits_len] >= '0' && t.start[digits_len] <= '9') || t.start[digits_len] == '.'))
        digits_len++;

    n->value.kind = VAL_MONEY;
    memcpy(n->value.currency, code, 3);
    n->value.currency[3] = '\0';
    n->value.amount = parse_digits(t, digits_len, &decimals);

    allowed = currency_decimals(n->value.currency);
    if (check(TOK_PER)) allowed = PERRY_DECIMALS;   /* 18.2034 ZAR per USD is an exchange rate */
    if (decimals > allowed)
        perry_fail(EXIT_PARSE_ERROR, t.line, "%s amounts can have at most %d decimal place%s",
                   n->value.currency, allowed, allowed == 1 ? "" : "s");
    return n;
}

static Node *literal_text(Token t)
{
    Node *n = new_node(N_LITERAL, t.line);
    char *out = (char *)malloc((size_t)t.length);
    int i, len = 0;

    if (out == NULL) { fprintf(stderr, "PERRY: out of memory\n"); exit(1); }
    for (i = 1; i < t.length - 1; i++) {          /* skip the quotes */
        char c = t.start[i];
        if (c == '\\' && i + 1 < t.length - 1) {
            c = t.start[++i];
            if      (c == 'n') c = '\n';
            else if (c == 't') c = '\t';
        }
        out[len++] = c;
    }
    n->value.kind     = VAL_TEXT;
    n->value.text     = out;
    n->value.text_len = len;
    return n;
}

/* ------------------------------------------------------------------ */
/* Expressions                                                        */
/* ------------------------------------------------------------------ */

static Node *expression(void);
static Node *sum(void);
static void  parse_filter(Node *n);
static int   word_is(Token t, const char *word);

static void not_built_yet(Token t)
{
    perry_fail(EXIT_PARSE_ERROR, t.line,
               "'%.*s' is part of Perry but isn't built yet", t.length, t.start);
}

static Node *unary(void);

/* name( value, value, ... ) - the name has just been read */
static Node *call(Token name)
{
    Node *n = new_node(N_CALL, name.line);
    n->name     = name.start;
    n->name_len = name.length;
    expect(TOK_LPAREN, "expected '('");
    if (!check(TOK_RPAREN)) {
        do {
            add_item(n, expression());
        } while (match(TOK_COMMA));
    }
    expect(TOK_RPAREN, "expected ')' to close the function call");
    return n;
}

static Node *primary(void)
{
    Token t = P.current;
    Node *n;

    switch (t.kind) {
    case TOK_NUMBER:  advance(); return literal_number(t);
    case TOK_PERCENT: advance(); return literal_percent(t);
    case TOK_MONEY:   advance(); return literal_money(t);
    case TOK_STRING:  advance(); return literal_text(t);
    case TOK_YES:
    case TOK_NO:
        advance();
        n = new_node(N_LITERAL, t.line);
        n->value.kind    = VAL_BOOL;
        n->value.boolean = (t.kind == TOK_YES);
        return n;
    case TOK_NOTHING:
        advance();
        n = new_node(N_LITERAL, t.line);
        n->value.kind = VAL_NOTHING;
        return n;
    case TOK_IDENT:
        advance();
        if (check(TOK_LPAREN)) return call(t);
        if (!P.no_tax_on && match(TOK_ON)) { /* income_tax on_ salary */
            n = new_node(N_TAX_APPLY, t.line);
            n->name     = t.start;
            n->name_len = t.length;
            n->a = unary();
            return n;
        }
        n = new_node(N_VAR, t.line);
        n->name     = t.start;
        n->name_len = t.length;
        return n;
    case TOK_ROUND_TO:                     /* round_to_ x 2 decimals the_bank_way_ */
        advance();
        n = new_node(N_ROUND, t.line);
        n->a = unary();
        n->b = unary();
        if (!(check(TOK_IDENT) &&
              ((P.current.length == 8 && memcmp(P.current.start, "decimals", 8) == 0) ||
               (P.current.length == 7 && memcmp(P.current.start, "decimal", 7) == 0))))
            fail_here("expected 'decimals', like: round_to_ total 2 decimals");
        advance();
        n->op = TOK_EOF;                   /* normal rounding */
        if (check(TOK_THE_BANK_WAY) || check(TOK_UP) || check(TOK_DOWN)) {
            n->op = P.current.kind;
            advance();
        }
        return n;
    case TOK_GO_GET:                       /* go_get_ "file.csv" as_ ZAR */
        advance();
        n = new_node(N_LOAD, t.line);
        n->a = unary();
        if (match(TOK_AS)) {
            if (!check(TOK_IDENT) || P.current.length != 3 ||
                !(P.current.start[0] >= 'A' && P.current.start[0] <= 'Z'))
                fail_here("expected a currency code after as_, like: as_ ZAR");
            n->name     = P.current.start;
            n->name_len = 3;
            advance();
        }
        return n;
    case TOK_GO_GET_IN:                    /* go_get_in_ list that_is_greater_than_ x */
        advance();
        n = new_node(N_FILTER, t.line);
        n->a = unary();
        parse_filter(n);
        return n;
    case TOK_LPAREN:
        advance();
        n = expression();
        expect(TOK_RPAREN, "expected ')' to close the '('");
        return n;
    case TOK_LBRACKET:                     /* [a, b, c] */
        advance();
        n = new_node(N_LIST, t.line);
        if (!check(TOK_RBRACKET)) {
            do {
                add_item(n, expression());
            } while (match(TOK_COMMA));
        }
        expect(TOK_RBRACKET, "expected ']' to close the list");
        return n;
    case TOK_SPLIT:                        /* split 100.00 ZAR into 3 */
        advance();
        n = new_node(N_SPLIT, t.line);
        n->a = sum();
        expect(TOK_INTO, "expected 'into', like: split 100.00 ZAR into 3");
        n->b = sum();
        return n;
    case TOK_TOTAL_OF:
    case TOK_COUNT_OF:
        advance();
        n = new_node(t.kind == TOK_TOTAL_OF ? N_TOTAL : N_COUNT, t.line);
        n->a = unary();
        return n;
    case TOK_AVERAGE_OF:                   /* average_of_ / biggest_of_ / smallest_of_ list */
    case TOK_BIGGEST_OF:
    case TOK_SMALLEST_OF:
        advance();
        n = new_node(N_STAT, t.line);
        n->op = t.kind;
        n->a = unary();
        return n;
    case TOK_VAT_ON:                       /* vat_on_ 1000.00 ZAR,  vat_inside_ 1150.00 ZAR */
    case TOK_VAT_INSIDE:
        advance();
        n = new_node(N_VAT, t.line);
        n->op = t.kind;
        n->a = unary();
        return n;
    case TOK_DUPLICATE_WHERE:              /* duplicate_where_ statement by_ amount */
        advance();
        n = new_node(N_DUPLICATES, t.line);
        n->a = unary();
        if (match(TOK_BY)) {
            if (!check(TOK_IDENT)) fail_here("expected a column name after by_");
            n->name     = P.current.start;
            n->name_len = P.current.length;
            advance();
        }
        return n;
    case TOK_ASK_USER:                     /* ask_user_("How much?") as_ ZAR */
        advance();
        n = new_node(N_ASK, t.line);
        n->a = unary();
        if (match(TOK_AS)) {
            static const char *kinds[] = { "number", "percent", "date", "text", "yes_no", NULL };
            int k, ok = 0;
            if (check(TOK_IDENT) && P.current.length == 3 && P.current.start[0] >= 'A' && P.current.start[0] <= 'Z')
                ok = 1;
            for (k = 0; kinds[k]; k++) if (word_is(P.current, kinds[k])) ok = 1;
            if (!ok) fail_here("after as_ write a currency (like ZAR) or number, percent, date, text or yes_no");
            n->name     = P.current.start;
            n->name_len = P.current.length;
            advance();
        }
        return n;
    case TOK_GROUP_BY:                     /* group_by_ statement by_ category */
        advance();
        n = new_node(N_GROUP, t.line);
        n->a = unary();
        if (match(TOK_BY)) {
            if (!check(TOK_IDENT)) fail_here("expected a column name after by_");
            n->name     = P.current.start;
            n->name_len = P.current.length;
            advance();
        }
        return n;
    case TOK_DATE:
        advance();
        n = new_node(N_LITERAL, t.line);
        n->value.kind = VAL_DATE;
        {
            long long days;
            if (!parse_date(t.start, t.length, &days))
                perry_fail(EXIT_PARSE_ERROR, t.line, "%.*s isn't a real date", t.length, t.start);
            n->value.amount = days;
        }
        return n;
    case TOK_TODAY:
        advance();
        return new_node(N_TODAY, t.line);
    case TOK_CONVERT:                      /* convert_ price to_ USD at_rate_ 18.20 ZAR per USD on_ 2026-09-25 */
        advance();
        n = new_node(N_CONVERT, t.line);
        P.no_tax_on++;
        n->a = unary();
        expect(TOK_TO, "expected to_ and a currency, like: convert_ price to_ USD");
        if (!check(TOK_IDENT) || P.current.length != 3 ||
            !(P.current.start[0] >= 'A' && P.current.start[0] <= 'Z'))
            fail_here("expected a 3-letter currency code after to_, like: to_ USD");
        n->name     = P.current.start;
        n->name_len = 3;
        advance();
        expect(TOK_AT_RATE, "expected at_rate_ and a rate, like: at_rate_ 18.20 ZAR per USD");
        n->b = unary();
        if (match(TOK_ON)) n->c = unary();
        P.no_tax_on--;
        return n;
    case TOK_DAYS_BETWEEN:                 /* days_between_ start and_ end */
    case TOK_BUSINESS_DAYS_BETWEEN:
        advance();
        n = new_node(N_BETWEEN, t.line);
        n->op = t.kind;
        n->a = sum();
        expect(TOK_AND, "expected and_ between the two dates, like: days_between_ start and_ end");
        n->b = sum();
        return n;
    case TOK_END_OF_MONTH:
        advance();
        n = new_node(N_END_OF_MONTH, t.line);
        n->a = unary();
        return n;
    default:
        if (token_is_keyword(t.kind)) not_built_yet(t);
        fail_here("expected a value, like a number, money, text or a name");
        return NULL;
    }
}

static int word_is(Token t, const char *word)
{
    return t.kind == TOK_IDENT && (int)strlen(word) == t.length && memcmp(t.start, word, (size_t)t.length) == 0;
}

/* day, days, week, weeks, month(s), year(s) -> a DUR_ unit (weeks count as 7 days) */
static int time_unit(Token t, int *times)
{
    *times = 1;
    if (word_is(t, "day")   || word_is(t, "days"))   return DUR_DAY;
    if (word_is(t, "week")  || word_is(t, "weeks"))  { *times = 7; return DUR_DAY; }
    if (word_is(t, "month") || word_is(t, "months")) return DUR_MONTH;
    if (word_is(t, "year")  || word_is(t, "years"))  return DUR_YEAR;
    return 0;
}

/* Things that can follow a value: row.amount, rate per month, 30 days */
static Node *postfix(Node *n)
{
    for (;;) {
        Token t = P.current;
        Node *p;
        int unit, times;

        if ((unit = time_unit(t, &times)) != 0 || word_is(t, "business")) {
            p = new_node(N_DURATION, t.line);
            p->a = n;
            if (word_is(t, "business")) {         /* 3 business days */
                advance();
                if (time_unit(P.current, &times) != DUR_DAY || times != 1)
                    fail_here("expected 'days' after business, like: 3 business days");
                unit = DUR_BUSINESS;
            }
            advance();
            p->op = (TokenKind)unit;
            if (times != 1) {                      /* 2 weeks = 14 days */
                Node *seven = new_node(N_LITERAL, t.line);
                seven->value.kind   = VAL_NUMBER;
                seven->value.amount = 7 * PERRY_SCALE;
                p->a = binary(N_BINARY, TOK_STAR, n, seven, t.line);
            }
            n = p;
            continue;
        }
        if (match(TOK_DOT)) {
            if (!check(TOK_IDENT)) fail_here("expected a column name after '.'");
            p = new_node(N_FIELD, t.line);
        } else if (match(TOK_PER)) {
            if (!check(TOK_IDENT)) fail_here("expected year or month after per");
            p = new_node(N_PER, t.line);
        } else {
            return n;
        }
        p->a        = n;
        p->name     = P.current.start;
        p->name_len = P.current.length;
        advance();
        n = p;
    }
}

static Node *unary(void)
{
    if (check(TOK_MINUS)) {
        int line = P.current.line;
        Node *n;
        advance();
        n = new_node(N_NEGATE, line);
        n->a = unary();
        return n;
    }
    return postfix(primary());
}

static Node *product(void)
{
    Node *left = unary();
    while (check(TOK_STAR) || check(TOK_SLASH) || check(TOK_OF)) {
        Token op = P.current;
        advance();
        left = binary(N_BINARY, op.kind, left, unary(), op.line);
    }
    return left;
}

static Node *sum(void)
{
    Node *left = product();
    while (check(TOK_PLUS) || check(TOK_MINUS)) {
        Token op = P.current;
        advance();
        left = binary(N_BINARY, op.kind, left, product(), op.line);
    }
    return left;
}

static int is_comparison(TokenKind k)
{
    return k == TOK_IS_EQUAL_TO || k == TOK_IS_NOT_EQUAL_TO ||
           k == TOK_IS_GREATER_THAN || k == TOK_IS_LESS_THAN ||
           k == TOK_IS_GREATER_EQUAL_TO || k == TOK_IS_LESS_EQUAL_TO ||
           k == TOK_IS_SIMILAR_TO || k == TOK_IS_REALLY_NOT_CLOSE_TO;
}

static int is_list_filter(TokenKind k)
{
    return k >= TOK_THAT_IS_EQUAL_TO && k <= TOK_THAT_IS_REALLY_NOT_CLOSE_TO;
}

/* [by_ column] that_is_..._ value [within_ tolerance] */
static void parse_filter(Node *n)
{
    if (match(TOK_BY)) {
        if (!check(TOK_IDENT)) fail_here("expected a column name after by_");
        n->name     = P.current.start;
        n->name_len = P.current.length;
        advance();
    }
    if (!is_list_filter(P.current.kind))
        fail_here("expected a that_is_... filter, like: that_is_greater_than_ 1000.00 ZAR");
    n->op = P.current.kind;
    advance();
    n->b = sum();
    if (n->op == TOK_THAT_IS_SIMILAR_TO || n->op == TOK_THAT_IS_REALLY_NOT_CLOSE_TO) {
        expect(TOK_WITHIN, "expected within_ and a tolerance, like: within_ 100.00 ZAR");
        n->c = sum();
    }
}

static Node *comparison(void)
{
    Node *left = sum();
    Token op = P.current;
    Node *n, *negated = NULL;

    if (op.kind == TOK_NOT) {                   /* x not_ is_equal_to_ 5  ==  not_ (x is_equal_to_ 5) */
        negated = new_node(N_NOT, op.line);
        advance();
        op = P.current;
        if (!is_comparison(op.kind))
            fail_here("after not_ comes a comparison, like: x not_ is_equal_to_ 5");
    }
    if (op.kind == TOK_IDENT && op.length > 3 && memcmp(op.start, "is_", 3) == 0 &&
        op.start[op.length - 1] == '_')
        perry_fail(EXIT_PARSE_ERROR, op.line,
                   "'%.*s' isn't a Perry comparison - use is_equal_to_, is_not_equal_to_, is_greater_than_, "
                   "is_less_than_, is_greater_equal_to_ or is_less_equal_to_, and put not_ in front for the "
                   "opposite, like: not_ x is_greater_than_ 5", op.length, op.start);

    if (is_list_filter(op.kind))
        perry_fail(EXIT_PARSE_ERROR, op.line,
                   "'%.*s' is for filtering lists; to compare two values use '%.*s'",
                   op.length, op.start, op.length - 5, op.start + 5);
    if (!is_comparison(op.kind)) return left;
    advance();

    if (op.kind == TOK_IS_SIMILAR_TO || op.kind == TOK_IS_REALLY_NOT_CLOSE_TO) {
        n = binary(N_SIMILAR, op.kind, left, sum(), op.line);
        expect(TOK_WITHIN, "expected within_ and a tolerance, like: within_ 100.00 ZAR");
        n->c = sum();
    } else {
        n = binary(N_COMPARE, op.kind, left, sum(), op.line);
    }

    if (is_comparison(P.current.kind))
        fail_here("compare one thing at a time; join comparisons with and_ or or_");
    if (negated != NULL) {
        negated->a = n;
        return negated;
    }
    return n;
}

static Node *not_expr(void)
{
    if (check(TOK_NOT)) {
        int line = P.current.line;
        Node *n;
        advance();
        n = new_node(N_NOT, line);
        n->a = not_expr();
        return n;
    }
    return comparison();
}

static Node *and_expr(void)
{
    Node *left = not_expr();
    while (check(TOK_AND)) {
        int line = P.current.line;
        advance();
        left = binary(N_AND, TOK_AND, left, not_expr(), line);
    }
    return left;
}

static Node *expression(void)
{
    Node *left = and_expr();
    while (check(TOK_OR)) {
        int line = P.current.line;
        advance();
        left = binary(N_OR, TOK_OR, left, and_expr(), line);
    }
    return left;
}

/* ------------------------------------------------------------------ */
/* Statements                                                         */
/* ------------------------------------------------------------------ */

static Node *statement(void);

static Node *block(void)
{
    Node *list = new_node(N_BLOCK, P.current.line);
    expect(TOK_COLON, "expected ':' at the end of this line");
    expect(TOK_NEWLINE, "after ':' the block goes on the next lines, indented");
    if (!check(TOK_INDENT))
        fail_here("expected an indented block after ':'");
    advance();
    P.block_depth++;
    while (!check(TOK_DEDENT) && !check(TOK_EOF)) {
        if (match(TOK_NEWLINE)) continue;
        add_item(list, statement());
    }
    P.block_depth--;
    match(TOK_DEDENT);
    return list;
}

/* check_if_ / thats_if_ / since_then_ */
static Node *if_chain(void)
{
    Node *n = new_node(N_IF, P.previous.line);
    n->a = expression();
    n->b = block();
    if (match(TOK_THATS_IF)) {
        n->c = if_chain();
    } else if (match(TOK_SINCE_THEN)) {
        n->c = block();
    }
    return n;
}

/* Statements that fit on one line (also used after if_then_ ... :) */
static Node *simple_statement(void)
{
    Token t = P.current;
    Node *n;

    switch (t.kind) {
    case TOK_SHOW_ME_RESULTS:
        advance();
        n = new_node(N_SHOW, t.line);
        expect(TOK_LPAREN, "expected '(' after show_me_results");
        if (!check(TOK_RPAREN)) {
            do {
                add_item(n, expression());
            } while (match(TOK_COMMA));
        }
        expect(TOK_RPAREN, "expected ')' to close show_me_results(");
        end_of_line();
        return n;

    case TOK_IDENT:
        advance();
        if (check(TOK_LPAREN)) {          /* a function call on its own line */
            n = call(t);
            end_of_line();
            return n;
        }
        if ((word_is(t, "return") || word_is(t, "print")) && !check(TOK_ASSIGN))
            perry_fail(EXIT_PARSE_ERROR, t.line, "Perry uses %s instead of %.*s",
                       word_is(t, "return") ? "give_back_" : "show_me_results(...)", t.length, t.start);
        n = new_node(N_ASSIGN, t.line);
        n->name     = t.start;
        n->name_len = t.length;
        expect(TOK_ASSIGN, "expected '=' after the name");
        n->a = expression();
        end_of_line();
        return n;

    case TOK_STOP_NOW:
    case TOK_SKIP_THIS_ONE:
        advance();
        n = new_node(t.kind == TOK_STOP_NOW ? N_STOP : N_SKIP, t.line);
        end_of_line();
        return n;

    case TOK_NOT_UNLESS:
        advance();
        n = new_node(N_REQUIRE, t.line);
        n->a = expression();
        end_of_line();
        return n;

    case TOK_GIVE_BACK:
        advance();
        if (!P.in_function)
            perry_fail(EXIT_PARSE_ERROR, t.line, "give_back_ only works inside a define_ function");
        n = new_node(N_RETURN, t.line);
        n->a = expression();
        end_of_line();
        return n;

    case TOK_ASK_USER:              /* ask_user_("Press Enter to continue") on its own line */
        n = expression();
        end_of_line();
        return n;

    case TOK_BECAUSE_IS:
        advance();
        n = new_node(N_BECAUSE, t.line);
        if (!check(TOK_IDENT)) fail_here("expected a name after because_is_, like: because_is_ total");
        n->name     = P.current.start;
        n->name_len = P.current.length;
        advance();
        end_of_line();
        return n;

    case TOK_SPLIT:
        perry_fail(EXIT_PARSE_ERROR, t.line,
                   "keep the parts of a split in a name, like: parts = split 100.00 ZAR into 3");
        return NULL;

    case TOK_REMOVE_WHERE:          /* remove_where_ list [by_ col] that_is_... value */
    case TOK_SORT:                  /* sort_ list [by_ col] highest_first_ */
        advance();
        n = new_node(t.kind == TOK_REMOVE_WHERE ? N_REMOVE : N_SORT, t.line);
        if (!check(TOK_IDENT))
            fail_here(t.kind == TOK_SORT ? "expected the name of a list after sort_"
                                         : "expected the name of a list after remove_where_");
        n->a = new_node(N_VAR, P.current.line);
        n->a->name     = P.current.start;
        n->a->name_len = P.current.length;
        advance();
        if (t.kind == TOK_REMOVE_WHERE) {
            parse_filter(n);
        } else {
            if (match(TOK_BY)) {
                if (!check(TOK_IDENT)) fail_here("expected a column name after by_");
                n->name     = P.current.start;
                n->name_len = P.current.length;
                advance();
            }
            n->op = TOK_LOWEST_FIRST;
            if (match(TOK_HIGHEST_FIRST))     n->op = TOK_HIGHEST_FIRST;
            else if (match(TOK_LOWEST_FIRST)) n->op = TOK_LOWEST_FIRST;
        }
        end_of_line();
        return n;

    case TOK_THATS_IF:
    case TOK_SINCE_THEN:
        perry_fail(EXIT_PARSE_ERROR, t.line,
                   "'%.*s' needs a check_if_ block right above it", t.length, t.start);
        return NULL;

    case TOK_EVEN_IF:
    case TOK_BUT_THEN:
        perry_fail(EXIT_PARSE_ERROR, t.line,
                   "'%.*s' needs a try_this_ block right above it", t.length, t.start);
        return NULL;

    case TOK_ADD_WHERE:             /* add_where_ expenses 80.00 ZAR */
    case TOK_ERASE:                 /* erase_ expenses */
        advance();
        n = new_node(t.kind == TOK_ADD_WHERE ? N_ADD : N_ERASE, t.line);
        if (!check(TOK_IDENT))
            fail_here(t.kind == TOK_ADD_WHERE ? "expected the name of a list after add_where_"
                                              : "expected a name after erase_");
        n->a = new_node(N_VAR, P.current.line);
        n->a->name     = P.current.start;
        n->a->name_len = P.current.length;
        n->name        = P.current.start;
        n->name_len    = P.current.length;
        advance();
        if (t.kind == TOK_ADD_WHERE) {
            if (check(TOK_NEWLINE) || check(TOK_EOF))
                fail_here("say what to add, like: add_where_ expenses 80.00 ZAR");
            n->b = expression();
        }
        end_of_line();
        return n;

    case TOK_SAVE_TO:               /* save_to_ "report.csv" table */
        advance();
        n = new_node(N_SAVE, t.line);
        n->a = unary();
        if (check(TOK_NEWLINE) || check(TOK_EOF))
            fail_here("say what to save, like: save_to_ \"report.csv\" statement");
        n->b = expression();
        end_of_line();
        return n;

    case TOK_RAISE_ALARM:           /* raise_alarm_ "message" */
        advance();
        n = new_node(N_ALARM, t.line);
        n->a = expression();
        end_of_line();
        return n;

    case TOK_STEAL_FROM:            /* steal_from_ holidays_za   or   steal_from_ my_tools */
    case TOK_IN_THAT:               /* in_that_ my_tools steal_from_ late_fee, VAT */
        advance();
        if (P.block_depth > 0)
            perry_fail(EXIT_PARSE_ERROR, t.line, "%.*s goes at the top of the file, not inside a block",
                       t.length, t.start);
        n = new_node(N_IMPORT, t.line);
        if (token_is_keyword(P.current.kind))
            perry_fail(EXIT_PARSE_ERROR, P.current.line,
                       "'%.*s' is built into Perry - you don't need steal_from_ to use it",
                       P.current.length, P.current.start);
        if (!check(TOK_IDENT)) fail_here("expected a module name, like: steal_from_ my_tools");
        n->name     = P.current.start;
        n->name_len = P.current.length;
        advance();
        if (t.kind == TOK_IN_THAT) {
            expect(TOK_STEAL_FROM, "expected steal_from_ and the names to take, like: in_that_ tools steal_from_ late_fee");
            do {
                Node *want;
                if (token_is_keyword(P.current.kind))
                    perry_fail(EXIT_PARSE_ERROR, P.current.line,
                               "'%.*s' is built into Perry - you don't need steal_from_ to use it",
                               P.current.length, P.current.start);
                if (!check(TOK_IDENT)) fail_here("expected a name to take from the module");
                want = new_node(N_VAR, P.current.line);
                want->name     = P.current.start;
                want->name_len = P.current.length;
                add_item(n, want);
                advance();
            } while (match(TOK_COMMA));
        }
        end_of_line();
        return n;

    case TOK_SHARE:                 /* share_ late_fee, VAT */
        advance();
        if (P.block_depth > 0)
            perry_fail(EXIT_PARSE_ERROR, t.line, "share_ goes at the top of the file, not inside a block");
        n = new_node(N_SHARE, t.line);
        do {
            Node *name;
            if (!check(TOK_IDENT)) fail_here("expected a name to share, like: share_ late_fee");
            name = new_node(N_VAR, P.current.line);
            name->name     = P.current.start;
            name->name_len = P.current.length;
            add_item(n, name);
            advance();
        } while (match(TOK_COMMA));
        end_of_line();
        return n;

    case TOK_INDENT:
        fail_here("this line is indented, but nothing above it starts a block");
        return NULL;

    default:
        if (token_is_keyword(t.kind)) not_built_yet(t);
        fail_here("Perry doesn't understand this line");
        return NULL;
    }
}

static Node *statement(void)
{
    Token t = P.current;
    Node *n, *body;

    switch (t.kind) {
    case TOK_CHECK_IF:
        advance();
        return if_chain();

    case TOK_IF_THEN:               /* if_then_ condition: one statement */
        advance();
        n = new_node(N_IF, t.line);
        n->a = expression();
        expect(TOK_COLON, "expected ':' after the if_then_ condition");
        body = new_node(N_BLOCK, t.line);
        add_item(body, simple_statement());
        n->b = body;
        return n;

    case TOK_EXACTLY:
        advance();
        n = new_node(N_REPEAT, t.line);
        n->a = expression();
        expect(TOK_TIMES, "expected 'times' after the number, like: exactly_ 12 times:");
        n->b = block();
        return n;

    case TOK_KEEP_GOING_WHILE:
        advance();
        n = new_node(N_WHILE, t.line);
        n->a = expression();
        n->b = block();
        return n;

    case TOK_GO_THROUGH:            /* go_through_ list as_ name: */
        advance();
        n = new_node(N_FOR, t.line);
        n->a = expression();
        expect(TOK_AS, "expected as_ and a name, like: go_through_ bills as_ bill:");
        if (!check(TOK_IDENT)) fail_here("expected a name after as_");
        n->name     = P.current.start;
        n->name_len = P.current.length;
        advance();
        n->b = block();
        return n;

    case TOK_DEFINE:                /* define_ name(a, b): */
        advance();
        if (P.block_depth > 0)
            perry_fail(EXIT_PARSE_ERROR, t.line, "define_ functions at the top of the file, not inside a block");
        n = new_node(N_FUNCTION, t.line);
        if (!check(TOK_IDENT)) fail_here("expected the function's name after define_");
        n->name     = P.current.start;
        n->name_len = P.current.length;
        advance();
        if (check(TOK_COLON)) {                /* define_ hello:   - a function with no inputs */
            P.in_function = 1;
            n->b = block();
            P.in_function = 0;
            return n;
        }
        expect(TOK_LPAREN, "expected '(' after the function name, like: define_ late_fee(amount):");
        if (!check(TOK_RPAREN)) {
            do {
                Node *param;
                if (!check(TOK_IDENT)) fail_here("expected a name for the function's input");
                param = new_node(N_VAR, P.current.line);
                param->name     = P.current.start;
                param->name_len = P.current.length;
                add_item(n, param);
                advance();
            } while (match(TOK_COMMA));
        }
        expect(TOK_RPAREN, "expected ')' after the function's inputs");
        P.in_function = 1;
        n->b = block();
        P.in_function = 0;
        return n;

    case TOK_BLUEPRINT:             /* blueprint_ Transaction:  date: date  amount: money ... */
        advance();
        if (P.block_depth > 0)
            perry_fail(EXIT_PARSE_ERROR, t.line, "blueprint_ goes at the top of the file, not inside a block");
        n = new_node(N_BLUEPRINT, t.line);
        if (!check(TOK_IDENT)) fail_here("expected a name for the blueprint, like: blueprint_ Transaction:");
        n->name     = P.current.start;
        n->name_len = P.current.length;
        advance();
        expect(TOK_COLON, "expected ':' after the blueprint's name");
        expect(TOK_NEWLINE, "the fields go on the next lines, indented");
        if (!check(TOK_INDENT)) fail_here("expected indented fields, like: amount: money");
        advance();
        while (!check(TOK_DEDENT) && !check(TOK_EOF)) {
            static const char *types[] = { "number", "money", "percent", "text", "date",
                                           "yes_no", "list", "anything", NULL };
            Node *field;
            int k, known = 0;
            if (match(TOK_NEWLINE)) continue;
            if (!check(TOK_IDENT)) fail_here("expected a field name, like: amount: money");
            field = new_node(N_VAR, P.current.line);
            field->name     = P.current.start;
            field->name_len = P.current.length;
            for (k = 0; k < n->count; k++)
                if (n->items[k]->name_len == field->name_len &&
                    memcmp(n->items[k]->name, field->name, (size_t)field->name_len) == 0)
                    fail_here("this blueprint already has a field with that name");
            advance();
            expect(TOK_COLON, "expected ':' after the field name, like: amount: money");
            if (!check(TOK_IDENT)) fail_here("expected a type: number, money, percent, text, date, yes_no, list or anything");
            for (k = 0; types[k]; k++)
                if (word_is(P.current, types[k])) known = 1;
            if (!known) fail_here("unknown type - use number, money, percent, text, date, yes_no, list or anything");
            field->a = new_node(N_VAR, P.current.line);
            field->a->name     = P.current.start;
            field->a->name_len = P.current.length;
            advance();
            end_of_line();
            add_item(n, field);
        }
        match(TOK_DEDENT);
        if (n->count == 0) fail_here("a blueprint needs at least one field");
        return n;

    case TOK_BOOK:                  /* book_ "Paid rent": debit_ ... credit_ ... */
        advance();
        n = new_node(N_BOOK, t.line);
        n->a = expression();
        expect(TOK_COLON, "expected ':' after the description, like: book_ \"Paid rent\":");
        expect(TOK_NEWLINE, "the debit_ and credit_ lines go on the next lines, indented");
        if (!check(TOK_INDENT)) fail_here("expected indented debit_ and credit_ lines");
        advance();
        while (!check(TOK_DEDENT) && !check(TOK_EOF)) {
            Node *entry;
            if (match(TOK_NEWLINE)) continue;
            if (!check(TOK_DEBIT) && !check(TOK_CREDIT))
                fail_here("each line of book_ starts with debit_ or credit_");
            entry = new_node(N_ENTRY, P.current.line);
            entry->op = P.current.kind;
            advance();
            if (!check(TOK_IDENT)) fail_here("expected an account name, like: debit_ rent_expense 8000.00 ZAR");
            entry->name     = P.current.start;
            entry->name_len = P.current.length;
            advance();
            entry->a = sum();
            end_of_line();
            add_item(n, entry);
        }
        match(TOK_DEDENT);
        return n;

    case TOK_TRY_THIS:              /* try_this_: ... even_if_ ...: ... but_then_: ... */
        advance();
        n = new_node(N_TRY, t.line);
        n->a = block();
        while (check(TOK_EVEN_IF)) {
            Node *handler = new_node(N_HANDLER, P.current.line);
            advance();
            if (!(check(TOK_THERES_AN_ERROR) || check(TOK_THERES_A_CURRENCY_ERROR) ||
                  check(TOK_THERES_A_FILE_ERROR) || check(TOK_THERES_A_BALANCE_ERROR)))
                fail_here("expected the kind of error after even_if_, like: even_if_ theres_an_error_:");
            handler->op = P.current.kind;
            advance();
            handler->b = block();
            add_item(n, handler);
        }
        if (match(TOK_BUT_THEN)) n->c = block();
        if (n->count == 0 && n->c == NULL)
            fail_here("try_this_ needs an even_if_ ...: or but_then_: block after it");
        return n;

    case TOK_TAX_TABLE:             /* tax_table_ name: up_to_ ... taxed_at_ ... */
        advance();
        n = new_node(N_TAX_TABLE, t.line);
        if (!check(TOK_IDENT)) fail_here("expected a name for the tax table, like: tax_table_ income_tax:");
        n->name     = P.current.start;
        n->name_len = P.current.length;
        advance();
        expect(TOK_COLON, "expected ':' after the tax table's name");
        expect(TOK_NEWLINE, "the brackets go on the next lines, indented");
        if (!check(TOK_INDENT)) fail_here("expected indented up_to_ ... taxed_at_ ... lines");
        advance();
        while (!check(TOK_DEDENT) && !check(TOK_EOF)) {
            Node *bracket;
            if (match(TOK_NEWLINE)) continue;
            bracket = new_node(N_BRACKET, P.current.line);
            if (match(TOK_UP_TO))       bracket->a = sum();
            else if (!match(TOK_ABOVE)) fail_here("each line of a tax table starts with up_to_ or above_");
            expect(TOK_TAXED_AT, "expected taxed_at_ and a percent, like: taxed_at_ 18%");
            bracket->b = sum();
            end_of_line();
            add_item(n, bracket);
        }
        match(TOK_DEDENT);
        return n;

    case TOK_NEVER_CHANGES:
        advance();
        n = new_node(N_CONSTANT, t.line);
        if (!check(TOK_IDENT)) fail_here("expected a name after never_changes_");
        n->name     = P.current.start;
        n->name_len = P.current.length;
        advance();
        expect(TOK_ASSIGN, "expected '=' after the name");
        n->a = expression();
        end_of_line();
        return n;

    default:
        return simple_statement();
    }
}

Node *parse_program(const char *source)
{
    Node *program = new_node(N_BLOCK, 1);

    lexer_init(&P.lx, source);
    advance();
    while (!check(TOK_EOF)) {
        if (match(TOK_NEWLINE)) continue;
        add_item(program, statement());
    }
    return program;
}

/* ------------------------------------------------------------------ */
/* Printing the tree (PERRY --tree)                                   */
/* ------------------------------------------------------------------ */

static void indent(int depth)
{
    int i;
    for (i = 0; i < depth; i++) printf("  ");
}

static void labelled(const char *label, const Node *n, int depth)
{
    indent(depth);
    printf("%s\n", label);
    ast_print(n, depth + 1);
}

void ast_print(const Node *n, int depth)
{
    char buf[128];
    int i;

    if (n == NULL) return;
    indent(depth);

    switch (n->kind) {
    case N_LITERAL:
        value_format(buf, sizeof buf, n->value);
        if (n->value.kind == VAL_TEXT) printf("text \"%s\"\n", buf);
        else                           printf("%s %s\n", n->value.kind == VAL_MONEY ? "money" :
                                                         n->value.kind == VAL_PERCENT ? "percent" :
                                                         n->value.kind == VAL_NUMBER ? "number" : "value", buf);
        break;
    case N_VAR:
        printf("name %.*s\n", n->name_len, n->name);
        break;
    case N_NEGATE:
        printf("negative\n");
        ast_print(n->a, depth + 1);
        break;
    case N_BINARY:
        printf("%s\n", n->op == TOK_PLUS ? "+" : n->op == TOK_MINUS ? "-" :
                       n->op == TOK_STAR ? "*" : n->op == TOK_SLASH ? "/" : "of");
        ast_print(n->a, depth + 1);
        ast_print(n->b, depth + 1);
        break;
    case N_COMPARE:
    case N_SIMILAR:
        printf("%s\n", token_keyword_text(n->op));
        ast_print(n->a, depth + 1);
        ast_print(n->b, depth + 1);
        if (n->c) labelled("within_", n->c, depth + 1);
        break;
    case N_AND:
    case N_OR:
        printf("%s\n", n->kind == N_AND ? "and_" : "or_");
        ast_print(n->a, depth + 1);
        ast_print(n->b, depth + 1);
        break;
    case N_NOT:
        printf("not_\n");
        ast_print(n->a, depth + 1);
        break;
    case N_BLOCK:
        printf("block\n");
        for (i = 0; i < n->count; i++) ast_print(n->items[i], depth + 1);
        break;
    case N_ASSIGN:
    case N_CONSTANT:
        printf("%s %.*s =\n", n->kind == N_CONSTANT ? "never_changes_" : "set", n->name_len, n->name);
        ast_print(n->a, depth + 1);
        break;
    case N_SHOW:
        printf("show_me_results\n");
        for (i = 0; i < n->count; i++) ast_print(n->items[i], depth + 1);
        break;
    case N_IF:
        printf("check_if_\n");
        labelled("condition:", n->a, depth + 1);
        labelled("then:", n->b, depth + 1);
        if (n->c) labelled(n->c->kind == N_IF ? "thats_if_:" : "since_then_:", n->c, depth + 1);
        break;
    case N_REPEAT:
        printf("exactly_ ... times\n");
        labelled("count:", n->a, depth + 1);
        labelled("do:", n->b, depth + 1);
        break;
    case N_WHILE:
        printf("keep_going_while_\n");
        labelled("condition:", n->a, depth + 1);
        labelled("do:", n->b, depth + 1);
        break;
    case N_REQUIRE:
        printf("not_unless_\n");
        ast_print(n->a, depth + 1);
        break;
    case N_STOP: printf("stop_now_\n"); break;
    case N_SKIP: printf("skip_this_one_\n"); break;
    case N_LIST:
        printf("list\n");
        for (i = 0; i < n->count; i++) ast_print(n->items[i], depth + 1);
        break;
    case N_SPLIT:
        printf("split\n");
        ast_print(n->a, depth + 1);
        labelled("into:", n->b, depth + 1);
        break;
    case N_TOTAL:
    case N_COUNT:
        printf("%s\n", n->kind == N_TOTAL ? "total_of_" : "count_of_");
        ast_print(n->a, depth + 1);
        break;
    case N_FOR:
        printf("go_through_ ... as_ %.*s\n", n->name_len, n->name);
        labelled("list:", n->a, depth + 1);
        labelled("do:", n->b, depth + 1);
        break;
    case N_FUNCTION:
        printf("define_ %.*s(", n->name_len, n->name);
        for (i = 0; i < n->count; i++)
            printf("%s%.*s", i ? ", " : "", n->items[i]->name_len, n->items[i]->name);
        printf(")\n");
        ast_print(n->b, depth + 1);
        break;
    case N_CALL:
        printf("call %.*s\n", n->name_len, n->name);
        for (i = 0; i < n->count; i++) ast_print(n->items[i], depth + 1);
        break;
    case N_RETURN:
        printf("give_back_\n");
        ast_print(n->a, depth + 1);
        break;
    case N_BECAUSE:
        printf("because_is_ %.*s\n", n->name_len, n->name);
        break;
    case N_FIELD:
        printf(".%.*s\n", n->name_len, n->name);
        ast_print(n->a, depth + 1);
        break;
    case N_PER:
        printf("per %.*s\n", n->name_len, n->name);
        ast_print(n->a, depth + 1);
        break;
    case N_ROUND:
        printf("round_to_ %s\n", n->op == TOK_EOF ? "(normal)" : token_keyword_text(n->op));
        ast_print(n->a, depth + 1);
        labelled("decimals:", n->b, depth + 1);
        break;
    case N_LOAD:
        if (n->name) printf("go_get_ as_ %.*s\n", n->name_len, n->name);
        else         printf("go_get_\n");
        ast_print(n->a, depth + 1);
        break;
    case N_FILTER:
    case N_REMOVE:
        printf("%s", n->kind == N_FILTER ? "go_get_in_" : "remove_where_");
        if (n->name) printf(" by_ %.*s", n->name_len, n->name);
        printf(" %s\n", token_keyword_text(n->op));
        ast_print(n->a, depth + 1);
        ast_print(n->b, depth + 1);
        if (n->c) labelled("within_", n->c, depth + 1);
        break;
    case N_SORT:
        printf("sort_");
        if (n->name) printf(" by_ %.*s", n->name_len, n->name);
        printf(" %s\n", token_keyword_text(n->op));
        ast_print(n->a, depth + 1);
        break;
    case N_TAX_TABLE:
        printf("tax_table_ %.*s\n", n->name_len, n->name);
        for (i = 0; i < n->count; i++) ast_print(n->items[i], depth + 1);
        break;
    case N_BRACKET:
        printf("%s\n", n->a ? "up_to_" : "above_");
        if (n->a) ast_print(n->a, depth + 1);
        labelled("taxed_at_", n->b, depth + 1);
        break;
    case N_TAX_APPLY:
        printf("%.*s on_\n", n->name_len, n->name);
        ast_print(n->a, depth + 1);
        break;
    case N_TODAY:
        printf("today_\n");
        break;
    case N_DURATION:
        printf("%s\n", n->op == DUR_MONTH ? "months" : n->op == DUR_YEAR ? "years" :
                       n->op == DUR_BUSINESS ? "business days" : "days");
        ast_print(n->a, depth + 1);
        break;
    case N_BETWEEN:
        printf("%s\n", token_keyword_text(n->op));
        ast_print(n->a, depth + 1);
        labelled("and_", n->b, depth + 1);
        break;
    case N_END_OF_MONTH:
        printf("end_of_month_\n");
        ast_print(n->a, depth + 1);
        break;
    case N_IMPORT:
        if (n->count) printf("in_that_ %.*s steal_from_", n->name_len, n->name);
        else          printf("steal_from_ %.*s", n->name_len, n->name);
        for (i = 0; i < n->count; i++) printf("%s %.*s", i ? "," : "", n->items[i]->name_len, n->items[i]->name);
        printf("\n");
        break;
    case N_SHARE:
        printf("share_");
        for (i = 0; i < n->count; i++) printf("%s %.*s", i ? "," : "", n->items[i]->name_len, n->items[i]->name);
        printf("\n");
        break;
    case N_BLUEPRINT:
        printf("blueprint_ %.*s\n", n->name_len, n->name);
        for (i = 0; i < n->count; i++) {
            indent(depth + 1);
            printf("%.*s: %.*s\n", n->items[i]->name_len, n->items[i]->name,
                   n->items[i]->a->name_len, n->items[i]->a->name);
        }
        break;
    case N_DUPLICATES:
        if (n->name) printf("duplicate_where_ by_ %.*s\n", n->name_len, n->name);
        else         printf("duplicate_where_\n");
        ast_print(n->a, depth + 1);
        break;
    case N_ASK:
        if (n->name) printf("ask_user_ as_ %.*s\n", n->name_len, n->name);
        else         printf("ask_user_\n");
        ast_print(n->a, depth + 1);
        break;
    case N_TRY:
        printf("try_this_\n");
        ast_print(n->a, depth + 1);
        for (i = 0; i < n->count; i++) ast_print(n->items[i], depth + 1);
        if (n->c) labelled("but_then_:", n->c, depth + 1);
        break;
    case N_HANDLER:
        printf("even_if_ %s\n", token_keyword_text(n->op));
        ast_print(n->b, depth + 1);
        break;
    case N_ALARM:
        printf("raise_alarm_\n");
        ast_print(n->a, depth + 1);
        break;
    case N_BOOK:
        printf("book_\n");
        ast_print(n->a, depth + 1);
        for (i = 0; i < n->count; i++) ast_print(n->items[i], depth + 1);
        break;
    case N_ENTRY:
        printf("%s %.*s\n", n->op == TOK_DEBIT ? "debit_" : "credit_", n->name_len, n->name);
        ast_print(n->a, depth + 1);
        break;
    case N_SAVE:
        printf("save_to_\n");
        ast_print(n->a, depth + 1);
        ast_print(n->b, depth + 1);
        break;
    case N_STAT:
    case N_VAT:
        printf("%s\n", token_keyword_text(n->op));
        ast_print(n->a, depth + 1);
        break;
    case N_GROUP:
        if (n->name) printf("group_by_ by_ %.*s\n", n->name_len, n->name);
        else         printf("group_by_\n");
        ast_print(n->a, depth + 1);
        break;
    case N_ADD:
        printf("add_where_ %.*s\n", n->name_len, n->name);
        ast_print(n->b, depth + 1);
        break;
    case N_ERASE:
        printf("erase_ %.*s\n", n->name_len, n->name);
        break;
    case N_CONVERT:
        printf("convert_ to_ %.*s\n", n->name_len, n->name);
        ast_print(n->a, depth + 1);
        labelled("at_rate_", n->b, depth + 1);
        if (n->c) labelled("on_", n->c, depth + 1);
        break;
    }
}
