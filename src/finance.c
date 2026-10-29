/*
 * finance.c - Perry for Universities: finance, engineering economics,
 * actuarial maths and statistics.
 *
 *   npv_ irr_ payback_ discounted_payback_
 *   straight_line_ reducing_balance_ sum_of_years_
 *   annuity_ annuity_due_ deferred_annuity_ accumulated_annuity_ perpetuity_
 *   bond_price_ bond_yield_
 *   variance_ standard_deviation_ population_variance_
 *   population_standard_deviation_ median_ percentile_ correlation_ regression_
 *   survival_ life_expectancy_ life_annuity_ temporary_life_annuity_
 *   life_insurance_ term_insurance_ pure_endowment_ endowment_
 *   square_root_ power_
 *
 * Still no floating point anywhere:
 *
 *  - Money maths works with 24 decimal places inside (XP below) and rounds
 *    once, at the end, to Perry's 12 decimals (and then to cents for money).
 *  - Statistics are worked out as exact fractions of big whole numbers
 *    (up to 4096 bits), so a variance or a correlation is rounded only once.
 */
#include <stdlib.h>
#include <string.h>
#include "finance.h"
#include "interp.h"

__extension__ typedef unsigned __int128 u128;

#define XP (PERRY_SCALE * PERRY_SCALE)      /* 1 with 24 decimal places */
#define MAX_PERIODS 100000
#define fail perry_runtime_fail

/* ------------------------------------------------------------------ */
/* Making values                                                      */
/* ------------------------------------------------------------------ */

static Value blank(ValueKind kind)
{
    Value v;
    memset(&v, 0, sizeof v);
    v.kind = kind;
    return v;
}

static Value num(dec_t amount)
{
    Value v = blank(VAL_NUMBER);
    v.amount = amount;
    return v;
}

static Value money(dec_t amount, const char *currency)
{
    Value v = blank(VAL_MONEY);
    memcpy(v.currency, currency, 4);
    v.amount = dec_round(amount, currency_decimals(currency));
    return v;
}

static Value percent(dec_t fraction, int period)
{
    Value v = blank(VAL_PERCENT);
    v.amount = fraction;
    v.period = period;
    return v;
}

/* Same kind as the example: money keeps its currency, a percent its period. */
static Value like(Value example, dec_t amount)
{
    if (example.kind == VAL_MONEY)   return money(amount, example.currency);
    if (example.kind == VAL_PERCENT) return percent(amount, example.period);
    return num(amount);
}

static void too_big(int line)
{
    fail(line, "the result is too big for Perry");
}

/* n / d, halves away from zero */
static dec_t div_round(dec_t n, dec_t d)
{
    dec_t q = n / d, r = n % d;
    dec_t ar = r < 0 ? -r : r, ad = d < 0 ? -d : d;
    if (2 * ar >= ad) q += ((n < 0) != (d < 0)) ? -1 : 1;
    return q;
}

/* ------------------------------------------------------------------ */
/* 24-decimal helpers                                                 */
/* ------------------------------------------------------------------ */

static dec_t xmul(dec_t a, dec_t b, int line)
{
    dec_t r;
    if (!dec_muldiv_raw(a, b, XP, &r))
        fail(line, "the rate is too high or the time too long to calculate");
    return r;
}

static dec_t xdiv(dec_t a, dec_t b, int line)
{
    dec_t r;
    if (b == 0) fail(line, "can't divide by zero");
    if (!dec_muldiv_raw(a, XP, b, &r))
        fail(line, "the rate is too high or the time too long to calculate");
    return r;
}

/* base^n with 24 decimals (n >= 0), by repeated squaring */
static dec_t xpow(dec_t base, long long n, int line)
{
    dec_t result = XP;
    while (n > 0) {
        if (n & 1) result = xmul(result, base, line);
        n >>= 1;
        if (n > 0) base = xmul(base, base, line);
    }
    return result;
}

/* 24 decimals -> Perry's 12 */
static dec_t to12(dec_t x)
{
    return div_round(x, PERRY_SCALE);
}

/* amount (12 decimals) * factor (24 decimals) -> 12 decimals */
static dec_t times_factor(dec_t amount, dec_t factor, int line)
{
    dec_t r;
    if (!dec_muldiv_raw(amount, factor, XP, &r) || !dec_fits(r)) too_big(line);
    return r;
}

/* ------------------------------------------------------------------ */
/* Checking what was given                                            */
/* ------------------------------------------------------------------ */

static const char *fname;       /* the function being called, for messages */

static void need_count(int count, int want, int line, const char *example)
{
    if (count != want)
        fail(line, "%s needs %d value%s, like: %s", fname, want, want == 1 ? "" : "s", example);
}

/* A rate as a fraction with 24 decimals: 10% -> 0.1 */
static dec_t rate_xp(Value rate, int line, const char *which)
{
    dec_t r;
    if (rate.kind != VAL_PERCENT)
        fail(line, "%s needs %s as a percent, like 10%% (got %s)", fname, which, value_kind_name(rate.kind));
    if (!dec_muldiv_raw(rate.amount, PERRY_SCALE, value_divisor(rate), &r)) too_big(line);
    if (r <= -XP) fail(line, "%s can't work with a rate of -100%% or less", fname);
    return r;
}

/* How many rate periods: a whole number, or 20 years / 240 months to match the rate. */
static long long periods_of(Value n, int rate_period, int line, const char *what)
{
    long long count;
    if (n.kind == VAL_DURATION) {
        if (n.period == DUR_YEAR && rate_period == PERIOD_YEAR)        count = (long long)n.amount;
        else if (n.period == DUR_YEAR && rate_period == PERIOD_MONTH)  count = (long long)n.amount * 12;
        else if (n.period == DUR_MONTH && rate_period == PERIOD_MONTH) count = (long long)n.amount;
        else if (n.period == DUR_MONTH && rate_period == PERIOD_YEAR) {
            if (n.amount % 12 != 0)
                fail(line, "the rate is per year, so give %s in whole years (or use 'rate per month')", what);
            count = (long long)(n.amount / 12);
        } else if (n.period == DUR_YEAR || n.period == DUR_MONTH) {
            fail(line, "say if the rate is per year or per month, like 5%% per year, "
                       "or give %s as a plain number of periods", what);
            return 0;
        } else {
            fail(line, "%s must be in years or months, or a plain number of periods", what);
            return 0;
        }
    } else if (n.kind == VAL_NUMBER) {
        if (n.amount % PERRY_SCALE != 0) fail(line, "%s must be a whole number", what);
        if (n.amount / PERRY_SCALE > MAX_PERIODS || n.amount < 0) count = -1;
        else count = (long long)(n.amount / PERRY_SCALE);
    } else {
        fail(line, "%s needs %s as a number or a length of time like 10 years (got %s)",
             fname, what, value_kind_name(n.kind));
        return 0;
    }
    if (count < 0 || count > MAX_PERIODS) fail(line, "%s must be from 0 to %d", what, MAX_PERIODS);
    return count;
}

/* A payment: money or a plain number (1 gives the annuity factor itself). */
static void need_amount(Value v, int line, const char *what)
{
    if (v.kind != VAL_MONEY && v.kind != VAL_NUMBER)
        fail(line, "%s needs %s as money or a number (got %s)", fname, what, value_kind_name(v.kind));
}

/* Cash flows: a list of money (one currency) or of numbers; nothing_ counts as 0. */
static dec_t *cash_flows(Value list, int line, int *count, Value *example)
{
    dec_t *flows;
    int i, found = 0;

    if (list.kind != VAL_LIST)
        fail(line, "%s needs the cash flows as a list, like [-1000.00 ZAR, 400.00 ZAR, 700.00 ZAR] "
                   "(got %s)", fname, value_kind_name(list.kind));
    if (list.list->count < 2) fail(line, "%s needs at least 2 cash flows", fname);
    flows = (dec_t *)malloc(sizeof(dec_t) * (size_t)list.list->count);
    if (flows == NULL) { fprintf(stderr, "PERRY: out of memory\n"); exit(1); }

    *example = num(0);
    for (i = 0; i < list.list->count; i++) {
        Value it = list.list->items[i];
        flows[i] = 0;
        if (it.kind == VAL_NOTHING) continue;
        if (it.kind != VAL_MONEY && it.kind != VAL_NUMBER)
            fail(line, "cash flows must be money or numbers (item %d is %s)", i + 1, value_kind_name(it.kind));
        if (!found) { *example = it; found = 1; }
        else if (it.kind != example->kind ||
                 (it.kind == VAL_MONEY && strcmp(it.currency, example->currency) != 0))
            fail(line, "all the cash flows must be the same kind - item %d is %s, item 1 is %s",
                 i + 1, it.kind == VAL_MONEY ? it.currency : "a number",
                 example->kind == VAL_MONEY ? example->currency : "a number");
        flows[i] = it.amount;
    }
    *count = list.list->count;
    return flows;
}

/* ------------------------------------------------------------------ */
/* NPV, IRR and payback                                               */
/* ------------------------------------------------------------------ */

/*
 * Sum of flow[t] / (1+r)^t, with 24 decimals. The first flow is today (t = 0).
 * Returns 0 if the numbers get too big (a very negative rate).
 */
static int npv_xp(const dec_t *flow, int count, dec_t r, dec_t *out)
{
    dec_t v, vt = XP, sum = 0, term;
    int t;
    if (XP + r <= 0) return 0;
    if (!dec_muldiv_raw(XP, XP, XP + r, &v)) return 0;          /* v = 1 / (1 + r) */
    for (t = 0; t < count; t++) {
        if (t > 0 && !dec_muldiv_raw(vt, v, XP, &vt)) return 0;
        if (!dec_muldiv_raw(flow[t], vt, PERRY_SCALE, &term)) return 0;   /* 24 decimals */
        if (__builtin_add_overflow(sum, term, &sum)) return 0;
    }
    *out = sum;
    return 1;
}

static int sign_of(dec_t x) { return x > 0 ? 1 : x < 0 ? -1 : 0; }

/* The rate (24 decimals) that makes the NPV zero. */
static dec_t solve_rate(const dec_t *flow, int count, int line)
{
    /* rates to try, from 0 outwards: -90% .. 1000% */
    static const int tries[] = { 0, 5, -5, 10, -10, 20, -20, 35, -35, 50, -50, 75, -75, 100, -90,
                                 150, 200, 300, 500, 1000 };
    dec_t pts[32], vals[32], lo, hi, mid, flo = 0, fmid;
    int ok[32], n = (int)(sizeof tries / sizeof tries[0]), i, j, pos = 0, neg = 0, found = 0;

    for (i = 0; i < count; i++) { if (flow[i] > 0) pos = 1; if (flow[i] < 0) neg = 1; }
    if (!pos || !neg)
        fail(line, "%s needs at least one negative cash flow (money out) and one positive (money in)", fname);

    /* sort the tries by rate so neighbours can be checked for a sign change */
    for (i = 0; i < n; i++) pts[i] = (dec_t)tries[i] * XP / 100;
    for (i = 1; i < n; i++)
        for (j = i; j > 0 && pts[j - 1] > pts[j]; j--) { dec_t t = pts[j]; pts[j] = pts[j - 1]; pts[j - 1] = t; }
    for (i = 0; i < n; i++) {
        ok[i] = npv_xp(flow, count, pts[i], &vals[i]);
        if (ok[i] && vals[i] == 0) return pts[i];
    }
    /* the bracket closest to 0% wins */
    lo = hi = 0;
    {
        int zero = 0, best = -1, dist, best_dist = 1 << 30;
        for (i = 0; i < n; i++) if (pts[i] == 0) zero = i;
        for (i = 0; i + 1 < n; i++) {
            if (!ok[i] || !ok[i + 1] || sign_of(vals[i]) * sign_of(vals[i + 1]) >= 0) continue;
            dist = i >= zero ? i - zero : zero - (i + 1);
            if (dist < best_dist) { best_dist = dist; best = i; }
        }
        if (best >= 0) { lo = pts[best]; hi = pts[best + 1]; flo = vals[best]; found = 1; }
    }
    if (!found)
        fail(line, "%s couldn't find a rate between -90%% and 1000%% that makes the NPV zero", fname);

    while (hi - lo > 1) {                          /* halve the gap until it's 10^-24 */
        mid = lo + (hi - lo) / 2;
        if (!npv_xp(flow, count, mid, &fmid)) fail(line, "%s: the numbers got too big", fname);
        if (fmid == 0) return mid;
        if (sign_of(fmid) == sign_of(flo)) { lo = mid; flo = fmid; } else hi = mid;
    }
    return lo + (hi - lo) / 2;
}

static Value do_npv(const Value *a, int count, int line)
{
    Value example;
    dec_t *flows, r, sum;
    int n;
    need_count(count, 2, line, "npv_(10% per year, [-1000.00 ZAR, 600.00 ZAR, 600.00 ZAR])");
    r = rate_xp(a[0], line, "the rate first");
    flows = cash_flows(a[1], line, &n, &example);
    if (!npv_xp(flows, n, r, &sum)) too_big(line);
    free(flows);
    sum = to12(sum);
    if (!dec_fits(sum)) too_big(line);
    return like(example, sum);
}

static Value do_irr(const Value *a, int count, int line)
{
    Value example;
    dec_t *flows, r;
    int n;
    need_count(count, 1, line, "irr_([-1000.00 ZAR, 600.00 ZAR, 600.00 ZAR])");
    flows = cash_flows(a[0], line, &n, &example);
    r = solve_rate(flows, n, line);
    free(flows);
    return percent(to12(r), PERIOD_NONE);
}

/* payback_(flows) and discounted_payback_(rate, flows): periods until the money is back */
static Value do_payback(const Value *a, int count, int line, int discounted)
{
    Value example;
    dec_t *flows, r = 0, v = XP, vt = XP, cum = 0, prev, step, part;
    int n, t;

    if (discounted) {
        need_count(count, 2, line, "discounted_payback_(10% per year, [-1000.00 ZAR, 600.00 ZAR, 600.00 ZAR])");
        r = rate_xp(a[0], line, "the rate first");
        flows = cash_flows(a[1], line, &n, &example);
        v = xdiv(XP, XP + r, line);
    } else {
        need_count(count, 1, line, "payback_([-1000.00 ZAR, 600.00 ZAR, 600.00 ZAR])");
        flows = cash_flows(a[0], line, &n, &example);
    }
    for (t = 0; t < n; t++) {
        if (t > 0 && discounted) vt = xmul(vt, v, line);
        if (discounted) {                        /* today's value of this flow, 24 decimals */
            if (!dec_muldiv_raw(flows[t], vt, PERRY_SCALE, &step)) too_big(line);
        } else {
            step = flows[t];
        }
        prev = cum;
        cum += step;
        if (t == 0 && cum >= 0) { free(flows); return num(0); }
        if (t > 0 && prev < 0 && cum >= 0) {
            /* part of period t: the shortfall at the start / what period t brings in */
            if (!dec_muldiv_raw(-prev, PERRY_SCALE, step, &part)) too_big(line);
            free(flows);
            return num((dec_t)(t - 1) * PERRY_SCALE + part);
        }
    }
    free(flows);
    fail(line, "%s: the money is never paid back within these cash flows", fname);
    return num(0);
}

/* ------------------------------------------------------------------ */
/* Depreciation                                                       */
/* ------------------------------------------------------------------ */

static char *depreciation_columns[] = { "year", "depreciation", "accumulated", "book_value" };

static long long years_of(Value v, int line)
{
    long long n;
    if (v.kind == VAL_DURATION && v.period != DUR_YEAR)
        fail(line, "%s needs the useful life in years, like 5 years or 5", fname);
    n = periods_of(v, PERIOD_YEAR, line, "the useful life");
    if (n < 1 || n > 200) fail(line, "the useful life must be from 1 to 200 years");
    return n;
}

static Value depreciation(const Value *a, int count, int line, int method)
{
    /* method 0: straight line, 1: reducing balance, 2: sum of the years' digits */
    Value cost = count > 0 ? a[0] : blank(VAL_NOTHING), row[4];
    PerryTable *t;
    dec_t residual = 0, rate = 0, book, dep, accumulated = 0, base, syd, share;
    long long life, y;
    int d;

    if (method == 1) need_count(count, 3, line, "reducing_balance_(50000.00 ZAR, 20%, 5 years)");
    else if (method == 0) need_count(count, 3, line, "straight_line_(50000.00 ZAR, 5000.00 ZAR, 5 years)");
    else need_count(count, 3, line, "sum_of_years_(50000.00 ZAR, 5000.00 ZAR, 5 years)");

    if (cost.kind != VAL_MONEY) fail(line, "%s needs the cost as money first", fname);
    if (cost.amount <= 0) fail(line, "the cost must be more than zero");
    if (method == 1) {
        if (a[1].kind != VAL_PERCENT) fail(line, "%s needs the yearly rate second, like 20%%", fname);
        rate = percent_fraction(a[1]);
        if (rate <= 0 || rate >= PERRY_SCALE) fail(line, "the reducing balance rate must be between 0%% and 100%%");
    } else {
        if (a[1].kind != VAL_MONEY || strcmp(a[1].currency, cost.currency) != 0)
            fail(line, "%s needs the residual (scrap) value second, as %s money", fname, cost.currency);
        residual = a[1].amount;
        if (residual < 0 || residual > cost.amount)
            fail(line, "the residual value must be between 0 and the cost");
    }
    life = years_of(a[2], line);
    d = currency_decimals(cost.currency);

    t = table_new(4, depreciation_columns);
    book = cost.amount;
    base = cost.amount - residual;
    syd  = (dec_t)life * (life + 1) / 2;
    for (y = 1; y <= life; y++) {
        if (method == 0) {
            dep = y == life ? book - residual : dec_round(div_round(base, (dec_t)life), d);
        } else if (method == 1) {
            if (!dec_mul(book, rate, &dep)) too_big(line);
            dep = dec_round(dep, d);
        } else {
            share = div_round(base * (dec_t)(life - y + 1), syd);
            dep = y == life ? book - residual : dec_round(share, d);
        }
        accumulated += dep;
        book -= dep;
        row[0] = num((dec_t)y * PERRY_SCALE);
        row[1] = money(dep, cost.currency);
        row[2] = money(accumulated, cost.currency);
        row[3] = money(book, cost.currency);
        table_add_row(t, row);
    }
    {
        Value v = blank(VAL_TABLE);
        v.table = t;
        return v;
    }
}

/* ------------------------------------------------------------------ */
/* Annuities and bonds                                                */
/* ------------------------------------------------------------------ */

/* kind 0: a (payments at the end), 1: a-due (at the start), 2: s (value at the end) */
static dec_t annuity_factor(dec_t i, long long n, int kind, int line)
{
    dec_t f, vn, a;
    if (i == 0) return (dec_t)n * XP;
    f  = xpow(XP + i, n, line);
    if (kind == 2) return xdiv(f - XP, i, line);
    vn = xdiv(XP, f, line);
    a  = xdiv(XP - vn, i, line);
    if (kind == 1) a = xmul(a, XP + i, line);
    return a;
}

static Value do_annuity(const Value *a, int count, int line, int kind)
{
    dec_t i, factor;
    long long n, m = 0;
    Value pay = count > 0 ? a[0] : blank(VAL_NOTHING);

    if (kind == 3) need_count(count, 4, line, "deferred_annuity_(1000.00 ZAR, 5% per year, 10 years, 5 years)");
    else if (kind == 1) need_count(count, 3, line, "annuity_due_(1000.00 ZAR, 5% per year, 10 years)");
    else if (kind == 2) need_count(count, 3, line, "accumulated_annuity_(1000.00 ZAR, 5% per year, 10 years)");
    else need_count(count, 3, line, "annuity_(1000.00 ZAR, 5% per year, 10 years)");

    need_amount(pay, line, "the payment first");
    i = rate_xp(a[1], line, "the rate second");
    n = periods_of(a[2], a[1].period, line, "the number of payments");
    if (kind == 3) {
        m = periods_of(a[3], a[1].period, line, "how long the payments are deferred");
        factor = xmul(annuity_factor(i, n, 0, line), xdiv(XP, xpow(XP + i, m, line), line), line);
    } else {
        factor = annuity_factor(i, n, kind, line);
    }
    return like(pay, times_factor(pay.amount, factor, line));
}

static Value do_perpetuity(const Value *a, int count, int line)
{
    dec_t i;
    need_count(count, 2, line, "perpetuity_(1000.00 ZAR, 5% per year)");
    need_amount(a[0], line, "the payment first");
    i = rate_xp(a[1], line, "the rate second");
    if (i <= 0) fail(line, "a perpetuity needs a rate above 0%%");
    return like(a[0], times_factor(a[0].amount, xdiv(XP, i, line), line));
}

static int matching_period(Value r1, Value r2, int line)
{
    if (r1.period != PERIOD_NONE && r2.period != PERIOD_NONE && r1.period != r2.period)
        fail(line, "%s: one rate is per %s and the other per %s - use the same period for both",
             fname, period_name(r1.period), period_name(r2.period));
    return r1.period != PERIOD_NONE ? r1.period : r2.period;
}

static Value do_bond_price(const Value *a, int count, int line)
{
    dec_t c, y, factor;
    long long n;
    need_count(count, 4, line, "bond_price_(1000.00 ZAR, 8%, 10%, 10)");
    if (a[0].kind != VAL_MONEY) fail(line, "%s needs the face (nominal) value as money first", fname);
    c = rate_xp(a[1], line, "the coupon rate second");
    y = rate_xp(a[2], line, "the yield third");
    n = periods_of(a[3], matching_period(a[1], a[2], line), line, "the number of coupon periods");
    /* price = face * (c * a_n + v^n), all at the yield */
    factor = xmul(c, annuity_factor(y, n, 0, line), line) + xdiv(XP, xpow(XP + y, n, line), line);
    return money(times_factor(a[0].amount, factor, line), a[0].currency);
}

static Value do_bond_yield(const Value *a, int count, int line)
{
    dec_t *flow, coupon, r;
    long long n, t;
    need_count(count, 4, line, "bond_yield_(950.00 ZAR, 1000.00 ZAR, 8%, 10)");
    if (a[0].kind != VAL_MONEY || a[1].kind != VAL_MONEY || strcmp(a[0].currency, a[1].currency) != 0)
        fail(line, "%s needs the price and the face value as money in the same currency", fname);
    if (a[0].amount <= 0 || a[1].amount <= 0) fail(line, "the price and face value must be more than zero");
    (void)rate_xp(a[2], line, "the coupon rate third");
    n = periods_of(a[3], a[2].period, line, "the number of coupon periods");
    if (n < 1) fail(line, "a bond needs at least 1 period");
    if (!dec_mul(a[1].amount, percent_fraction(a[2]), &coupon)) too_big(line);
    flow = (dec_t *)malloc(sizeof(dec_t) * (size_t)(n + 1));
    if (flow == NULL) { fprintf(stderr, "PERRY: out of memory\n"); exit(1); }
    flow[0] = -a[0].amount;
    for (t = 1; t <= n; t++) flow[t] = coupon + (t == n ? a[1].amount : 0);
    r = solve_rate(flow, (int)n + 1, line);
    free(flow);
    return percent(to12(r), a[2].period);
}

/* ------------------------------------------------------------------ */
/* square_root_                                                       */
/* ------------------------------------------------------------------ */

static Value do_sqrt(const Value *a, int count, int line)
{
    need_count(count, 1, line, "square_root_(2)");
    if (a[0].kind != VAL_NUMBER) fail(line, "%s works on plain numbers (got %s)", fname, value_kind_name(a[0].kind));
    if (a[0].amount < 0) fail(line, "a negative number has no square root");
    return num(dec_sqrt(a[0].amount));
}

/* ------------------------------------------------------------------ */
/* Big whole numbers for exact statistics                             */
/* ------------------------------------------------------------------ */

#define BN 128                     /* 128 x 32 bits = 4096 bits */

typedef struct { unsigned int d[BN]; } Big;          /* least significant first */
typedef struct { int neg; Big m; } SBig;

static int stat_line;

static void big_zero(Big *a) { memset(a, 0, sizeof *a); }

static void big_from_u128(Big *a, u128 x)
{
    int i;
    big_zero(a);
    for (i = 0; i < 4; i++) { a->d[i] = (unsigned int)x; x >>= 32; }
}

static int big_cmp(const Big *a, const Big *b)
{
    int i;
    for (i = BN - 1; i >= 0; i--)
        if (a->d[i] != b->d[i]) return a->d[i] < b->d[i] ? -1 : 1;
    return 0;
}

static int big_is_zero(const Big *a)
{
    int i;
    for (i = 0; i < BN; i++) if (a->d[i]) return 0;
    return 1;
}

static void big_add(Big *r, const Big *a, const Big *b)
{
    unsigned long long carry = 0;
    int i;
    for (i = 0; i < BN; i++) {
        carry += (unsigned long long)a->d[i] + b->d[i];
        r->d[i] = (unsigned int)carry;
        carry >>= 32;
    }
    if (carry) fail(stat_line, "%s: the numbers are too big to work with", fname);
}

static void big_sub(Big *r, const Big *a, const Big *b)          /* a >= b */
{
    long long borrow = 0;
    int i;
    for (i = 0; i < BN; i++) {
        long long x = (long long)a->d[i] - b->d[i] - borrow;
        borrow = x < 0;
        r->d[i] = (unsigned int)(x + (borrow ? (1LL << 32) : 0));
    }
}

static void big_mul(Big *r, const Big *a, const Big *b)
{
    unsigned int out[2 * BN];
    int i, j;
    memset(out, 0, sizeof out);
    for (i = 0; i < BN; i++) {
        unsigned long long carry = 0;
        if (a->d[i] == 0) continue;
        for (j = 0; j < BN; j++) {
            carry += (unsigned long long)a->d[i] * b->d[j] + out[i + j];
            out[i + j] = (unsigned int)carry;
            carry >>= 32;
        }
        out[i + BN] = (unsigned int)carry;
    }
    for (i = BN; i < 2 * BN; i++)
        if (out[i]) fail(stat_line, "%s: the numbers are too big to work with", fname);
    memcpy(r->d, out, sizeof r->d);
}

static void big_mul_u128(Big *r, const Big *a, u128 x)
{
    Big b;
    big_from_u128(&b, x);
    big_mul(r, a, &b);
}

static int big_bits(const Big *a)
{
    int i, b;
    for (i = BN - 1; i >= 0; i--)
        if (a->d[i]) { for (b = 31; b >= 0; b--) if (a->d[i] >> b & 1) return i * 32 + b + 1; }
    return 0;
}

/* q = a / b, rem = a % b  (b != 0) */
static void big_divmod(const Big *a, const Big *b, Big *q, Big *rem)
{
    Big r;
    int i, k;
    big_zero(q);
    big_zero(&r);
    for (i = big_bits(a) - 1; i >= 0; i--) {
        for (k = BN - 1; k > 0; k--) r.d[k] = (r.d[k] << 1) | (r.d[k - 1] >> 31);   /* r <<= 1 */
        r.d[0] = (r.d[0] << 1) | (a->d[i / 32] >> (i % 32) & 1);
        if (big_cmp(&r, b) >= 0) { big_sub(&r, &r, b); q->d[i / 32] |= 1u << (i % 32); }
    }
    if (rem) *rem = r;
}

static u128 big_to_u128(const Big *a)
{
    u128 x = 0;
    int i;
    for (i = BN - 1; i >= 4; i--)
        if (a->d[i]) fail(stat_line, "%s: the result is too big for Perry", fname);
    for (i = 3; i >= 0; i--) x = (x << 32) | a->d[i];
    return x;
}

/* round(num / den), halves up (num, den >= 0) */
static u128 big_div_round(const Big *num, const Big *den)
{
    Big q, r, twice;
    big_divmod(num, den, &q, &r);
    big_add(&twice, &r, &r);
    if (big_cmp(&twice, den) >= 0) { Big one; big_from_u128(&one, 1); big_add(&q, &q, &one); }
    return big_to_u128(&q);
}

/* round(sqrt(num / den)) exactly (num, den >= 0) */
static u128 big_sqrt_ratio(const Big *num, const Big *den)
{
    Big q, sq, lhs, rhs, four;
    u128 x = 0, bit;
    int top;

    big_divmod(num, den, &q, NULL);                /* floor(num / den) */
    top = (big_bits(&q) + 1) / 2;                  /* the root has about half the bits */
    if (top > 126) fail(stat_line, "%s: the result is too big for Perry", fname);
    for (bit = (u128)1 << top; bit > 0; bit >>= 1) {         /* build the root bit by bit */
        Big t;
        big_from_u128(&t, x | bit);
        big_mul(&sq, &t, &t);
        if (big_cmp(&sq, &q) <= 0) x |= bit;
    }
    /* round: take x + 1 if (x + 1/2)^2 <= num/den, i.e. (2x + 1)^2 * den <= 4 * num */
    big_from_u128(&lhs, 2 * x + 1);
    big_mul(&lhs, &lhs, &lhs);
    big_mul(&lhs, &lhs, den);
    big_from_u128(&four, 4);
    big_mul(&rhs, num, &four);
    if (big_cmp(&lhs, &rhs) <= 0) x++;
    return x;
}

/* signed helpers */
static void sb_from(SBig *a, dec_t x) { a->neg = x < 0; big_from_u128(&a->m, (u128)(x < 0 ? -x : x)); }

static void sb_mul(SBig *r, const SBig *a, const SBig *b)
{
    int neg = a->neg != b->neg;
    big_mul(&r->m, &a->m, &b->m);
    r->neg = neg && !big_is_zero(&r->m);
}

static void sb_add(SBig *r, const SBig *a, const SBig *b)
{
    if (a->neg == b->neg) { big_add(&r->m, &a->m, &b->m); r->neg = a->neg; }
    else if (big_cmp(&a->m, &b->m) >= 0) { big_sub(&r->m, &a->m, &b->m); r->neg = a->neg; }
    else { big_sub(&r->m, &b->m, &a->m); r->neg = b->neg; }
    if (big_is_zero(&r->m)) r->neg = 0;
}

static void sb_sub(SBig *r, const SBig *a, const SBig *b)
{
    SBig nb = *b;
    if (!big_is_zero(&nb.m)) nb.neg = !nb.neg;
    sb_add(r, a, &nb);
}

static void sb_mul_small(SBig *r, const SBig *a, long long k)
{
    SBig b;
    sb_from(&b, (dec_t)k);
    sb_mul(r, a, &b);
}

/* round(num / den) with signs, as a Perry amount */
static dec_t sb_div_round(const SBig *num, const SBig *den)
{
    u128 q = big_div_round(&num->m, &den->m);
    dec_t r;
    if (q >> 126) fail(stat_line, "%s: the result is too big for Perry", fname);
    r = (dec_t)q;
    if (num->neg != den->neg) r = -r;
    if (!dec_fits(r)) fail(stat_line, "%s: the result is too big for Perry", fname);
    return r;
}

/* ------------------------------------------------------------------ */
/* power_                                                             */
/* ------------------------------------------------------------------ */

/*
 * power_(b, n): b = B / 10^12, so b^n = B^n / 10^(12n), worked out exactly
 * with big whole numbers and rounded once. Very big powers (where even
 * 4096 bits aren't enough) use 24 decimals instead.
 */
static Value do_power(const Value *a, int count, int line)
{
    long long n, k;
    int negative, odd_negative;
    u128 B;

    need_count(count, 2, line, "power_(1.05, 10)");
    if (a[0].kind != VAL_NUMBER) fail(line, "%s needs a plain number first (got %s)", fname, value_kind_name(a[0].kind));
    if (a[1].kind != VAL_NUMBER || a[1].amount % PERRY_SCALE != 0)
        fail(line, "%s needs a whole number as the power, like power_(1.05, 10)", fname);
    if (a[1].amount / PERRY_SCALE > 100000 || a[1].amount / PERRY_SCALE < -100000)
        fail(line, "the power must be from -100000 to 100000");
    n = (long long)(a[1].amount / PERRY_SCALE);
    negative = n < 0;
    if (negative) n = -n;
    if (n == 0) return num(PERRY_SCALE);
    if (a[0].amount == 0) {
        if (negative) fail(line, "0 can't be raised to a negative power");
        return num(0);
    }
    odd_negative = a[0].amount < 0 && (n & 1);
    B = (u128)(a[0].amount < 0 ? -a[0].amount : a[0].amount);

    {
        Big b, p, scale, t;
        int bits_b;
        big_from_u128(&b, B);
        bits_b = big_bits(&b);
        /* both B^n and 10^(12(n+1)) must fit in the big numbers */
        if ((long long)bits_b * n + 64 < BN * 32 && 40LL * (n + 1) + 64 < BN * 32) {
            u128 r;
            dec_t result;
            big_from_u128(&p, 1);                      /* p = B^n */
            for (k = 0; k < n; k++) big_mul(&p, &p, &b);
            big_from_u128(&scale, 1);                  /* 10^12 to the power (n - 1) or (n + 1) */
            big_from_u128(&t, (u128)PERRY_SCALE);
            for (k = 0; k < (negative ? n + 1 : n - 1); k++) big_mul(&scale, &scale, &t);
            r = negative ? big_div_round(&scale, &p) : big_div_round(&p, &scale);
            if (r >> 126) too_big(line);
            result = (dec_t)r;
            if (!dec_fits(result)) too_big(line);
            return num(odd_negative ? -result : result);
        }
    }
    {                                                  /* very big powers: 24 decimals */
        dec_t base = (dec_t)B * PERRY_SCALE, result = XP, r;
        if (negative && !dec_muldiv_raw(XP, XP, base, &base)) too_big(line);
        k = n;
        while (k > 0) {
            if ((k & 1) && !dec_muldiv_raw(result, base, XP, &result)) too_big(line);
            k >>= 1;
            if (k > 0 && !dec_muldiv_raw(base, base, XP, &base)) too_big(line);
        }
        r = to12(result);
        if (!dec_fits(r)) too_big(line);
        return num(odd_negative ? -r : r);
    }
}

/* ------------------------------------------------------------------ */
/* Statistics                                                         */
/* ------------------------------------------------------------------ */

/* The numbers in a list (blanks skipped). Numbers, percents or money of one currency. */
static dec_t *stat_values(Value list, int line, int *count, Value *example)
{
    dec_t *out;
    int i, n = 0, found = 0;
    if (list.kind != VAL_LIST)
        fail(line, "%s needs a list or a column, like: %s(statement.amount) (got %s)",
             fname, fname, value_kind_name(list.kind));
    out = (dec_t *)malloc(sizeof(dec_t) * (size_t)(list.list->count + 1));
    if (out == NULL) { fprintf(stderr, "PERRY: out of memory\n"); exit(1); }
    for (i = 0; i < list.list->count; i++) {
        Value it = list.list->items[i];
        if (it.kind == VAL_NOTHING) continue;
        if (!value_is_numeric(it))
            fail(line, "%s works on numbers, percents and money (item %d is %s)", fname, i + 1, value_kind_name(it.kind));
        if (!found) { *example = it; found = 1; }
        else if (it.kind != example->kind || (it.kind == VAL_MONEY && strcmp(it.currency, example->currency) != 0))
            fail(line, "%s: all the values must be the same kind (item %d is different)", fname, i + 1);
        out[n++] = it.kind == VAL_PERCENT ? percent_fraction(it) : it.amount;
    }
    *count = n;
    return out;
}

/* n * sum(x^2) - (sum x)^2  and  sum x, exactly (scale 10^24) */
static void spread(const dec_t *x, int n, SBig *d)
{
    SBig sum, sumsq, t, nsq;
    int i;
    sb_from(&sum, 0);
    sb_from(&sumsq, 0);
    for (i = 0; i < n; i++) {
        SBig xi;
        sb_from(&xi, x[i]);
        sb_add(&sum, &sum, &xi);
        sb_mul(&t, &xi, &xi);
        sb_add(&sumsq, &sumsq, &t);
    }
    sb_mul_small(&nsq, &sumsq, n);
    sb_mul(&t, &sum, &sum);
    sb_sub(d, &nsq, &t);
}

static Value do_variance(const Value *a, int count, int line, int population, int root)
{
    Value example = num(0);
    dec_t *x;
    int n;
    SBig d, den;
    need_count(count, 1, line, root ? "standard_deviation_(scores)" : "variance_(scores)");
    x = stat_values(a[0], line, &n, &example);
    if (n < (population ? 1 : 2)) fail(line, "%s needs at least %d values", fname, population ? 1 : 2);
    spread(x, n, &d);
    free(x);
    sb_from(&den, (dec_t)n * (population ? n : n - 1));

    if (root) {                                   /* sqrt(d / den): scale 10^24 -> 10^12 */
        u128 r = big_sqrt_ratio(&d.m, &den.m);
        if (example.kind == VAL_PERCENT) return percent((dec_t)r, example.period);
        return like(example, (dec_t)r);
    } else {                                      /* d / den / 10^12 */
        SBig big_den = den;
        big_mul_u128(&big_den.m, &big_den.m, (u128)PERRY_SCALE);
        return num(sb_div_round(&d, &big_den));
    }
}

static int compare_dec(const void *p, const void *q)
{
    dec_t a = *(const dec_t *)p, b = *(const dec_t *)q;
    return a < b ? -1 : a > b ? 1 : 0;
}

static Value do_percentile(const Value *a, int count, int line, int median)
{
    Value example = num(0);
    dec_t *x, p, h, frac, gap, result;
    long long lo;
    int n;
    if (median) need_count(count, 1, line, "median_(scores)");
    else        need_count(count, 2, line, "percentile_(scores, 90%)");
    x = stat_values(a[0], line, &n, &example);
    if (n < 1) fail(line, "%s needs at least 1 value", fname);
    if (median) p = PERRY_SCALE / 2;
    else {
        if (a[1].kind != VAL_PERCENT) fail(line, "%s needs the percentile as a percent, like 90%%", fname);
        p = percent_fraction(a[1]);
        if (p < 0 || p > PERRY_SCALE) fail(line, "the percentile must be from 0%% to 100%%");
    }
    qsort(x, (size_t)n, sizeof(dec_t), compare_dec);
    /* position (n - 1) * p, between two sorted values: the same method as Excel's PERCENTILE.INC */
    h    = (dec_t)(n - 1) * p;
    lo   = (long long)(h / PERRY_SCALE);
    frac = h % PERRY_SCALE;
    result = x[lo];
    if (frac != 0 && lo + 1 < n) {
        gap = x[lo + 1] - x[lo];
        if (!dec_muldiv(gap, frac, PERRY_SCALE, &gap)) too_big(line);
        result += gap;
    }
    free(x);
    return like(example, result);
}

/* Pairs from two lists (a pair is skipped if either side is blank). */
static int stat_pairs(const Value *a, int line, dec_t **xs, dec_t **ys)
{
    Value lx = a[0], ly = a[1], ex = num(0), ey = num(0);
    int i, n = 0, fx = 0, fy = 0;
    if (lx.kind != VAL_LIST || ly.kind != VAL_LIST)
        fail(line, "%s needs two lists or columns, like: %s(table.hours, table.cost)", fname, fname);
    if (lx.list->count != ly.list->count)
        fail(line, "%s: the two lists must be the same length (%d and %d)", fname, lx.list->count, ly.list->count);
    *xs = (dec_t *)malloc(sizeof(dec_t) * (size_t)(lx.list->count + 1));
    *ys = (dec_t *)malloc(sizeof(dec_t) * (size_t)(lx.list->count + 1));
    if (*xs == NULL || *ys == NULL) { fprintf(stderr, "PERRY: out of memory\n"); exit(1); }
    for (i = 0; i < lx.list->count; i++) {
        Value x = lx.list->items[i], y = ly.list->items[i];
        if (x.kind == VAL_NOTHING || y.kind == VAL_NOTHING) continue;
        if (!value_is_numeric(x) || !value_is_numeric(y))
            fail(line, "%s works on numbers, percents and money (item %d)", fname, i + 1);
        if (!fx) { ex = x; fx = 1; }
        if (!fy) { ey = y; fy = 1; }
        if (x.kind != ex.kind || (x.kind == VAL_MONEY && strcmp(x.currency, ex.currency) != 0) ||
            y.kind != ey.kind || (y.kind == VAL_MONEY && strcmp(y.currency, ey.currency) != 0))
            fail(line, "%s: each list must hold one kind of value (item %d is different)", fname, i + 1);
        (*xs)[n] = x.kind == VAL_PERCENT ? percent_fraction(x) : x.amount;
        (*ys)[n] = y.kind == VAL_PERCENT ? percent_fraction(y) : y.amount;
        n++;
    }
    if (n < 2) fail(line, "%s needs at least 2 pairs of values", fname);
    return n;
}

static char *regression_columns[] = { "slope", "intercept", "r_squared", "correlation", "count" };

static Value do_correlation(const Value *a, int count, int line, int regression)
{
    dec_t *x, *y;
    int n, i;
    SBig sx, sy, sxy, t, u, N, Dx, Dy;

    need_count(count, 2, line, regression ? "regression_(tonnes, cost)" : "correlation_(hours, marks)");
    n = stat_pairs(a, line, &x, &y);

    sb_from(&sx, 0); sb_from(&sy, 0); sb_from(&sxy, 0);
    for (i = 0; i < n; i++) {
        SBig xi, yi;
        sb_from(&xi, x[i]); sb_from(&yi, y[i]);
        sb_add(&sx, &sx, &xi);
        sb_add(&sy, &sy, &yi);
        sb_mul(&t, &xi, &yi);
        sb_add(&sxy, &sxy, &t);
    }
    spread(x, n, &Dx);
    spread(y, n, &Dy);
    sb_mul_small(&t, &sxy, n);
    sb_mul(&u, &sx, &sy);
    sb_sub(&N, &t, &u);                          /* n*sum(xy) - sum(x)*sum(y) */
    free(x); free(y);

    if (big_is_zero(&Dx.m)) fail(line, "%s: all the first values are the same, so there's no line or correlation", fname);

    {
        Big T, D, q;
        dec_t r = 0, r2 = 0;
        int has_r = !big_is_zero(&Dy.m);
        if (has_r) {
            /* r = N / sqrt(Dx * Dy);  r^2 = N^2 / (Dx * Dy) */
            big_mul(&T, &N.m, &N.m);
            big_mul(&D, &Dx.m, &Dy.m);
            big_mul_u128(&q, &T, (u128)PERRY_SCALE);
            r2 = (dec_t)big_div_round(&q, &D);
            big_mul_u128(&q, &q, (u128)PERRY_SCALE);          /* N^2 * 10^24 */
            r  = (dec_t)big_sqrt_ratio(&q, &D);
            if (N.neg) r = -r;
        }
        if (!regression) {
            if (!has_r) fail(line, "%s: all the second values are the same, so there's no correlation", fname);
            return num(r);
        }
        {
            PerryTable *tab = table_new(5, regression_columns);
            Value row[5], out;
            SBig num1, num2, top, bottom, scaled;
            dec_t slope, intercept;

            /* slope = N / Dx  (both scale 10^24) -> 12 decimals */
            sb_mul_small(&scaled, &N, 1);
            big_mul_u128(&scaled.m, &scaled.m, (u128)PERRY_SCALE);
            slope = sb_div_round(&scaled, &Dx);

            /* intercept = (sum(y)*Dx - N*sum(x)) / (n * Dx) */
            sb_mul(&num1, &sy, &Dx);
            sb_mul(&num2, &N, &sx);
            sb_sub(&top, &num1, &num2);
            sb_mul_small(&bottom, &Dx, n);
            intercept = sb_div_round(&top, &bottom);

            row[0] = num(slope);
            row[1] = num(intercept);
            row[2] = has_r ? num(r2) : blank(VAL_NOTHING);
            row[3] = has_r ? num(r) : blank(VAL_NOTHING);
            row[4] = num((dec_t)n * PERRY_SCALE);
            table_add_row(tab, row);
            out = blank(VAL_ROW);
            out.table = tab;
            out.row = 0;
            return out;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Life tables                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    long long first_age;
    int       count;
    dec_t    *l;              /* survivors at each age, 24 decimals (l at the first age = 1) */
} Life;

static int find_col(const PerryTable *t, const char *a, const char *b)
{
    int c = table_column(t, a, (int)strlen(a));
    if (c < 0 && b) c = table_column(t, b, (int)strlen(b));
    return c;
}

static void read_life_table(Value v, int line, Life *life)
{
    const PerryTable *t;
    int age_col, q_col, l_col, r;
    if (v.kind != VAL_TABLE)
        fail(line, "%s needs a life table first - load it with: table = go_get_ \"life_table.csv\"", fname);
    t = v.table;
    age_col = find_col(t, "age", "x");
    q_col   = find_col(t, "qx", "q_x");
    l_col   = find_col(t, "lx", "l_x");
    if (age_col < 0 || (q_col < 0 && l_col < 0))
        fail(line, "a life table needs an 'age' column and a 'qx' (chance of dying) or 'lx' (survivors) column");
    if (t->nrows < 2) fail(line, "a life table needs at least 2 ages");

    life->count = t->nrows;
    life->first_age = 0;
    life->l = (dec_t *)malloc(sizeof(dec_t) * (size_t)(t->nrows + 1));
    if (life->l == NULL) { fprintf(stderr, "PERRY: out of memory\n"); exit(1); }

    for (r = 0; r < t->nrows; r++) {
        Value age = table_cell(t, r, age_col);
        if (age.kind != VAL_NUMBER || age.amount % PERRY_SCALE != 0)
            fail(line, "life table row %d: the age must be a whole number", r + 1);
        if (r == 0) life->first_age = (long long)(age.amount / PERRY_SCALE);
        else if ((long long)(age.amount / PERRY_SCALE) != life->first_age + r)
            fail(line, "life table row %d: ages must go up by 1 with none missing", r + 1);
    }
    if (q_col >= 0) {
        life->l[0] = XP;
        for (r = 0; r < t->nrows; r++) {
            Value q = table_cell(t, r, q_col);
            dec_t qx = q.kind == VAL_PERCENT ? percent_fraction(q) : q.amount;
            if ((q.kind != VAL_NUMBER && q.kind != VAL_PERCENT) || qx < 0 || qx > PERRY_SCALE)
                fail(line, "life table row %d: qx must be a chance between 0 and 1", r + 1);
            /* l(x+1) = l(x) * (1 - q(x));  after the last age nobody is left */
            life->l[r + 1] = r == t->nrows - 1 ? 0 : xmul(life->l[r], (PERRY_SCALE - qx) * PERRY_SCALE, line);
        }
    } else {
        Value l0 = table_cell(t, 0, l_col);
        if (l0.kind != VAL_NUMBER || l0.amount <= 0) fail(line, "life table: lx at the first age must be more than 0");
        for (r = 0; r < t->nrows; r++) {
            Value l = table_cell(t, r, l_col);
            if (l.kind != VAL_NUMBER || l.amount < 0 || (r > 0 && l.amount > table_cell(t, r - 1, l_col).amount))
                fail(line, "life table row %d: lx must be a number that never goes up", r + 1);
            if (!dec_muldiv_raw(l.amount, XP, l0.amount, &life->l[r])) too_big(line);
        }
        life->l[t->nrows] = 0;
    }
}

/* where age x is in the table */
static int age_index(const Life *life, Value age, int line)
{
    long long x;
    if (age.kind != VAL_NUMBER || age.amount % PERRY_SCALE != 0)
        fail(line, "%s needs the age as a whole number", fname);
    x = (long long)(age.amount / PERRY_SCALE);
    if (x < life->first_age || x >= life->first_age + life->count)
        fail(line, "age %lld isn't in the life table (it covers %lld to %lld)",
             x, life->first_age, life->first_age + life->count - 1);
    return (int)(x - life->first_age);
}

/* chance that someone aged (index i) is alive k years later, 24 decimals */
static dec_t kpx(const Life *life, int i, long long k, int line)
{
    if (i + k > life->count) return 0;
    if (life->l[i] == 0) return 0;
    return xdiv(life->l[i + k], life->l[i], line);
}

static Value do_life(const Value *a, int count, int line, int kind)
{
    /* kind: 0 survival_, 1 life_expectancy_, 2 life_annuity_, 3 temporary_life_annuity_,
             4 life_insurance_, 5 term_insurance_, 6 pure_endowment_, 7 endowment_ */
    static const char *examples[] = {
        "survival_(table, 40, 10)", "life_expectancy_(table, 40)",
        "life_annuity_(table, 60, 4% per year)", "temporary_life_annuity_(table, 60, 10, 4% per year)",
        "life_insurance_(table, 40, 4% per year)", "term_insurance_(table, 40, 20, 4% per year)",
        "pure_endowment_(table, 40, 20, 4% per year)", "endowment_(table, 40, 20, 4% per year)" };
    static const int wants[] = { 3, 2, 3, 4, 3, 4, 4, 4 };
    Life life;
    dec_t v = XP, i_rate = 0, vk, total = 0, p, q_part;
    long long n = 0, k, limit;
    int x;

    need_count(count, wants[kind], line, examples[kind]);
    read_life_table(a[0], line, &life);
    x = age_index(&life, a[1], line);

    if (kind == 0 || kind == 3 || kind >= 5) {
        n = periods_of(a[2], PERIOD_YEAR, line, "the number of years");
    }
    if (kind >= 2) {
        Value rate = a[wants[kind] - 1];
        i_rate = rate_xp(rate, line, "the yearly interest rate last");
        if (rate.period == PERIOD_MONTH) fail(line, "life tables work in years - give the rate per year");
        v = xdiv(XP, XP + i_rate, line);
    }

    switch (kind) {
    case 0:                                        /* t_p_x */
        total = kpx(&life, x, n, line);
        break;
    case 1:                                        /* curtate e_x = sum of k_p_x, k >= 1 */
        for (k = 1; x + k <= life.count; k++) total += kpx(&life, x, k, line);
        break;
    case 2: case 3:                                /* a-due: sum of v^k k_p_x */
        limit = kind == 2 ? life.count - x : n;
        vk = XP;
        for (k = 0; k < limit; k++) {
            p = kpx(&life, x, k, line);
            if (p == 0) break;
            total += xmul(vk, p, line);
            vk = xmul(vk, v, line);
        }
        break;
    case 4: case 5: case 7:                        /* A: sum of v^(k+1) k_p_x q_(x+k) */
        limit = kind == 4 ? life.count - x : n;
        vk = v;
        for (k = 0; k < limit; k++) {
            /* k_p_x * q_(x+k) = (l(x+k) - l(x+k+1)) / l(x) */
            if (x + k >= life.count) break;
            q_part = xdiv(life.l[x + k] - life.l[x + k + 1], life.l[x], line);
            total += xmul(vk, q_part, line);
            vk = xmul(vk, v, line);
        }
        if (kind == 7) total += xmul(xpow(v, n, line), kpx(&life, x, n, line), line);
        break;
    case 6:                                        /* n_E_x = v^n n_p_x */
        total = xmul(xpow(v, n, line), kpx(&life, x, n, line), line);
        break;
    }
    free(life.l);
    return num(to12(total));
}

/* ------------------------------------------------------------------ */
/* The list of functions                                              */
/* ------------------------------------------------------------------ */

typedef enum {
    F_NPV, F_IRR, F_PAYBACK, F_DPAYBACK, F_STRAIGHT, F_REDUCING, F_SYD,
    F_ANNUITY, F_ANNUITY_DUE, F_DEFERRED, F_ACCUMULATED, F_PERPETUITY, F_BOND_PRICE, F_BOND_YIELD,
    F_VARIANCE, F_SD, F_PVARIANCE, F_PSD, F_MEDIAN, F_PERCENTILE, F_CORRELATION, F_REGRESSION,
    F_SURVIVAL, F_EXPECTANCY, F_LIFE_ANNUITY, F_TEMP_ANNUITY, F_LIFE_INS, F_TERM_INS, F_PURE_ENDOW, F_ENDOW,
    F_SQRT, F_POWER, F_COUNT
} FinanceFn;

static const char *names[F_COUNT] = {
    "npv_", "irr_", "payback_", "discounted_payback_", "straight_line_", "reducing_balance_", "sum_of_years_",
    "annuity_", "annuity_due_", "deferred_annuity_", "accumulated_annuity_", "perpetuity_", "bond_price_", "bond_yield_",
    "variance_", "standard_deviation_", "population_variance_", "population_standard_deviation_",
    "median_", "percentile_", "correlation_", "regression_",
    "survival_", "life_expectancy_", "life_annuity_", "temporary_life_annuity_", "life_insurance_",
    "term_insurance_", "pure_endowment_", "endowment_",
    "square_root_", "power_"
};

int finance_lookup(const char *name, int len)
{
    int i;
    for (i = 0; i < F_COUNT; i++)
        if ((int)strlen(names[i]) == len && memcmp(names[i], name, (size_t)len) == 0) return i;
    return -1;
}

const char *finance_name(int index)
{
    return index >= 0 && index < F_COUNT ? names[index] : NULL;
}

Value finance_call(int fn, const Value *a, int count, int line)
{
    fname = names[fn];
    stat_line = line;
    switch ((FinanceFn)fn) {
    case F_NPV:          return do_npv(a, count, line);
    case F_IRR:          return do_irr(a, count, line);
    case F_PAYBACK:      return do_payback(a, count, line, 0);
    case F_DPAYBACK:     return do_payback(a, count, line, 1);
    case F_STRAIGHT:     return depreciation(a, count, line, 0);
    case F_REDUCING:     return depreciation(a, count, line, 1);
    case F_SYD:          return depreciation(a, count, line, 2);
    case F_ANNUITY:      return do_annuity(a, count, line, 0);
    case F_ANNUITY_DUE:  return do_annuity(a, count, line, 1);
    case F_ACCUMULATED:  return do_annuity(a, count, line, 2);
    case F_DEFERRED:     return do_annuity(a, count, line, 3);
    case F_PERPETUITY:   return do_perpetuity(a, count, line);
    case F_BOND_PRICE:   return do_bond_price(a, count, line);
    case F_BOND_YIELD:   return do_bond_yield(a, count, line);
    case F_VARIANCE:     return do_variance(a, count, line, 0, 0);
    case F_SD:           return do_variance(a, count, line, 0, 1);
    case F_PVARIANCE:    return do_variance(a, count, line, 1, 0);
    case F_PSD:          return do_variance(a, count, line, 1, 1);
    case F_MEDIAN:       return do_percentile(a, count, line, 1);
    case F_PERCENTILE:   return do_percentile(a, count, line, 0);
    case F_CORRELATION:  return do_correlation(a, count, line, 0);
    case F_REGRESSION:   return do_correlation(a, count, line, 1);
    case F_SURVIVAL:     return do_life(a, count, line, 0);
    case F_EXPECTANCY:   return do_life(a, count, line, 1);
    case F_LIFE_ANNUITY: return do_life(a, count, line, 2);
    case F_TEMP_ANNUITY: return do_life(a, count, line, 3);
    case F_LIFE_INS:     return do_life(a, count, line, 4);
    case F_TERM_INS:     return do_life(a, count, line, 5);
    case F_PURE_ENDOW:   return do_life(a, count, line, 6);
    case F_ENDOW:        return do_life(a, count, line, 7);
    case F_SQRT:         return do_sqrt(a, count, line);
    case F_POWER:        return do_power(a, count, line);
    case F_COUNT:        break;
    }
    return blank(VAL_NOTHING);
}
