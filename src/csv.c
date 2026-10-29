/*
 * csv.c - a small CSV reader.
 *
 *   date,description,amount
 *   2026-09-01,Rent,-8000.00
 *   2026-09-02,"Groceries, Checkers",-1250.50
 *
 * Column names become Perry names: "Transaction Date" -> transaction_date.
 * Cells that look like numbers become numbers (or money, if a currency
 * was given); "15%" becomes a percent; everything else stays text.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "csv.h"
#include "dates.h"

typedef struct {
    char  *text;
    size_t len, cap;
} Field;

static void field_add(Field *f, char c)
{
    if (f->len + 2 > f->cap) {
        f->cap = f->cap ? f->cap * 2 : 32;
        f->text = (char *)realloc(f->text, f->cap);
        if (f->text == NULL) { fprintf(stderr, "PERRY: out of memory\n"); exit(1); }
    }
    f->text[f->len++] = c;
    f->text[f->len] = '\0';
}

static char *read_whole_file(const char *path)
{
    FILE *fp = fopen(path, "rb");
    char *buf;
    long size;
    size_t got;
    if (fp == NULL) return NULL;
    fseek(fp, 0L, SEEK_END);
    size = ftell(fp);
    rewind(fp);
    buf = (char *)malloc((size_t)size + 1);
    if (buf == NULL) { fclose(fp); return NULL; }
    got = fread(buf, 1, (size_t)size, fp);
    buf[got] = '\0';
    fclose(fp);
    return buf;
}

/*
 * Reads one line of fields starting at *p. Handles "quoted, fields"
 * and "" inside quotes. Returns the number of fields, 0 at end of file.
 */
static int read_row(const char **p, int *line, Field **fields, int *capacity)
{
    const char *s = *p;
    int count = 0;

    if (*s == '\0') return 0;
    for (;;) {
        Field *f;
        if (count == *capacity) {
            int old = *capacity;
            *capacity = *capacity ? *capacity * 2 : 8;
            *fields = (Field *)realloc(*fields, sizeof(Field) * (size_t)*capacity);
            if (*fields == NULL) { fprintf(stderr, "PERRY: out of memory\n"); exit(1); }
            memset(*fields + old, 0, sizeof(Field) * (size_t)(*capacity - old));
        }
        f = &(*fields)[count++];
        f->len = 0;
        field_add(f, '\0');          /* makes sure the text exists ...  */
        f->len = 0;                  /* ... and starts out empty        */
        f->text[0] = '\0';

        if (*s == '"') {
            s++;
            for (;;) {
                if (*s == '\0') break;
                if (*s == '"') {
                    if (s[1] == '"') { field_add(f, '"'); s += 2; continue; }
                    s++;
                    break;
                }
                if (*s == '\n') (*line)++;
                field_add(f, *s++);
            }
            while (*s && *s != ',' && *s != '\n' && *s != '\r') s++;
        } else {
            while (*s && *s != ',' && *s != '\n' && *s != '\r') field_add(f, *s++);
        }

        if (*s == ',') { s++; continue; }
        if (*s == '\r') s++;
        if (*s == '\n') { s++; (*line)++; }
        break;
    }
    *p = s;
    return count;
}

/* "Transaction Date" -> "transaction_date" */
static char *column_name(const char *raw, int index)
{
    char *name = (char *)malloc(strlen(raw) + 16);
    int n = 0, i;
    if (name == NULL) { fprintf(stderr, "PERRY: out of memory\n"); exit(1); }
    for (i = 0; raw[i]; i++) {
        unsigned char c = (unsigned char)raw[i];
        if (isalnum(c))            name[n++] = (char)tolower(c);
        else if (n > 0 && name[n - 1] != '_') name[n++] = '_';
    }
    while (n > 0 && name[n - 1] == '_') n--;
    name[n] = '\0';
    if (n == 0 || isdigit((unsigned char)name[0]))
        sprintf(name, "column_%d", index + 1);
    return name;
}

static char *trim(char *s)
{
    char *end;
    while (*s == ' ' || *s == '\t') s++;
    end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t')) end--;
    *end = '\0';
    return s;
}

/* Turns one cell into a value. Returns 0 (and an error) if money has too many decimals. */
static int cell_value(char *raw, const char *currency, Value *out, char *error, size_t error_size)
{
    char *s = trim(raw);
    int len = (int)strlen(s), negative = 0, i, digits = 0, dots = 0, percent = 0, decimals;
    dec_t amount;
    long long days;
    Value v;

    memset(&v, 0, sizeof v);
    if (len == 0) {                             /* an empty cell is nothing_ */
        v.kind = VAL_NOTHING;
        *out = v;
        return 1;
    }
    if (parse_date(s, len, &days)) {            /* 2026-09-01 becomes a date */
        v.kind = VAL_DATE;
        v.amount = days;
        *out = v;
        return 1;
    }
    if (len > 0 && s[len - 1] == '%') { percent = 1; len--; }
    i = 0;
    if (i < len && (s[i] == '-' || s[i] == '+')) { negative = s[i] == '-'; i++; }
    for (; i < len; i++) {
        if (s[i] >= '0' && s[i] <= '9') digits++;
        else if (s[i] == '.' && dots == 0) dots++;
        else break;
    }

    if (digits > 0 && i == len && s[len - 1] != '.') {
        const char *num = s + ((s[0] == '-' || s[0] == '+') ? 1 : 0);
        int r = dec_parse(num, (int)(s + len - num), &amount, &decimals);
        if (r == 1) {
            if (negative) amount = -amount;
            if (percent) {
                if (decimals > PERRY_DECIMALS - 2) goto as_text;
                v.kind = VAL_PERCENT;
                v.amount = amount / 100;
            } else if (currency != NULL) {
                if (decimals > currency_decimals(currency)) {
                    snprintf(error, error_size, "'%s' has more decimal places than %s allows", s, currency);
                    return 0;
                }
                v.kind = VAL_MONEY;
                v.amount = amount;
                memcpy(v.currency, currency, 4);
            } else {
                v.kind = VAL_NUMBER;
                v.amount = amount;
            }
            *out = v;
            return 1;
        }
    }

as_text:
    v.kind = VAL_TEXT;
    v.text_len = (int)strlen(s);
    {
        char *copy = (char *)malloc((size_t)v.text_len + 1);
        if (copy == NULL) { fprintf(stderr, "PERRY: out of memory\n"); exit(1); }
        memcpy(copy, s, (size_t)v.text_len + 1);
        v.text = copy;
    }
    *out = v;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Writing                                                            */
/* ------------------------------------------------------------------ */

static void write_text(FILE *f, const char *s, int len)
{
    int i, needs_quotes = 0;
    for (i = 0; i < len; i++)
        if (s[i] == ',' || s[i] == '"' || s[i] == '\n' || s[i] == '\r') needs_quotes = 1;
    if (!needs_quotes) {
        fwrite(s, 1, (size_t)len, f);
        return;
    }
    fputc('"', f);
    for (i = 0; i < len; i++) {
        if (s[i] == '"') fputc('"', f);         /* " becomes "" inside quotes */
        fputc(s[i], f);
    }
    fputc('"', f);
}

static void write_cell(FILE *f, Value v)
{
    char buf[512];
    if (v.kind == VAL_TEXT) {
        write_text(f, v.text, v.text_len);
        return;
    }
    if (v.kind == VAL_NOTHING) return;          /* empty cell */
    if (v.kind == VAL_MONEY) {                  /* 8000.00, without the currency */
        Value plain = v;
        char *space;
        value_format(buf, sizeof buf, plain);
        space = strchr(buf, ' ');
        if (space) *space = '\0';
        fputs(buf, f);
        return;
    }
    value_format(buf, sizeof buf, v);
    write_text(f, buf, (int)strlen(buf));
}

int csv_save(const char *path, Value v, char *error, size_t error_size)
{
    FILE *f = fopen(path, "wb");
    int r, c;

    if (f == NULL) {
        snprintf(error, error_size, "can't write the file '%s'", path);
        return 0;
    }
    if (v.kind == VAL_TABLE || v.kind == VAL_ROW) {
        const PerryTable *t = v.table;
        int first = v.kind == VAL_ROW ? v.row : 0;
        int last  = v.kind == VAL_ROW ? v.row + 1 : t->nrows;
        for (c = 0; c < t->ncols; c++) {
            if (c) fputc(',', f);
            write_text(f, t->cols[c], (int)strlen(t->cols[c]));
        }
        fputs("\r\n", f);
        for (r = first; r < last; r++) {
            for (c = 0; c < t->ncols; c++) {
                if (c) fputc(',', f);
                write_cell(f, table_cell(t, r, c));
            }
            fputs("\r\n", f);
        }
    } else if (v.kind == VAL_LIST) {
        fputs("value\r\n", f);
        for (r = 0; r < v.list->count; r++) {
            write_cell(f, v.list->items[r]);
            fputs("\r\n", f);
        }
    } else {
        fputs("value\r\n", f);
        write_cell(f, v);
        fputs("\r\n", f);
    }
    if (fclose(f) != 0) {
        snprintf(error, error_size, "couldn't finish writing '%s'", path);
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Reading                                                            */
/* ------------------------------------------------------------------ */

PerryTable *csv_load(const char *path, const char *currency,
                     char *error, size_t error_size, int *error_line)
{
    char *source = read_whole_file(path);
    const char *p;
    Field *fields = NULL;
    int capacity = 0, ncols, n, i, line = 1, row_line;
    char **cols;
    Value *cells;
    PerryTable *table;

    *error_line = 0;
    if (source == NULL) {
        snprintf(error, error_size, "can't open the file '%s'", path);
        return NULL;
    }
    p = source;
    if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF)
        p += 3;                                     /* skip a UTF-8 marker from Excel */

    ncols = read_row(&p, &line, &fields, &capacity);
    if (ncols == 0) {
        snprintf(error, error_size, "'%s' is empty - it needs a first line with column names", path);
        free(source);
        return NULL;
    }
    cols = (char **)malloc(sizeof(char *) * (size_t)ncols);
    if (cols == NULL) { fprintf(stderr, "PERRY: out of memory\n"); exit(1); }
    for (i = 0; i < ncols; i++) cols[i] = column_name(fields[i].text, i);
    table = table_new(ncols, cols);

    cells = (Value *)malloc(sizeof(Value) * (size_t)ncols);
    if (cells == NULL) { fprintf(stderr, "PERRY: out of memory\n"); exit(1); }

    for (;;) {
        row_line = line;
        n = read_row(&p, &line, &fields, &capacity);
        if (n == 0) break;
        if (n == 1 && trim(fields[0].text)[0] == '\0') continue;   /* blank line */
        if (n != ncols) {
            snprintf(error, error_size, "line %d of '%s' has %d value%s but there are %d columns",
                     row_line, path, n, n == 1 ? "" : "s", ncols);
            *error_line = row_line;
            free(source);
            return NULL;
        }
        for (i = 0; i < ncols; i++) {
            if (!cell_value(fields[i].text, currency, &cells[i], error, error_size)) {
                char msg[256];
                snprintf(msg, sizeof msg, "line %d of '%s': %s", row_line, path, error);
                snprintf(error, error_size, "%s", msg);
                *error_line = row_line;
                free(source);
                return NULL;
            }
        }
        table_add_row(table, cells);
    }

    for (i = 0; i < capacity; i++) free(fields[i].text);
    free(fields);
    free(cells);
    free(source);
    return table;
}
