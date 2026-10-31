/*
 * check.c - PERRY --check: finds money mistakes without running the program.
 *
 * The checker walks the same tree the interpreter runs, but instead of
 * values it works with *types*: "money in ZAR", "a rate per year",
 * "a length of time in months". Wherever the interpreter would stop with
 * an error, the checker reports the same error - on every path, including
 * branches and functions a test run never reaches.
 *
 * It only reports a mistake when it is sure. Anything it can't know before
 * running (a CSV cell, an answer from ask_user_, a name from a module) is
 * "unknown", and unknown never causes an error. The interpreter's own
 * checks stay in place for those.
 *
 * Mistakes inside try_this_ that a matching even_if_ would catch are
 * reported as notes, not errors.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "check.h"
#include "errors.h"
#include "finance.h"
#include "parser.h"

/* ------------------------------------------------------------------ */
/* Types                                                              */
/* ------------------------------------------------------------------ */

#define T_UNKNOWN (-1)              /* can't know before running        */
#define P_UNKNOWN (-1)              /* rate period or duration unit     */

typedef struct Ty Ty;

struct Ty {
    int         kind;               /* a ValueKind, or T_UNKNOWN          */
    char        cur[4];             /* MONEY, FXRATE, TAX: "" = unknown   */
    char        base[4];            /* FXRATE: the "per USD" currency     */
    int         period;             /* PERCENT: PERIOD_*, DURATION: DUR_* */
    int         literal;            /* 1 = value below is known exactly  */
    Value       value;              /* NUMBER / PERCENT literal, DURATION count in amount */
    const Node *blueprint;          /* ROW made from a blueprint_         */
    Ty         *elem;               /* LIST: the type of its items        */
    Ty         *items;              /* LIST written out: each item; ROW: each field */
    int         nitems;
    Ty         *must;               /* LIST: kinds of item it surely holds, oldest first */
    int         nmust;
};

/*
 * An unknown type can still carry a currency in 'cur': "whatever this is,
 * if it's money it's in ZAR". A cell of a table loaded with go_get_ ... as_ ZAR
 * is like that. Adding, subtracting or comparing it with USD money fails
 * whatever the cell turns out to hold, so that much can be checked.
 */

static Ty unknown(void)
{
    Ty t;
    memset(&t, 0, sizeof t);
    t.kind   = T_UNKNOWN;
    t.period = P_UNKNOWN;
    return t;
}

static Ty of_kind(int kind)
{
    Ty t = unknown();
    t.kind = kind;
    return t;
}

static Ty money_in(const char *cur)
{
    Ty t = of_kind(VAL_MONEY);
    if (cur != NULL) memcpy(t.cur, cur, 4);
    return t;
}

static Ty percent_per(int period)
{
    Ty t = of_kind(VAL_PERCENT);
    t.period = period;
    return t;
}

static Ty *boxed(Ty t)
{
    Ty *p = (Ty *)malloc(sizeof(Ty));
    if (p == NULL) { fprintf(stderr, "PERRY: out of memory\n"); exit(1); }
    *p = t;
    return p;
}

static int known(Ty t)              { return t.kind != T_UNKNOWN; }
static int is_numeric(Ty t)         { return t.kind == VAL_NUMBER || t.kind == VAL_PERCENT || t.kind == VAL_MONEY; }
static int same_cur(const char *a, const char *b) { return a[0] && b[0] && strcmp(a, b) == 0; }
static int diff_cur(const char *a, const char *b) { return a[0] && b[0] && strcmp(a, b) != 0; }

static int same_item(Ty a, Ty b)
{
    return a.kind == b.kind && strcmp(a.cur, b.cur) == 0 && a.period == b.period;
}

/* A list that surely holds 'it' as well (only known kinds are worth keeping). */
static void must_add(Ty *list, Ty it)
{
    int i;
    Ty *more;
    if (!known(it) || it.kind == VAL_NOTHING) return;
    for (i = 0; i < list->nmust; i++) if (same_item(list->must[i], it)) return;
    more = (Ty *)calloc((size_t)list->nmust + 1, sizeof(Ty));
    if (more == NULL) { fprintf(stderr, "PERRY: out of memory\n"); exit(1); }
    if (list->nmust > 0) memcpy(more, list->must, sizeof(Ty) * (size_t)list->nmust);
    it.literal = 0;
    it.items = NULL; it.nitems = 0; it.must = NULL; it.nmust = 0;
    more[list->nmust++] = it;
    list->must = more;
}

/* Two types meet where branches join: keep what both agree on. */
static Ty merge(Ty a, Ty b)
{
    Ty t;
    if (a.kind != b.kind) return unknown();
    t = of_kind(a.kind);
    if (same_cur(a.cur, b.cur)) memcpy(t.cur, a.cur, 4);
    if (same_cur(a.base, b.base)) memcpy(t.base, a.base, 4);
    if (a.period == b.period) t.period = a.period;
    if (a.blueprint == b.blueprint) t.blueprint = a.blueprint;
    if (a.kind == VAL_LIST && a.elem && b.elem) t.elem = boxed(merge(*a.elem, *b.elem));
    if (a.items == b.items) { t.items = a.items; t.nitems = a.nitems; }
    if (a.kind == VAL_LIST && a.nmust > 0 && b.nmust > 0) {
        int i, k;
        t.must = (Ty *)calloc((size_t)a.nmust, sizeof(Ty));
        if (t.must == NULL) { fprintf(stderr, "PERRY: out of memory\n"); exit(1); }
        for (i = 0; i < a.nmust; i++)
            for (k = 0; k < b.nmust; k++)
                if (same_item(a.must[i], b.must[k])) { t.must[t.nmust++] = a.must[i]; break; }
    }
    if (a.literal && b.literal && a.value.kind == b.value.kind && a.value.amount == b.value.amount &&
        a.value.period == b.value.period && a.value.divisor == b.value.divisor) {
        t.literal = 1;
        t.value = a.value;
    }
    return t;
}

/* ------------------------------------------------------------------ */
/* Reporting                                                          */
/* ------------------------------------------------------------------ */

typedef enum { MISTAKE_GENERAL, MISTAKE_CURRENCY } MistakeKind;

typedef struct {
    int  line;
    int  note;                       /* 1 = caught by a try_this_ handler */
    int  order;                      /* keeps mistakes on one line in order */
    char text[300];
} Mistake;

static Mistake *mistakes;
static int      mistake_count, mistake_capacity;
static int      catch_all, catch_currency;   /* try_this_ handlers around us */

/*
 * While a module's code is being checked, 'other_file' is its file name and
 * 'reached_at' the line of this file that led there (the steal_from_ or the
 * call). Mistakes are reported on that line, so an editor underlines the
 * right place in the file that is open.
 */
static const char *other_file;
static int         reached_at;

static void report(MistakeKind kind, int line, const char *fmt, ...)
{
    Mistake m;
    va_list args;
    int i;

    memset(&m, 0, sizeof m);
    m.line  = line;
    m.order = mistake_count;
    m.note = catch_all > 0 || (kind == MISTAKE_CURRENCY && catch_currency > 0);
    va_start(args, fmt);
    vsnprintf(m.text, sizeof m.text, fmt, args);
    va_end(args);
    if (other_file != NULL) {
        char inner[300];
        memcpy(inner, m.text, sizeof inner);
        snprintf(m.text, sizeof m.text, "in %s line %d: %.240s", other_file, line, inner);
        m.line = reached_at;
    }

    for (i = 0; i < mistake_count; i++)          /* functions are checked once per call */
        if (mistakes[i].line == line && strcmp(mistakes[i].text, m.text) == 0) {
            if (!m.note) mistakes[i].note = 0;
            return;
        }
    if (mistake_count == mistake_capacity) {
        mistake_capacity = mistake_capacity ? mistake_capacity * 2 : 16;
        mistakes = (Mistake *)realloc(mistakes, sizeof(Mistake) * (size_t)mistake_capacity);
        if (mistakes == NULL) { fprintf(stderr, "PERRY: out of memory\n"); exit(1); }
    }
    mistakes[mistake_count++] = m;
}

#define fail(line, ...)          report(MISTAKE_GENERAL, (line), __VA_ARGS__)
#define fail_currency(line, ...) report(MISTAKE_CURRENCY, (line), __VA_ARGS__)

/* ------------------------------------------------------------------ */
/* Names                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *name;
    int         len;
    Ty          type;
    int         constant;
} Binding;

/*
 * The names known at one point of the program. A small hash table ('slots',
 * each holding an index into 'items' plus one, 0 = empty) keeps look-ups fast
 * in programs with thousands of names.
 */
typedef struct {
    Binding *items;
    int      count, capacity;
    int     *slots;
    int      nslots;
} Env;

static unsigned name_hash(const char *name, int len)
{
    unsigned h = 2166136261u;
    int i;
    for (i = 0; i < len; i++) h = (h ^ (unsigned char)name[i]) * 16777619u;
    return h;
}

static void env_reindex(Env *e)
{
    int i;
    free(e->slots);
    e->nslots = 16;
    while (e->nslots < e->capacity * 2) e->nslots *= 2;
    e->slots = (int *)calloc((size_t)e->nslots, sizeof(int));
    if (e->slots == NULL) { fprintf(stderr, "PERRY: out of memory\n"); exit(1); }
    for (i = 0; i < e->count; i++) {
        unsigned h = name_hash(e->items[i].name, e->items[i].len) & (unsigned)(e->nslots - 1);
        while (e->slots[h]) h = (h + 1) & (unsigned)(e->nslots - 1);
        e->slots[h] = i + 1;
    }
}

static Binding *env_find(Env *e, const char *name, int len)
{
    unsigned h;
    if (e == NULL || e->count == 0) return NULL;
    h = name_hash(name, len) & (unsigned)(e->nslots - 1);
    while (e->slots[h]) {
        Binding *b = &e->items[e->slots[h] - 1];
        if (b->len == len && memcmp(b->name, name, (size_t)len) == 0) return b;
        h = (h + 1) & (unsigned)(e->nslots - 1);
    }
    return NULL;
}

static void env_put(Env *e, const char *name, int len, Ty t, int constant)
{
    Binding *b = env_find(e, name, len);
    if (b == NULL) {
        unsigned h;
        if (e->count == e->capacity) {
            e->capacity = e->capacity ? e->capacity * 2 : 16;
            e->items = (Binding *)realloc(e->items, sizeof(Binding) * (size_t)e->capacity);
            if (e->items == NULL) { fprintf(stderr, "PERRY: out of memory\n"); exit(1); }
        }
        if (e->nslots < e->capacity * 2) env_reindex(e);
        b = &e->items[e->count++];
        b->name = name;
        b->len  = len;
        h = name_hash(name, len) & (unsigned)(e->nslots - 1);
        while (e->slots[h]) h = (h + 1) & (unsigned)(e->nslots - 1);
        e->slots[h] = e->count;
    }
    b->type     = t;
    b->constant = constant;
}

static void env_remove(Env *e, const char *name, int len)
{
    Binding *b = env_find(e, name, len);
    if (b == NULL) return;
    *b = e->items[--e->count];
    env_reindex(e);
}

static Env env_copy(const Env *e)
{
    Env c;
    c.count = c.capacity = e->count;
    c.items = NULL;
    c.slots = NULL;
    c.nslots = 0;
    if (e->count > 0) {
        c.items = (Binding *)malloc(sizeof(Binding) * (size_t)e->count);
        if (c.items == NULL) { fprintf(stderr, "PERRY: out of memory\n"); exit(1); }
        memcpy(c.items, e->items, sizeof(Binding) * (size_t)e->count);
        if (e->nslots >= c.capacity * 2) {         /* same names, same places: copy the table */
            c.nslots = e->nslots;
            c.slots = (int *)malloc(sizeof(int) * (size_t)c.nslots);
            if (c.slots == NULL) { fprintf(stderr, "PERRY: out of memory\n"); exit(1); }
            memcpy(c.slots, e->slots, sizeof(int) * (size_t)c.nslots);
        } else {
            env_reindex(&c);
        }
    }
    return c;
}

static void env_free(Env *e)
{
    free(e->items);
    free(e->slots);
    e->items = NULL;
    e->slots = NULL;
    e->count = e->capacity = e->nslots = 0;
}

/* After two paths join: a name set on only one path may or may not exist. */
static Env env_join(const Env *a, const Env *b)
{
    Env out = env_copy(a);
    int i;
    for (i = 0; i < out.count; i++) {
        Binding *other = env_find((Env *)b, out.items[i].name, out.items[i].len);
        if (other == NULL) {
            out.items[i].type = unknown();
            out.items[i].constant = 0;
        } else {
            out.items[i].type = merge(out.items[i].type, other->type);
            out.items[i].constant = out.items[i].constant && other->constant;
        }
    }
    for (i = 0; i < b->count; i++)
        if (env_find(&out, b->items[i].name, b->items[i].len) == NULL)
            env_put(&out, b->items[i].name, b->items[i].len, unknown(), 0);
    return out;
}

/* ------------------------------------------------------------------ */
/* The program as a whole                                             */
/* ------------------------------------------------------------------ */

typedef struct Module Module;

typedef struct {
    const Node *fn;
    Module     *home;                /* the file it was written in          */
} FnEntry;

/* A .perry file: this program, or one it takes names from with steal_from_. */
struct Module {
    char         name[64];
    char        *path;
    const Node  *program;
    Env          globals;
    FnEntry     *fns;
    int          nfns, fns_capacity;
    int          used_share;
    const Node **shared;
    int          nshared;
    int          loading;
};

typedef struct {
    Env    *names;                   /* where assignments go               */
    Env    *globals;                 /* a function's outside names, or NULL */
    Module *module;                  /* whose functions are in use         */
    int     in_function;
    Ty      returned;                /* give_back_ types seen so far      */
    int     has_returned;
} Ctx;

#define MAX_MODULES     32
#define MAX_CHECK_DEPTH 4

static Module       main_module;
static Module      *modules[MAX_MODULES];
static int          module_count;
static const Node **blueprints;
static int          blueprint_count, blueprint_capacity;
static const Node **checked_fns;     /* functions checked at least once    */
static int          checked_count, checked_capacity;
static int          has_imports;     /* a module couldn't be read: names may be missing */
static int          call_depth;

/* budget_tools.perry, without the folders in front */
static const char *file_part(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void *grow(void *p, int *capacity, size_t size)
{
    *capacity = *capacity ? *capacity * 2 : 16;
    p = realloc(p, size * (size_t)*capacity);
    if (p == NULL) { fprintf(stderr, "PERRY: out of memory\n"); exit(1); }
    return p;
}

static FnEntry *find_function_in(Module *m, const char *name, int len)
{
    int i;
    for (i = 0; i < m->nfns; i++)
        if (m->fns[i].fn->name_len == len && memcmp(m->fns[i].fn->name, name, (size_t)len) == 0) return &m->fns[i];
    return NULL;
}

static void add_function(Module *m, const Node *fn, Module *home)
{
    if (m->nfns == m->fns_capacity) m->fns = (FnEntry *)grow(m->fns, &m->fns_capacity, sizeof(FnEntry));
    m->fns[m->nfns].fn   = fn;
    m->fns[m->nfns].home = home;
    m->nfns++;
}

static void add_blueprint(const Node *bp)
{
    int i;
    for (i = 0; i < blueprint_count; i++) if (blueprints[i] == bp) return;
    if (blueprint_count == blueprint_capacity)
        blueprints = (const Node **)grow((void *)blueprints, &blueprint_capacity, sizeof(Node *));
    blueprints[blueprint_count++] = bp;
}

static int was_checked(const Node *fn)
{
    int i;
    for (i = 0; i < checked_count; i++) if (checked_fns[i] == fn) return 1;
    return 0;
}

static const Node *find_blueprint(const char *name, int len)
{
    int i;
    for (i = 0; i < blueprint_count; i++)
        if (blueprints[i]->name_len == len && memcmp(blueprints[i]->name, name, (size_t)len) == 0) return blueprints[i];
    return NULL;
}

static const Node *find_function(Ctx *c, const char *name, int len)
{
    FnEntry *e = find_function_in(c->module, name, len);
    return e != NULL ? e->fn : NULL;
}

static Binding *lookup(Ctx *c, const char *name, int len)
{
    Binding *b = env_find(c->names, name, len);
    return b != NULL ? b : env_find(c->globals, name, len);
}

static Ty   check_expr(Ctx *c, const Node *n);
static void check_stmt(Ctx *c, const Node *n);
static Ty   check_call(Ctx *c, const Node *n, int want_value);

/* ------------------------------------------------------------------ */
/* The interpreter's rules, on types                                  */
/* ------------------------------------------------------------------ */

static const char *unit_name(int unit)
{
    return unit == DUR_MONTH ? "months" : unit == DUR_YEAR ? "years" :
           unit == DUR_BUSINESS ? "business days" : "days";
}

static void literal_text(Ty t, char *buf, size_t size)
{
    if (t.literal) value_format(buf, size, t.value);
    else           buf[0] = '\0';
}

/* Money can only meet money of the same currency (interp.c: check_same_currency). */
static void same_currency(Ty a, Ty b, int line, const char *verb)
{
    char text[64], hint[64];
    if (!known(a) || !known(b)) return;
    if (a.kind == VAL_MONEY && b.kind == VAL_MONEY) {
        if (diff_cur(a.cur, b.cur))
            fail_currency(line, "can't %s %s and %s - convert one currency first", verb, a.cur, b.cur);
        return;
    }
    if (a.kind == VAL_MONEY || b.kind == VAL_MONEY) {
        Ty plain = a.kind == VAL_MONEY ? b : a;
        Ty money = a.kind == VAL_MONEY ? a : b;
        if (!is_numeric(plain)) return;            /* reported as "can't do maths with ..." */
        literal_text(plain, text, sizeof text);
        if (plain.kind == VAL_NUMBER && plain.literal && money.cur[0]) {
            Value as_money = plain.value;
            as_money.kind = VAL_MONEY;
            memcpy(as_money.currency, money.cur, 4);
            value_format(hint, sizeof hint, as_money);
            fail_currency(line, "can't %s money and %s (%s) - write it as money, like %s",
                          verb, value_kind_name((ValueKind)plain.kind), text, hint);
        } else if (text[0]) {
            fail_currency(line, "can't %s money and %s (%s)", verb, value_kind_name((ValueKind)plain.kind), text);
        } else {
            fail_currency(line, "can't %s money and %s", verb, value_kind_name((ValueKind)plain.kind));
        }
    }
}

/* Money in one currency meeting a value that, if it's money, is in another. */
static int hint_clash(Ty a, Ty b, int line, const char *verb)
{
    if ((a.kind == VAL_MONEY && b.kind == T_UNKNOWN && diff_cur(a.cur, b.cur)) ||
        (b.kind == VAL_MONEY && a.kind == T_UNKNOWN && diff_cur(a.cur, b.cur))) {
        fail_currency(line, "can't %s %s and %s - convert one currency first", verb, a.cur, b.cur);
        return 1;
    }
    return 0;
}

/* 1% per month and 12% per year can't be mixed directly. */
static void same_period(Ty a, Ty b, int line)
{
    if (a.kind != VAL_PERCENT || b.kind != VAL_PERCENT) return;
    if (a.period == P_UNKNOWN || b.period == P_UNKNOWN || a.period == b.period) return;
    if (a.period == PERIOD_NONE || b.period == PERIOD_NONE) {
        const char *p = period_name(a.period ? a.period : b.period);
        fail(line, "can't mix a rate per %s with a percent that has no period - add 'per %s' to it", p, p);
        return;
    }
    fail(line, "can't mix a rate per %s with a rate per %s - convert one, like: rate per %s",
         period_name(a.period), period_name(b.period), period_name(a.period));
}

static Ty date_arithmetic(TokenKind op, Ty a, Ty b, int line)
{
    if (!known(a) || !known(b)) {
        if (a.kind == VAL_DATE && (op == TOK_PLUS || op == TOK_MINUS) && b.kind != VAL_DATE) return of_kind(VAL_DATE);
        return unknown();
    }
    if (op == TOK_PLUS && a.kind == VAL_DATE && b.kind == VAL_DURATION) return of_kind(VAL_DATE);
    if (op == TOK_PLUS && a.kind == VAL_DURATION && b.kind == VAL_DATE) return of_kind(VAL_DATE);
    if (op == TOK_MINUS && a.kind == VAL_DATE && b.kind == VAL_DURATION) return of_kind(VAL_DATE);
    if (op == TOK_MINUS && a.kind == VAL_DATE && b.kind == VAL_DATE) return of_kind(VAL_NUMBER);
    if ((op == TOK_PLUS || op == TOK_MINUS) && a.kind == VAL_DURATION && b.kind == VAL_DURATION) {
        Ty t = of_kind(VAL_DURATION);
        if (a.period != P_UNKNOWN && b.period != P_UNKNOWN && a.period != b.period) {
            fail(line, "can't add %s and %s together", unit_name(a.period), unit_name(b.period));
            return unknown();
        }
        t.period = a.period;
        return t;
    }
    if (op == TOK_STAR && (a.kind == VAL_DURATION || b.kind == VAL_DURATION) &&
        (a.kind == VAL_NUMBER || b.kind == VAL_NUMBER)) {
        Ty t = of_kind(VAL_DURATION);
        t.period = a.kind == VAL_DURATION ? a.period : b.period;
        return t;
    }
    if ((a.kind == VAL_DATE || b.kind == VAL_DATE) && (a.kind == VAL_NUMBER || b.kind == VAL_NUMBER)) {
        fail(line, "a plain number can't be added to a date - say what it is, like: 5 days or 1 month");
        return unknown();
    }
    fail(line, "can't do that with %s and %s", value_kind_name((ValueKind)a.kind), value_kind_name((ValueKind)b.kind));
    return unknown();
}

static Ty arithmetic(TokenKind op, Ty a, Ty b, int line)
{
    if (a.kind == VAL_DATE || a.kind == VAL_DURATION || b.kind == VAL_DATE || b.kind == VAL_DURATION)
        return date_arithmetic(op, a, b, line);

    if (op == TOK_OF && known(a) && a.kind != VAL_PERCENT && is_numeric(a)) {
        fail(line, "'of' needs a percent on its left, like: 15%% of price");
        return unknown();
    }
    if (op == TOK_OF) return arithmetic(TOK_STAR, b, a, line);

    if (!known(a) || !known(b)) {
        if ((op == TOK_PLUS || op == TOK_MINUS) && hint_clash(a, b, line, op == TOK_PLUS ? "add" : "subtract"))
            return unknown();
        /* if it works at all, money keeps its currency */
        Ty m = a.kind == VAL_MONEY ? a : b;
        if (m.kind == VAL_MONEY && (op == TOK_PLUS || op == TOK_MINUS || op == TOK_STAR)) return money_in(m.cur);
        if (a.kind == VAL_MONEY && op == TOK_SLASH) return unknown();
        return unknown();
    }

    if (a.kind == VAL_TEXT && b.kind == VAL_TEXT && op == TOK_PLUS) return of_kind(VAL_TEXT);
    if (!is_numeric(a) || !is_numeric(b)) {
        Ty bad = is_numeric(a) ? b : a;
        if (bad.kind == VAL_NOTHING) return unknown();
        if (bad.kind == VAL_TEXT)
            fail(line, "can't do maths with text - numbers and money are written without quotes");
        else
            fail(line, "can't do maths with %s", value_kind_name((ValueKind)bad.kind));
        return unknown();
    }

    switch (op) {
    case TOK_PLUS:
    case TOK_MINUS:
        if (a.kind == VAL_MONEY || b.kind == VAL_MONEY) {
            same_currency(a, b, line, op == TOK_PLUS ? "add" : "subtract");
            return money_in(a.kind == VAL_MONEY ? a.cur : b.cur);
        }
        if (a.kind != b.kind) {
            fail(line, "can't mix a percent and a plain number with + or - (did you mean: 15%% of amount?)");
            return unknown();
        }
        if (a.kind == VAL_PERCENT) {
            same_period(a, b, line);
            return percent_per(a.period);
        }
        return of_kind(VAL_NUMBER);

    case TOK_STAR:
        if (a.kind == VAL_MONEY && b.kind == VAL_MONEY) {
            fail(line, "can't multiply money by money");
            return unknown();
        }
        if (a.kind == VAL_MONEY) return money_in(a.cur);
        if (b.kind == VAL_MONEY) return money_in(b.cur);
        if (a.kind == VAL_PERCENT && b.kind == VAL_PERCENT) return percent_per(PERIOD_NONE);
        if (a.kind == VAL_PERCENT) return percent_per(a.period);
        if (b.kind == VAL_PERCENT) return percent_per(b.period);
        return of_kind(VAL_NUMBER);

    case TOK_SLASH:
        if (b.literal && b.value.amount == 0) {
            fail(line, "can't divide by zero");
            return unknown();
        }
        if (b.kind == VAL_MONEY && a.kind != VAL_MONEY) {
            fail(line, "can't divide %s by money", value_kind_name((ValueKind)a.kind));
            return unknown();
        }
        if (a.kind == VAL_MONEY && b.kind == VAL_MONEY) {
            same_currency(a, b, line, "divide");
            return of_kind(VAL_NUMBER);
        }
        if (a.kind == VAL_MONEY) return money_in(a.cur);
        if (a.kind == VAL_PERCENT && b.kind == VAL_NUMBER) return percent_per(a.period);
        return of_kind(VAL_NUMBER);

    default:
        return unknown();
    }
}

static void comparable(TokenKind op, Ty a, Ty b, int line)
{
    if (hint_clash(a, b, line, "compare")) return;
    if (!known(a) || !known(b) || a.kind == VAL_NOTHING || b.kind == VAL_NOTHING) return;

    if (op == TOK_IS_EQUAL_TO || op == TOK_IS_NOT_EQUAL_TO) {
        if (is_numeric(a) && is_numeric(b)) {
            same_currency(a, b, line, "compare");
            same_period(a, b, line);
            return;
        }
        if (a.kind == VAL_DURATION && b.kind == VAL_DURATION) {
            if (a.period != P_UNKNOWN && b.period != P_UNKNOWN && a.period != b.period)
                fail(line, "can't compare %s with %s", unit_name(a.period), unit_name(b.period));
            return;
        }
        if (a.kind == VAL_TABLE || a.kind == VAL_ROW || a.kind == VAL_TAX || a.kind == VAL_LIST ||
            b.kind == VAL_TABLE || b.kind == VAL_ROW || b.kind == VAL_TAX || b.kind == VAL_LIST)
            return;
        if (a.kind != b.kind)
            fail(line, "can't compare %s with %s", value_kind_name((ValueKind)a.kind), value_kind_name((ValueKind)b.kind));
        return;
    }

    if ((a.kind == VAL_DATE && b.kind == VAL_DATE) || (a.kind == VAL_DURATION && b.kind == VAL_DURATION)) {
        if (a.kind == VAL_DURATION && a.period != P_UNKNOWN && b.period != P_UNKNOWN && a.period != b.period)
            fail(line, "can't compare %s with %s", unit_name(a.period), unit_name(b.period));
        return;
    }
    if (a.kind == VAL_DATE || b.kind == VAL_DATE) {
        fail(line, "can't compare %s with %s", value_kind_name((ValueKind)a.kind), value_kind_name((ValueKind)b.kind));
        return;
    }
    if (!is_numeric(a) || !is_numeric(b)) {
        if (a.kind == VAL_LIST || b.kind == VAL_LIST || a.kind == VAL_ROW || b.kind == VAL_ROW) return;
        fail(line, "only numbers, percents and money can be bigger or smaller");
        return;
    }
    same_currency(a, b, line, "compare");
    same_period(a, b, line);
}

static void condition(Ty t, int line, const char *where)
{
    if (known(t) && t.kind != VAL_BOOL && t.kind != VAL_NOTHING)
        fail(line, "%s needs a yes/no condition, like: total is_greater_than_ 100.00 ZAR (got %s)",
             where, value_kind_name((ValueKind)t.kind));
}

static int is_currency_code(const char *s, int len)
{
    return len == 3 && s[0] >= 'A' && s[0] <= 'Z' && s[1] >= 'A' && s[1] <= 'Z' && s[2] >= 'A' && s[2] <= 'Z';
}

static int name_is(const char *name, int len, const char *word)
{
    return len == (int)strlen(word) && memcmp(name, word, (size_t)len) == 0;
}

/* 11.75% per year,  rate per month,  18.20 ZAR per USD */
static Ty per_value(Ctx *c, const Node *n)
{
    Ty v = check_expr(c, n->a), t;
    int unit;

    if ((v.kind == VAL_MONEY || !known(v)) && is_currency_code(n->name, n->name_len)) {
        if (!known(v)) return unknown();
        t = of_kind(VAL_FXRATE);
        memcpy(t.cur, v.cur, 4);
        memcpy(t.base, n->name, 3);
        t.base[3] = '\0';
        if (same_cur(t.cur, t.base))
            fail(n->line, "an exchange rate needs two different currencies, like: 18.20 ZAR per USD");
        return t;
    }
    if (name_is(n->name, n->name_len, "year") || name_is(n->name, n->name_len, "years"))        unit = PERIOD_YEAR;
    else if (name_is(n->name, n->name_len, "month") || name_is(n->name, n->name_len, "months")) unit = PERIOD_MONTH;
    else {
        fail(n->line, "per works with year or month (got '%.*s')", n->name_len, n->name);
        return unknown();
    }
    if (known(v) && v.kind != VAL_PERCENT) {
        fail(n->line, "per works with rates, like: 11.75%% per year or 18.20 ZAR per USD (got %s)",
             value_kind_name((ValueKind)v.kind));
        return unknown();
    }
    return percent_per(unit);
}

/* convert_ price to_ USD at_rate_ 18.20 ZAR per USD on_ 2026-09-25 */
static Ty convert_money(Ctx *c, const Node *n)
{
    Ty amount = check_expr(c, n->a), rate = check_expr(c, n->b);
    char target[4];

    memcpy(target, n->name, 3);
    target[3] = '\0';
    if (n->c != NULL) {
        Ty when = check_expr(c, n->c);
        if (known(when) && when.kind != VAL_DATE)
            fail(n->line, "on_ needs the date of the rate, like: on_ 2026-09-25");
    }
    if (known(amount) && amount.kind != VAL_MONEY) {
        fail(n->line, "convert_ works on money (got %s)", value_kind_name((ValueKind)amount.kind));
        return money_in(target);
    }
    if (known(rate) && rate.kind != VAL_FXRATE) {
        fail(n->line, "at_rate_ needs an exchange rate, like: at_rate_ 18.20 ZAR per USD");
        return money_in(target);
    }
    if (amount.kind == VAL_MONEY && rate.kind == VAL_FXRATE && amount.cur[0] && rate.cur[0] && rate.base[0] &&
        strcmp(amount.cur, target) != 0 &&
        !(strcmp(amount.cur, rate.cur) == 0 && strcmp(target, rate.base) == 0) &&
        !(strcmp(amount.cur, rate.base) == 0 && strcmp(target, rate.cur) == 0))
        fail_currency(n->line, "that rate is for %s and %s, but this converts %s to %s",
                      rate.cur, rate.base, amount.cur, target);
    return money_in(target);
}

/* simple_interest_ / compound_interest_ / future_value_ / present_value_ (amount, rate, time) */
static Ty interest(const Node *call, int kind, Ty amount, Ty rate, Ty time)
{
    int line = call->line;
    long long top = 0, bottom = 1, count;

    if (known(amount) && amount.kind != VAL_MONEY) {
        fail(line, "%.*s needs money first", call->name_len, call->name);
        return unknown();
    }
    if (known(rate) && rate.kind != VAL_PERCENT) {
        fail(line, "%.*s needs a rate second, like: 10%% per year", call->name_len, call->name);
        return money_in(amount.cur);
    }
    if (rate.kind == VAL_PERCENT && rate.period == PERIOD_NONE) {
        fail(line, "say if the rate is per year or per month, like: 10%% per year");
        return money_in(amount.cur);
    }
    if (known(time) && (time.kind != VAL_DURATION || time.period == DUR_BUSINESS)) {
        fail(line, "%.*s needs a length of time third, like: 5 years, 18 months or 90 days",
             call->name_len, call->name);
        return money_in(amount.cur);
    }
    /* compounding needs whole rate periods: 18 months at a yearly rate doesn't fit */
    if (kind != 0 && rate.kind == VAL_PERCENT && rate.period != P_UNKNOWN &&
        time.kind == VAL_DURATION && time.period != P_UNKNOWN && time.literal) {
        count = (long long)time.value.amount;
        if (rate.period == PERIOD_YEAR) {
            if (time.period == DUR_YEAR)       { top = count;      bottom = 1;   }
            else if (time.period == DUR_MONTH) { top = count;      bottom = 12;  }
            else                               { top = count;      bottom = 365; }
        } else {
            if (time.period == DUR_YEAR)       { top = count * 12; bottom = 1;   }
            else if (time.period == DUR_MONTH) { top = count;      bottom = 1;   }
            else                               { top = count * 12; bottom = 365; }
        }
        if (top >= 0 && top % bottom != 0)
            fail(line, "compounding needs whole %ss - write the time in %ss, or change the rate with 'per'",
                 period_name(rate.period), period_name(rate.period));
    }
    return money_in(amount.cur);
}

/* monthly_payment / schedule_of_ (loan, rate per month, time) */
static void loan_inputs(const Node *call, Ty loan, Ty rate, Ty time)
{
    int line = call->line;
    if (known(loan) && loan.kind != VAL_MONEY) {
        fail(line, "%.*s needs the loan amount as money first", call->name_len, call->name);
        return;
    }
    if (known(rate) && rate.kind != VAL_PERCENT) {
        fail(line, "%.*s needs a rate second, like: rate per month", call->name_len, call->name);
        return;
    }
    if (rate.kind == VAL_PERCENT && rate.period == PERIOD_NONE) {
        fail(line, "say if the rate is per year or per month, like: 11.75%% per year");
        return;
    }
    if (rate.kind == VAL_PERCENT && rate.period != P_UNKNOWN && rate.period != PERIOD_MONTH) {
        fail(line, "this rate is per %s but payments are monthly - write: rate per month", period_name(rate.period));
        return;
    }
    if (time.kind == VAL_DURATION && time.period != P_UNKNOWN && time.period != DUR_YEAR && time.period != DUR_MONTH)
        fail(line, "a loan's length must be in months or years");
}

/* finance.c: how many rate periods fit in a length of time */
static void periods_of(Ty n, Ty rate, int line, const char *fname, const char *what)
{
    int rate_period = rate.kind == VAL_PERCENT ? rate.period : P_UNKNOWN;
    if (!known(n)) return;
    if (n.kind == VAL_DURATION) {
        if (n.period == P_UNKNOWN) return;
        if (n.period != DUR_YEAR && n.period != DUR_MONTH) {
            fail(line, "%s must be in years or months, or a plain number of periods", what);
            return;
        }
        if (rate_period == P_UNKNOWN) return;
        if (rate_period == PERIOD_NONE) {
            fail(line, "say if the rate is per year or per month, like 5%% per year, "
                       "or give %s as a plain number of periods", what);
            return;
        }
        if (n.period == DUR_MONTH && rate_period == PERIOD_YEAR && n.literal && (long long)n.value.amount % 12 != 0)
            fail(line, "the rate is per year, so give %s in whole years (or use 'rate per month')", what);
        return;
    }
    if (n.kind != VAL_NUMBER)
        fail(line, "%s needs %s as a number or a length of time like 10 years (got %s)",
             fname, what, value_kind_name((ValueKind)n.kind));
}

/* ------------------------------------------------------------------ */
/* Lists written out item by item                                     */
/* ------------------------------------------------------------------ */

static Ty element_of(Ty list)
{
    return list.kind == VAL_LIST && list.elem != NULL ? *list.elem : unknown();
}

static Ty element_list(Ty elem)
{
    Ty t = of_kind(VAL_LIST);
    t.elem = boxed(elem);
    return t;
}

/* total_of_ / average_of_: the items are added one by one, as when running */
static Ty add_up(Ty list, int line)
{
    Ty sum = unknown();
    int i, have = 0, before = mistake_count;
    if (list.items == NULL) {
        Ty e = element_of(list);
        for (i = 1; i < list.nmust; i++) {
            Ty first = list.must[0], it = list.must[i];
            if (first.kind == VAL_MONEY && it.kind == VAL_MONEY && diff_cur(first.cur, it.cur)) {
                fail_currency(line, "can't add %s and %s - convert one currency first", first.cur, it.cur);
                return unknown();
            }
            if ((first.kind == VAL_MONEY) != (it.kind == VAL_MONEY) && is_numeric(first) && is_numeric(it)) {
                Ty plain = first.kind == VAL_MONEY ? it : first;
                fail_currency(line, "can't add money and %s", value_kind_name((ValueKind)plain.kind));
                return unknown();
            }
        }
        return e.kind == VAL_MONEY ? money_in(e.cur) : e.kind == VAL_NUMBER ? of_kind(VAL_NUMBER) : unknown();
    }
    for (i = 0; i < list.nitems; i++) {
        if (!have) { sum = list.items[i]; have = 1; continue; }
        sum = arithmetic(TOK_PLUS, sum, list.items[i], line);
        sum.literal = 0;
        if (mistake_count > before) return unknown();
    }
    return have ? sum : of_kind(VAL_NUMBER);
}

/* biggest_of_ / smallest_of_: each item is compared with the best so far */
static Ty best_of(Ty list, int line)
{
    Ty best;
    int i, before = mistake_count;
    if (list.items == NULL || list.nitems == 0) return element_of(list);
    best = list.items[0];
    for (i = 1; i < list.nitems; i++) {
        Ty it = list.items[i];
        if (!known(it) || !known(best)) continue;
        if (is_numeric(it) && is_numeric(best)) {
            same_currency(it, best, line, "compare");
            same_period(it, best, line);
        }
        if (mistake_count > before) return unknown();
    }
    best.literal = 0;
    return best;
}

/* go_get_in_ / remove_where_: every item is compared with the target */
static void filter_items(Ty list, const Node *n, Ty target, Ty tol)
{
    TokenKind op = (TokenKind)(n->op - (TOK_THAT_IS_EQUAL_TO - TOK_IS_EQUAL_TO));
    int i, count = list.items ? list.nitems : 1, before = mistake_count;
    for (i = 0; i < count && mistake_count == before; i++) {
        Ty it = list.items ? list.items[i] : element_of(list);
        if (op == TOK_IS_SIMILAR_TO || op == TOK_IS_REALLY_NOT_CLOSE_TO) {
            if (hint_clash(it, target, n->line, "compare")) return;
            if (it.kind == T_UNKNOWN && tol.kind == VAL_MONEY && hint_clash(it, tol, n->line, "compare")) return;
            if (is_numeric(it) && is_numeric(target)) {
                same_currency(it, target, n->line, "compare");
                if (known(tol) && (it.kind == VAL_MONEY || tol.kind == VAL_MONEY) && is_numeric(tol))
                    same_currency(it, tol, n->line, "compare");
                same_period(it, target, n->line);
            }
        } else {
            comparable(op, it, target, n->line);
        }
    }
}

static void filter_by(Ty src, const Node *n, Ty target, Ty tol)
{
    if (src.kind == VAL_LIST && n->name == NULL) {
        filter_items(src, n, target, tol);
    } else if (src.kind == VAL_TABLE && n->name != NULL && src.cur[0]) {
        Ty cell = unknown();                       /* money cells are in the table's currency */
        memcpy(cell.cur, src.cur, 4);
        filter_items(element_list(cell), n, target, tol);
    }
}

static const char *flow_kind(const Ty *t) { return t->kind == VAL_MONEY ? t->cur : "a number"; }

/* finance.c: cash flows are money in one currency, or numbers */
static Ty cash_flows(Ty list, int line, const char *fname)
{
    Ty example = unknown();
    int i, found = 0;
    if (known(list) && list.kind != VAL_LIST) {
        fail(line, "%s needs the cash flows as a list, like [-1000.00 ZAR, 400.00 ZAR, 700.00 ZAR] (got %s)",
             fname, value_kind_name((ValueKind)list.kind));
        return unknown();
    }
    if (list.items == NULL) {                    /* built up bit by bit: what it surely holds */
        for (i = 1; i < list.nmust; i++) {
            Ty first = list.must[0], it = list.must[i];
            if ((first.kind == VAL_MONEY || first.kind == VAL_NUMBER) && (it.kind == VAL_MONEY || it.kind == VAL_NUMBER) &&
                (first.kind != it.kind || diff_cur(first.cur, it.cur))) {
                fail(line, "all the cash flows must be the same kind - some items are %s, item 1 is %s",
                     flow_kind(&it), flow_kind(&first));
                return unknown();
            }
        }
        return element_of(list);
    }
    for (i = 0; i < list.nitems; i++) {
        Ty it = list.items[i];
        if (!known(it)) { if (!found) return unknown(); continue; }
        if (it.kind != VAL_MONEY && it.kind != VAL_NUMBER) {
            fail(line, "cash flows must be money or numbers (item %d is %s)", i + 1, value_kind_name((ValueKind)it.kind));
            return unknown();
        }
        if (!found) { example = it; found = 1; continue; }
        if (it.kind != example.kind || diff_cur(it.cur, example.cur)) {
            if (it.kind == VAL_MONEY && example.kind == VAL_MONEY && (!it.cur[0] || !example.cur[0])) continue;
            fail(line, "all the cash flows must be the same kind - item %d is %s, item 1 is %s",
                 i + 1, flow_kind(&it), flow_kind(&example));
            return unknown();
        }
    }
    return example;
}

/* finance.c: statistics need one kind of value */
static void stat_values(Ty list, int line, const char *fname)
{
    Ty example = unknown();
    int i, found = 0;
    if (list.items == NULL) {
        for (i = 1; i < list.nmust; i++)
            if (is_numeric(list.must[0]) && is_numeric(list.must[i]) &&
                (list.must[0].kind != list.must[i].kind || diff_cur(list.must[0].cur, list.must[i].cur))) {
                fail(line, "%s: all the values must be the same kind (some items are different)", fname);
                return;
            }
        return;
    }
    for (i = 0; i < list.nitems; i++) {
        Ty it = list.items[i];
        if (!known(it)) { if (!found) return; continue; }
        if (!is_numeric(it)) {
            fail(line, "%s works on numbers, percents and money (item %d is %s)", fname, i + 1,
                 value_kind_name((ValueKind)it.kind));
            return;
        }
        if (!found) { example = it; found = 1; continue; }
        if (it.kind != example.kind || diff_cur(it.cur, example.cur)) {
            fail(line, "%s: all the values must be the same kind (item %d is different)", fname, i + 1);
            return;
        }
    }
}

static void stat_pairs(Ty x, Ty y, int line, const char *fname)
{
    Ty ex = unknown(), ey = unknown();
    int i, fx = 0, fy = 0;
    if (x.items == NULL || y.items == NULL) return;
    if (x.nitems != y.nitems) {
        fail(line, "%s: the two lists must be the same length (%d and %d)", fname, x.nitems, y.nitems);
        return;
    }
    for (i = 0; i < x.nitems; i++) {
        Ty a = x.items[i], b = y.items[i];
        if (!known(a) || !known(b)) return;
        if (!is_numeric(a) || !is_numeric(b)) {
            fail(line, "%s works on numbers, percents and money (item %d)", fname, i + 1);
            return;
        }
        if (!fx) { ex = a; fx = 1; }
        if (!fy) { ey = b; fy = 1; }
        if (a.kind != ex.kind || diff_cur(a.cur, ex.cur) || b.kind != ey.kind || diff_cur(b.cur, ey.cur)) {
            fail(line, "%s: each list must hold one kind of value (item %d is different)", fname, i + 1);
            return;
        }
    }
}

static Ty like(Ty example)
{
    return example.kind == VAL_MONEY ? money_in(example.cur) :
           example.kind == VAL_NUMBER ? of_kind(VAL_NUMBER) : unknown();
}

static Ty table_in(const char *cur)
{
    Ty t = of_kind(VAL_TABLE);
    if (cur != NULL) memcpy(t.cur, cur, 4);
    return t;
}

static int is_named(const char *fname, const char *word) { return strcmp(fname, word) == 0; }

/* Perry for Universities: npv_, irr_, annuities, bonds, depreciation, statistics */
static Ty finance(int fn, const Node *call, Ty *a, int count)
{
    const char *fname = finance_name(fn);
    int line = call->line;
    Ty pay = count > 0 ? a[0] : unknown();

    if (is_named(fname, "npv_") || is_named(fname, "discounted_payback_")) {
        Ty example;
        if (count != 2) return unknown();
        if (known(a[0]) && a[0].kind != VAL_PERCENT) {
            fail(line, "%s needs the rate first as a percent, like 10%% (got %s)", fname, value_kind_name((ValueKind)a[0].kind));
            return unknown();
        }
        example = cash_flows(a[1], line, fname);
        return is_named(fname, "npv_") ? like(example) : of_kind(VAL_NUMBER);
    }
    if (is_named(fname, "irr_") || is_named(fname, "payback_")) {
        if (count != 1) return unknown();
        cash_flows(a[0], line, fname);
        return is_named(fname, "irr_") ? percent_per(PERIOD_NONE) : of_kind(VAL_NUMBER);
    }
    if (is_named(fname, "straight_line_") || is_named(fname, "sum_of_years_") || is_named(fname, "reducing_balance_")) {
        if (count != 3) return unknown();
        if (known(pay) && pay.kind != VAL_MONEY) {
            fail(line, "%s needs the cost as money first", fname);
            return unknown();
        }
        if (is_named(fname, "reducing_balance_")) {
            if (known(a[1]) && a[1].kind != VAL_PERCENT)
                fail(line, "%s needs the yearly rate second, like 20%%", fname);
        } else if (pay.kind == VAL_MONEY && pay.cur[0] && known(a[1]) &&
                   (a[1].kind != VAL_MONEY || diff_cur(a[1].cur, pay.cur))) {
            fail(line, "%s needs the residual (scrap) value second, as %s money", fname, pay.cur);
        }
        if (a[2].kind == VAL_DURATION && a[2].period != P_UNKNOWN && a[2].period != DUR_YEAR)
            fail(line, "%s needs the useful life in years, like 5 years or 5", fname);
        return table_in(pay.kind == VAL_MONEY ? pay.cur : NULL);
    }
    if (is_named(fname, "annuity_") || is_named(fname, "annuity_due_") ||
        is_named(fname, "accumulated_annuity_") || is_named(fname, "deferred_annuity_")) {
        int want = is_named(fname, "deferred_annuity_") ? 4 : 3;
        if (count != want) return unknown();             /* the interpreter explains the shape */
        if (known(pay) && pay.kind != VAL_MONEY && pay.kind != VAL_NUMBER) {
            fail(line, "%s needs the payment first as money or a number (got %s)", fname,
                 value_kind_name((ValueKind)pay.kind));
            return unknown();
        }
        if (known(a[1]) && a[1].kind != VAL_PERCENT) {
            fail(line, "%s needs the rate second as a percent, like 10%% (got %s)", fname,
                 value_kind_name((ValueKind)a[1].kind));
            return unknown();
        }
        periods_of(a[2], a[1], line, fname, "the number of payments");
        if (want == 4) periods_of(a[3], a[1], line, fname, "how long the payments are deferred");
        return like(pay);
    }
    if (is_named(fname, "perpetuity_")) return like(pay);
    if (is_named(fname, "bond_price_")) {
        if (count != 4) return unknown();
        if (known(pay) && pay.kind != VAL_MONEY) {
            fail(line, "%s needs the face (nominal) value as money first", fname);
            return unknown();
        }
        if (a[1].kind == VAL_PERCENT && a[2].kind == VAL_PERCENT && a[1].period != P_UNKNOWN &&
            a[2].period != P_UNKNOWN && a[1].period != PERIOD_NONE && a[2].period != PERIOD_NONE &&
            a[1].period != a[2].period)
            fail(line, "%s: one rate is per %s and the other per %s - use the same period for both",
                 fname, period_name(a[1].period), period_name(a[2].period));
        return like(pay);
    }
    if (is_named(fname, "bond_yield_")) {
        if (count != 4) return unknown();
        if (known(a[0]) && known(a[1]) &&
            (a[0].kind != VAL_MONEY || a[1].kind != VAL_MONEY || diff_cur(a[0].cur, a[1].cur)))
            fail(line, "%s needs the price and the face value as money in the same currency", fname);
        return a[2].kind == VAL_PERCENT ? percent_per(a[2].period) : unknown();
    }
    if (is_named(fname, "variance_") || is_named(fname, "standard_deviation_") ||
        is_named(fname, "population_variance_") || is_named(fname, "population_standard_deviation_") ||
        is_named(fname, "median_") || is_named(fname, "percentile_")) {
        if (count >= 1) stat_values(a[0], line, fname);
        return unknown();
    }
    if (is_named(fname, "correlation_") || is_named(fname, "regression_")) {
        if (count == 2) stat_pairs(a[0], a[1], line, fname);
        return is_named(fname, "correlation_") ? of_kind(VAL_NUMBER) : of_kind(VAL_TABLE);
    }
    if (is_named(fname, "life_annuity_") || is_named(fname, "temporary_life_annuity_") ||
        is_named(fname, "life_insurance_") || is_named(fname, "term_insurance_") ||
        is_named(fname, "pure_endowment_") || is_named(fname, "endowment_")) {
        Ty rate = count > 0 ? a[count - 1] : unknown();
        if (rate.kind == VAL_PERCENT && rate.period == PERIOD_MONTH)
            fail(line, "life tables work in years - give the rate per year");
        return unknown();
    }
    if (is_named(fname, "square_root_") || is_named(fname, "power_"))
        return known(pay) ? pay : unknown();
    return unknown();
}

static Ty blueprint_field(const Node *field_type)
{
    const char *w = field_type->name;
    int len = field_type->name_len;
    if (name_is(w, len, "number"))  return of_kind(VAL_NUMBER);
    if (name_is(w, len, "money"))   return of_kind(VAL_MONEY);
    if (name_is(w, len, "percent")) return of_kind(VAL_PERCENT);
    if (name_is(w, len, "text"))    return of_kind(VAL_TEXT);
    if (name_is(w, len, "date"))    return of_kind(VAL_DATE);
    if (name_is(w, len, "yes_no"))  return of_kind(VAL_BOOL);
    if (name_is(w, len, "list"))    return of_kind(VAL_LIST);
    return unknown();                                   /* anything */
}

/* row.name: a record's field, or a table cell (whose money is in the table's currency) */
static Ty field_type(Ty row, const Node *n)
{
    const Node *bp = row.blueprint;
    Ty t = unknown();
    int i;
    if (bp != NULL) {
        for (i = 0; i < bp->count; i++)
            if (bp->items[i]->name_len == n->name_len &&
                memcmp(bp->items[i]->name, n->name, (size_t)n->name_len) == 0) {
                if (row.items != NULL && i < row.nitems && known(row.items[i])) {
                    t = row.items[i];
                    t.literal = 0;
                    return t;
                }
                return blueprint_field(bp->items[i]->a);
            }
        return unknown();
    }
    memcpy(t.cur, row.cur, 4);
    return t;
}

static Ty make_record(Ctx *c, const Node *bp, const Node *n)
{
    Ty row = of_kind(VAL_ROW);
    int i;

    row.blueprint = bp;
    if (n->count == bp->count && n->count > 0) {
        row.items  = (Ty *)calloc((size_t)n->count, sizeof(Ty));
        row.nitems = n->count;
        if (row.items == NULL) { fprintf(stderr, "PERRY: out of memory\n"); exit(1); }
    }
    for (i = 0; i < n->count; i++) {
        Ty v = check_expr(c, n->items[i]), want;
        if (row.items != NULL) row.items[i] = v;
        if (i >= bp->count || !known(v) || v.kind == VAL_NOTHING) continue;
        want = blueprint_field(bp->items[i]->a);
        if (known(want) && want.kind != v.kind)
            fail(n->items[i]->line, "%.*s's %.*s must be %s (got %s)", n->name_len, n->name,
                 bp->items[i]->name_len, bp->items[i]->name,
                 want.kind == VAL_NUMBER ? "a number" : want.kind == VAL_BOOL ? "yes_ or no_" :
                 want.kind == VAL_MONEY ? "money" : value_kind_name((ValueKind)want.kind),
                 value_kind_name((ValueKind)v.kind));
    }
    if (n->count != bp->count) {
        char fields[300];
        size_t pos = 0;
        fields[0] = '\0';
        for (i = 0; i < bp->count && pos < sizeof fields - 70; i++)
            pos += (size_t)snprintf(fields + pos, sizeof fields - pos, "%s%.*s", i ? ", " : "",
                                    bp->items[i]->name_len, bp->items[i]->name);
        fail(n->line, "%.*s needs %d value%s, in this order: %s", n->name_len, n->name,
             bp->count, bp->count == 1 ? "" : "s", fields);
    }
    return row;
}

/* ------------------------------------------------------------------ */
/* Calls                                                              */
/* ------------------------------------------------------------------ */

static int has_give_back(const Node *n)
{
    int i;
    if (n == NULL) return 0;
    if (n->kind == N_RETURN) return 1;
    if (has_give_back(n->a) || has_give_back(n->b) || has_give_back(n->c)) return 1;
    for (i = 0; i < n->count; i++) if (has_give_back(n->items[i])) return 1;
    return 0;
}

/* Checks a function's body with these inputs. Each call is checked on its own,
   so with_vat(100.00 ZAR) and with_vat(5.00 USD) are both fine. */
static Ty check_body(const FnEntry *entry, const Ty *args, int line)
{
    const Node *fn = entry->fn;
    Env locals = { NULL, 0, 0, NULL, 0 };
    Ctx inner;
    const char *saved_file = other_file;
    int saved_line = reached_at, i;

    inner.names        = &locals;
    inner.globals      = &entry->home->globals;
    inner.module       = entry->home;
    inner.in_function  = 1;
    inner.returned     = unknown();
    inner.has_returned = 0;
    for (i = 0; i < fn->count; i++)
        env_put(&locals, fn->items[i]->name, fn->items[i]->name_len, args ? args[i] : unknown(), 0);

    if (entry->home != &main_module) {            /* report it where this file calls it */
        if (other_file == NULL) reached_at = line;
        other_file = file_part(entry->home->path);
    }
    call_depth++;
    check_stmt(&inner, fn->b);
    call_depth--;
    other_file = saved_file;
    reached_at = saved_line;

    if (!was_checked(fn)) {
        if (checked_count == checked_capacity)
            checked_fns = (const Node **)grow((void *)checked_fns, &checked_capacity, sizeof(Node *));
        checked_fns[checked_count++] = fn;
    }
    env_free(&locals);
    return inner.has_returned ? inner.returned : unknown();
}

static Ty check_call(Ctx *c, const Node *n, int want_value)
{
    FnEntry *entry = find_function_in(c->module, n->name, n->name_len);
    const Node *fn = entry ? entry->fn : NULL, *bp;
    Ty args[8], result;
    int i, kind, k;

    if (fn == NULL) {
        if ((bp = find_blueprint(n->name, n->name_len)) != NULL) return make_record(c, bp, n);

        k = finance_lookup(n->name, n->name_len);
        if (k >= 0) {
            for (i = 0; i < n->count; i++) {
                Ty t = check_expr(c, n->items[i]);
                if (i < 8) args[i] = t;
            }
            return n->count <= 8 ? finance(k, n, args, n->count) : unknown();
        }
        {
            static const char *names[] = { "simple_interest_", "compound_interest_", "future_value_", "present_value_" };
            for (kind = 0; kind < 4; kind++)
                if (name_is(n->name, n->name_len, names[kind])) {
                    for (i = 0; i < n->count; i++) {
                        Ty t = check_expr(c, n->items[i]);
                        if (i < 3) args[i] = t;
                    }
                    if (n->count != 3) {
                        fail(n->line, "%s needs 3 values: the amount, the rate and the time, like: "
                                      "%s(10000.00 ZAR, 10%% per year, 5 years)", names[kind], names[kind]);
                        return unknown();
                    }
                    return interest(n, kind, args[0], args[1], args[2]);
                }
        }
        if (name_is(n->name, n->name_len, "monthly_payment") || name_is(n->name, n->name_len, "schedule_of_")) {
            for (i = 0; i < n->count; i++) {
                Ty t = check_expr(c, n->items[i]);
                if (i < 3) args[i] = t;
            }
            if (n->count != 3) return unknown();
            loan_inputs(n, args[0], args[1], args[2]);
            if (n->name[0] == 'm') return money_in(args[0].kind == VAL_MONEY ? args[0].cur : NULL);
            return table_in(args[0].kind == VAL_MONEY ? args[0].cur : NULL);
        }
        for (i = 0; i < n->count; i++) check_expr(c, n->items[i]);
        if (name_is(n->name, n->name_len, "balance_of_")) return of_kind(VAL_MONEY);
        if (name_is(n->name, n->name_len, "journal_") || name_is(n->name, n->name_len, "trial_balance_"))
            return of_kind(VAL_TABLE);
        if (!has_imports)
            fail(n->line, "there's no function called '%.*s' - create it with define_ %.*s(...):",
                 n->name_len, n->name, n->name_len, n->name);
        return unknown();
    }

    if (want_value && !has_give_back(fn->b)) {
        fail(n->line, "%.*s doesn't give_back_ anything, so it has no answer to use here - "
                      "add a line like: give_back_ result", n->name_len, n->name);
        return unknown();
    }
    if (fn->count != n->count) {
        fail(n->line, "%.*s needs %d value%s but got %d", n->name_len, n->name,
             fn->count, fn->count == 1 ? "" : "s", n->count);
        return unknown();
    }
    {
        Ty *values = (Ty *)malloc(sizeof(Ty) * (size_t)(n->count + 1));
        if (values == NULL) { fprintf(stderr, "PERRY: out of memory\n"); exit(1); }
        for (i = 0; i < n->count; i++) values[i] = check_expr(c, n->items[i]);
        result = call_depth < MAX_CHECK_DEPTH ? check_body(entry, values, n->line) : unknown();
        free(values);
    }
    return result;
}

/* ------------------------------------------------------------------ */
/* Expressions                                                        */
/* ------------------------------------------------------------------ */

static Ty literal(const Node *n)
{
    Ty t;
    if (n->value.kind == VAL_NOTHING) return unknown();       /* "not known yet" */
    t = of_kind(n->value.kind);
    if (n->value.kind == VAL_MONEY) memcpy(t.cur, n->value.currency, 4);
    if (n->value.kind == VAL_PERCENT) t.period = n->value.period;
    if (n->value.kind == VAL_NUMBER || n->value.kind == VAL_PERCENT) {
        t.literal = 1;
        t.value = n->value;
    }
    return t;
}

/*
 * PERRY_CHECK_STATS=1 prints how many expressions got a known type: the
 * share of the program the checker can see into with no annotations at all.
 * An expression counts as known if any check of it (any call, any round) knew it.
 */
typedef struct { const Node *node; int known; } Seen;
static Seen *seen;
static int   seen_capacity, seen_count, stats_on = -1;

static void count_expr(const Node *n, Ty t)
{
    size_t h;
    if (stats_on < 0) stats_on = getenv("PERRY_CHECK_STATS") != NULL;
    if (!stats_on || n == NULL || other_file != NULL) return;
    if (seen_count * 2 >= seen_capacity) {           /* grow and rehash */
        Seen *old = seen;
        int old_capacity = seen_capacity, i;
        seen_capacity = seen_capacity ? seen_capacity * 2 : 1024;
        seen = (Seen *)calloc((size_t)seen_capacity, sizeof(Seen));
        if (seen == NULL) { fprintf(stderr, "PERRY: out of memory\n"); exit(1); }
        seen_count = 0;
        for (i = 0; i < old_capacity; i++)
            if (old[i].node != NULL) {
                h = ((size_t)old[i].node >> 4) % (size_t)seen_capacity;
                while (seen[h].node != NULL) h = (h + 1) % (size_t)seen_capacity;
                seen[h] = old[i];
                seen_count++;
            }
        free(old);
    }
    h = ((size_t)n >> 4) % (size_t)seen_capacity;
    while (seen[h].node != NULL && seen[h].node != n) h = (h + 1) % (size_t)seen_capacity;
    if (seen[h].node == NULL) { seen[h].node = n; seen_count++; }
    if (known(t) || (t.kind == T_UNKNOWN && t.cur[0])) seen[h].known = 1;
}

static Ty check_expr_inner(Ctx *c, const Node *n);

static Ty check_expr(Ctx *c, const Node *n)
{
    Ty t = check_expr_inner(c, n);
    count_expr(n, t);
    return t;
}

static Ty check_expr_inner(Ctx *c, const Node *n)
{
    Ty a, b, t;
    Binding *var;
    int i;

    if (n == NULL) return unknown();
    switch (n->kind) {
    case N_LITERAL:
        return literal(n);

    case N_VAR:
        var = lookup(c, n->name, n->name_len);
        if (var == NULL) {
            if (!has_imports && find_function(c, n->name, n->name_len) == NULL)
                fail(n->line, "'%.*s' doesn't have a value yet - give it one first, like: %.*s = 0",
                     n->name_len, n->name, n->name_len, n->name);
            return unknown();
        }
        return var->type;

    case N_NEGATE:
        a = check_expr(c, n->a);
        if (known(a) && !is_numeric(a) && a.kind != VAL_NOTHING)
            fail(n->line, "can't make %s negative", value_kind_name((ValueKind)a.kind));
        a.literal = a.literal && is_numeric(a);
        if (a.literal) a.value.amount = -a.value.amount;
        return is_numeric(a) || !known(a) ? a : unknown();

    case N_BINARY:
        a = check_expr(c, n->a);
        b = check_expr(c, n->b);
        return arithmetic(n->op, a, b, n->line);

    case N_COMPARE:
        a = check_expr(c, n->a);
        b = check_expr(c, n->b);
        comparable(n->op, a, b, n->line);
        return of_kind(VAL_BOOL);

    case N_SIMILAR:
        a = check_expr(c, n->a);
        b = check_expr(c, n->b);
        t = check_expr(c, n->c);
        if (hint_clash(a, b, n->line, "compare")) return of_kind(VAL_BOOL);
        if (known(a) && known(b) && known(t) && is_numeric(a) && is_numeric(b) && is_numeric(t)) {
            same_currency(a, b, n->line, "compare");
            if (a.kind == VAL_MONEY || t.kind == VAL_MONEY) same_currency(a, t, n->line, "compare");
            same_period(a, b, n->line);
        }
        return of_kind(VAL_BOOL);

    case N_AND:
    case N_OR:
        condition(check_expr(c, n->a), n->line, n->kind == N_AND ? "and_" : "or_");
        condition(check_expr(c, n->b), n->line, n->kind == N_AND ? "and_" : "or_");
        return of_kind(VAL_BOOL);

    case N_NOT:
        condition(check_expr(c, n->a), n->line, "not_");
        return of_kind(VAL_BOOL);

    case N_LIST:
        t = of_kind(VAL_LIST);
        if (n->count > 0) {
            t.items  = (Ty *)calloc((size_t)n->count, sizeof(Ty));
            t.nitems = n->count;
            if (t.items == NULL) { fprintf(stderr, "PERRY: out of memory\n"); exit(1); }
        }
        for (i = 0; i < n->count; i++) {
            t.items[i] = check_expr(c, n->items[i]);
            t.elem = boxed(i == 0 ? t.items[i] : merge(*t.elem, t.items[i]));
            t.elem->literal = 0;
            must_add(&t, t.items[i]);
        }
        return t;

    case N_SPLIT:
        a = check_expr(c, n->a);
        b = check_expr(c, n->b);
        if (known(a) && a.kind != VAL_MONEY) {
            fail(n->line, "split works on money, like: split 100.00 ZAR into 3");
            return unknown();
        }
        t = of_kind(VAL_LIST);
        t.elem = boxed(a.kind == VAL_MONEY ? money_in(a.cur) : unknown());
        return t;

    case N_TOTAL:
        a = check_expr(c, n->a);
        if (a.kind == VAL_TABLE) {
            fail(n->line, "say which column to add up, like: total_of_ statement.amount");
            return unknown();
        }
        return add_up(a, n->line);

    case N_COUNT:
        check_expr(c, n->a);
        return of_kind(VAL_NUMBER);

    case N_CALL:
        return check_call(c, n, 1);

    case N_FIELD:
        a = check_expr(c, n->a);
        if (a.kind == VAL_LIST && a.items != NULL) {         /* records.amount: one per record */
            t = of_kind(VAL_LIST);
            t.items  = (Ty *)calloc((size_t)a.nitems, sizeof(Ty));
            t.nitems = a.nitems;
            if (t.items == NULL) { fprintf(stderr, "PERRY: out of memory\n"); exit(1); }
            for (i = 0; i < a.nitems; i++) {
                if (a.items[i].kind != VAL_ROW) return unknown();
                t.items[i] = field_type(a.items[i], n);
                t.elem = boxed(i == 0 ? t.items[i] : merge(*t.elem, t.items[i]));
            }
            return t;
        }
        if (a.kind == VAL_LIST && a.elem != NULL && a.elem->kind == VAL_ROW)
            return element_list(field_type(*a.elem, n));
        if (a.kind == VAL_ROW) return field_type(a, n);
        if (a.kind == VAL_TABLE && a.cur[0]) {               /* a whole column */
            Ty cell = unknown();
            memcpy(cell.cur, a.cur, 4);
            return element_list(cell);
        }
        return unknown();

    case N_PER:
        return per_value(c, n);

    case N_ROUND:
        a = check_expr(c, n->a);
        check_expr(c, n->b);
        a.literal = 0;
        return is_numeric(a) ? a : unknown();

    case N_LOAD:
        check_expr(c, n->a);
        if (n->name != NULL) {                        /* go_get_ ... as_ ZAR: amounts are ZAR money */
            char cur[4];
            memcpy(cur, n->name, 3);
            cur[3] = '\0';
            return table_in(cur);
        }
        return of_kind(VAL_TABLE);

    case N_FILTER:
        a = check_expr(c, n->a);
        b = check_expr(c, n->b);
        t = check_expr(c, n->c);
        filter_by(a, n, b, t);
        a.items = NULL;                        /* which items stay isn't known */
        a.nitems = 0;
        return a.kind == VAL_LIST || a.kind == VAL_TABLE ? a : unknown();

    case N_TAX_APPLY:
        a = check_expr(c, n->a);
        var = lookup(c, n->name, n->name_len);
        if (known(a) && a.kind != VAL_MONEY) {
            fail(n->line, "on_ needs money, like: income_tax on_ 500000.00 ZAR");
            return unknown();
        }
        if (var != NULL && var->type.kind == VAL_TAX && a.kind == VAL_MONEY && diff_cur(var->type.cur, a.cur))
            fail_currency(n->line, "can't mix %s and %s - convert one currency first", var->type.cur, a.cur);
        return money_in(a.cur);

    case N_CONVERT:
        return convert_money(c, n);

    case N_STAT:
        a = check_expr(c, n->a);
        if (a.kind == VAL_TABLE) {
            fail(n->line, "say which column, like: %s statement.amount", token_keyword_text(n->op));
            return unknown();
        }
        if (n->op == TOK_AVERAGE_OF) {
            b = add_up(a, n->line);
            return b.kind == VAL_MONEY ? money_in(b.cur) : unknown();
        }
        return best_of(a, n->line);

    case N_GROUP:
        a = check_expr(c, n->a);
        return table_in(a.kind == VAL_TABLE ? a.cur : NULL);

    case N_VAT:
        a = check_expr(c, n->a);
        if (lookup(c, "VAT", 3) == NULL && !has_imports)
            fail(n->line, "set the VAT rate first, like: never_changes_ VAT = 15%%");
        if (known(a) && a.kind != VAL_MONEY) {
            fail(n->line, "%s needs money (got %s)", token_keyword_text(n->op), value_kind_name((ValueKind)a.kind));
            return unknown();
        }
        return money_in(a.cur);

    case N_DUPLICATES:
        a = check_expr(c, n->a);
        return a.kind == VAL_LIST || a.kind == VAL_TABLE ? a : unknown();

    case N_ASK:
        check_expr(c, n->a);
        return unknown();

    case N_TODAY:
    case N_END_OF_MONTH:
        if (n->a) check_expr(c, n->a);
        return of_kind(VAL_DATE);

    case N_DURATION:
        a = check_expr(c, n->a);
        if (known(a) && a.kind != VAL_NUMBER && a.kind != VAL_NOTHING) {
            fail(n->line, "the number of %s must be a whole number", unit_name((int)n->op));
            return unknown();
        }
        t = of_kind(VAL_DURATION);
        t.period = (int)n->op;
        if (a.literal && a.value.amount % PERRY_SCALE == 0) {
            t.literal = 1;
            t.value.amount = a.value.amount / PERRY_SCALE;
        }
        return t;

    case N_BETWEEN:
        check_expr(c, n->a);
        check_expr(c, n->b);
        return of_kind(VAL_NUMBER);

    default:
        return unknown();
    }
}

/* ------------------------------------------------------------------ */
/* Statements                                                         */
/* ------------------------------------------------------------------ */

static void store(Ctx *c, const Node *n, Ty t, int constant)
{
    Binding *mine = env_find(c->names, n->name, n->name_len);

    if (mine == NULL && c->in_function && env_find(c->globals, n->name, n->name_len) != NULL)
        fail(n->line, "'%.*s' belongs outside this function, so the function can't change it "
                      "- use give_back_ to hand back a result instead", n->name_len, n->name);
    else if (mine != NULL && constant && mine->constant)
        fail(n->line, "'%.*s' already has a value", n->name_len, n->name);
    else if (mine != NULL && mine->constant)
        fail(n->line, "'%.*s' never changes - it was set with never_changes_", n->name_len, n->name);
    env_put(c->names, n->name, n->name_len, t, constant);
}

/* Runs a block on a copy of the names, so branches don't leak into each other. */
static Env check_branch(Ctx *c, const Node *n)
{
    Env saved = *c->names, copy = env_copy(c->names);
    *c->names = copy;
    if (n != NULL) check_stmt(c, n);
    copy = *c->names;
    *c->names = saved;
    return copy;
}

static void join_into(Ctx *c, Env *a, Env *b)
{
    Env joined = env_join(a, b);
    env_free(c->names);
    *c->names = joined;
}

static int same_type(Ty a, Ty b)
{
    return a.kind == b.kind && strcmp(a.cur, b.cur) == 0 && a.period == b.period && a.blueprint == b.blueprint &&
           a.items == b.items;
}

/* Do two types say exactly the same thing? (Used to see when a loop has settled.) */
static int type_equal(Ty a, Ty b)
{
    int i, k;
    if (a.kind != b.kind || strcmp(a.cur, b.cur) != 0 || strcmp(a.base, b.base) != 0 || a.period != b.period ||
        a.blueprint != b.blueprint || a.literal != b.literal || a.items != b.items || a.nmust != b.nmust)
        return 0;
    if (a.literal && (a.value.amount != b.value.amount || a.value.kind != b.value.kind)) return 0;
    if ((a.elem == NULL) != (b.elem == NULL)) return 0;
    if (a.elem != NULL && !same_item(*a.elem, *b.elem)) return 0;
    for (i = 0; i < a.nmust; i++) {
        for (k = 0; k < b.nmust; k++) if (same_item(a.must[i], b.must[k])) break;
        if (k == b.nmust) return 0;
    }
    return 1;
}

static int env_equal(const Env *a, const Env *b)
{
    int i;
    if (a->count != b->count) return 0;
    for (i = 0; i < a->count; i++) {
        Binding *other = env_find((Env *)b, a->items[i].name, a->items[i].len);
        if (other == NULL || other->constant != a->items[i].constant || !type_equal(a->items[i].type, other->type))
            return 0;
    }
    return 1;
}

#define MAX_ROUNDS 12

/* Can the body leave a round early (so some later lines may not run)? */
static int can_leave_early(const Node *n)
{
    int i;
    if (n == NULL) return 0;
    if (n->kind == N_STOP || n->kind == N_SKIP || n->kind == N_RETURN || n->kind == N_TRY) return 1;
    if (can_leave_early(n->a) || can_leave_early(n->b) || can_leave_early(n->c)) return 1;
    for (i = 0; i < n->count; i++) if (can_leave_early(n->items[i])) return 1;
    return 0;
}

/* Does the body give this name a new value, or take items out of it? */
static int replaces(const Node *n, const char *name, int len)
{
    int i;
    if (n == NULL) return 0;
    if ((n->kind == N_ASSIGN || n->kind == N_CONSTANT || n->kind == N_FOR || n->kind == N_ERASE) &&
        n->name_len == len && memcmp(n->name, name, (size_t)len) == 0) return 1;
    if (n->kind == N_REMOVE && n->a->name_len == len && memcmp(n->a->name, name, (size_t)len) == 0) return 1;
    if (replaces(n->a, name, len) || replaces(n->b, name, len) || replaces(n->c, name, len)) return 1;
    for (i = 0; i < n->count; i++) if (replaces(n->items[i], name, len)) return 1;
    return 0;
}

/*
 * A loop body may run zero, one or many times. With go_through_ over a list
 * written out item by item, the body is checked once for each kind of item,
 * as if each item came round in turn.
 *
 * 'always' means every path surely runs: go_through_ a list of known items,
 * or exactly_ 3 times, with no way to leave a round early. Then whatever the
 * body surely adds to a list (add_where_) is surely in the list afterwards.
 */
static void check_loop(Ctx *c, const Node *body, const Node *loop_var, const Ty *items, int nitems, int always)
{
    Env result, paths[16], start;
    int pass, i, k, npaths;
    always = always && !can_leave_early(body);
    for (pass = 0; pass < MAX_ROUNDS; pass++) {    /* until another round changes nothing */
        start = env_copy(c->names);
        npaths = 0;
        result = env_copy(c->names);                     /* zero times round */
        for (i = 0; i < (nitems > 0 ? nitems : 1); i++) {
            Env path;
            for (k = 0; k < i; k++) if (same_type(items[k], items[i])) break;
            if (k < i) continue;
            if (loop_var != NULL) {
                Env saved = *c->names;
                Ty item = nitems > 0 ? items[i] : unknown();
                item.literal = 0;
                *c->names = env_copy(&saved);
                env_put(c->names, loop_var->name, loop_var->name_len, item, 0);
                path = check_branch(c, body);
                env_free(c->names);
                *c->names = saved;
            } else {
                path = check_branch(c, body);
            }
            if (always && npaths == 0) {                 /* never zero times round */
                env_free(&result);
                result = env_copy(&path);
            } else {
                Env joined = env_join(&result, &path);
                env_free(&result);
                result = joined;
            }
            if (npaths < 16) paths[npaths++] = path;
            else             env_free(&path);
        }
        if (always) {                                    /* how many rounds isn't known: positions aren't */
            for (i = 0; i < result.count; i++) {
                Binding *b = &result.items[i], *was = env_find(c->names, b->name, b->len);
                if (b->type.kind == VAL_LIST && (was == NULL || was->type.items != b->type.items)) {
                    b->type.items = NULL;
                    b->type.nitems = 0;
                }
            }
        }
        if (always && npaths < 16) {
            for (i = 0; i < result.count; i++) {
                Binding *b = &result.items[i];
                if (b->type.kind != VAL_LIST || replaces(body, b->name, b->len)) continue;
                for (k = 0; k < npaths; k++) {
                    Binding *p = env_find(&paths[k], b->name, b->len);
                    int m;
                    if (p == NULL || p->type.kind != VAL_LIST) continue;
                    for (m = 0; m < p->type.nmust; m++) must_add(&b->type, p->type.must[m]);
                }
            }
        }
        for (k = 0; k < npaths; k++) env_free(&paths[k]);
        env_free(c->names);
        *c->names = result;
        if (env_equal(&start, c->names)) { env_free(&start); return; }
        if (pass == MAX_ROUNDS - 1)                      /* not settled: forget what still changes */
            for (i = 0; i < c->names->count; i++) {
                Binding *was = env_find(&start, c->names->items[i].name, c->names->items[i].len);
                if (was == NULL || !type_equal(was->type, c->names->items[i].type)) c->names->items[i].type = unknown();
            }
        env_free(&start);
    }
}

static void collect_definitions(Module *m, const Node *program)
{
    int i;
    for (i = 0; i < program->count; i++) {
        const Node *n = program->items[i];
        if (n->kind == N_FUNCTION && find_function_in(m, n->name, n->name_len) == NULL) add_function(m, n, m);
        if (n->kind == N_BLUEPRINT) add_blueprint(n);
    }
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    char *text;
    long size;
    size_t got;
    if (f == NULL) return NULL;
    fseek(f, 0L, SEEK_END);
    size = ftell(f);
    rewind(f);
    text = (char *)malloc((size_t)size + 1);
    if (text == NULL) { fclose(f); return NULL; }
    got = fread(text, 1, (size_t)size, f);
    text[got] = '\0';
    fclose(f);
    return text;
}

/* steal_from_ budget_tools: budget_tools.perry next to this file, checked once with its own names */
static Module *load_module(Ctx *c, const Node *n)
{
    Module *m, *from = c->module;
    const char *slash = strrchr(from->path, '/');
    const char *saved_name = g_file_name, *saved_source = g_source, *saved_file = other_file;
    int saved_line = reached_at, i;
    char *source;
    Ctx top;

    for (i = 0; i < module_count; i++)
        if (name_is(n->name, n->name_len, modules[i]->name)) {
            if (modules[i]->loading) {
                fail(n->line, "'%.*s' is already being loaded - two modules can't steal_from_ each other",
                     n->name_len, n->name);
                return NULL;
            }
            return modules[i];
        }
    if (name_is(n->name, n->name_len, from->name)) {
        fail(n->line, "a file can't steal_from_ itself");
        return NULL;
    }
    if (module_count >= MAX_MODULES || n->name_len > 60) return NULL;

    m = (Module *)calloc(1, sizeof(Module));
    if (m == NULL) { fprintf(stderr, "PERRY: out of memory\n"); exit(1); }
    snprintf(m->name, sizeof m->name, "%.*s", n->name_len, n->name);
    m->path = (char *)malloc(strlen(from->path) + (size_t)n->name_len + 16);
    if (m->path == NULL) { fprintf(stderr, "PERRY: out of memory\n"); exit(1); }
    if (slash) sprintf(m->path, "%.*s/%.*s.perry", (int)(slash - from->path), from->path, n->name_len, n->name);
    else       sprintf(m->path, "%.*s.perry", n->name_len, n->name);
    source = read_file(m->path);
    if (source == NULL) {
        fail(n->line, "can't find the module file '%s'", m->path);
        free(m->path);
        free(m);
        return NULL;
    }
    modules[module_count++] = m;

    g_file_name = m->path;                  /* a parse error points at the module's own file */
    g_source    = source;
    m->program  = parse_program(source);
    g_file_name = saved_name;
    g_source    = saved_source;

    collect_definitions(m, m->program);
    top.names        = &m->globals;
    top.globals      = NULL;
    top.module       = m;
    top.in_function  = 0;
    top.returned     = unknown();
    top.has_returned = 0;
    if (other_file == NULL) reached_at = n->line;
    other_file = file_part(m->path);
    m->loading = 1;
    check_stmt(&top, m->program);
    m->loading = 0;
    other_file = saved_file;
    reached_at = saved_line;
    return m;
}

static int is_shared(Module *m, const char *name, int len)
{
    int i;
    if (!m->used_share) return 1;            /* no share_ at all: everything is shared */
    for (i = 0; i < m->nshared; i++)
        if (m->shared[i]->name_len == len && memcmp(m->shared[i]->name, name, (size_t)len) == 0) return 1;
    return 0;
}

/* Brings one name from a module into this file; taken values never change. */
static void take_name(Ctx *c, Module *m, const char *name, int len, int line)
{
    FnEntry *fe = find_function_in(m, name, len), *existing;
    Binding *v = env_find(&m->globals, name, len);

    if (fe != NULL) {
        existing = find_function_in(c->module, name, len);
        if (existing != NULL && existing->fn != fe->fn)
            fail(line, "there's already a function called '%.*s' here", len, name);
        if (existing == NULL) add_function(c->module, fe->fn, fe->home);
        return;
    }
    if (v != NULL) {
        Ty t = v->type;
        env_put(&c->module->globals, name, len, t, 1);
        return;
    }
    if (find_blueprint(name, len) != NULL) return;
    fail(line, "%s has no function or value called '%.*s'", m->name, len, name);
}

static void import_module(Ctx *c, const Node *n)
{
    Module *m;
    int i;

    if (name_is(n->name, n->name_len, "holidays_za")) {
        if (n->count > 0) fail(n->line, "holidays_za is built in - just write: steal_from_ holidays_za");
        return;
    }
    m = load_module(c, n);
    if (m == NULL) {
        has_imports = 1;                     /* its names can't be known */
        return;
    }
    if (n->count > 0) {                      /* in_that_ m steal_from_ a, b */
        for (i = 0; i < n->count; i++) {
            const Node *want = n->items[i];
            if (!is_shared(m, want->name, want->name_len)) {
                fail(want->line, "%s doesn't share '%.*s' - add 'share_ %.*s' to %s.perry",
                     m->name, want->name_len, want->name, want->name_len, want->name, m->name);
                continue;
            }
            take_name(c, m, want->name, want->name_len, want->line);
        }
        return;
    }
    if (m->used_share) {
        for (i = 0; i < m->nshared; i++)
            take_name(c, m, m->shared[i]->name, m->shared[i]->name_len, n->line);
    } else {
        for (i = 0; i < m->nfns; i++)
            if (m->fns[i].home == m) take_name(c, m, m->fns[i].fn->name, m->fns[i].fn->name_len, n->line);
        for (i = 0; i < m->globals.count; i++)
            take_name(c, m, m->globals.items[i].name, m->globals.items[i].len, n->line);
    }
}

static void check_stmt(Ctx *c, const Node *n)
{
    Ty t;
    int i;

    if (n == NULL) return;
    switch (n->kind) {
    case N_BLOCK:
        for (i = 0; i < n->count; i++) check_stmt(c, n->items[i]);
        return;

    case N_ASSIGN:
        store(c, n, check_expr(c, n->a), 0);
        return;

    case N_CONSTANT:
        store(c, n, check_expr(c, n->a), 1);
        return;

    case N_SHOW:
        for (i = 0; i < n->count; i++) check_expr(c, n->items[i]);
        return;

    case N_IF: {
        Env yes, no;
        condition(check_expr(c, n->a), n->line, "check_if_");
        yes = check_branch(c, n->b);
        no  = check_branch(c, n->c);
        join_into(c, &yes, &no);
        env_free(&yes);
        env_free(&no);
        return;
    }

    case N_REPEAT:
        t = check_expr(c, n->a);
        if (known(t) && t.kind != VAL_NUMBER)
            fail(n->line, "exactly_ needs a whole number of times, like: exactly_ 12 times:");
        check_loop(c, n->b, NULL, NULL, 0, t.literal && t.value.amount >= PERRY_SCALE);
        return;

    case N_WHILE:
        condition(check_expr(c, n->a), n->line, "keep_going_while_");
        check_loop(c, n->b, NULL, NULL, 0, 0);
        return;

    case N_FOR:
        t = check_expr(c, n->a);
        if (known(t) && t.kind != VAL_LIST && t.kind != VAL_TABLE)
            fail(n->line, "go_through_ needs a list (got %s)", value_kind_name((ValueKind)t.kind));
        if (t.kind == VAL_LIST && t.items != NULL) {
            check_loop(c, n->b, n, t.items, t.nitems, t.nitems > 0);
        } else {
            Ty item = element_of(t);
            if (t.kind == VAL_TABLE) {                   /* a row: its money is in the table's currency */
                item = of_kind(VAL_ROW);
                memcpy(item.cur, t.cur, 4);
            }
            check_loop(c, n->b, n, &item, 1, 0);
        }
        return;

    case N_REQUIRE:
        condition(check_expr(c, n->a), n->line, "not_unless_");
        return;

    case N_TRY: {
        Env body, handled, joined;
        int all = 0, cur = 0;
        for (i = 0; i < n->count; i++) {
            if (n->items[i]->op == TOK_THERES_AN_ERROR) all = 1;
            if (n->items[i]->op == TOK_THERES_A_CURRENCY_ERROR) cur = 1;
        }
        catch_all += all;
        catch_currency += cur;
        body = check_branch(c, n->a);
        catch_all -= all;
        catch_currency -= cur;
        for (i = 0; i < n->count; i++) {
            handled = check_branch(c, n->items[i]->b);
            joined = env_join(&body, &handled);
            env_free(&body);
            env_free(&handled);
            body = joined;
        }
        join_into(c, &body, c->names);           /* the body may stop part-way */
        env_free(&body);
        if (n->c != NULL) check_stmt(c, n->c);
        return;
    }

    case N_BOOK: {
        char currency[4] = "";
        check_expr(c, n->a);
        for (i = 0; i < n->count; i++) {
            const Node *e = n->items[i];
            t = check_expr(c, e->a);
            if (known(t) && t.kind != VAL_MONEY) {
                fail(e->line, "debit_ and credit_ need money, like: debit_ rent_expense 8000.00 ZAR");
                continue;
            }
            if (t.kind != VAL_MONEY || !t.cur[0]) continue;
            if (currency[0] && strcmp(currency, t.cur) != 0)
                fail_currency(e->line, "one book_ entry can't mix %s and %s", currency, t.cur);
            else
                memcpy(currency, t.cur, 4);
        }
        return;
    }

    case N_SAVE:
        check_expr(c, n->a);
        check_expr(c, n->b);
        return;

    case N_ADD: {
        Ty list = check_expr(c, n->a), item = check_expr(c, n->b);
        Binding *var = lookup(c, n->a->name, n->a->name_len);
        if (list.kind == VAL_LIST && var != NULL) {
            Ty grown = of_kind(VAL_LIST);
            item.literal = 0;
            grown.elem = boxed(list.elem ? merge(*list.elem, item) : item);
            if (list.items != NULL || list.elem == NULL) {
                grown.items  = (Ty *)calloc((size_t)list.nitems + 1, sizeof(Ty));
                grown.nitems = list.nitems + 1;
                if (grown.items == NULL) { fprintf(stderr, "PERRY: out of memory\n"); exit(1); }
                if (list.nitems > 0) memcpy(grown.items, list.items, sizeof(Ty) * (size_t)list.nitems);
                grown.items[list.nitems] = item;
            }
            grown.must = list.must;
            grown.nmust = list.nmust;
            must_add(&grown, item);
            if (env_find(c->names, n->a->name, n->a->name_len) != NULL) store(c, n->a, grown, 0);
        }
        return;
    }

    case N_ERASE:
        env_remove(c->names, n->name, n->name_len);
        return;

    case N_ALARM:
        check_expr(c, n->a);
        return;

    case N_IMPORT:
        import_module(c, n);
        return;

    case N_SHARE:
        c->module->used_share = 1;
        for (i = 0; i < n->count; i++) {
            const Node *name = n->items[i];
            if (find_function_in(c->module, name->name, name->name_len) == NULL &&
                lookup(c, name->name, name->name_len) == NULL && find_blueprint(name->name, name->name_len) == NULL)
                fail(name->line, "can't share '%.*s' - nothing with that name exists yet (put share_ after it)",
                     name->name_len, name->name);
            c->module->shared = (const Node **)realloc((void *)c->module->shared,
                                                       sizeof(Node *) * (size_t)(c->module->nshared + 1));
            if (c->module->shared == NULL) { fprintf(stderr, "PERRY: out of memory\n"); exit(1); }
            c->module->shared[c->module->nshared++] = name;
        }
        return;

    case N_BLUEPRINT:
    case N_FUNCTION:
    case N_STOP:
    case N_SKIP:
    case N_BECAUSE:
        return;

    case N_RETURN:
        t = check_expr(c, n->a);
        c->returned = c->has_returned ? merge(c->returned, t) : t;
        c->has_returned = 1;
        return;

    case N_CALL:
        check_call(c, n, 0);
        return;

    case N_REMOVE:
    case N_SORT: {
        Ty list = check_expr(c, n->a), target = unknown(), tol = unknown();
        if (n->b) target = check_expr(c, n->b);
        if (n->c) tol = check_expr(c, n->c);
        if (n->kind == N_REMOVE) filter_by(list, n, target, tol);
        if (n->kind == N_REMOVE && list.items != NULL && env_find(c->names, n->a->name, n->a->name_len) != NULL) {
            list.items = NULL;
            list.nitems = 0;
            store(c, n->a, list, 0);
        }
        return;
    }

    case N_TAX_TABLE: {
        Ty tax = of_kind(VAL_TAX);
        for (i = 0; i < n->count; i++) {
            const Node *br = n->items[i];
            Ty rate = check_expr(c, br->b);
            if (known(rate) && (rate.kind != VAL_PERCENT || (rate.period != PERIOD_NONE && rate.period != P_UNKNOWN)))
                fail(br->line, "taxed_at_ needs a percent, like: taxed_at_ 18%%");
            if (br->a == NULL) continue;
            t = check_expr(c, br->a);
            if (known(t) && t.kind != VAL_MONEY) {
                fail(br->line, "up_to_ needs money, like: up_to_ 237100.00 ZAR");
                continue;
            }
            if (t.kind != VAL_MONEY || !t.cur[0]) continue;
            if (tax.cur[0] && strcmp(tax.cur, t.cur) != 0)
                fail_currency(br->line, "can't mix %s and %s - convert one currency first", tax.cur, t.cur);
            else
                memcpy(tax.cur, t.cur, 4);
        }
        if (n->count < 2) tax.cur[0] = '\0';     /* one bracket: any currency works */
        store(c, n, tax, 1);
        return;
    }

    default:
        check_expr(c, n);
        return;
    }
}

/* ------------------------------------------------------------------ */
/* Entry point                                                        */
/* ------------------------------------------------------------------ */

static int by_line(const void *x, const void *y)
{
    const Mistake *a = (const Mistake *)x, *b = (const Mistake *)y;
    return a->line != b->line ? a->line - b->line : a->order - b->order;
}

static void print_source_line(int line)
{
    const char *p = g_source, *end;
    int n = 1;
    if (p == NULL || line < 1) return;
    while (*p && n < line) {
        if (*p == '\n') n++;
        p++;
    }
    if (*p == '\0') return;
    while (*p == ' ' || *p == '\t') p++;
    end = p;
    while (*end && *end != '\n' && *end != '\r') end++;
    if (end > p) fprintf(stderr, "%6d | %.*s\n", line, (int)(end - p), p);
}

int check_program(const Node *program)
{
    Ctx c;
    int i, errors = 0;

    snprintf(main_module.name, sizeof main_module.name, "main");
    main_module.path    = (char *)g_file_name;
    main_module.program = program;
    collect_definitions(&main_module, program);
    c.names        = &main_module.globals;
    c.globals      = NULL;
    c.module       = &main_module;
    c.in_function  = 0;
    c.returned     = unknown();
    c.has_returned = 0;
    main_module.loading = 1;
    check_stmt(&c, program);
    main_module.loading = 0;

    /* this file's functions nobody calls are still checked, with inputs of any type */
    for (i = 0; i < main_module.nfns; i++)
        if (main_module.fns[i].home == &main_module && !was_checked(main_module.fns[i].fn))
            check_body(&main_module.fns[i], NULL, main_module.fns[i].fn->line);

    if (stats_on > 0) {
        int known_count = 0;
        for (i = 0; i < seen_capacity; i++) if (seen[i].node != NULL && seen[i].known) known_count++;
        fprintf(stderr, "%s: stats: %d expressions, %d with a known type\n", g_file_name, seen_count, known_count);
    }
    if (mistake_count > 1) qsort(mistakes, (size_t)mistake_count, sizeof(Mistake), by_line);
    for (i = 0; i < mistake_count; i++) {
        fprintf(stderr, "%s:%d: %s: %s\n", g_file_name, mistakes[i].line,
                mistakes[i].note ? "note" : "error", mistakes[i].text);
        if (mistakes[i].note)
            fprintf(stderr, "        (caught by even_if_ here, so it won't stop the program)\n");
        print_source_line(mistakes[i].line);
        if (!mistakes[i].note) errors++;
    }
    return errors;
}
