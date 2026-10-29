/*
 * value.c - exact decimal arithmetic and printing of Perry values.
 */
#include <stdlib.h>
#include <string.h>
#include "value.h"
#include "dates.h"

/* Percentages are shown with up to this many decimals (0.9791666667%). */
#define PERCENT_SHOWN (PERRY_DECIMALS - 2)


__extension__ typedef unsigned __int128 u128;
typedef unsigned long long u64;

/* The biggest amount: 10^30 - 1 trillionths, i.e. just under 10^18 whole units. */
static dec_t limit_value(void)
{
    static dec_t lim = 0;
    if (lim == 0) {
        int i;
        lim = 1;
        for (i = 0; i < 30; i++) lim *= 10;
        lim -= 1;
    }
    return lim;
}

int dec_fits(dec_t x)
{
    dec_t lim = limit_value();
    return x <= lim && x >= -lim;
}

static void out_of_memory(void)
{
    fprintf(stderr, "PERRY: out of memory\n");
    exit(1);
}

static dec_t pow10_dec(int n)
{
    dec_t p = 1;
    while (n-- > 0) p *= 10;
    return p;
}

/* ------------------------------------------------------------------ */
/* 256-bit helpers: a * b / d without overflowing                     */
/* ------------------------------------------------------------------ */

/* hi:lo = a * b  (unsigned 128 x 128 -> 256 bits) */
static void mul_256(u128 a, u128 b, u128 *hi, u128 *lo)
{
    u64 a0 = (u64)a, a1 = (u64)(a >> 64), b0 = (u64)b, b1 = (u64)(b >> 64);
    u128 p00 = (u128)a0 * b0, p01 = (u128)a0 * b1, p10 = (u128)a1 * b0, p11 = (u128)a1 * b1;
    u128 mid = (p00 >> 64) + (u64)p01 + (u64)p10;          /* can't overflow: < 3 * 2^64 */
    *lo = ((u128)(u64)mid << 64) | (u64)p00;
    *hi = p11 + (p01 >> 64) + (p10 >> 64) + (mid >> 64);
}

/*
 * (hi:lo) / d, rounding halves up. d must be below 2^127.
 * Returns 0 if the answer doesn't fit in 127 bits.
 */
static int div_256(u128 hi, u128 lo, u128 d, u128 *q_out)
{
    u128 q = 0, r = 0;
    int i;

    if (hi == 0) {
        q = lo / d;
        r = lo % d;
    } else {
        if (hi >= d) return 0;                      /* quotient would need more than 128 bits */
        r = hi;
        for (i = 127; i >= 0; i--) {                /* long division, one bit at a time */
            r = (r << 1) | ((lo >> i) & 1);
            q <<= 1;
            if (r >= d) { r -= d; q |= 1; }
        }
    }
    if (2 * r >= d) q++;                            /* round halves up (away from zero) */
    if (q >> 126) return 0;
    *q_out = q;
    return 1;
}

int dec_muldiv_raw(dec_t a, dec_t b, dec_t d, dec_t *out)
{
    int negative = (a < 0) != (b < 0);
    u128 ua = (u128)(a < 0 ? -a : a), ub = (u128)(b < 0 ? -b : b), ud, hi, lo, q;

    if (d == 0) return 0;
    if (d < 0) { negative = !negative; ud = (u128)(-d); } else ud = (u128)d;
    mul_256(ua, ub, &hi, &lo);
    if (!div_256(hi, lo, ud, &q)) return 0;
    *out = negative ? -(dec_t)q : (dec_t)q;
    return 1;
}

int dec_muldiv(dec_t a, dec_t b, dec_t d, dec_t *out)
{
    int negative = (a < 0) != (b < 0);
    u128 ua = (u128)(a < 0 ? -a : a), ub = (u128)(b < 0 ? -b : b), ud, hi, lo, q;
    dec_t r;

    if (d == 0) return 0;
    if (d < 0) { negative = !negative; ud = (u128)(-d); } else ud = (u128)d;
    mul_256(ua, ub, &hi, &lo);
    if (!div_256(hi, lo, ud, &q)) return 0;
    r = (dec_t)q;
    if (negative) r = -r;
    if (!dec_fits(r)) return 0;
    *out = r;
    return 1;
}

/* n / d, rounding halves away from zero. */
static dec_t div_round(dec_t n, dec_t d)
{
    dec_t q = n / d;
    dec_t r = n % d;
    dec_t ar = r < 0 ? -r : r;
    dec_t ad = d < 0 ? -d : d;
    if (2 * ar >= ad)
        q += ((n < 0) != (d < 0)) ? -1 : 1;
    return q;
}

/* ------------------------------------------------------------------ */
/* Decimal arithmetic                                                 */
/* ------------------------------------------------------------------ */

int dec_add(dec_t a, dec_t b, dec_t *out)
{
    dec_t r = a + b;                     /* both are far below 2^127, so this can't overflow */
    if (!dec_fits(r)) return 0;
    *out = r;
    return 1;
}

int dec_sub(dec_t a, dec_t b, dec_t *out)
{
    dec_t r = a - b;
    if (!dec_fits(r)) return 0;
    *out = r;
    return 1;
}

int dec_mul_frac(dec_t a, dec_t b, long long d, dec_t *out)
{
    return dec_muldiv(a, b, PERRY_SCALE * (d > 1 ? d : 1), out);
}

int dec_mul(dec_t a, dec_t b, dec_t *out)
{
    return dec_mul_frac(a, b, 1, out);
}

int dec_div(dec_t a, dec_t b, dec_t *out)
{
    return dec_muldiv(a, PERRY_SCALE, b, out);
}

/* Square root with 12 decimals: the whole-number square root of a * 10^12, rounded. */
dec_t dec_sqrt(dec_t a)
{
    u128 n, x, y;
    if (a <= 0) return 0;
    /* The answer is sqrt(a * 10^12). For numbers above about 340 trillion,
       a * 10^12 doesn't fit in 128 bits, so use sqrt(a) * 10^6 instead
       (the answer is then exact to 6 decimals). */
    if ((u128)a > ((~(u128)0) / (u128)PERRY_SCALE)) {
        x = (u128)a;
        y = (x + 1) / 2;
        while (y < x) { x = y; y = (x + (u128)a / x) / 2; }
        return (dec_t)(x * 1000000);
    }
    n = (u128)a * (u128)PERRY_SCALE;
    x = n;
    y = (x + 1) / 2;
    while (y < x) { x = y; y = (x + n / x) / 2; }   /* Newton's method on whole numbers */
    if (n - x * x > x) x++;                         /* round to nearest: (x+0.5)^2 = x^2 + x + 0.25 */
    return (dec_t)x;
}

dec_t dec_round(dec_t v, int decimals)
{
    return dec_round_mode(v, decimals, ROUND_NORMAL);
}

dec_t dec_round_mode(dec_t v, int decimals, RoundMode mode)
{
    dec_t step, r, ar, base;
    if (decimals >= PERRY_DECIMALS) return v;
    step = pow10_dec(PERRY_DECIMALS - decimals);
    r    = v % step;                 /* what gets cut off  */
    ar   = r < 0 ? -r : r;
    base = v - r;                    /* rounded towards zero */
    if (r == 0) return v;

    switch (mode) {
    case ROUND_UP:                   /* towards +infinity */
        return r > 0 ? base + step : base;
    case ROUND_DOWN:                 /* towards -infinity */
        return r < 0 ? base - step : base;
    case ROUND_BANK:                 /* halves go to the even neighbour */
        if (2 * ar > step || (2 * ar == step && (base / step) % 2 != 0))
            return base + (r < 0 ? -step : step);
        return base;
    case ROUND_NORMAL:
    default:                         /* halves away from zero */
        return 2 * ar >= step ? base + (r < 0 ? -step : step) : base;
    }
}

int dec_parse(const char *s, int len, dec_t *out, int *decimals)
{
    dec_t whole = 0, frac = 0, total;
    dec_t whole_limit = limit_value() / PERRY_SCALE;
    int i = 0, places = 0;

    while (i < len && s[i] >= '0' && s[i] <= '9') {
        whole = whole * 10 + (s[i] - '0');
        if (whole > whole_limit) return -1;
        i++;
    }
    if (i < len && s[i] == '.') {
        i++;
        while (i < len && s[i] >= '0' && s[i] <= '9') {
            if (places == PERRY_DECIMALS) return 0;
            frac = frac * 10 + (s[i] - '0');
            places++;
            i++;
        }
    }
    total = whole * PERRY_SCALE + frac * pow10_dec(PERRY_DECIMALS - places);
    if (!dec_fits(total)) return -1;
    *out = total;
    *decimals = places;
    return 1;
}

int currency_decimals(const char *code)
{
    static const char *none[]  = { "JPY", "KRW", "VND", "CLP", "ISK", "UGX", "XAF", "XOF", NULL };
    static const char *three[] = { "BHD", "KWD", "OMR", "JOD", "TND", "IQD", "LYD", NULL };
    int i;
    for (i = 0; none[i];  i++) if (strcmp(code, none[i])  == 0) return 0;
    for (i = 0; three[i]; i++) if (strcmp(code, three[i]) == 0) return 3;
    return 2;
}

/* ------------------------------------------------------------------ */
/* Kinds                                                              */
/* ------------------------------------------------------------------ */

int value_is_numeric(Value v)
{
    return v.kind == VAL_NUMBER || v.kind == VAL_PERCENT || v.kind == VAL_MONEY;
}

long long value_divisor(Value v)
{
    return (v.kind == VAL_PERCENT && v.divisor > 1) ? v.divisor : 1;
}

dec_t percent_fraction(Value v)
{
    long long d = value_divisor(v);
    return d == 1 ? v.amount : div_round(v.amount, d);
}

const char *value_kind_name(ValueKind kind)
{
    switch (kind) {
    case VAL_NOTHING: return "nothing_";
    case VAL_BOOL:    return "a yes/no";
    case VAL_NUMBER:  return "a plain number";
    case VAL_PERCENT: return "a percent";
    case VAL_MONEY:   return "money";
    case VAL_TEXT:    return "text";
    case VAL_LIST:    return "a list";
    case VAL_TABLE:   return "a table";
    case VAL_ROW:     return "a table row";
    case VAL_TAX:     return "a tax table";
    case VAL_DATE:    return "a date";
    case VAL_DURATION:return "a length of time";
    case VAL_FXRATE:  return "an exchange rate";
    }
    return "?";
}

const char *period_name(int period)
{
    switch (period) {
    case PERIOD_YEAR:  return "year";
    case PERIOD_MONTH: return "month";
    default:           return "";
    }
}

/* ------------------------------------------------------------------ */
/* Lists and tables                                                   */
/* ------------------------------------------------------------------ */

PerryList *list_new(void)
{
    PerryList *list = (PerryList *)calloc(1, sizeof(PerryList));
    if (list == NULL) out_of_memory();
    return list;
}

void list_push(PerryList *list, Value v)
{
    if (list->count == list->capacity) {
        list->capacity = list->capacity ? list->capacity * 2 : 8;
        list->items = (Value *)realloc(list->items, sizeof(Value) * (size_t)list->capacity);
        if (list->items == NULL) out_of_memory();
    }
    list->items[list->count++] = v;
}

PerryTable *table_new(int ncols, char **cols)
{
    PerryTable *t = (PerryTable *)calloc(1, sizeof(PerryTable));
    if (t == NULL) out_of_memory();
    t->ncols = ncols;
    t->cols  = cols;
    return t;
}

void table_add_row(PerryTable *t, const Value *cells)
{
    if (t->nrows == t->capacity) {
        t->capacity = t->capacity ? t->capacity * 2 : 16;
        t->cells = (Value *)realloc(t->cells, sizeof(Value) * (size_t)t->capacity * (size_t)t->ncols);
        if (t->cells == NULL) out_of_memory();
    }
    memcpy(t->cells + (size_t)t->nrows * (size_t)t->ncols, cells, sizeof(Value) * (size_t)t->ncols);
    t->nrows++;
}

int table_column(const PerryTable *t, const char *name, int len)
{
    int i;
    for (i = 0; i < t->ncols; i++)
        if ((int)strlen(t->cols[i]) == len && memcmp(t->cols[i], name, (size_t)len) == 0)
            return i;
    return -1;
}

Value table_cell(const PerryTable *t, int row, int col)
{
    return t->cells[(size_t)row * (size_t)t->ncols + (size_t)col];
}

/* ------------------------------------------------------------------ */
/* Printing                                                           */
/* ------------------------------------------------------------------ */

/* Adds text to buf without overflowing it; ends with "..." if full. */
static void append(char *buf, size_t size, size_t *pos, const char *s)
{
    size_t n = strlen(s);
    if (*pos + n + 4 >= size) {
        if (*pos + 4 < size) { strcpy(buf + *pos, "..."); *pos += 3; }
        return;
    }
    memcpy(buf + *pos, s, n + 1);
    *pos += n;
}

/* Writes a decimal with at least min_dec and at most max_dec places. */
void dec_format(char *buf, size_t size, dec_t v, int min_dec, int max_dec)
{
    char digits[64], frac[PERRY_DECIMALS + 1];
    u128 a = (u128)(v < 0 ? -v : v);
    u128 whole = a / (u128)PERRY_SCALE;
    u128 part  = a % (u128)PERRY_SCALE;
    int n, i;

    /* the whole part, written backwards then turned round */
    n = 0;
    do { digits[n++] = (char)('0' + (int)(whole % 10)); whole /= 10; } while (whole > 0);
    for (i = 0; i < n / 2; i++) { char t = digits[i]; digits[i] = digits[n - 1 - i]; digits[n - 1 - i] = t; }
    digits[n] = '\0';

    for (i = PERRY_DECIMALS - 1; i >= 0; i--) { frac[i] = (char)('0' + (int)(part % 10)); part /= 10; }
    frac[PERRY_DECIMALS] = '\0';
    n = max_dec;
    while (n > min_dec && frac[n - 1] == '0') n--;
    frac[n] = '\0';

    if (n > 0) snprintf(buf, size, "%s%s.%s", v < 0 ? "-" : "", digits, frac);
    else       snprintf(buf, size, "%s%s",    v < 0 ? "-" : "", digits);
}

/* A value inside a list, row or table: text gets quotes. */
static void format_item(char *buf, size_t size, Value v, int quote_text)
{
    if (v.kind == VAL_TEXT && quote_text) snprintf(buf, size, "\"%.*s\"", v.text_len, v.text);
    else if (v.kind == VAL_NOTHING && !quote_text) buf[0] = '\0';    /* empty table cell */
    else                                  value_format(buf, size, v);
}

void value_format(char *buf, size_t size, Value v)
{
    char num[64], item[256];
    size_t pos = 0;
    int d, i;

    switch (v.kind) {
    case VAL_NOTHING:
        snprintf(buf, size, "nothing");
        break;
    case VAL_BOOL:
        snprintf(buf, size, "%s", v.boolean ? "yes" : "no");
        break;
    case VAL_NUMBER:
        dec_format(buf, size, v.amount, 0, PERRY_DECIMALS);
        break;
    case VAL_PERCENT:
        /* 0.15 is shown as 15% */
        dec_format(num, sizeof num,
                       dec_round(div_round(v.amount * 100, value_divisor(v)), PERCENT_SHOWN), 0, PERCENT_SHOWN);
        if (v.period != PERIOD_NONE) snprintf(buf, size, "%s%% per %s", num, period_name(v.period));
        else                         snprintf(buf, size, "%s%%", num);
        break;
    case VAL_MONEY:
        d = currency_decimals(v.currency);
        dec_format(num, sizeof num, v.amount, d, d);
        snprintf(buf, size, "%s %s", num, v.currency);
        break;
    case VAL_TEXT:
        snprintf(buf, size, "%.*s", v.text_len, v.text);
        break;
    case VAL_LIST:
        buf[0] = '\0';
        append(buf, size, &pos, "[");
        for (i = 0; i < v.list->count; i++) {
            if (i > 0) append(buf, size, &pos, ", ");
            format_item(item, sizeof item, v.list->items[i], 1);
            append(buf, size, &pos, item);
        }
        append(buf, size, &pos, "]");
        break;
    case VAL_ROW:
        buf[0] = '\0';
        append(buf, size, &pos, "{");
        for (i = 0; i < v.table->ncols; i++) {
            if (i > 0) append(buf, size, &pos, ", ");
            append(buf, size, &pos, v.table->cols[i]);
            append(buf, size, &pos, ": ");
            format_item(item, sizeof item, table_cell(v.table, v.row, i), 1);
            append(buf, size, &pos, item);
        }
        append(buf, size, &pos, "}");
        break;
    case VAL_TABLE:
        buf[0] = '\0';
        snprintf(item, sizeof item, "a table of %d row%s with columns ",
                 v.table->nrows, v.table->nrows == 1 ? "" : "s");
        append(buf, size, &pos, item);
        for (i = 0; i < v.table->ncols; i++) {
            if (i > 0) append(buf, size, &pos, ", ");
            append(buf, size, &pos, v.table->cols[i]);
        }
        break;
    case VAL_TAX:
        snprintf(buf, size, "a tax table with %d brackets", v.tax->count);
        break;
    case VAL_DATE:
        format_date(item, (long long)v.amount);
        snprintf(buf, size, "%s", item);
        break;
    case VAL_DURATION: {
        const char *unit = v.period == DUR_MONTH ? "month" :
                           v.period == DUR_YEAR  ? "year"  :
                           v.period == DUR_BUSINESS ? "business day" : "day";
        long long n = (long long)v.amount;
        snprintf(buf, size, "%lld %s%s", n, unit, (n == 1 || n == -1) ? "" : "s");
        break;
    }
    case VAL_FXRATE:
        dec_format(num, sizeof num, v.amount, currency_decimals(v.currency), PERRY_DECIMALS);
        snprintf(buf, size, "%s %s per %s", num, v.currency, v.base);
        break;
    }
}

/* Prints a table with its columns lined up; numbers on the right. */
static void print_table(FILE *f, const PerryTable *t)
{
    int *width = (int *)calloc((size_t)t->ncols + 1, sizeof(int));
    int *right = (int *)calloc((size_t)t->ncols + 1, sizeof(int));
    char cell[256];
    int r, c, n;

    if (width == NULL || right == NULL) out_of_memory();
    for (c = 0; c < t->ncols; c++) {
        width[c] = (int)strlen(t->cols[c]);
        right[c] = 0;
        for (r = 0; r < t->nrows; r++)                /* numbers line up on the right */
            if (value_is_numeric(table_cell(t, r, c))) { right[c] = 1; break; }
        for (r = 0; r < t->nrows; r++) {
            format_item(cell, sizeof cell, table_cell(t, r, c), 0);
            n = (int)strlen(cell);
            if (n > width[c]) width[c] = n;
        }
    }
    for (c = 0; c < t->ncols; c++)
        fprintf(f, "%s%*s", c ? "  " : "", right[c] ? width[c] : -width[c], t->cols[c]);
    for (r = 0; r < t->nrows; r++) {
        fputc('\n', f);
        for (c = 0; c < t->ncols; c++) {
            format_item(cell, sizeof cell, table_cell(t, r, c), 0);
            if (c == t->ncols - 1 && !right[c]) fprintf(f, "%s%s", c ? "  " : "", cell);
            else fprintf(f, "%s%*s", c ? "  " : "", right[c] ? width[c] : -width[c], cell);
        }
    }
    free(width);
    free(right);
}

void value_print(FILE *f, Value v)
{
    char buf[512];
    int i;
    switch (v.kind) {
    case VAL_TEXT:
        fwrite(v.text, 1, (size_t)v.text_len, f);
        return;
    case VAL_LIST:
        fputc('[', f);
        for (i = 0; i < v.list->count; i++) {
            Value it = v.list->items[i];
            if (i > 0) fputs(", ", f);
            if (it.kind == VAL_TEXT) fputc('"', f);
            value_print(f, it);
            if (it.kind == VAL_TEXT) fputc('"', f);
        }
        fputc(']', f);
        return;
    case VAL_TABLE:
        print_table(f, v.table);
        return;
    default:
        value_format(buf, sizeof buf, v);
        fputs(buf, f);
    }
}
