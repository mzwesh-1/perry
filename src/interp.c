/*
 * interp.c - a tree-walking interpreter.
 *
 * It walks the syntax tree from the top. eval() works out the value of
 * an expression; run() carries out a statement. This is where Perry's
 * money rules live: you can't add ZAR to USD, you can't add a plain
 * number to money, and money is always rounded to whole cents.
 *
 * Every time a name gets a value, Perry also remembers HOW it got it
 * (the formula, and the formula with the numbers filled in). That is
 * what because_is_ prints.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <setjmp.h>
#include <time.h>
#include <ctype.h>
#include "interp.h"
#include "errors.h"
#include "csv.h"
#include "finance.h"
#include "dates.h"
#include "parser.h"

/* ------------------------------------------------------------------ */
/* Errors                                                             */
/*                                                                    */
/* A runtime error either stops the program with a message, or - if   */
/* it happens inside try_this_ - jumps back to that try_this_ block   */
/* (with C's setjmp/longjmp) so an even_if_ block can handle it.      */
/* ------------------------------------------------------------------ */

typedef enum { ERR_GENERAL, ERR_CURRENCY, ERR_FILE, ERR_BALANCE } ErrorKind;

#define MAX_TRY_DEPTH 64

typedef struct Scope Scope;
typedef struct Module Module;

typedef struct {
    jmp_buf env;
    Scope  *locals;          /* what to restore after jumping back */
    Module *module;
    int     loop_depth;
    int     call_depth;
} TryFrame;

static TryFrame  try_stack[MAX_TRY_DEPTH];
static int       try_depth;
static ErrorKind error_kind;
static int       error_line;
static char      error_message[512];

static void raise_error(ErrorKind kind, int line, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(error_message, sizeof error_message, fmt, args);
    va_end(args);
    error_kind = kind;
    error_line = line;
    if (try_depth > 0)
        longjmp(try_stack[try_depth - 1].env, 1);
    perry_fail(EXIT_RUNTIME_ERROR, line, "%s", error_message);
}

#define fail(line, ...) raise_error(ERR_GENERAL, (line), __VA_ARGS__)

/* For finance.c: an error that try_this_ can catch. */
void perry_runtime_fail(int line, const char *fmt, ...)
{
    char message[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof message, fmt, args);
    va_end(args);
    raise_error(ERR_GENERAL, line, "%s", message);
}

#define MAX_CALL_DEPTH 500
#define MAX_SPLIT      100000

static void out_of_memory(void)
{
    fprintf(stderr, "PERRY: out of memory\n");
    exit(1);
}

/* ------------------------------------------------------------------ */
/* Variables and scopes                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *name;
    int         name_len;
    Value       value;
    int         constant;
    const Node *source;     /* the expression that gave the value      */
    char       *formula;    /* "price + tax"                           */
    char       *worked;     /* "4000.00 ZAR + 600.00 ZAR"              */
    char        note[64];   /* "given", "passed into late_fee", ...    */
} Variable;

struct Scope {
    Variable *vars;
    int       count;
    int       capacity;
};

/*
 * Every .perry file is a module with its own names and functions.
 * The program you run is the main module; steal_from_ loads others.
 */
typedef struct {
    const Node *fn;          /* the define_ node                       */
    Module     *home;        /* the module it was written in           */
} FunctionEntry;

struct Module {
    char           name[64];
    const char    *path;
    const char    *source;
    Scope          globals;
    FunctionEntry *fns;
    int            nfns;
    const Node   **shared;   /* names listed with share_ (N_VAR nodes) */
    int            nshared;
    int            used_share;
    int            loading, loaded;
};

#define MAX_MODULES 64

static Module  main_module;
static Module *modules[MAX_MODULES];
static int     module_count;
static Module *current_module;   /* whose names are being used right now  */
static Scope  *globals;          /* = &current_module->globals             */

static Scope *locals;            /* the running function's scope, or NULL */
static int    loop_depth;
static int    call_depth;
static Value  return_value;

/* Switches to a module's names, and its file for error messages. */
static void enter_module(Module *m)
{
    current_module = m;
    globals        = &m->globals;
    g_file_name    = m->path;
    g_source       = m->source;
}

static Variable *scope_find(Scope *s, const char *name, int len)
{
    int i;
    for (i = 0; i < s->count; i++)
        if (s->vars[i].name_len == len && memcmp(s->vars[i].name, name, (size_t)len) == 0)
            return &s->vars[i];
    return NULL;
}

static Variable *find_var(const char *name, int len)
{
    Variable *v = NULL;
    if (locals != NULL) v = scope_find(locals, name, len);
    if (v == NULL)      v = scope_find(globals, name, len);
    return v;
}

static Variable *scope_add(Scope *s, const char *name, int len)
{
    Variable *v;
    if (s->count == s->capacity) {
        s->capacity = s->capacity ? s->capacity * 2 : 16;
        s->vars = (Variable *)realloc(s->vars, sizeof(Variable) * (size_t)s->capacity);
        if (s->vars == NULL) out_of_memory();
    }
    v = &s->vars[s->count++];
    memset(v, 0, sizeof *v);
    v->name     = name;
    v->name_len = len;
    return v;
}

static void forget_explanation(Variable *v)
{
    free(v->formula);
    free(v->worked);
    v->formula = NULL;
    v->worked  = NULL;
    v->source  = NULL;
    v->note[0] = '\0';
}

static void scope_free(Scope *s)
{
    int i;
    for (i = 0; i < s->count; i++) forget_explanation(&s->vars[i]);
    free(s->vars);
}

static FunctionEntry *find_function_in(Module *m, const char *name, int len)
{
    int i;
    for (i = 0; i < m->nfns; i++)
        if (m->fns[i].fn->name_len == len && memcmp(m->fns[i].fn->name, name, (size_t)len) == 0)
            return &m->fns[i];
    return NULL;
}

static FunctionEntry *find_function(const char *name, int len)
{
    return find_function_in(current_module, name, len);
}

static void add_function(Module *m, const Node *fn, Module *home)
{
    m->fns = (FunctionEntry *)realloc(m->fns, sizeof(FunctionEntry) * (size_t)(m->nfns + 1));
    if (m->fns == NULL) out_of_memory();
    m->fns[m->nfns].fn   = fn;
    m->fns[m->nfns].home = home;
    m->nfns++;
}

/* blueprint_ record types - one list for the whole program */
typedef struct {
    const Node *def;
    char      **cols;
} Blueprint;

static Blueprint *blueprints;
static int        blueprint_count;

static Blueprint *find_blueprint(const char *name, int len)
{
    int i;
    for (i = 0; i < blueprint_count; i++)
        if (blueprints[i].def->name_len == len && memcmp(blueprints[i].def->name, name, (size_t)len) == 0)
            return &blueprints[i];
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static Value make_number(ValueKind kind, dec_t amount)
{
    Value v;
    memset(&v, 0, sizeof v);
    v.kind   = kind;
    v.amount = amount;
    return v;
}

static Value make_money(dec_t amount, const char *currency)
{
    Value v = make_number(VAL_MONEY, 0);
    memcpy(v.currency, currency, 4);
    v.amount = dec_round(amount, currency_decimals(currency));
    return v;
}

static Value make_percent(dec_t amount, int period)
{
    Value v = make_number(VAL_PERCENT, amount);
    v.period = period;
    return v;
}

/* 1% per month and 12% per year can't be mixed directly. */
static void check_same_period(Value a, Value b, int line)
{
    if (a.kind != VAL_PERCENT || b.kind != VAL_PERCENT || a.period == b.period) return;
    if (a.period == PERIOD_NONE || b.period == PERIOD_NONE)
        fail(line, "can't mix a rate per %s with a percent that has no period - add 'per %s' to it",
             period_name(a.period ? a.period : b.period), period_name(a.period ? a.period : b.period));
    fail(line, "can't mix a rate per %s with a rate per %s - convert one, like: rate per %s",
         period_name(a.period), period_name(b.period), period_name(a.period));
}

static Value make_bool(int b)
{
    Value v;
    memset(&v, 0, sizeof v);
    v.kind    = VAL_BOOL;
    v.boolean = b ? 1 : 0;
    return v;
}

static Value make_nothing(void)
{
    Value v;
    memset(&v, 0, sizeof v);
    v.kind = VAL_NOTHING;
    return v;
}

static int is_true(Value v, int line, const char *where)
{
    if (v.kind != VAL_BOOL)
        fail(line, "%s needs a yes/no condition, like: total is_greater_than_ 100.00 ZAR "
                   "(got %s)", where, value_kind_name(v.kind));
    return v.boolean;
}

static void too_big(int line)
{
    fail(line, "the result is too big for Perry");
}

static long long whole_number(Value v, int line, const char *what)
{
    if (v.kind != VAL_NUMBER || v.amount % PERRY_SCALE != 0)
        fail(line, "%s must be a whole number", what);
    if (v.amount / PERRY_SCALE > 1000000000000000LL || v.amount / PERRY_SCALE < -1000000000000000LL)
        fail(line, "%s is too big", what);
    return (long long)(v.amount / PERRY_SCALE);
}

/* Money can only meet money of the same currency. */
static void check_same_currency(Value a, Value b, int line, const char *verb)
{
    char text[64];
    if (a.kind == VAL_MONEY && b.kind == VAL_MONEY) {
        if (strcmp(a.currency, b.currency) != 0)
            raise_error(ERR_CURRENCY, line, "can't %s %s and %s - convert one currency first",
                        verb, a.currency, b.currency);
        return;
    }
    if (a.kind == VAL_MONEY || b.kind == VAL_MONEY) {
        Value plain = a.kind == VAL_MONEY ? b : a;
        Value money = a.kind == VAL_MONEY ? a : b;
        char hint[64];
        value_format(text, sizeof text, plain);
        if (plain.kind == VAL_NUMBER) {
            Value as_money = plain;          /* show 5 as "5.00 ZAR" */
            as_money.kind = VAL_MONEY;
            memcpy(as_money.currency, money.currency, 4);
            value_format(hint, sizeof hint, as_money);
            raise_error(ERR_CURRENCY, line, "can't %s money and %s (%s) - write it as money, like %s",
                        verb, value_kind_name(plain.kind), text, hint);
        }
        raise_error(ERR_CURRENCY, line, "can't %s money and %s (%s)", verb, value_kind_name(plain.kind), text);
    }
}

/* ------------------------------------------------------------------ */
/* Dates                                                              */
/* ------------------------------------------------------------------ */

static int use_za_holidays;          /* switched on by: steal_from_ holidays_za */

#define MAX_DURATION 1000000         /* about 2700 years of days */

static Value make_date(long long days, int line)
{
    Value v = make_nothing();
    if (days < days_from_date(1, 1, 1) || days > days_from_date(9999, 12, 31))
        fail(line, "that date is outside the years 1 to 9999");
    v.kind   = VAL_DATE;
    v.amount = days;
    return v;
}

static Value make_duration(long long count, int unit, int line)
{
    Value v = make_nothing();
    if (count > MAX_DURATION || count < -MAX_DURATION) fail(line, "that length of time is too long");
    v.kind   = VAL_DURATION;
    v.amount = count;
    v.period = unit;
    return v;
}

static const char *unit_name(int unit)
{
    return unit == DUR_MONTH ? "months" : unit == DUR_YEAR ? "years" :
           unit == DUR_BUSINESS ? "business days" : "days";
}

static long long add_business_days(long long day, long long n)
{
    long long step = n > 0 ? 1 : -1;
    while (n != 0) {
        day += step;
        if (is_business_day(day, use_za_holidays)) n -= step;
    }
    return day;
}

/* date + length of time (sign = -1 to go backwards) */
static Value shift_date(Value date, Value dur, int sign, int line)
{
    long long n = dur.amount * sign;
    switch (dur.period) {
    case DUR_MONTH:    return make_date(add_months(date.amount, n), line);
    case DUR_YEAR:     return make_date(add_months(date.amount, n * 12), line);
    case DUR_BUSINESS: return make_date(add_business_days(date.amount, n), line);
    default:           return make_date(date.amount + n, line);
    }
}

/* business days after 'from', up to and including 'to' */
static long long count_business_days(long long from, long long to)
{
    long long count = 0, d, lo = from < to ? from : to, hi = from < to ? to : from;
    for (d = lo + 1; d <= hi; d++)
        if (is_business_day(d, use_za_holidays)) count++;
    return from <= to ? count : -count;
}

static Value date_arithmetic(TokenKind op, Value a, Value b, int line)
{
    if (op == TOK_PLUS) {
        if (a.kind == VAL_DATE && b.kind == VAL_DURATION) return shift_date(a, b, 1, line);
        if (a.kind == VAL_DURATION && b.kind == VAL_DATE) return shift_date(b, a, 1, line);
    }
    if (op == TOK_MINUS) {
        if (a.kind == VAL_DATE && b.kind == VAL_DURATION) return shift_date(a, b, -1, line);
        if (a.kind == VAL_DATE && b.kind == VAL_DATE)       /* how many days apart */
            return make_number(VAL_NUMBER, (a.amount - b.amount) * PERRY_SCALE);
    }
    if ((op == TOK_PLUS || op == TOK_MINUS) && a.kind == VAL_DURATION && b.kind == VAL_DURATION) {
        if (a.period != b.period)
            fail(line, "can't add %s and %s together", unit_name(a.period), unit_name(b.period));
        return make_duration(op == TOK_PLUS ? a.amount + b.amount : a.amount - b.amount, a.period, line);
    }
    if (op == TOK_STAR && (a.kind == VAL_DURATION || b.kind == VAL_DURATION)) {
        Value dur = a.kind == VAL_DURATION ? a : b, times = a.kind == VAL_DURATION ? b : a;
        if (times.kind == VAL_NUMBER && times.amount % PERRY_SCALE == 0)
            return make_duration(dur.amount * (times.amount / PERRY_SCALE), dur.period, line);
    }
    if ((a.kind == VAL_DATE || b.kind == VAL_DATE) &&
        (a.kind == VAL_NUMBER || b.kind == VAL_NUMBER))
        fail(line, "a plain number can't be added to a date - say what it is, like: 5 days or 1 month");
    fail(line, "can't do that with %s and %s", value_kind_name(a.kind), value_kind_name(b.kind));
    return a;
}

/* ------------------------------------------------------------------ */
/* Arithmetic                                                         */
/* ------------------------------------------------------------------ */

static Value arithmetic(TokenKind op, Value a, Value b, int line)
{
    dec_t r = 0;

    if (a.kind == VAL_DATE || a.kind == VAL_DURATION || b.kind == VAL_DATE || b.kind == VAL_DURATION)
        return date_arithmetic(op, a, b, line);

    /* text + text joins the text */
    if (a.kind == VAL_TEXT && b.kind == VAL_TEXT && op == TOK_PLUS) {
        Value v = a;
        char *joined = (char *)malloc((size_t)(a.text_len + b.text_len + 1));
        if (joined == NULL) out_of_memory();
        memcpy(joined, a.text, (size_t)a.text_len);
        memcpy(joined + a.text_len, b.text, (size_t)b.text_len);
        v.text = joined;
        v.text_len = a.text_len + b.text_len;
        return v;
    }
    if (!value_is_numeric(a) || !value_is_numeric(b)) {
        Value bad = value_is_numeric(a) ? b : a;
        if (bad.kind == VAL_TEXT)
            fail(line, "can't do maths with text (\"%.*s\") - numbers and money are written without quotes",
                 bad.text_len > 40 ? 40 : bad.text_len, bad.text);
        fail(line, "can't do maths with %s", value_kind_name(bad.kind));
    }

    switch (op) {
    case TOK_OF:
        /* 15% of price  ==  price * 15% */
        if (a.kind != VAL_PERCENT)
            fail(line, "'of' needs a percent on its left, like: 15%% of price");
        return arithmetic(TOK_STAR, b, a, line);

    case TOK_PLUS:
    case TOK_MINUS:
        if (a.kind == VAL_MONEY || b.kind == VAL_MONEY) {
            check_same_currency(a, b, line, op == TOK_PLUS ? "add" : "subtract");
            if (!(op == TOK_PLUS ? dec_add(a.amount, b.amount, &r) : dec_sub(a.amount, b.amount, &r)))
                too_big(line);
            return make_money(r, a.currency);
        }
        if (a.kind != b.kind)
            fail(line, "can't mix a percent and a plain number with + or - "
                       "(did you mean: 15%% of amount?)");
        if (a.kind == VAL_PERCENT) {
            check_same_period(a, b, line);
            if (!(op == TOK_PLUS ? dec_add(percent_fraction(a), percent_fraction(b), &r)
                                 : dec_sub(percent_fraction(a), percent_fraction(b), &r)))
                too_big(line);
            return make_percent(r, a.period);
        }
        if (!(op == TOK_PLUS ? dec_add(a.amount, b.amount, &r) : dec_sub(a.amount, b.amount, &r)))
            too_big(line);
        return make_number(a.kind, r);

    case TOK_STAR:
        if (a.kind == VAL_MONEY && b.kind == VAL_MONEY)
            fail(line, "can't multiply money by money");
        /* rates like 11.75% per year turned monthly are divided here, once */
        if (!dec_mul_frac(a.amount, b.amount, value_divisor(a) * value_divisor(b), &r)) too_big(line);
        if (a.kind == VAL_MONEY) return make_money(r, a.currency);
        if (b.kind == VAL_MONEY) return make_money(r, b.currency);
        if (a.kind == VAL_PERCENT && b.kind == VAL_PERCENT) return make_percent(r, PERIOD_NONE);
        if (a.kind == VAL_PERCENT) return make_percent(r, a.period);
        if (b.kind == VAL_PERCENT) return make_percent(r, b.period);
        return make_number(VAL_NUMBER, r);

    case TOK_SLASH:
        if (b.amount == 0) fail(line, "can't divide by zero");
        if (b.kind == VAL_MONEY && a.kind != VAL_MONEY)
            fail(line, "can't divide %s by money", value_kind_name(a.kind));
        if (a.kind == VAL_MONEY && b.kind == VAL_MONEY) {
            check_same_currency(a, b, line, "divide");
            if (!dec_div(a.amount, b.amount, &r)) too_big(line);
            return make_number(VAL_NUMBER, r);           /* a ratio */
        }
        /* a percent divided by a whole number stays exact: 11.75% / 12 */
        if (a.kind == VAL_PERCENT && b.kind == VAL_NUMBER && b.amount > 0 &&
            b.amount % PERRY_SCALE == 0 && b.amount / PERRY_SCALE <= 1000000) {
            Value v = a;
            v.divisor = value_divisor(a) * (long long)(b.amount / PERRY_SCALE);
            return v;
        }
        {
            dec_t top    = a.kind == VAL_PERCENT ? percent_fraction(a) : a.amount;
            dec_t bottom = b.kind == VAL_PERCENT ? percent_fraction(b) : b.amount;
            if (bottom == 0) fail(line, "can't divide by zero");
            if (!dec_div(top, bottom, &r)) too_big(line);
        }
        if (a.kind == VAL_MONEY) return make_money(r, a.currency);
        if (a.kind == VAL_PERCENT && b.kind == VAL_NUMBER) return make_percent(r, a.period);
        return make_number(VAL_NUMBER, r);

    default:
        fail(line, "unknown operator");
    }
    return a;
}

/* ------------------------------------------------------------------ */
/* Comparisons                                                        */
/* ------------------------------------------------------------------ */

/* The amount to compare with: a converted rate is divided first. */
static dec_t plain_amount(Value v)
{
    return v.kind == VAL_PERCENT ? percent_fraction(v) : v.amount;
}

static int values_equal(Value a, Value b, int line)
{
    int i;
    if (value_is_numeric(a) && value_is_numeric(b)) {
        check_same_currency(a, b, line, "compare");
        check_same_period(a, b, line);
        return plain_amount(a) == plain_amount(b);
    }
    if (a.kind == VAL_DATE && b.kind == VAL_DATE) return a.amount == b.amount;
    if (a.kind == VAL_DURATION && b.kind == VAL_DURATION) {
        if (a.period != b.period)
            fail(line, "can't compare %s with %s", unit_name(a.period), unit_name(b.period));
        return a.amount == b.amount;
    }
    if (a.kind == VAL_ROW && b.kind == VAL_ROW) {  /* records: same fields, same values */
        if (a.table->ncols != b.table->ncols) return 0;
        for (i = 0; i < a.table->ncols; i++) {
            Value x = table_cell(a.table, a.row, i), y = table_cell(b.table, b.row, i);
            if (strcmp(a.table->cols[i], b.table->cols[i]) != 0) return 0;
            if (x.kind != y.kind) return 0;
            if (x.kind == VAL_MONEY && strcmp(x.currency, y.currency) != 0) return 0;
            if (!values_equal(x, y, line)) return 0;
        }
        return 1;
    }
    if (a.kind == VAL_TABLE || a.kind == VAL_ROW || a.kind == VAL_TAX ||
        b.kind == VAL_TABLE || b.kind == VAL_ROW || b.kind == VAL_TAX)
        fail(line, "can't compare whole tables - compare a column or a cell instead");
    if (a.kind != b.kind)
        fail(line, "can't compare %s with %s", value_kind_name(a.kind), value_kind_name(b.kind));
    switch (a.kind) {
    case VAL_TEXT:    return a.text_len == b.text_len && memcmp(a.text, b.text, (size_t)a.text_len) == 0;
    case VAL_BOOL:    return a.boolean == b.boolean;
    case VAL_NOTHING: return 1;
    case VAL_LIST:
        if (a.list->count != b.list->count) return 0;
        for (i = 0; i < a.list->count; i++)
            if (!values_equal(a.list->items[i], b.list->items[i], line)) return 0;
        return 1;
    default:          return 0;
    }
}

static Value compare(TokenKind op, Value a, Value b, int line)
{
    if (op == TOK_IS_EQUAL_TO)     return make_bool(values_equal(a, b, line));
    if (op == TOK_IS_NOT_EQUAL_TO) return make_bool(!values_equal(a, b, line));

    /* dates: earlier is smaller;  lengths of time: in the same unit */
    if ((a.kind == VAL_DATE && b.kind == VAL_DATE) ||
        (a.kind == VAL_DURATION && b.kind == VAL_DURATION)) {
        if (a.kind == VAL_DURATION && a.period != b.period)
            fail(line, "can't compare %s with %s", unit_name(a.period), unit_name(b.period));
        switch (op) {
        case TOK_IS_GREATER_THAN:     return make_bool(a.amount >  b.amount);
        case TOK_IS_LESS_THAN:        return make_bool(a.amount <  b.amount);
        case TOK_IS_GREATER_EQUAL_TO: return make_bool(a.amount >= b.amount);
        case TOK_IS_LESS_EQUAL_TO:    return make_bool(a.amount <= b.amount);
        default:                      break;
        }
    }
    if (a.kind == VAL_DATE || b.kind == VAL_DATE)
        fail(line, "can't compare %s with %s", value_kind_name(a.kind), value_kind_name(b.kind));

    if (!value_is_numeric(a) || !value_is_numeric(b))
        fail(line, "only numbers, percents and money can be bigger or smaller");
    check_same_currency(a, b, line, "compare");
    check_same_period(a, b, line);

    switch (op) {
    case TOK_IS_GREATER_THAN:     return make_bool(plain_amount(a) >  plain_amount(b));
    case TOK_IS_LESS_THAN:        return make_bool(plain_amount(a) <  plain_amount(b));
    case TOK_IS_GREATER_EQUAL_TO: return make_bool(plain_amount(a) >= plain_amount(b));
    case TOK_IS_LESS_EQUAL_TO:    return make_bool(plain_amount(a) <= plain_amount(b));
    default:                      fail(line, "unknown comparison");
    }
    return make_bool(0);
}

/* a is_similar_to_ b within_ tolerance */
static Value similar(TokenKind op, Value a, Value b, Value tol, int line)
{
    dec_t diff;

    if (!value_is_numeric(a) || !value_is_numeric(b) || !value_is_numeric(tol))
        fail(line, "is_similar_to_ works on numbers, percents and money");
    check_same_currency(a, b, line, "compare");
    if (a.kind == VAL_MONEY || tol.kind == VAL_MONEY) check_same_currency(a, tol, line, "compare");
    if (tol.amount < 0) fail(line, "the within_ tolerance can't be negative");

    check_same_period(a, b, line);

    if (!dec_sub(plain_amount(a), plain_amount(b), &diff)) too_big(line);
    if (diff < 0) diff = -diff;

    if (op == TOK_IS_SIMILAR_TO) return make_bool(diff <= plain_amount(tol));
    return make_bool(diff > plain_amount(tol));
}

/* ------------------------------------------------------------------ */
/* Lists and split                                                    */
/* ------------------------------------------------------------------ */

static PerryList *need_list(Value v, int line, const char *where)
{
    if (v.kind != VAL_LIST)
        fail(line, "%s needs a list (got %s)", where, value_kind_name(v.kind));
    return v.list;
}

/*
 * split 100.00 ZAR into 3  ->  [33.34 ZAR, 33.33 ZAR, 33.33 ZAR]
 * Works in whole cents; the leftover cents go one each to the first
 * parts, so the parts always add up to exactly the original amount.
 */
static Value split_money(Value amount, Value parts, int line)
{
    Value result = make_nothing();
    long long n, i;
    dec_t step, units, base, left;
    int negative;

    if (amount.kind != VAL_MONEY)
        fail(line, "split works on money, like: split 100.00 ZAR into 3");
    n = whole_number(parts, line, "the number of parts");
    if (n < 1) fail(line, "can't split into %lld parts", n);
    if (n > MAX_SPLIT) fail(line, "can't split into more than %d parts", MAX_SPLIT);

    step  = 1;
    for (i = currency_decimals(amount.currency); i < PERRY_DECIMALS; i++) step *= 10;
    units = amount.amount / step;                 /* whole cents */
    negative = units < 0;
    if (negative) units = -units;
    base = units / n;
    left = units % n;

    result.kind = VAL_LIST;
    result.list = list_new();
    for (i = 0; i < n; i++) {
        dec_t cents = base + (i < left ? 1 : 0);
        list_push(result.list, make_money((negative ? -cents : cents) * step, amount.currency));
    }
    return result;
}

static Value total_of(Value v, int line)
{
    PerryList *list;
    Value sum = make_nothing();
    int i;
    if (v.kind == VAL_TABLE)
        fail(line, "say which column to add up, like: total_of_ statement.amount");
    list = need_list(v, line, "total_of_");
    for (i = 0; i < list->count; i++) {        /* blanks (nothing_) are skipped */
        if (list->items[i].kind == VAL_NOTHING) continue;
        sum = sum.kind == VAL_NOTHING ? list->items[i] : arithmetic(TOK_PLUS, sum, list->items[i], line);
    }
    if (sum.kind == VAL_NOTHING) return make_number(VAL_NUMBER, 0);
    return sum;
}

/* ------------------------------------------------------------------ */
/* Tables: columns, filters, sorting, CSV files                       */
/* ------------------------------------------------------------------ */

static Value eval(const Node *n);

static Value make_table(PerryTable *t)
{
    Value v = make_nothing();
    v.kind  = VAL_TABLE;
    v.table = t;
    return v;
}

static int column_of(const PerryTable *t, const char *name, int len, int line)
{
    int c = table_column(t, name, len);
    if (c < 0) {
        char cols[300];
        size_t pos = 0;
        int i;
        cols[0] = '\0';
        for (i = 0; i < t->ncols && pos < sizeof cols - 40; i++)
            pos += (size_t)snprintf(cols + pos, sizeof cols - pos, "%s%s", i ? ", " : "", t->cols[i]);
        fail(line, "there's no column or field called '%.*s' - the names are: %s", len, name, cols);
    }
    return c;
}

/* row.amount -> one cell;  table.amount -> a list of the whole column */
static Value field_of(Value v, const Node *n)
{
    int c, r;
    Value list;
    if (v.kind == VAL_ROW) {
        c = column_of(v.table, n->name, n->name_len, n->line);
        return table_cell(v.table, v.row, c);
    }
    if (v.kind == VAL_TABLE) {
        c = column_of(v.table, n->name, n->name_len, n->line);
        list = make_nothing();
        list.kind = VAL_LIST;
        list.list = list_new();
        for (r = 0; r < v.table->nrows; r++) list_push(list.list, table_cell(v.table, r, c));
        return list;
    }
    if (v.kind == VAL_LIST) {                  /* a list of records: that field of each one */
        list = make_nothing();
        list.kind = VAL_LIST;
        list.list = list_new();
        for (r = 0; r < v.list->count; r++) {
            Value it = v.list->items[r];
            if (it.kind != VAL_ROW)
                fail(n->line, "'.%.*s' needs every item in the list to be a record (found %s)",
                     n->name_len, n->name, value_kind_name(it.kind));
            c = column_of(it.table, n->name, n->name_len, n->line);
            list_push(list.list, table_cell(it.table, it.row, c));
        }
        return list;
    }
    fail(n->line, "'.%.*s' only works on a table or a row of a table (got %s)",
         n->name_len, n->name, value_kind_name(v.kind));
    return v;
}

/* Does one value pass a that_is_... test? */
static int passes(const Node *n, Value key, Value target, Value tol)
{
    TokenKind op = (TokenKind)(n->op - (TOK_THAT_IS_EQUAL_TO - TOK_IS_EQUAL_TO));
    Value r;
    if (op == TOK_IS_SIMILAR_TO || op == TOK_IS_REALLY_NOT_CLOSE_TO)
        r = similar(op, key, target, tol, n->line);
    else
        r = compare(op, key, target, n->line);
    return r.boolean;
}

/* go_get_in_ (keep = 1) and remove_where_ (keep = 0) */
static Value filter_values(Value src, const Node *n, int keep)
{
    Value target = eval(n->b);
    Value tol = n->c ? eval(n->c) : make_nothing();
    Value out = make_nothing();
    int i, col;

    if (src.kind == VAL_TABLE) {
        PerryTable *t = src.table, *result;
        if (n->name == NULL)
            fail(n->line, "this is a table - say which column to check, like: by_ amount");
        col = column_of(t, n->name, n->name_len, n->line);
        result = table_new(t->ncols, t->cols);
        for (i = 0; i < t->nrows; i++)
            if (passes(n, table_cell(t, i, col), target, tol) == keep)
                table_add_row(result, t->cells + (size_t)i * (size_t)t->ncols);
        return make_table(result);
    }
    if (src.kind != VAL_LIST)
        fail(n->line, "can only filter a list or a table (got %s)", value_kind_name(src.kind));
    if (n->name != NULL)
        fail(n->line, "by_ %.*s only works on tables; this is a plain list", n->name_len, n->name);
    out.kind = VAL_LIST;
    out.list = list_new();
    for (i = 0; i < src.list->count; i++)
        if (passes(n, src.list->items[i], target, tol) == keep)
            list_push(out.list, src.list->items[i]);
    return out;
}

typedef struct {
    Value key;
    int   index;
} SortItem;

static int sort_line, sort_descending;

/* Which comes first? -1, 0 or 1. Numbers/money/percents, dates or text. */
static int order_values(Value a, Value b, int line, const char *verb)
{
    int c;
    if (value_is_numeric(a) && value_is_numeric(b)) {
        dec_t pa, pb;
        check_same_currency(a, b, line, verb);
        check_same_period(a, b, line);
        pa = plain_amount(a);
        pb = plain_amount(b);
        return pa < pb ? -1 : pa > pb ? 1 : 0;
    }
    if (a.kind == VAL_DATE && b.kind == VAL_DATE)
        return a.amount < b.amount ? -1 : a.amount > b.amount ? 1 : 0;
    if (a.kind == VAL_TEXT && b.kind == VAL_TEXT) {
        int n = a.text_len < b.text_len ? a.text_len : b.text_len;
        c = memcmp(a.text, b.text, (size_t)n);
        if (c == 0) c = a.text_len - b.text_len;
        return c < 0 ? -1 : c > 0 ? 1 : 0;
    }
    fail(line, "can't %s %s and %s together", verb, value_kind_name(a.kind), value_kind_name(b.kind));
    return 0;
}

static int compare_sort_items(const void *x, const void *y)
{
    const SortItem *a = (const SortItem *)x, *b = (const SortItem *)y;
    int c = order_values(a->key, b->key, sort_line, "sort");
    if (sort_descending) c = -c;
    return c != 0 ? c : a->index - b->index;     /* keep ties in their order */
}

/* ------------------------------------------------------------------ */
/* Statistics and grouping                                            */
/* ------------------------------------------------------------------ */

/* average_of_ / biggest_of_ / smallest_of_ - blanks (nothing_) are skipped */
static Value statistic(const Node *n)
{
    Value v = eval(n->a), best = make_nothing(), sum = make_nothing();
    const char *what = token_keyword_text(n->op);
    long long count = 0;
    int i;

    if (v.kind == VAL_TABLE)
        fail(n->line, "say which column, like: %s statement.amount", what);
    if (v.kind != VAL_LIST)
        fail(n->line, "%s needs a list (got %s)", what, value_kind_name(v.kind));

    for (i = 0; i < v.list->count; i++) {
        Value it = v.list->items[i];
        if (it.kind == VAL_NOTHING) continue;
        count++;
        if (n->op == TOK_AVERAGE_OF) {
            sum = sum.kind == VAL_NOTHING ? it : arithmetic(TOK_PLUS, sum, it, n->line);
        } else if (best.kind == VAL_NOTHING) {
            best = it;
        } else {
            int c = order_values(it, best, n->line, "compare");
            if ((n->op == TOK_BIGGEST_OF && c > 0) || (n->op == TOK_SMALLEST_OF && c < 0)) best = it;
        }
    }
    if (count == 0) fail(n->line, "%s an empty list has no answer", what);
    if (n->op != TOK_AVERAGE_OF) return best;
    if (!value_is_numeric(sum)) fail(n->line, "average_of_ works on numbers, percents and money");
    return arithmetic(TOK_SLASH, sum, make_number(VAL_NUMBER, (dec_t)count * PERRY_SCALE), n->line);
}

static char *group_list_columns[] = { "value", "count" };

/* Same group? Different kinds, or money in different currencies, are simply different. */
static int same_group(Value a, Value b, int line)
{
    if (a.kind != b.kind) return 0;
    if (a.kind == VAL_MONEY && strcmp(a.currency, b.currency) != 0) return 0;
    if (a.kind == VAL_PERCENT && a.period != b.period) return 0;
    if (a.kind == VAL_DURATION && a.period != b.period) return 0;
    if (a.kind == VAL_TABLE || a.kind == VAL_TAX || a.kind == VAL_FXRATE) return 0;
    return values_equal(a, b, line);
}

/*
 * group_by_ statement by_ category  ->  one row per category, with a count
 * and the total of every number/money column.
 * group_by_ list                    ->  each different value and how often it appears.
 */
static Value group_values(const Node *n)
{
    Value src = eval(n->a);
    PerryTable *t, *out;
    int key, *keys, nkeys = 0, r, g, c, ncols, *sum_col, nsum = 0;
    Value *row;
    char **cols;

    if (src.kind == VAL_LIST) {
        if (n->name != NULL) fail(n->line, "by_ only works on tables; for a list just write: group_by_ list");
        out = table_new(2, group_list_columns);
        for (r = 0; r < src.list->count; r++) {
            Value cells[2];
            int found = -1;
            for (g = 0; g < out->nrows && found < 0; g++)
                if (same_group(table_cell(out, g, 0), src.list->items[r], n->line)) found = g;
            if (found >= 0) {
                out->cells[(size_t)found * 2 + 1].amount += PERRY_SCALE;
            } else {
                cells[0] = src.list->items[r];
                cells[1] = make_number(VAL_NUMBER, PERRY_SCALE);
                table_add_row(out, cells);
            }
        }
        return make_table(out);
    }
    if (src.kind != VAL_TABLE)
        fail(n->line, "group_by_ needs a table or a list (got %s)", value_kind_name(src.kind));
    if (n->name == NULL)
        fail(n->line, "say which column to group by, like: group_by_ statement by_ category");

    t = src.table;
    key = column_of(t, n->name, n->name_len, n->line);

    /* the columns to add up: every value numeric (blanks allowed), not the key */
    sum_col = (int *)malloc(sizeof(int) * (size_t)(t->ncols + 1));
    if (sum_col == NULL) out_of_memory();
    for (c = 0; c < t->ncols; c++) {
        int numeric = c != key, seen = 0;
        for (r = 0; r < t->nrows && numeric; r++) {
            Value cell = table_cell(t, r, c);
            if (cell.kind == VAL_NOTHING) continue;
            if (!value_is_numeric(cell)) numeric = 0;
            seen = 1;
        }
        if (numeric && seen) sum_col[nsum++] = c;
    }

    ncols = 2 + nsum;
    cols = (char **)malloc(sizeof(char *) * (size_t)ncols);
    if (cols == NULL) out_of_memory();
    cols[0] = t->cols[key];
    cols[1] = "count";
    for (c = 0; c < nsum; c++) cols[2 + c] = t->cols[sum_col[c]];
    out = table_new(ncols, cols);

    keys = (int *)malloc(sizeof(int) * (size_t)(t->nrows + 1));
    row  = (Value *)malloc(sizeof(Value) * (size_t)ncols);
    if (keys == NULL || row == NULL) out_of_memory();

    for (r = 0; r < t->nrows; r++) {
        Value k = table_cell(t, r, key);
        int found = -1;
        for (g = 0; g < nkeys && found < 0; g++)
            if (same_group(table_cell(out, g, 0), k, n->line)) found = g;
        if (found < 0) {                           /* a new group */
            row[0] = k;
            row[1] = make_number(VAL_NUMBER, 0);
            for (c = 0; c < nsum; c++) row[2 + c] = make_nothing();
            table_add_row(out, row);
            found = nkeys++;
        }
        out->cells[(size_t)found * (size_t)ncols + 1].amount += PERRY_SCALE;
        for (c = 0; c < nsum; c++) {
            Value cell = table_cell(t, r, sum_col[c]);
            Value *slot = &out->cells[(size_t)found * (size_t)ncols + 2 + (size_t)c];
            if (cell.kind == VAL_NOTHING) continue;
            *slot = slot->kind == VAL_NOTHING ? cell : arithmetic(TOK_PLUS, *slot, cell, n->line);
        }
    }
    free(keys);
    free(row);
    free(sum_col);
    return make_table(out);
}

/* add_where_ list value  (a table takes a list with one value per column) */
static Value add_value(Value target, Value item, const Node *n)
{
    Value out = make_nothing();
    int i;
    if (target.kind == VAL_LIST) {
        out.kind = VAL_LIST;
        out.list = list_new();
        for (i = 0; i < target.list->count; i++) list_push(out.list, target.list->items[i]);
        list_push(out.list, item);
        return out;
    }
    if (target.kind == VAL_TABLE) {
        PerryTable *t = target.table, *copy = table_new(t->ncols, t->cols);
        Value *cells = (Value *)malloc(sizeof(Value) * (size_t)(t->ncols + 1));
        if (cells == NULL) out_of_memory();
        if (item.kind == VAL_ROW) {                /* a record or row: match fields to columns by name */
            for (i = 0; i < t->ncols; i++) {
                int c = table_column(item.table, t->cols[i], (int)strlen(t->cols[i]));
                if (c < 0)
                    fail(n->line, "the new row has no '%s' - it needs the same names as the table's columns",
                         t->cols[i]);
                cells[i] = table_cell(item.table, item.row, c);
            }
            if (item.table->ncols != t->ncols)
                fail(n->line, "the new row has %d fields but the table has %d columns",
                     item.table->ncols, t->ncols);
        } else if (item.kind == VAL_LIST && item.list->count == t->ncols) {
            for (i = 0; i < t->ncols; i++) cells[i] = item.list->items[i];
        } else {
            fail(n->line, "a new row needs %d values in [ ], one for each column: %s...",
                 t->ncols, t->cols[0]);
        }
        for (i = 0; i < t->nrows; i++) table_add_row(copy, t->cells + (size_t)i * (size_t)t->ncols);
        table_add_row(copy, cells);
        free(cells);
        return make_table(copy);
    }
    fail(n->line, "add_where_ needs a list or a table (got %s)", value_kind_name(target.kind));
    return out;
}

static Value sort_values(Value src, const Node *n)
{
    SortItem *items;
    int count, i, col = -1;
    Value out = make_nothing();

    if (src.kind == VAL_TABLE) {
        if (n->name == NULL) fail(n->line, "say which column to sort by, like: sort_ statement by_ amount");
        col = column_of(src.table, n->name, n->name_len, n->line);
        count = src.table->nrows;
    } else if (src.kind == VAL_LIST) {
        if (n->name != NULL)
            fail(n->line, "by_ %.*s only works on tables; this is a plain list", n->name_len, n->name);
        count = src.list->count;
    } else {
        fail(n->line, "sort_ needs a list or a table (got %s)", value_kind_name(src.kind));
        return src;
    }

    items = (SortItem *)malloc(sizeof(SortItem) * (size_t)(count + 1));
    if (items == NULL) out_of_memory();
    for (i = 0; i < count; i++) {
        items[i].key   = col >= 0 ? table_cell(src.table, i, col) : src.list->items[i];
        items[i].index = i;
    }
    sort_line = n->line;
    sort_descending = (n->op == TOK_HIGHEST_FIRST);
    qsort(items, (size_t)count, sizeof(SortItem), compare_sort_items);

    if (col >= 0) {
        PerryTable *t = table_new(src.table->ncols, src.table->cols);
        for (i = 0; i < count; i++)
            table_add_row(t, src.table->cells + (size_t)items[i].index * (size_t)src.table->ncols);
        out = make_table(t);
    } else {
        out.kind = VAL_LIST;
        out.list = list_new();
        for (i = 0; i < count; i++) list_push(out.list, src.list->items[items[i].index]);
    }
    free(items);
    return out;
}

/* File names are relative to the .perry file, not to where you ran perry. */
static void program_relative_path(Value name, char *path, size_t size)
{
    const char *slash = strrchr(g_file_name, '/');
    if (name.text_len > 0 && name.text[0] != '/' && slash != NULL)
        snprintf(path, size, "%.*s/%.*s", (int)(slash - g_file_name), g_file_name,
                 name.text_len, name.text);
    else
        snprintf(path, size, "%.*s", name.text_len, name.text);
}

/* save_to_ "report.csv" value */
static void save_value(const Node *n)
{
    Value name = eval(n->a), v;
    char path[1024], error[512];

    if (name.kind != VAL_TEXT)
        fail(n->line, "save_to_ needs a file name in quotes, like: save_to_ \"report.csv\" statement");
    program_relative_path(name, path, sizeof path);
    v = eval(n->b);
    if (!csv_save(path, v, error, sizeof error)) raise_error(ERR_FILE, n->line, "%s", error);
}

/* go_get_ "statement.csv" as_ ZAR  - the path is relative to the .perry file */
static Value load_csv(const Node *n)
{
    Value name = eval(n->a);
    char path[1024], currency[4], error[512];
    int csv_line;
    PerryTable *t;

    if (name.kind != VAL_TEXT)
        fail(n->line, "go_get_ needs a file name in quotes, like: go_get_ \"statement.csv\"");
    program_relative_path(name, path, sizeof path);

    if (n->name != NULL) {
        memcpy(currency, n->name, 3);
        currency[3] = '\0';
    }
    t = csv_load(path, n->name ? currency : NULL, error, sizeof error, &csv_line);
    if (t == NULL) raise_error(ERR_FILE, n->line, "%s", error);
    return make_table(t);
}

/* ------------------------------------------------------------------ */
/* Rates, rounding and tax                                            */
/* ------------------------------------------------------------------ */

static int is_currency_code(const char *s, int len)
{
    return len == 3 && s[0] >= 'A' && s[0] <= 'Z' && s[1] >= 'A' && s[1] <= 'Z' &&
           s[2] >= 'A' && s[2] <= 'Z';
}

/* 11.75% per year,  rate per month,  18.20 ZAR per USD */
static Value per_value(const Node *n)
{
    Value v = eval(n->a);
    int unit;
    long long d;

    if (v.kind == VAL_MONEY && is_currency_code(n->name, n->name_len)) {   /* an exchange rate */
        memcpy(v.base, n->name, 3);
        v.base[3] = '\0';
        if (strcmp(v.base, v.currency) == 0)
            fail(n->line, "an exchange rate needs two different currencies, like: 18.20 ZAR per USD");
        if (v.amount <= 0) fail(n->line, "an exchange rate must be more than zero");
        v.kind = VAL_FXRATE;
        return v;
    }
    if ((n->name_len == 4 && memcmp(n->name, "year", 4) == 0) ||
        (n->name_len == 5 && memcmp(n->name, "years", 5) == 0))
        unit = PERIOD_YEAR;
    else if ((n->name_len == 5 && memcmp(n->name, "month", 5) == 0) ||
             (n->name_len == 6 && memcmp(n->name, "months", 6) == 0))
        unit = PERIOD_MONTH;
    else {
        fail(n->line, "per works with year or month (got '%.*s')", n->name_len, n->name);
        return v;
    }
    if (v.kind != VAL_PERCENT)
        fail(n->line, "per works with rates, like: 11.75%% per year or 18.20 ZAR per USD (got %s)",
             value_kind_name(v.kind));

    if (v.period == PERIOD_NONE || v.period == unit) {
        v.period = unit;
    } else if (v.period == PERIOD_YEAR && unit == PERIOD_MONTH) {
        v.divisor = value_divisor(v) * 12;          /* exact: divide later */
        v.period  = PERIOD_MONTH;
    } else {                                        /* monthly -> yearly */
        d = value_divisor(v);
        if (d % 12 == 0) v.divisor = d / 12;
        else if (!dec_mul_frac(v.amount, 12 * PERRY_SCALE, 1, &v.amount)) too_big(n->line);
        v.period = PERIOD_YEAR;
    }
    return v;
}

/* round_to_ x 2 decimals [the_bank_way_ | up_ | down_] */
static Value round_value(const Node *n)
{
    Value v = eval(n->a);
    long long places = whole_number(eval(n->b), n->line, "the number of decimals");
    RoundMode mode = n->op == TOK_THE_BANK_WAY ? ROUND_BANK :
                     n->op == TOK_UP           ? ROUND_UP   :
                     n->op == TOK_DOWN         ? ROUND_DOWN : ROUND_NORMAL;

    if (!value_is_numeric(v)) fail(n->line, "round_to_ works on numbers, percents and money");
    if (places < 0) fail(n->line, "can't round to a negative number of decimals");

    if (v.kind == VAL_MONEY) {
        if (places > currency_decimals(v.currency))
            fail(n->line, "%s only has %d decimal places", v.currency, currency_decimals(v.currency));
        v.amount = dec_round_mode(v.amount, (int)places, mode);
    } else if (v.kind == VAL_PERCENT) {
        if (places > PERRY_DECIMALS - 2) fail(n->line, "percentages can be rounded to at most %d decimals", PERRY_DECIMALS - 2);
        v = make_percent(dec_round_mode(percent_fraction(v), (int)places + 2, mode), v.period);
    } else {
        if (places > PERRY_DECIMALS) fail(n->line, "Perry numbers have at most %d decimals", PERRY_DECIMALS);
        v.amount = dec_round_mode(v.amount, (int)places, mode);
    }
    return v;
}

static Value build_tax_table(const Node *n)
{
    PerryTax *tax = (PerryTax *)calloc(1, sizeof(PerryTax));
    Value v = make_nothing();
    int i;

    if (tax == NULL) out_of_memory();
    if (n->count == 0) fail(n->line, "a tax table needs at least one line");
    tax->count  = n->count;
    tax->limits = (Value *)calloc((size_t)n->count, sizeof(Value));
    tax->rates  = (Value *)calloc((size_t)n->count, sizeof(Value));
    if (tax->limits == NULL || tax->rates == NULL) out_of_memory();

    for (i = 0; i < n->count; i++) {
        const Node *br = n->items[i];
        Value rate = eval(br->b);
        if (rate.kind != VAL_PERCENT || rate.period != PERIOD_NONE)
            fail(br->line, "taxed_at_ needs a percent, like: taxed_at_ 18%%");
        tax->rates[i] = rate;
        if (br->a == NULL) {
            if (i != n->count - 1) fail(br->line, "above_ must be the last line of the tax table");
            continue;
        }
        if (i == n->count - 1)
            fail(br->line, "the last line of a tax table must be: above_ taxed_at_ ...");
        tax->limits[i] = eval(br->a);
        if (tax->limits[i].kind != VAL_MONEY)
            fail(br->line, "up_to_ needs money, like: up_to_ 237100.00 ZAR");
        if (i > 0) {
            check_same_currency(tax->limits[i - 1], tax->limits[i], br->line, "mix");
            if (tax->limits[i].amount <= tax->limits[i - 1].amount)
                fail(br->line, "each up_to_ must be bigger than the one above it");
        } else if (tax->limits[i].amount <= 0) {
            fail(br->line, "up_to_ must be more than zero");
        }
    }
    v.kind = VAL_TAX;
    v.tax  = tax;
    return v;
}

/* income_tax on_ salary: each slice of income is taxed at its own rate */
static Value apply_tax(const Node *n)
{
    Variable *var = find_var(n->name, n->name_len);
    Value income, table;
    PerryTax *tax;
    dec_t owed = 0, bottom = 0, top, slice, part;
    int i;

    if (var == NULL || var->value.kind != VAL_TAX)
        fail(n->line, "'%.*s' isn't a tax table - create it with tax_table_ %.*s:",
             n->name_len, n->name, n->name_len, n->name);
    table  = var->value;
    tax    = table.tax;
    income = eval(n->a);
    if (income.kind != VAL_MONEY) fail(n->line, "on_ needs money, like: income_tax on_ 500000.00 ZAR");
    if (tax->count > 1) check_same_currency(tax->limits[0], income, n->line, "mix");

    for (i = 0; i < tax->count && income.amount > bottom; i++) {
        top   = (i < tax->count - 1) ? tax->limits[i].amount : income.amount;
        slice = (income.amount < top ? income.amount : top) - bottom;
        if (!dec_mul_frac(slice, tax->rates[i].amount, value_divisor(tax->rates[i]), &part)) too_big(n->line);
        if (!dec_add(owed, part, &owed)) too_big(n->line);
        bottom = top;
    }
    return make_money(owed, income.currency);
}

/* convert_ price to_ USD at_rate_ 18.20 ZAR per USD on_ 2026-09-25 */
static Value convert_money(const Node *n)
{
    Value amount = eval(n->a), rate = eval(n->b);
    char target[4];
    dec_t r;

    memcpy(target, n->name, 3);
    target[3] = '\0';
    if (n->c != NULL && eval(n->c).kind != VAL_DATE)
        fail(n->line, "on_ needs the date of the rate, like: on_ 2026-09-25");
    if (amount.kind != VAL_MONEY)
        fail(n->line, "convert_ works on money (got %s)", value_kind_name(amount.kind));
    if (rate.kind != VAL_FXRATE)
        fail(n->line, "at_rate_ needs an exchange rate, like: at_rate_ 18.20 ZAR per USD");
    if (strcmp(amount.currency, target) == 0) return amount;

    if (strcmp(amount.currency, rate.currency) == 0 && strcmp(target, rate.base) == 0) {
        if (!dec_div(amount.amount, rate.amount, &r)) too_big(n->line);      /* ZAR -> USD: divide */
    } else if (strcmp(amount.currency, rate.base) == 0 && strcmp(target, rate.currency) == 0) {
        if (!dec_mul(amount.amount, rate.amount, &r)) too_big(n->line);      /* USD -> ZAR: multiply */
    } else {
        raise_error(ERR_CURRENCY, n->line, "that rate is for %s and %s, but this converts %s to %s",
                    rate.currency, rate.base, amount.currency, target);
        return amount;
    }
    return make_money(r, target);
}

/* ------------------------------------------------------------------ */
/* Bookkeeping                                                        */
/*                                                                    */
/* Every book_ entry must balance: total debits = total credits.      */
/* An account's balance is its debits minus its credits.              */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *name;
    int         name_len;
    char        currency[4];
    dec_t       debit, credit;
} Account;

static Account    *accounts;
static int         account_count, account_capacity;
static PerryTable *journal;
static int         entry_count;
static char       *journal_columns[] = { "entry", "description", "account", "debit", "credit" };
static char       *trial_columns[]   = { "account", "debit", "credit", "balance" };

static Account *find_account(const char *name, int len)
{
    int i;
    for (i = 0; i < account_count; i++)
        if (accounts[i].name_len == len && memcmp(accounts[i].name, name, (size_t)len) == 0)
            return &accounts[i];
    return NULL;
}

static Value make_text(const char *s, int len)
{
    Value v = make_nothing();
    v.kind     = VAL_TEXT;
    v.text     = s;
    v.text_len = len;
    return v;
}

static void book_entry(const Node *n)
{
    Value desc = eval(n->a), *amounts;
    char currency[4] = "", total_d[64], total_c[64];
    dec_t debits = 0, credits = 0;
    int k, has_debit = 0, has_credit = 0;
    Value row[5];

    if (desc.kind != VAL_TEXT) {                    /* keep a text copy of the description */
        char buf[256], *copy;
        value_format(buf, sizeof buf, desc);
        copy = (char *)malloc(strlen(buf) + 1);
        if (copy == NULL) out_of_memory();
        strcpy(copy, buf);
        desc = make_text(copy, (int)strlen(copy));
    }

    /* 1. check everything before recording anything */
    amounts = (Value *)malloc(sizeof(Value) * (size_t)(n->count + 1));
    if (amounts == NULL) out_of_memory();
    for (k = 0; k < n->count; k++) {
        const Node *e = n->items[k];
        Account *acct;
        Value amt = eval(e->a);
        if (amt.kind != VAL_MONEY)
            fail(e->line, "debit_ and credit_ need money, like: debit_ rent_expense 8000.00 ZAR");
        if (amt.amount <= 0) fail(e->line, "debit_ and credit_ amounts must be more than zero");
        if (currency[0] == '\0') memcpy(currency, amt.currency, 4);
        else if (strcmp(currency, amt.currency) != 0)
            raise_error(ERR_CURRENCY, e->line, "one book_ entry can't mix %s and %s", currency, amt.currency);
        acct = find_account(e->name, e->name_len);
        if (acct != NULL && strcmp(acct->currency, amt.currency) != 0)
            raise_error(ERR_CURRENCY, e->line, "the account '%.*s' is kept in %s, not %s",
                        e->name_len, e->name, acct->currency, amt.currency);
        if (e->op == TOK_DEBIT) { has_debit = 1;  if (!dec_add(debits, amt.amount, &debits))  too_big(e->line); }
        else                    { has_credit = 1; if (!dec_add(credits, amt.amount, &credits)) too_big(e->line); }
        amounts[k] = amt;
    }
    if (!has_debit || !has_credit)
        raise_error(ERR_BALANCE, n->line, "book_ \"%.*s\" needs at least one debit_ and one credit_",
                    desc.text_len, desc.text);
    if (debits != credits) {
        value_format(total_d, sizeof total_d, make_money(debits, currency));
        value_format(total_c, sizeof total_c, make_money(credits, currency));
        raise_error(ERR_BALANCE, n->line, "book_ \"%.*s\" doesn't balance: debits are %s but credits are %s",
                    desc.text_len, desc.text, total_d, total_c);
    }

    /* 2. it balances: record it */
    if (journal == NULL) journal = table_new(5, journal_columns);
    entry_count++;
    for (k = 0; k < n->count; k++) {
        const Node *e = n->items[k];
        Account *acct = find_account(e->name, e->name_len);
        if (acct == NULL) {
            if (account_count == account_capacity) {
                account_capacity = account_capacity ? account_capacity * 2 : 16;
                accounts = (Account *)realloc(accounts, sizeof(Account) * (size_t)account_capacity);
                if (accounts == NULL) out_of_memory();
            }
            acct = &accounts[account_count++];
            memset(acct, 0, sizeof *acct);
            acct->name     = e->name;
            acct->name_len = e->name_len;
            memcpy(acct->currency, amounts[k].currency, 4);
        }
        if (e->op == TOK_DEBIT) acct->debit  += amounts[k].amount;
        else                    acct->credit += amounts[k].amount;

        row[0] = make_number(VAL_NUMBER, (dec_t)entry_count * PERRY_SCALE);
        row[1] = desc;
        row[2] = make_text(e->name, e->name_len);
        row[3] = e->op == TOK_DEBIT  ? amounts[k] : make_nothing();
        row[4] = e->op == TOK_CREDIT ? amounts[k] : make_nothing();
        table_add_row(journal, row);
    }
    free(amounts);
}

static Value balance_of(Value name, int line)
{
    Account *acct;
    if (name.kind != VAL_TEXT)
        fail(line, "balance_of_ needs an account name in quotes, like: balance_of_(\"bank\")");
    acct = find_account(name.text, name.text_len);
    if (acct == NULL)
        fail(line, "no account called '%.*s' has been booked yet", name.text_len, name.text);
    return make_money(acct->debit - acct->credit, acct->currency);
}

static Value trial_balance(void)
{
    PerryTable *t = table_new(4, trial_columns);
    Value row[4];
    dec_t total_d = 0, total_c = 0;
    int i, one_currency = 1;

    for (i = 0; i < account_count; i++) {
        Account *a = &accounts[i];
        row[0] = make_text(a->name, a->name_len);
        row[1] = make_money(a->debit, a->currency);
        row[2] = make_money(a->credit, a->currency);
        row[3] = make_money(a->debit - a->credit, a->currency);
        table_add_row(t, row);
        if (strcmp(a->currency, accounts[0].currency) != 0) one_currency = 0;
        total_d += a->debit;
        total_c += a->credit;
    }
    if (account_count > 0 && one_currency) {       /* the totals must match */
        row[0] = make_text("TOTAL", 5);
        row[1] = make_money(total_d, accounts[0].currency);
        row[2] = make_money(total_c, accounts[0].currency);
        row[3] = make_money(total_d - total_c, accounts[0].currency);
        table_add_row(t, row);
    }
    return make_table(t);
}

/* ------------------------------------------------------------------ */
/* Loans                                                              */
/* ------------------------------------------------------------------ */

__extension__ typedef __int128 i128;

#define HP       PERRY_SCALE            /* loan maths uses Perry's own 12 decimal places */
#define HP_LIMIT ((i128)HP * 1000000)   /* growth factors up to a million   */

static i128 div_round128(i128 n, i128 d)
{
    i128 q = n / d, r = n % d;
    i128 ar = r < 0 ? -r : r, ad = d < 0 ? -d : d;
    if (2 * ar >= ad) q += ((n < 0) != (d < 0)) ? -1 : 1;
    return q;
}

static void check_loan_inputs(const Node *call, Value loan, Value rate, long long *months, Value m)
{
    int line = call->line;
    if (loan.kind != VAL_MONEY)
        fail(line, "%.*s needs the loan amount as money first", call->name_len, call->name);
    if (loan.amount <= 0) fail(line, "the loan amount must be more than zero");
    if (rate.kind != VAL_PERCENT)
        fail(line, "%.*s needs a rate second, like: rate per month", call->name_len, call->name);
    if (rate.period == PERIOD_NONE)
        fail(line, "say if the rate is per year or per month, like: 11.75%% per year");
    if (rate.period != PERIOD_MONTH)
        fail(line, "this rate is per %s but payments are monthly - write: rate per month",
             period_name(rate.period));
    if (rate.amount < 0) fail(line, "the rate can't be negative");
    if (m.kind == VAL_DURATION) {                /* 20 years or 240 months */
        if (m.period == DUR_YEAR)       *months = m.amount * 12;
        else if (m.period == DUR_MONTH) *months = m.amount;
        else fail(line, "a loan's length must be in months or years");
    } else {
        *months = whole_number(m, line, "the number of months");
    }
    if (*months < 1 || *months > 1200) fail(line, "the number of months must be from 1 to 1200");
}

/* A rate as a fraction with 12 decimal places (11.75% per year per month -> 0.009791666667) */
static i128 rate_hp(Value rate)
{
    return div_round128(rate.amount, value_divisor(rate));
}

/* (1 + r)^n with 12 decimal places, by repeated squaring */
static i128 growth_factor(i128 r, long long n, int line)
{
    i128 f = HP, base = HP + r;
    if (base <= 0) fail(line, "a rate of -100%% or less can't grow money");
    while (n > 0) {
        if (n & 1) {
            f = div_round128(f * base, HP);
            if (f > HP_LIMIT) fail(line, "the rate is too high or the time too long to calculate");
        }
        n >>= 1;
        if (n > 0) {
            base = div_round128(base * base, HP);
            if (base > HP_LIMIT) fail(line, "the rate is too high or the time too long to calculate");
        }
    }
    return f;
}

/* The standard fixed monthly payment: P * r * (1+r)^n / ((1+r)^n - 1) */
static dec_t loan_payment(Value loan, Value rate, long long months, int line)
{
    i128 r = rate_hp(rate), f, ratio, p;

    if (r == 0) {
        p = div_round128((i128)loan.amount, months);
    } else {
        f = growth_factor(r, months, line);
        ratio = div_round128(r * f, f - HP);
        if (!dec_muldiv(loan.amount, ratio, HP, &p)) too_big(line);
    }
    return dec_round(p, currency_decimals(loan.currency));
}

/* ------------------------------------------------------------------ */
/* VAT and interest                                                   */
/* ------------------------------------------------------------------ */

/* vat_on_ 1000.00 ZAR -> 150.00 ZAR;  vat_inside_ 1150.00 ZAR -> 150.00 ZAR (uses the VAT name) */
static Value vat_value(const Node *n)
{
    Variable *var = find_var("VAT", 3);
    Value rate, amount;
    long long d;
    dec_t result;

    if (var == NULL)
        fail(n->line, "set the VAT rate first, like: never_changes_ VAT = 15%%");
    if (var->value.kind != VAL_PERCENT || var->value.period != PERIOD_NONE)
        fail(n->line, "VAT must be a plain percent, like: never_changes_ VAT = 15%%");
    rate   = var->value;
    amount = eval(n->a);
    if (amount.kind != VAL_MONEY)
        fail(n->line, "%s needs money (got %s)", token_keyword_text(n->op), value_kind_name(amount.kind));
    if (n->op == TOK_VAT_ON) return arithmetic(TOK_STAR, amount, rate, n->line);

    /* the VAT already inside a price: amount * r / (1 + r) */
    d = value_divisor(rate);
    if (!dec_muldiv(amount.amount, rate.amount, PERRY_SCALE * d + rate.amount, &result)) too_big(n->line);
    return make_money(result, amount.currency);
}

/*
 * How many rate periods fit in a length of time, as a fraction top/bottom.
 * 18 months at a yearly rate = 18/12 years; 90 days = 90/365 years.
 */
static void periods_in(Value rate, Value time, const Node *call, long long *top, long long *bottom)
{
    int line = call->line;
    if (rate.kind != VAL_PERCENT)
        fail(line, "%.*s needs a rate second, like: 10%% per year", call->name_len, call->name);
    if (rate.period == PERIOD_NONE)
        fail(line, "say if the rate is per year or per month, like: 10%% per year");
    if (time.kind != VAL_DURATION || time.period == DUR_BUSINESS)
        fail(line, "%.*s needs a length of time third, like: 5 years, 18 months or 90 days",
             call->name_len, call->name);
    if (time.amount < 0) fail(line, "the length of time can't be negative");

    if (rate.period == PERIOD_YEAR) {
        if (time.period == DUR_YEAR)       { *top = time.amount;      *bottom = 1;   }
        else if (time.period == DUR_MONTH) { *top = time.amount;      *bottom = 12;  }
        else                               { *top = time.amount;      *bottom = 365; }
    } else {
        if (time.period == DUR_YEAR)       { *top = time.amount * 12; *bottom = 1;   }
        else if (time.period == DUR_MONTH) { *top = time.amount;      *bottom = 1;   }
        else                               { *top = time.amount * 12; *bottom = 365; }
    }
}

/* simple_interest_ / compound_interest_ / future_value_ / present_value_ (amount, rate, time) */
static Value interest(const Node *call, int kind, Value amount, Value rate, Value time)
{
    long long top, bottom, n;
    i128 f, result;
    int line = call->line;

    if (amount.kind != VAL_MONEY)
        fail(line, "%.*s needs money first", call->name_len, call->name);
    periods_in(rate, time, call, &top, &bottom);

    if (kind == 0) {                                   /* simple: P * r * t */
        if (!dec_muldiv(amount.amount, rate.amount * top, PERRY_SCALE * value_divisor(rate) * bottom, &result))
            too_big(line);
        return make_money(result, amount.currency);
    }

    if (top % bottom != 0)
        fail(line, "compounding needs whole %ss - write the time in %ss, or change the rate with 'per'",
             period_name(rate.period), period_name(rate.period));
    n = top / bottom;
    f = growth_factor(rate_hp(rate), n, line);

    if (kind == 3) {                                   /* present value: F / (1+r)^n */
        if (!dec_muldiv(amount.amount, HP, f, &result)) too_big(line);
    } else {                                           /* future value: P * (1+r)^n */
        if (!dec_muldiv(amount.amount, f, HP, &result)) too_big(line);
    }
    if (kind == 1) result -= amount.amount;            /* compound interest = FV - P */
    return make_money(result, amount.currency);
}

static char *schedule_columns[] = { "month", "payment", "interest", "principal", "balance" };

static Value loan_schedule(Value loan, Value rate, long long months, int line)
{
    PerryTable *t = table_new(5, schedule_columns);
    dec_t payment = loan_payment(loan, rate, months, line);
    dec_t balance = loan.amount, interest, principal;
    long long m;
    int d = currency_decimals(loan.currency);
    Value row[5];

    for (m = 1; m <= months; m++) {
        if (!dec_mul_frac(balance, rate.amount, value_divisor(rate), &interest)) too_big(line);
        interest  = dec_round(interest, d);
        principal = payment - interest;
        if (m == months || principal > balance) principal = balance;    /* last payment clears it */
        balance  -= principal;
        row[0] = make_number(VAL_NUMBER, (dec_t)m * PERRY_SCALE);
        row[1] = make_money(principal + interest, loan.currency);
        row[2] = make_money(interest, loan.currency);
        row[3] = make_money(principal, loan.currency);
        row[4] = make_money(balance, loan.currency);
        table_add_row(t, row);
    }
    return make_table(t);
}

/* Functions built into Perry. Returns 0 if the name isn't one. */
static int call_builtin(const Node *n, Value *result)
{
    Value args[3];
    long long months = 0;
    int i, is_payment, is_schedule;

    /* finance, actuarial maths and statistics (finance.c) */
    {
        int fn = finance_lookup(n->name, n->name_len);
        if (fn >= 0) {
            Value values[8];
            if (n->count > 8) fail(n->line, "%s takes at most 8 values", finance_name(fn));
            for (i = 0; i < n->count; i++) values[i] = eval(n->items[i]);
            *result = finance_call(fn, values, n->count, n->line);
            return 1;
        }
    }

    /* interest */
    {
        static const char *names[] = { "simple_interest_", "compound_interest_", "future_value_", "present_value_" };
        int kind;
        for (kind = 0; kind < 4; kind++) {
            if (n->name_len == (int)strlen(names[kind]) && memcmp(n->name, names[kind], (size_t)n->name_len) == 0) {
                if (n->count != 3)
                    fail(n->line, "%s needs 3 values: the amount, the rate and the time, like: "
                                  "%s(10000.00 ZAR, 10%% per year, 5 years)", names[kind], names[kind]);
                for (i = 0; i < 3; i++) args[i] = eval(n->items[i]);
                *result = interest(n, kind, args[0], args[1], args[2]);
                return 1;
            }
        }
    }

    /* bookkeeping */
    if ((n->name_len == 11 && memcmp(n->name, "balance_of_", 11) == 0)) {
        if (n->count != 1) fail(n->line, "balance_of_ needs one account name, like: balance_of_(\"bank\")");
        *result = balance_of(eval(n->items[0]), n->line);
        return 1;
    }
    if ((n->name_len == 8 && memcmp(n->name, "journal_", 8) == 0) ||
        (n->name_len == 14 && memcmp(n->name, "trial_balance_", 14) == 0)) {
        if (n->count != 0) fail(n->line, "%.*s() doesn't take any values", n->name_len, n->name);
        if (n->name[0] == 'j') *result = make_table(journal ? journal : table_new(5, journal_columns));
        else                   *result = trial_balance();
        return 1;
    }

    is_payment  = n->name_len == 15 && memcmp(n->name, "monthly_payment", 15) == 0;
    is_schedule = n->name_len == 12 && memcmp(n->name, "schedule_of_", 12) == 0;
    if (!is_payment && !is_schedule) return 0;

    if (n->count != 3)
        fail(n->line, "%.*s needs 3 values: the loan, the monthly rate and the number of months, "
                      "like: %.*s(loan, rate per month, 240)",
             n->name_len, n->name, n->name_len, n->name);
    for (i = 0; i < 3; i++) args[i] = eval(n->items[i]);
    check_loan_inputs(n, args[0], args[1], &months, args[2]);

    if (is_payment) *result = make_money(loan_payment(args[0], args[1], months, n->line), args[0].currency);
    else            *result = loan_schedule(args[0], args[1], months, n->line);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Writing expressions back out as text (for because_is_)             */
/* ------------------------------------------------------------------ */

typedef struct {
    char  *text;
    size_t len, cap;
} Buf;

static void buf_add(Buf *b, const char *s)
{
    size_t n = strlen(s);
    if (b->len + n + 1 > b->cap) {
        b->cap = (b->len + n + 1) * 2;
        b->text = (char *)realloc(b->text, b->cap);
        if (b->text == NULL) out_of_memory();
    }
    memcpy(b->text + b->len, s, n + 1);
    b->len += n;
}

static void buf_add_n(Buf *b, const char *s, int n)
{
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%.*s", n, s);
    buf_add(b, tmp);
}

static void buf_add_value(Buf *b, Value v)
{
    char tmp[512];
    if (v.kind == VAL_TEXT) {
        buf_add(b, "\"");
        buf_add_n(b, v.text, v.text_len);
        buf_add(b, "\"");
        return;
    }
    value_format(tmp, sizeof tmp, v);
    buf_add(b, tmp);
}

static int precedence(const Node *n)
{
    switch (n->kind) {
    case N_OR:      return 1;
    case N_AND:     return 2;
    case N_NOT:     return 3;
    case N_COMPARE:
    case N_SIMILAR: return 4;
    case N_BINARY:  return (n->op == TOK_PLUS || n->op == TOK_MINUS) ? 5 : 6;
    case N_NEGATE:  return 7;
    default:        return 8;
    }
}

static const char *op_text(TokenKind op)
{
    switch (op) {
    case TOK_PLUS:  return " + ";
    case TOK_MINUS: return " - ";
    case TOK_STAR:  return " * ";
    case TOK_SLASH: return " / ";
    case TOK_OF:    return " of ";
    default:        return " ? ";
    }
}

/* worked = 1 replaces each name with its current value */
static void render(Buf *b, const Node *n, int worked, int parent_prec, int right_side)
{
    int p = precedence(n);
    int parens = p < parent_prec || (right_side && p == parent_prec && p < 8);
    Variable *v;
    int i;

    if (parens) buf_add(b, "(");
    switch (n->kind) {
    case N_LITERAL:
        buf_add_value(b, n->value);
        break;
    case N_VAR:
        v = worked ? find_var(n->name, n->name_len) : NULL;
        if (v != NULL) buf_add_value(b, v->value);
        else           buf_add_n(b, n->name, n->name_len);
        break;
    case N_NEGATE:
        buf_add(b, "-");
        render(b, n->a, worked, p, 0);
        break;
    case N_BINARY:
        render(b, n->a, worked, p, 0);
        buf_add(b, op_text(n->op));
        render(b, n->b, worked, p, 1);
        break;
    case N_COMPARE:
    case N_SIMILAR:
        render(b, n->a, worked, p, 0);
        buf_add(b, " ");
        buf_add(b, token_keyword_text(n->op));
        buf_add(b, " ");
        render(b, n->b, worked, p, 1);
        if (n->c) {
            buf_add(b, " within_ ");
            render(b, n->c, worked, p, 1);
        }
        break;
    case N_AND:
    case N_OR:
        render(b, n->a, worked, p, 0);
        buf_add(b, n->kind == N_AND ? " and_ " : " or_ ");
        render(b, n->b, worked, p, 1);
        break;
    case N_NOT:
        buf_add(b, "not_ ");
        render(b, n->a, worked, p, 0);
        break;
    case N_LIST:
        buf_add(b, "[");
        for (i = 0; i < n->count; i++) {
            if (i) buf_add(b, ", ");
            render(b, n->items[i], worked, 0, 0);
        }
        buf_add(b, "]");
        break;
    case N_SPLIT:
        buf_add(b, "split ");
        render(b, n->a, worked, 5, 0);
        buf_add(b, " into ");
        render(b, n->b, worked, 5, 0);
        break;
    case N_TOTAL:
    case N_COUNT:
        buf_add(b, n->kind == N_TOTAL ? "total_of_ " : "count_of_ ");
        render(b, n->a, worked, 7, 0);
        break;
    case N_CALL:
        buf_add_n(b, n->name, n->name_len);
        buf_add(b, "(");
        for (i = 0; i < n->count; i++) {
            if (i) buf_add(b, ", ");
            render(b, n->items[i], worked, 0, 0);
        }
        buf_add(b, ")");
        break;
    case N_FIELD:
        render(b, n->a, 0, 8, 0);            /* keep "row.amount" readable */
        buf_add(b, ".");
        buf_add_n(b, n->name, n->name_len);
        break;
    case N_PER:
        render(b, n->a, worked, 8, 0);
        buf_add(b, " per ");
        buf_add_n(b, n->name, n->name_len);
        break;
    case N_ROUND:
        buf_add(b, "round_to_ ");
        render(b, n->a, worked, 8, 0);
        buf_add(b, " ");
        render(b, n->b, worked, 8, 0);
        buf_add(b, " decimals");
        if (n->op != TOK_EOF) { buf_add(b, " "); buf_add(b, token_keyword_text(n->op)); }
        break;
    case N_LOAD:
        buf_add(b, "go_get_ ");
        render(b, n->a, worked, 8, 0);
        if (n->name) { buf_add(b, " as_ "); buf_add_n(b, n->name, n->name_len); }
        break;
    case N_FILTER:
        buf_add(b, "go_get_in_ ");
        render(b, n->a, 0, 8, 0);
        if (n->name) { buf_add(b, " by_ "); buf_add_n(b, n->name, n->name_len); }
        buf_add(b, " ");
        buf_add(b, token_keyword_text(n->op));
        buf_add(b, " ");
        render(b, n->b, worked, 5, 0);
        break;
    case N_TAX_APPLY:
        buf_add_n(b, n->name, n->name_len);
        buf_add(b, " on_ ");
        render(b, n->a, worked, 8, 0);
        break;
    case N_TODAY:
        buf_add(b, "today_");
        break;
    case N_DURATION: {
        const char *unit = unit_name((int)n->op);
        int one = n->a->kind == N_LITERAL && n->a->value.kind == VAL_NUMBER &&
                  n->a->value.amount == PERRY_SCALE;
        render(b, n->a, worked, 8, 0);
        buf_add(b, " ");
        if (one) buf_add_n(b, unit, (int)strlen(unit) - 1);   /* "1 day", not "1 days" */
        else     buf_add(b, unit);
        break;
    }
    case N_BETWEEN:
        buf_add(b, token_keyword_text(n->op));
        buf_add(b, " ");
        render(b, n->a, worked, 5, 0);
        buf_add(b, " and_ ");
        render(b, n->b, worked, 5, 0);
        break;
    case N_END_OF_MONTH:
        buf_add(b, "end_of_month_ ");
        render(b, n->a, worked, 8, 0);
        break;
    case N_STAT:
    case N_VAT:
        buf_add(b, token_keyword_text(n->op));
        buf_add(b, " ");
        render(b, n->a, worked, 7, 0);
        break;
    case N_GROUP:
        buf_add(b, "group_by_ ");
        render(b, n->a, 0, 8, 0);
        if (n->name) { buf_add(b, " by_ "); buf_add_n(b, n->name, n->name_len); }
        break;
    case N_CONVERT:
        buf_add(b, "convert_ ");
        render(b, n->a, worked, 8, 0);
        buf_add(b, " to_ ");
        buf_add_n(b, n->name, n->name_len);
        buf_add(b, " at_rate_ ");
        render(b, n->b, worked, 8, 0);
        if (n->c) { buf_add(b, " on_ "); render(b, n->c, worked, 8, 0); }
        break;
    default:
        buf_add(b, "?");
    }
    if (parens) buf_add(b, ")");
}

static char *render_text(const Node *n, int worked)
{
    Buf b = { NULL, 0, 0 };
    buf_add(&b, "");
    render(&b, n, worked, 0, 0);
    return b.text;
}

/* ------------------------------------------------------------------ */
/* Giving names their values                                          */
/* ------------------------------------------------------------------ */

/*
 * Stores a value under a name, following Perry's rules:
 *  - a never_changes_ name can't be changed
 *  - inside a function, names from outside can be read but not changed
 * Remembers how the value was made (for because_is_).
 */
/* A value typed straight into the program, like 5, -2.50 ZAR or [1, 2]. */
static int is_given(const Node *n)
{
    int i;
    if (n->kind == N_LITERAL) return 1;
    if (n->kind == N_NEGATE || n->kind == N_PER) return is_given(n->a);
    if (n->kind != N_LIST)    return 0;
    for (i = 0; i < n->count; i++)
        if (!is_given(n->items[i])) return 0;
    return 1;
}

static void store(const Node *n, Value v, int constant, const Node *source, const char *note)
{
    Scope *scope = locals != NULL ? locals : globals;
    Variable *var = scope_find(scope, n->name, n->name_len);
    char *formula = NULL, *worked = NULL;

    if (source != NULL && !is_given(source)) {
        formula = render_text(source, 0);       /* before the value changes */
        worked  = render_text(source, 1);
    }

    if (var == NULL && locals != NULL && scope_find(globals, n->name, n->name_len) != NULL)
        fail(n->line, "'%.*s' belongs outside this function, so the function can't change it "
                      "- use give_back_ to hand back a result instead", n->name_len, n->name);
    if (var != NULL && constant)
        fail(n->line, "'%.*s' already has a value", n->name_len, n->name);
    if (var != NULL && var->constant)
        fail(n->line, "'%.*s' never changes - it was set with never_changes_", n->name_len, n->name);

    if (var == NULL) var = scope_add(scope, n->name, n->name_len);
    else             forget_explanation(var);

    var->value    = v;
    var->constant = constant;
    var->source   = formula ? source : NULL;
    var->formula  = formula;
    var->worked   = worked;
    if (note != NULL) snprintf(var->note, sizeof var->note, "%s", note);
    else if (formula == NULL && !constant) snprintf(var->note, sizeof var->note, "given");
}

/* erase_ name - the name no longer has a value */
static void erase_name(const Node *n)
{
    Scope *scope = locals != NULL ? locals : globals;
    int i;
    for (i = 0; i < scope->count; i++) {
        Variable *v = &scope->vars[i];
        if (v->name_len != n->name_len || memcmp(v->name, n->name, (size_t)n->name_len) != 0) continue;
        if (v->constant)
            fail(n->line, "'%.*s' never changes, so it can't be erased", n->name_len, n->name);
        forget_explanation(v);
        memmove(v, v + 1, sizeof(Variable) * (size_t)(scope->count - i - 1));
        scope->count--;
        return;
    }
    if (locals != NULL && scope_find(globals, n->name, n->name_len) != NULL)
        fail(n->line, "'%.*s' belongs outside this function, so the function can't erase it",
             n->name_len, n->name);
    fail(n->line, "'%.*s' doesn't have a value, so there's nothing to erase", n->name_len, n->name);
}

/* ------------------------------------------------------------------ */
/* because_is_                                                        */
/* ------------------------------------------------------------------ */

#define MAX_EXPLAINED 64

typedef struct {
    const char *names[MAX_EXPLAINED];
    int         lens[MAX_EXPLAINED];
    int         count;
} Seen;

static int seen_before(Seen *s, const char *name, int len)
{
    int i;
    for (i = 0; i < s->count; i++)
        if (s->lens[i] == len && memcmp(s->names[i], name, (size_t)len) == 0) return 1;
    if (s->count < MAX_EXPLAINED) {
        s->names[s->count] = name;
        s->lens[s->count]  = len;
        s->count++;
    }
    return 0;
}

static void explain(const char *name, int len, int depth, Seen *seen, int line);

/* Explains every name used inside an expression. */
static void explain_inputs(const Node *n, int depth, Seen *seen, int line)
{
    int i;
    if (n == NULL) return;
    if (n->kind == N_VAR) {
        if (!seen_before(seen, n->name, n->name_len))
            explain(n->name, n->name_len, depth, seen, line);
        return;
    }
    explain_inputs(n->a, depth, seen, line);
    explain_inputs(n->b, depth, seen, line);
    explain_inputs(n->c, depth, seen, line);
    for (i = 0; i < n->count; i++) explain_inputs(n->items[i], depth, seen, line);
}

static void explain(const char *name, int len, int depth, Seen *seen, int line)
{
    Variable *var = find_var(name, len);
    const Node *source;
    int pad = depth * 2, prefix;

    if (var == NULL)
        fail(line, "'%.*s' doesn't have a value yet, so there's nothing to explain", len, name);

    printf("%*s%.*s is ", pad, "", len, name);
    if (var->value.kind == VAL_TABLE) {           /* a one-line summary, not every row */
        char summary[512];
        value_format(summary, sizeof summary, var->value);
        fputs(summary, stdout);
    } else {
        value_print(stdout, var->value);
    }
    if (var->constant)     printf(" (never changes)");
    else if (var->note[0]) printf(" (%s)", var->note);
    printf("\n");

    if (var->formula == NULL) return;

    prefix = printf("%*s  because %.*s = ", pad, "", len, name);
    printf("%s\n", var->formula);
    if (strcmp(var->formula, var->worked) != 0)
        printf("%*s= %s\n", prefix - 2, "", var->worked);

    if (depth >= 4) return;
    source = var->source;             /* copy: var may move in memory */
    explain_inputs(source, depth + 1, seen, line);
}

/* ------------------------------------------------------------------ */
/* Evaluating expressions                                             */
/* ------------------------------------------------------------------ */

static Value eval(const Node *n);
typedef enum { FLOW_NORMAL, FLOW_STOP, FLOW_SKIP, FLOW_RETURN } Flow;
static Flow run(const Node *n);
static void  import_module(const Node *n);
static void  share_names(const Node *n);
static Value find_duplicates(const Node *n);
static Value ask_user(const Node *n);

/* Transaction(2026-09-01, -120.00 ZAR, "Coffee") - builds a record, checking each field's type */
static Value make_record(Blueprint *bp, const Node *n)
{
    const Node *def = bp->def;
    PerryTable *t = table_new(def->count, bp->cols);
    Value *cells, record = make_nothing();
    int i;

    if (n->count != def->count) {
        char fields[300];
        size_t pos = 0;
        fields[0] = '\0';
        for (i = 0; i < def->count && pos < sizeof fields - 40; i++)
            pos += (size_t)snprintf(fields + pos, sizeof fields - pos, "%s%s", i ? ", " : "", bp->cols[i]);
        fail(n->line, "%.*s needs %d value%s, in this order: %s", n->name_len, n->name,
             def->count, def->count == 1 ? "" : "s", fields);
    }
    cells = (Value *)malloc(sizeof(Value) * (size_t)(def->count + 1));
    if (cells == NULL) out_of_memory();
    for (i = 0; i < def->count; i++) {
        const Node *type = def->items[i]->a;
        Value v = eval(n->items[i]);
        const char *want = NULL;
        int ok = 1;
        if (v.kind != VAL_NOTHING) {                    /* nothing_ means "not known yet" */
            #define TYPE_IS(word) (type->name_len == (int)strlen(word) && memcmp(type->name, word, strlen(word)) == 0)
            if      (TYPE_IS("number"))   { ok = v.kind == VAL_NUMBER;   want = "a number"; }
            else if (TYPE_IS("money"))    { ok = v.kind == VAL_MONEY;    want = "money"; }
            else if (TYPE_IS("percent"))  { ok = v.kind == VAL_PERCENT;  want = "a percent"; }
            else if (TYPE_IS("text"))     { ok = v.kind == VAL_TEXT;     want = "text"; }
            else if (TYPE_IS("date"))     { ok = v.kind == VAL_DATE;     want = "a date"; }
            else if (TYPE_IS("yes_no"))   { ok = v.kind == VAL_BOOL;     want = "yes_ or no_"; }
            else if (TYPE_IS("list"))     { ok = v.kind == VAL_LIST;     want = "a list"; }
            #undef TYPE_IS
        }
        if (!ok)
            fail(n->items[i]->line, "%.*s's %s must be %s (got %s)", n->name_len, n->name,
                 bp->cols[i], want, value_kind_name(v.kind));
        cells[i] = v;
    }
    table_add_row(t, cells);
    free(cells);
    record.kind  = VAL_ROW;
    record.table = t;
    record.row   = 0;
    return record;
}

/* Is there a name that only differs in capitals? (Perry names are case-sensitive.) */
static int capitals_match(const char *a, int alen, const char *b, int blen)
{
    int i;
    if (alen != blen || memcmp(a, b, (size_t)alen) == 0) return 0;
    for (i = 0; i < alen; i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return 0;
    return 1;
}

static int similar_name(const char *name, int len, const char **found, int *found_len)
{
    Scope *scopes[2];
    int i, k;
    scopes[0] = locals; scopes[1] = globals;
    for (k = 0; k < 2; k++) {
        if (scopes[k] == NULL) continue;
        for (i = 0; i < scopes[k]->count; i++)
            if (capitals_match(name, len, scopes[k]->vars[i].name, scopes[k]->vars[i].name_len)) {
                *found = scopes[k]->vars[i].name; *found_len = scopes[k]->vars[i].name_len; return 1;
            }
    }
    for (i = 0; i < current_module->nfns; i++) {
        const Node *fn = current_module->fns[i].fn;
        if (capitals_match(name, len, fn->name, fn->name_len)) { *found = fn->name; *found_len = fn->name_len; return 1; }
    }
    for (i = 0; i < blueprint_count; i++) {
        const Node *d = blueprints[i].def;
        if (capitals_match(name, len, d->name, d->name_len)) { *found = d->name; *found_len = d->name_len; return 1; }
    }
    return 0;
}

/* Does a function have a give_back_ anywhere in it? */
static int has_give_back(const Node *n)
{
    int i;
    if (n == NULL) return 0;
    if (n->kind == N_RETURN) return 1;
    if (has_give_back(n->a) || has_give_back(n->b) || has_give_back(n->c)) return 1;
    for (i = 0; i < n->count; i++) if (has_give_back(n->items[i])) return 1;
    return 0;
}

static Value call_function(const Node *n, int want_value)
{
    FunctionEntry *entry = find_function(n->name, n->name_len);
    const Node *fn;
    Blueprint *bp;
    Module *caller = current_module;
    Value *args;
    Scope scope = { NULL, 0, 0 };
    Scope *saved_locals = locals;
    int saved_loops = loop_depth;
    char note[64];
    Value result;
    Flow f;
    int i;

    if (entry == NULL) {
        if ((bp = find_blueprint(n->name, n->name_len)) != NULL) return make_record(bp, n);
        if (call_builtin(n, &result)) return result;
        {
            const char *other; int other_len;
            if (n->name_len == 5 && memcmp(n->name, "print", 5) == 0)
                fail(n->line, "Perry uses show_me_results(...) instead of print(...)");
            if (n->name_len == 5 && memcmp(n->name, "input", 5) == 0)
                fail(n->line, "Perry uses ask_user_(...) instead of input(...)");
            if (similar_name(n->name, n->name_len, &other, &other_len))
                fail(n->line, "there's no function called '%.*s' - did you mean '%.*s'? (capitals matter in Perry)",
                     n->name_len, n->name, other_len, other);
        }
        fail(n->line, "there's no function called '%.*s' - create it with define_ %.*s(...):",
             n->name_len, n->name, n->name_len, n->name);
    }
    if (want_value && !has_give_back(entry->fn->b))
        fail(n->line, "%.*s doesn't give_back_ anything, so it has no answer to use here - "
                      "add a line like: give_back_ result", n->name_len, n->name);
    fn = entry->fn;
    if (fn->count != n->count)
        fail(n->line, "%.*s needs %d value%s but got %d", n->name_len, n->name,
             fn->count, fn->count == 1 ? "" : "s", n->count);
    if (call_depth >= MAX_CALL_DEPTH)
        fail(n->line, "too many function calls inside each other - does %.*s call itself forever?",
             n->name_len, n->name);

    /* work out the inputs in the caller's scope first */
    args = (Value *)malloc(sizeof(Value) * (size_t)(n->count + 1));
    if (args == NULL) out_of_memory();
    for (i = 0; i < n->count; i++) args[i] = eval(n->items[i]);

    enter_module(entry->home);          /* the function sees its own module's names */
    locals = &scope;
    loop_depth = 0;
    call_depth++;
    snprintf(note, sizeof note, "passed into %.*s", n->name_len > 40 ? 40 : n->name_len, n->name);
    for (i = 0; i < fn->count; i++) store(fn->items[i], args[i], 0, NULL, note);
    free(args);

    f = run(fn->b);
    result = (f == FLOW_RETURN) ? return_value : make_nothing();

    call_depth--;
    loop_depth = saved_loops;
    locals = saved_locals;
    enter_module(caller);
    scope_free(&scope);
    return result;
}

static Value eval(const Node *n)
{
    Variable *var;
    Value a, b;
    int i;

    switch (n->kind) {
    case N_LITERAL:
        return n->value;

    case N_VAR:
        var = find_var(n->name, n->name_len);
        if (var == NULL) {
            const char *other; int other_len;
            if (similar_name(n->name, n->name_len, &other, &other_len))
                fail(n->line, "'%.*s' doesn't have a value yet - did you mean '%.*s'? (capitals matter in Perry)",
                     n->name_len, n->name, other_len, other);
            fail(n->line, "'%.*s' doesn't have a value yet - give it one first, like: %.*s = 0",
                 n->name_len, n->name, n->name_len, n->name);
        }
        return var->value;

    case N_NEGATE:
        a = eval(n->a);
        if (!value_is_numeric(a)) fail(n->line, "can't make %s negative", value_kind_name(a.kind));
        a.amount = -a.amount;
        return a;

    case N_BINARY:
        a = eval(n->a);
        b = eval(n->b);
        return arithmetic(n->op, a, b, n->line);

    case N_COMPARE:
        a = eval(n->a);
        b = eval(n->b);
        return compare(n->op, a, b, n->line);

    case N_SIMILAR:
        a = eval(n->a);
        b = eval(n->b);
        return similar(n->op, a, b, eval(n->c), n->line);

    case N_AND:
        if (!is_true(eval(n->a), n->line, "and_")) return make_bool(0);
        return make_bool(is_true(eval(n->b), n->line, "and_"));

    case N_OR:
        if (is_true(eval(n->a), n->line, "or_")) return make_bool(1);
        return make_bool(is_true(eval(n->b), n->line, "or_"));

    case N_NOT:
        return make_bool(!is_true(eval(n->a), n->line, "not_"));

    case N_LIST:
        a = make_nothing();
        a.kind = VAL_LIST;
        a.list = list_new();
        for (i = 0; i < n->count; i++) list_push(a.list, eval(n->items[i]));
        return a;

    case N_SPLIT:
        a = eval(n->a);
        b = eval(n->b);
        return split_money(a, b, n->line);

    case N_TOTAL:
        return total_of(eval(n->a), n->line);

    case N_COUNT:
        a = eval(n->a);
        if (a.kind == VAL_TABLE) return make_number(VAL_NUMBER, (dec_t)a.table->nrows * PERRY_SCALE);
        return make_number(VAL_NUMBER, (dec_t)need_list(a, n->line, "count_of_")->count * PERRY_SCALE);

    case N_CALL:
        return call_function(n, 1);

    case N_FIELD:
        return field_of(eval(n->a), n);

    case N_PER:
        return per_value(n);

    case N_ROUND:
        return round_value(n);

    case N_LOAD:
        return load_csv(n);

    case N_FILTER:
        return filter_values(eval(n->a), n, 1);

    case N_TAX_APPLY:
        return apply_tax(n);

    case N_CONVERT:
        return convert_money(n);

    case N_STAT:
        return statistic(n);

    case N_GROUP:
        return group_values(n);

    case N_VAT:
        return vat_value(n);

    case N_DUPLICATES:
        return find_duplicates(n);

    case N_ASK:
        return ask_user(n);

    case N_TODAY: {
        time_t now = time(NULL);
        struct tm *local = localtime(&now);
        return make_date(days_from_date(local->tm_year + 1900, local->tm_mon + 1, local->tm_mday), n->line);
    }

    case N_DURATION:
        a = eval(n->a);
        if (a.kind != VAL_NUMBER || a.amount % PERRY_SCALE != 0)
            fail(n->line, "the number of %s must be a whole number", unit_name((int)n->op));
        return make_duration((long long)(a.amount / PERRY_SCALE), (int)n->op, n->line);

    case N_BETWEEN:
        a = eval(n->a);
        b = eval(n->b);
        if (a.kind != VAL_DATE || b.kind != VAL_DATE)
            fail(n->line, "%s needs two dates", token_keyword_text(n->op));
        if (n->op == TOK_DAYS_BETWEEN)
            return make_number(VAL_NUMBER, (b.amount - a.amount) * PERRY_SCALE);
        return make_number(VAL_NUMBER, (dec_t)count_business_days((long long)a.amount, (long long)b.amount) * PERRY_SCALE);

    case N_END_OF_MONTH: {
        int y, m, d;
        a = eval(n->a);
        if (a.kind != VAL_DATE) fail(n->line, "end_of_month_ needs a date");
        date_from_days(a.amount, &y, &m, &d);
        return make_date(days_from_date(y, m, days_in_month(y, m)), n->line);
    }

    default:
        fail(n->line, "this is a statement, not a value");
    }
    return make_nothing();
}

/* ------------------------------------------------------------------ */
/* Running statements                                                 */
/* ------------------------------------------------------------------ */

/*
 * Runs a block, but catches any runtime error inside it.
 * Returns 0 if the block finished, 1 if an error happened.
 */
static int protect(const Node *body, Flow *flow)
{
    int index = try_depth;
    if (index >= MAX_TRY_DEPTH) fail(body->line, "too many try_this_ blocks inside each other");
    try_stack[index].locals     = locals;
    try_stack[index].loop_depth = loop_depth;
    try_stack[index].call_depth = call_depth;
    try_stack[index].module     = current_module;
    try_depth = index + 1;

    if (setjmp(try_stack[index].env) == 0) {
        *flow = run(body);
        try_depth = index;
        return 0;
    }
    /* An error jumped back here: put everything back as it was. */
    locals     = try_stack[index].locals;
    loop_depth = try_stack[index].loop_depth;
    call_depth = try_stack[index].call_depth;
    enter_module(try_stack[index].module);
    try_depth  = index;
    return 1;
}

static int handler_matches(TokenKind kind, ErrorKind err)
{
    switch (kind) {
    case TOK_THERES_AN_ERROR:          return 1;
    case TOK_THERES_A_CURRENCY_ERROR:  return err == ERR_CURRENCY;
    case TOK_THERES_A_FILE_ERROR:      return err == ERR_FILE;
    case TOK_THERES_A_BALANCE_ERROR:   return err == ERR_BALANCE;
    default:                           return 0;
    }
}

/* try_this_ / even_if_ ... / but_then_ */
static Flow run_try(const Node *n)
{
    Flow flow = FLOW_NORMAL, after;
    int pending = 0, k;
    ErrorKind kind = ERR_GENERAL;
    int line = n->line;
    char message[sizeof error_message];

    if (protect(n->a, &flow)) {
        const Node *handler = NULL;
        for (k = 0; k < n->count && handler == NULL; k++)
            if (handler_matches(n->items[k]->op, error_kind)) handler = n->items[k];

        flow = FLOW_NORMAL;
        if (handler == NULL || protect(handler->b, &flow))
            pending = 1;                       /* not handled, or the handler failed too */
        if (pending) {
            kind = error_kind;
            line = error_line;
            memcpy(message, error_message, sizeof message);
        }
    }

    if (n->c != NULL) {                        /* but_then_ always runs */
        after = run(n->c);
        if (after != FLOW_NORMAL && !pending) flow = after;
    }
    if (pending) raise_error(kind, line, "%s", message);
    return flow;
}

static Flow run(const Node *n)
{
    Value v;
    PerryList *list;
    Seen seen;
    long long i, times;
    int k;
    Flow f;

    switch (n->kind) {
    case N_BLOCK:
        for (k = 0; k < n->count; k++) {
            f = run(n->items[k]);
            if (f != FLOW_NORMAL) return f;
        }
        return FLOW_NORMAL;

    case N_ASSIGN:
        v = eval(n->a);
        store(n, v, 0, n->a, NULL);
        return FLOW_NORMAL;

    case N_CONSTANT:
        v = eval(n->a);
        store(n, v, 1, n->a, NULL);
        return FLOW_NORMAL;

    case N_SHOW: {
        /* work out every value first, so an error never leaves half a line printed */
        Value *values = (Value *)malloc(sizeof(Value) * (size_t)(n->count + 1));
        if (values == NULL) out_of_memory();
        for (k = 0; k < n->count; k++) values[k] = eval(n->items[k]);
        for (k = 0; k < n->count; k++) {
            if (k > 0) putchar(values[k].kind == VAL_TABLE ? '\n' : ' ');
            value_print(stdout, values[k]);
        }
        free(values);
        putchar('\n');
        return FLOW_NORMAL;
    }

    case N_IF:
        if (is_true(eval(n->a), n->line, "check_if_"))
            return run(n->b);
        if (n->c != NULL)
            return run(n->c);
        return FLOW_NORMAL;

    case N_REPEAT:
        v = eval(n->a);
        if (v.kind != VAL_NUMBER || v.amount < 0 || v.amount % PERRY_SCALE != 0)
            fail(n->line, "exactly_ needs a whole number of times, like: exactly_ 12 times:");
        times = (long long)(v.amount / PERRY_SCALE);
        loop_depth++;
        for (i = 0; i < times; i++) {
            f = run(n->b);
            if (f == FLOW_STOP) break;
            if (f == FLOW_RETURN) { loop_depth--; return f; }
        }
        loop_depth--;
        return FLOW_NORMAL;

    case N_WHILE:
        loop_depth++;
        while (is_true(eval(n->a), n->line, "keep_going_while_")) {
            f = run(n->b);
            if (f == FLOW_STOP) break;
            if (f == FLOW_RETURN) { loop_depth--; return f; }
        }
        loop_depth--;
        return FLOW_NORMAL;

    case N_FOR:
        v = eval(n->a);
        if (v.kind == VAL_TABLE) {           /* go_through_ a table row by row */
            PerryTable *t = v.table;
            loop_depth++;
            for (k = 0; k < t->nrows; k++) {
                Value row = make_nothing();
                row.kind  = VAL_ROW;
                row.table = t;
                row.row   = k;
                store(n, row, 0, NULL, "from go_through_");
                f = run(n->b);
                if (f == FLOW_STOP) break;
                if (f == FLOW_RETURN) { loop_depth--; return f; }
            }
            loop_depth--;
            return FLOW_NORMAL;
        }
        list = need_list(v, n->line, "go_through_");
        loop_depth++;
        for (k = 0; k < list->count; k++) {
            store(n, list->items[k], 0, NULL, "from go_through_");
            f = run(n->b);
            if (f == FLOW_STOP) break;
            if (f == FLOW_RETURN) { loop_depth--; return f; }
        }
        loop_depth--;
        return FLOW_NORMAL;

    case N_REQUIRE:
        if (!is_true(eval(n->a), n->line, "not_unless_"))
            raise_error(ERR_BALANCE, n->line, "requirement not met - this must be true to continue");
        return FLOW_NORMAL;

    case N_TRY:
        return run_try(n);

    case N_BOOK:
        book_entry(n);
        return FLOW_NORMAL;

    case N_SAVE:
        save_value(n);
        return FLOW_NORMAL;

    case N_ADD:
        v = add_value(eval(n->a), eval(n->b), n);
        store(n->a, v, 0, NULL, "after add_where_");
        return FLOW_NORMAL;

    case N_ERASE:
        erase_name(n);
        return FLOW_NORMAL;

    case N_ALARM: {
        char message[400];
        v = eval(n->a);
        value_format(message, sizeof message, v);
        fail(n->line, "%s", message);
        return FLOW_NORMAL;
    }

    case N_IMPORT:
        import_module(n);
        return FLOW_NORMAL;

    case N_SHARE:
        share_names(n);
        return FLOW_NORMAL;

    case N_BLUEPRINT:
        return FLOW_NORMAL;          /* already registered before the run */

    case N_STOP:
    case N_SKIP:
        if (loop_depth == 0)
            fail(n->line, "%s only works inside a loop", n->kind == N_STOP ? "stop_now_" : "skip_this_one_");
        return n->kind == N_STOP ? FLOW_STOP : FLOW_SKIP;

    case N_FUNCTION:
        return FLOW_NORMAL;          /* already registered before the run */

    case N_RETURN:
        return_value = eval(n->a);
        return FLOW_RETURN;

    case N_BECAUSE:
        seen.count = 0;
        seen_before(&seen, n->name, n->name_len);
        explain(n->name, n->name_len, 0, &seen, n->line);
        return FLOW_NORMAL;

    case N_CALL:
        call_function(n, 0);         /* a call on its own line: no answer needed */
        return FLOW_NORMAL;

    case N_REMOVE:
        v = filter_values(eval(n->a), n, 0);
        store(n->a, v, 0, NULL, "after remove_where_");
        return FLOW_NORMAL;

    case N_SORT:
        v = sort_values(eval(n->a), n);
        store(n->a, v, 0, NULL, "sorted with sort_");
        return FLOW_NORMAL;

    case N_TAX_TABLE:
        store(n, build_tax_table(n), 1, NULL, NULL);
        return FLOW_NORMAL;

    default:
        eval(n);
        return FLOW_NORMAL;
    }
}

static void register_definitions(Module *m, const Node *program);

/* ------------------------------------------------------------------ */
/* Modules: steal_from_, in_that_, share_                             */
/* ------------------------------------------------------------------ */

static int name_is(const char *name, int len, const char *word)
{
    return len == (int)strlen(word) && memcmp(name, word, (size_t)len) == 0;
}

static char *read_text_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    long size;
    size_t got;
    if (f == NULL) return NULL;
    fseek(f, 0L, SEEK_END);
    size = ftell(f);
    rewind(f);
    buf = (char *)malloc((size_t)size + 1);
    if (buf == NULL) out_of_memory();
    got = fread(buf, 1, (size_t)size, f);
    buf[got] = '\0';
    fclose(f);
    return buf;
}

/* Loads name.perry (next to the file that asks for it) and runs it once. */
static Module *load_module(const Node *n)
{
    Module *m, *caller = current_module;
    Scope *saved_locals = locals;
    const char *slash = strrchr(caller->path, '/');
    char *path;
    const Node *program;
    int i;

    for (i = 0; i < module_count; i++) {
        if (name_is(n->name, n->name_len, modules[i]->name)) {
            if (modules[i]->loading)
                fail(n->line, "'%.*s' is already being loaded - two modules can't steal_from_ each other",
                     n->name_len, n->name);
            return modules[i];
        }
    }
    if (name_is(n->name, n->name_len, caller->name) || (caller == &main_module && name_is(n->name, n->name_len, "main")))
        fail(n->line, "a file can't steal_from_ itself");
    if (module_count >= MAX_MODULES) fail(n->line, "too many modules");
    if (n->name_len > 60) fail(n->line, "that module name is too long");

    path = (char *)malloc(strlen(caller->path) + (size_t)n->name_len + 16);
    if (path == NULL) out_of_memory();
    if (slash) sprintf(path, "%.*s/%.*s.perry", (int)(slash - caller->path), caller->path, n->name_len, n->name);
    else       sprintf(path, "%.*s.perry", n->name_len, n->name);

    m = (Module *)calloc(1, sizeof(Module));
    if (m == NULL) out_of_memory();
    snprintf(m->name, sizeof m->name, "%.*s", n->name_len, n->name);
    m->path   = path;
    m->source = read_text_file(path);
    if (m->source == NULL) {
        free(m);
        raise_error(ERR_FILE, n->line, "can't find the module file '%s'", path);
    }
    modules[module_count++] = m;

    /* parse and run it with its own names; errors point at the module's file */
    enter_module(m);
    program = parse_program(m->source);
    m->loading = 1;
    locals = NULL;
    register_definitions(m, program);
    run(program);
    m->loading = 0;
    m->loaded  = 1;
    locals = saved_locals;
    enter_module(caller);
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

/* Brings one name from a module into the current file. */
static void take_name(Module *m, const char *name, int len, int line)
{
    FunctionEntry *fe = find_function_in(m, name, len);
    Variable *v = scope_find(&m->globals, name, len), *mine;

    if (fe != NULL) {
        FunctionEntry *existing = find_function(name, len);
        if (existing != NULL && existing->fn != fe->fn)
            fail(line, "there's already a function called '%.*s' here", len, name);
        if (existing == NULL) add_function(current_module, fe->fn, fe->home);
        return;
    }
    if (v != NULL) {
        Variable *existing = scope_find(globals, name, len);
        char from[80];
        snprintf(from, sizeof from, "from %.50s", m->name);
        if (existing != NULL && strcmp(existing->note, from) == 0) return;   /* taken before */
        if (existing != NULL)
            fail(line, "'%.*s' already has a value here, so it can't be taken from %s", len, name, m->name);
        mine = scope_add(globals, name, len);
        mine->value    = v->value;
        mine->constant = 1;                  /* taken values never change */
        snprintf(mine->note, sizeof mine->note, "from %.50s", m->name);
        return;
    }
    if (find_blueprint(name, len) != NULL) return;   /* blueprints are available everywhere */
    fail(line, "%s has no function or value called '%.*s'", m->name, len, name);
}

static void import_module(const Node *n)
{
    Module *m;
    int i;

    if (name_is(n->name, n->name_len, "holidays_za")) {    /* built into Perry */
        if (n->count > 0) fail(n->line, "holidays_za is built in - just write: steal_from_ holidays_za");
        use_za_holidays = 1;
        return;
    }
    m = load_module(n);

    if (n->count > 0) {                                      /* in_that_ m steal_from_ a, b */
        for (i = 0; i < n->count; i++) {
            const Node *want = n->items[i];
            if (!is_shared(m, want->name, want->name_len))
                fail(want->line, "%s doesn't share '%.*s' - add 'share_ %.*s' to %s.perry",
                     m->name, want->name_len, want->name, want->name_len, want->name, m->name);
            take_name(m, want->name, want->name_len, want->line);
        }
        return;
    }
    if (m->used_share) {                                     /* steal_from_ m: everything it shares */
        for (i = 0; i < m->nshared; i++)
            take_name(m, m->shared[i]->name, m->shared[i]->name_len, n->line);
    } else {                                                 /* ... or everything, if it shares nothing */
        for (i = 0; i < m->nfns; i++)
            if (m->fns[i].home == m) take_name(m, m->fns[i].fn->name, m->fns[i].fn->name_len, n->line);
        for (i = 0; i < m->globals.count; i++)
            take_name(m, m->globals.vars[i].name, m->globals.vars[i].name_len, n->line);
    }
}

static void share_names(const Node *n)
{
    Module *m = current_module;
    int i;
    m->used_share = 1;
    for (i = 0; i < n->count; i++) {
        const Node *name = n->items[i];
        if (find_function(name->name, name->name_len) == NULL &&
            scope_find(globals, name->name, name->name_len) == NULL &&
            find_blueprint(name->name, name->name_len) == NULL)
            fail(name->line, "can't share '%.*s' - nothing with that name exists yet (put share_ after it)",
                 name->name_len, name->name);
        m->shared = (const Node **)realloc(m->shared, sizeof(Node *) * (size_t)(m->nshared + 1));
        if (m->shared == NULL) out_of_memory();
        m->shared[m->nshared++] = name;
    }
}

/* ------------------------------------------------------------------ */
/* duplicate_where_ and ask_user_                                     */
/* ------------------------------------------------------------------ */

static int same_group(Value a, Value b, int line);

/* duplicate_where_ statement by_ amount -> every item whose value appears more than once */
static Value find_duplicates(const Node *n)
{
    Value src = eval(n->a), out = make_nothing(), *keys;
    int count, i, j, col = -1;

    if (src.kind == VAL_TABLE) {
        count = src.table->nrows;
        if (n->name != NULL) col = column_of(src.table, n->name, n->name_len, n->line);
    } else if (src.kind == VAL_LIST) {
        count = src.list->count;
    } else {
        fail(n->line, "duplicate_where_ needs a list or a table (got %s)", value_kind_name(src.kind));
        return src;
    }

    keys = (Value *)malloc(sizeof(Value) * (size_t)(count + 1));
    if (keys == NULL) out_of_memory();
    for (i = 0; i < count; i++) {
        Value item;
        if (src.kind == VAL_TABLE) {
            item = make_nothing();
            item.kind = VAL_ROW; item.table = src.table; item.row = i;
        } else {
            item = src.list->items[i];
        }
        if (n->name != NULL) {
            if (item.kind != VAL_ROW)
                fail(n->line, "by_ %.*s needs a table or a list of records", n->name_len, n->name);
            keys[i] = table_cell(item.table, item.row, column_of(item.table, n->name, n->name_len, n->line));
        } else {
            keys[i] = item;
        }
    }

    if (src.kind == VAL_TABLE) {
        PerryTable *t = table_new(src.table->ncols, src.table->cols);
        for (i = 0; i < count; i++)
            for (j = 0; j < count; j++)
                if (i != j && same_group(keys[i], keys[j], n->line)) {
                    table_add_row(t, src.table->cells + (size_t)i * (size_t)src.table->ncols);
                    break;
                }
        out = make_table(t);
    } else {
        out.kind = VAL_LIST;
        out.list = list_new();
        for (i = 0; i < count; i++)
            for (j = 0; j < count; j++)
                if (i != j && same_group(keys[i], keys[j], n->line)) {
                    list_push(out.list, src.list->items[i]);
                    break;
                }
    }
    (void)col;
    free(keys);
    return out;
}

/* Compares two words, ignoring capitals. */
static int same_word(const char *a, const char *b)
{
    while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { a++; b++; }
    return *a == '\0' && *b == '\0';
}

/*
 * Reads an amount the way people type it:
 *   25000   25000.50   -35   25 000   25,000.00   25 000,50 (comma for cents)
 *   R25000  R 25 000   $5    ZAR 25000   25000 ZAR   25000zar   15%
 * Fills the amount, how many decimals were typed, a currency code (or "")
 * and whether it ended in %. Returns 0 if it isn't an amount at all.
 */
static int parse_typed_amount(const char *text, dec_t *amount, int *decimals, char code[4], int *percent)
{
    char buf[256], digits[256];
    const char *s = text;
    int len, n = 0, i, neg = 0, commas = 0, dots = 0, last_comma = -1;

    code[0] = '\0';
    *percent = 0;
    while (*s == ' ') s++;
    snprintf(buf, sizeof buf, "%s", s);
    len = (int)strlen(buf);
    while (len > 0 && buf[len - 1] == ' ') buf[--len] = '\0';
    s = buf;

    /* a currency in front: R, $, EUR sign, pound sign, or a 3-letter code */
    if ((s[0] == 'R' || s[0] == 'r') && (s[1] == ' ' || s[1] == '-' || (s[1] >= '0' && s[1] <= '9'))) {
        strcpy(code, "ZAR"); s++;
    } else if (s[0] == '$') {
        strcpy(code, "USD"); s++;
    } else if ((unsigned char)s[0] == 0xE2 && (unsigned char)s[1] == 0x82 && (unsigned char)s[2] == 0xAC) {
        strcpy(code, "EUR"); s += 3;
    } else if ((unsigned char)s[0] == 0xC2 && (unsigned char)s[1] == 0xA3) {
        strcpy(code, "GBP"); s += 2;
    } else if (len > 4 && isalpha((unsigned char)s[0]) && isalpha((unsigned char)s[1]) &&
               isalpha((unsigned char)s[2]) && (s[3] == ' ' || s[3] == '-' || isdigit((unsigned char)s[3]))) {
        for (i = 0; i < 3; i++) code[i] = (char)toupper((unsigned char)s[i]);
        code[3] = '\0'; s += 3;
    }
    while (*s == ' ') s++;
    len = (int)strlen(s);

    /* a currency or % at the end */
    if (len > 0 && s[len - 1] == '%') { *percent = 1; len--; }
    else if (len > 3 && isalpha((unsigned char)s[len - 1]) && isalpha((unsigned char)s[len - 2]) &&
             isalpha((unsigned char)s[len - 3]) && !isalpha((unsigned char)s[len - 4])) {
        char end_code[4];
        for (i = 0; i < 3; i++) end_code[i] = (char)toupper((unsigned char)s[len - 3 + i]);
        end_code[3] = '\0';
        if (code[0] && strcmp(code, end_code) != 0) return 0;
        strcpy(code, end_code);
        len -= 3;
    }
    while (len > 0 && s[len - 1] == ' ') len--;

    /* the number itself, with spaces or commas between thousands */
    for (i = 0; i < len; i++) {
        char c = s[i];
        if (c == '-' && n == 0 && !neg) { neg = 1; continue; }
        if (isdigit((unsigned char)c)) { digits[n++] = c; continue; }
        if (c == ' ') continue;
        if (c == ',') { commas++; last_comma = n; continue; }
        if (c == '.') { dots++; digits[n++] = '.'; continue; }
        return 0;
    }
    digits[n] = '\0';
    if (n == 0 || dots > 1) return 0;
    /* one comma followed by 1 or 2 digits at the end is a decimal comma: 25 000,50 */
    if (commas == 1 && dots == 0 && n - last_comma >= 1 && n - last_comma <= 2) {
        memmove(digits + last_comma + 1, digits + last_comma, (size_t)(n - last_comma + 1));
        digits[last_comma] = '.';
        n++;
    }
    if (dec_parse(digits, n, amount, decimals) != 1 || digits[n - 1] == '.') return 0;
    if (neg) *amount = -*amount;
    return 1;
}

/*
 * Turns what someone typed into a value. 'want' is NULL (take it as it comes),
 * a currency code like "ZAR", or number / percent / date / text / yes_no.
 * Returns 0 and fills 'problem' if the answer doesn't fit.
 */
static int read_answer(char *s, const char *want, Value *out, char *problem, size_t size)
{
    char *end, code[4];
    int len, decimals, percent;
    dec_t amount;
    char *copy;

    while (*s == ' ' || *s == '\t') s++;
    end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' || end[-1] == '\r')) end--;
    *end = '\0';
    len = (int)(end - s);
    *out = make_nothing();

    if (want != NULL && strcmp(want, "text") == 0) goto as_text;

    {
        long long days;
        if (parse_date(s, len, &days)) {
            if (want == NULL || strcmp(want, "date") == 0) { out->kind = VAL_DATE; out->amount = days; return 1; }
        }
    }
    if (want != NULL && strcmp(want, "date") == 0) {
        snprintf(problem, size, "Please type a date as year-month-day, like 2026-12-31");
        return 0;
    }
    {
        int yes = same_word(s, "yes") || same_word(s, "y");
        int no  = same_word(s, "no")  || same_word(s, "n");
        if ((yes || no) && (want == NULL || strcmp(want, "yes_no") == 0) &&
            (want != NULL || !same_word(s, "y")) && (want != NULL || !same_word(s, "n"))) {
            out->kind = VAL_BOOL; out->boolean = yes; return 1;
        }
        if (want != NULL && strcmp(want, "yes_no") == 0) {
            snprintf(problem, size, "Please type yes or no");
            return 0;
        }
    }

    if (parse_typed_amount(s, &amount, &decimals, code, &percent)) {
        if (want != NULL && strcmp(want, "number") == 0) {
            if (!code[0] && !percent) { out->kind = VAL_NUMBER; out->amount = amount; return 1; }
            snprintf(problem, size, "Please type just a number, like 12");
            return 0;
        }
        if (want != NULL && strcmp(want, "percent") == 0) {
            if (code[0] || decimals > PERRY_DECIMALS - 2) {
                snprintf(problem, size, "Please type a percentage, like 15%%");
                return 0;
            }
            out->kind = VAL_PERCENT; out->amount = amount / 100; return 1;
        }
        if (want != NULL) {                           /* a currency like ZAR */
            if (percent || (code[0] && strcmp(code, want) != 0)) {
                snprintf(problem, size, "Please type an amount in %s, like 25000.00", want);
                return 0;
            }
            strcpy(code, want);
        }
        if (percent) {
            if (decimals > PERRY_DECIMALS - 2) goto as_text;
            out->kind = VAL_PERCENT; out->amount = amount / 100; return 1;
        }
        if (code[0]) {
            if (decimals > currency_decimals(code)) {
                snprintf(problem, size, "%s amounts have at most %d decimal places - please try again",
                         code, currency_decimals(code));
                return 0;
            }
            *out = make_money(amount, code);
            return 1;
        }
        out->kind = VAL_NUMBER; out->amount = amount; return 1;
    }
    if (want != NULL) {
        if (strcmp(want, "number") == 0)       snprintf(problem, size, "Please type a number, like 12");
        else if (strcmp(want, "percent") == 0) snprintf(problem, size, "Please type a percentage, like 15%%");
        else                                   snprintf(problem, size, "Please type an amount in %s, like 25000.00", want);
        return 0;
    }

as_text:
    copy = (char *)malloc((size_t)len + 1);
    if (copy == NULL) out_of_memory();
    memcpy(copy, s, (size_t)len + 1);
    *out = make_text(copy, len);
    return 1;
}

/* ask_user_("Your salary?") as_ ZAR - asks again until the answer fits */
static Value ask_user(const Node *n)
{
    Value prompt = eval(n->a), answer;
    char line[1024], want[16], problem[160];

    if (n->name != NULL) snprintf(want, sizeof want, "%.*s", n->name_len, n->name);
    for (;;) {
        value_print(stdout, prompt);
        if (prompt.kind != VAL_TEXT || prompt.text_len == 0 || prompt.text[prompt.text_len - 1] != ' ')
            putchar(' ');
        fflush(stdout);
        if (fgets(line, sizeof line, stdin) == NULL)
            fail(n->line, "no answer could be read - run this program in a terminal to type answers");
        if (read_answer(line, n->name ? want : NULL, &answer, problem, sizeof problem)) return answer;
        printf("%s\n", problem);
    }
}

/* Registers a file's define_ functions and blueprint_ records before it runs. */
static void register_definitions(Module *m, const Node *program)
{
    int k, i;
    for (k = 0; k < program->count; k++) {
        const Node *d = program->items[k];
        if (d->kind == N_FUNCTION) {
            if (find_function_in(m, d->name, d->name_len) != NULL)
                fail(d->line, "there are two functions called '%.*s'", d->name_len, d->name);
            add_function(m, d, m);
        } else if (d->kind == N_BLUEPRINT) {
            Blueprint *bp = find_blueprint(d->name, d->name_len);
            if (bp != NULL && bp->def != d)
                fail(d->line, "there are two blueprints called '%.*s'", d->name_len, d->name);
            if (bp != NULL) continue;
            blueprints = (Blueprint *)realloc(blueprints, sizeof(Blueprint) * (size_t)(blueprint_count + 1));
            if (blueprints == NULL) out_of_memory();
            bp = &blueprints[blueprint_count++];
            bp->def  = d;
            bp->cols = (char **)malloc(sizeof(char *) * (size_t)(d->count + 1));
            if (bp->cols == NULL) out_of_memory();
            for (i = 0; i < d->count; i++) {
                bp->cols[i] = (char *)malloc((size_t)d->items[i]->name_len + 1);
                if (bp->cols[i] == NULL) out_of_memory();
                memcpy(bp->cols[i], d->items[i]->name, (size_t)d->items[i]->name_len);
                bp->cols[i][d->items[i]->name_len] = '\0';
            }
        }
    }
}

void interpret(const Node *program)
{
    snprintf(main_module.name, sizeof main_module.name, "main");
    main_module.path   = g_file_name;
    main_module.source = g_source;
    main_module.loading = 1;
    enter_module(&main_module);
    register_definitions(&main_module, program);
    run(program);
}
