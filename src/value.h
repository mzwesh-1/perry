/*
 * value.h - the values a Perry program works with.
 *
 * Perry never uses floating point for numbers. Every number, percent
 * and money amount is stored as a whole number of trillionths
 * (12 decimal places) in a 128-bit integer:
 *
 *     1          is stored as  1000000000000
 *     0.1        is stored as   100000000000
 *     15%        is stored as   150000000000   (15% = 0.15)
 *     199.99 ZAR is stored as 199990000000000, currency "ZAR"
 *
 * Amounts can be up to 999 999 999 999 999 999 (18 digits) before the
 * decimal point.
 *
 * So 0.1 + 0.2 is exactly 0.3, and money never loses a cent.
 *
 * A rate can also carry a period: 11.75% per year. Turning it into a
 * monthly rate doesn't divide (which would lose digits); it records a
 * divisor of 12 and divides only at the moment it is used.
 */
#ifndef PERRY_VALUE_H
#define PERRY_VALUE_H

#include <stdio.h>

/* A 128-bit whole number (GCC and Clang have it on 64-bit computers). */
__extension__ typedef __int128 dec_t;

#define PERRY_DECIMALS 12
#define PERRY_SCALE    ((dec_t)1000000000000LL)

typedef enum {
    VAL_NOTHING,
    VAL_BOOL,      /* yes_ / no_                     */
    VAL_NUMBER,    /* 12, 3.5                        */
    VAL_PERCENT,   /* 15%, 11.75% per year           */
    VAL_MONEY,     /* 199.99 ZAR                     */
    VAL_TEXT,      /* "hello"                        */
    VAL_LIST,      /* [33.34 ZAR, 33.33 ZAR]         */
    VAL_TABLE,     /* rows and columns (CSV, loan schedule) */
    VAL_ROW,       /* one row of a table             */
    VAL_TAX,       /* a tax_table_                   */
    VAL_DATE,      /* 2026-12-31   (amount = day number)            */
    VAL_DURATION,  /* 3 days, 1 month  (amount = count, period = unit) */
    VAL_FXRATE     /* 18.20 ZAR per USD: amount of 'currency' for 1 'base' */
} ValueKind;

/* Units for durations (stored in Value.period) */
#define DUR_DAY      1
#define DUR_MONTH    2
#define DUR_YEAR     3
#define DUR_BUSINESS 4

/* Periods for rates */
#define PERIOD_NONE  0
#define PERIOD_YEAR  1
#define PERIOD_MONTH 12

typedef struct PerryList  PerryList;
typedef struct PerryTable PerryTable;
typedef struct PerryTax   PerryTax;

typedef struct {
    ValueKind   kind;
    dec_t       amount;       /* NUMBER, PERCENT, MONEY: trillionths  */
    long long   divisor;      /* PERCENT: divide amount by this (0=1)  */
    int         period;       /* PERCENT: PERIOD_YEAR, PERIOD_MONTH    */
    char        currency[4];  /* MONEY: "ZAR", "USD", ...             */
    char        base[4];      /* FXRATE: the "per USD" currency       */
    int         boolean;      /* BOOL                                 */
    const char *text;         /* TEXT (not zero-terminated)           */
    int         text_len;
    PerryList  *list;         /* LIST                                 */
    PerryTable *table;        /* TABLE, ROW                           */
    int         row;          /* ROW: which row                       */
    PerryTax   *tax;          /* TAX                                  */
} Value;

struct PerryList {
    int    count;
    int    capacity;
    Value *items;
};

struct PerryTable {
    int    ncols;
    char **cols;              /* column names                         */
    int    nrows;
    int    capacity;
    Value *cells;             /* cells[row * ncols + col]             */
};

struct PerryTax {
    int    count;
    Value *limits;            /* up_to_ amounts (money)               */
    Value *rates;             /* taxed_at_ percentages                */
    /* the last bracket is always above_ and has no limit           */
};

PerryList  *list_new(void);
void        list_push(PerryList *list, Value v);

PerryTable *table_new(int ncols, char **cols);
void        table_add_row(PerryTable *t, const Value *cells);
int         table_column(const PerryTable *t, const char *name, int len);   /* -1 if none */
Value       table_cell(const PerryTable *t, int row, int col);

/* Decimal arithmetic. Each returns 0 if the result is too big. */
int dec_add(dec_t a, dec_t b, dec_t *out);
int dec_sub(dec_t a, dec_t b, dec_t *out);
int dec_mul(dec_t a, dec_t b, dec_t *out);
int dec_div(dec_t a, dec_t b, dec_t *out);   /* b != 0 */
/* a * b / d, rounded once at the end (for rates with a divisor) */
int dec_mul_frac(dec_t a, dec_t b, long long d, dec_t *out);
/* a * b / d with plain whole numbers, rounded once (no scale involved).
 * The in-between product can be up to 256 bits, so nothing overflows. */
int dec_muldiv(dec_t a, dec_t b, dec_t d, dec_t *out);
/* The same without Perry's size limit (answers up to about 8 x 10^37),
 * for the extra-precise in-between steps of finance and statistics. */
int dec_muldiv_raw(dec_t a, dec_t b, dec_t d, dec_t *out);
/* Square root of a decimal (a >= 0), rounded to 12 decimal places. */
dec_t dec_sqrt(dec_t a);
/* The biggest amount Perry allows (just under 10^18 whole units). */
int dec_fits(dec_t a);

typedef enum { ROUND_NORMAL, ROUND_BANK, ROUND_UP, ROUND_DOWN } RoundMode;

/* Rounds to a number of decimal places, halves away from zero. */
dec_t dec_round(dec_t v, int decimals);
/* ROUND_BANK: halves to even. ROUND_UP: towards +infinity. ROUND_DOWN: towards -infinity. */
dec_t dec_round_mode(dec_t v, int decimals, RoundMode mode);

/*
 * Reads digits like "199.99". Returns 1 on success, 0 if there are
 * more than 12 decimals, -1 if the number is too big.
 * *decimals receives how many decimal places were written.
 */
int dec_parse(const char *s, int len, dec_t *out, int *decimals);

/* How many decimal places a currency uses (ZAR 2, JPY 0, KWD 3). */
int currency_decimals(const char *code);

int         value_is_numeric(Value v);
long long   value_divisor(Value v);           /* 1 unless a converted rate */
dec_t       percent_fraction(Value v);         /* amount / divisor, rounded */
const char *value_kind_name(ValueKind kind);
const char *period_name(int period);

/* Writes a decimal as text with between min_dec and max_dec decimals. */
void dec_format(char *buf, size_t size, dec_t v, int min_dec, int max_dec);

/* Writes a value as text, e.g. "4600.00 ZAR", "15%", "yes". */
void value_format(char *buf, size_t size, Value v);
void value_print(FILE *f, Value v);

#endif
