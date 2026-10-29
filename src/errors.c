/*
 * errors.c - one place that prints every Perry error the same way:
 *
 *   test.perry:12: error: can't add ZAR and USD
 *      12 | total = price + fee
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "errors.h"

const char *g_file_name = "?";
const char *g_source    = NULL;

static void print_source_line(int line)
{
    const char *p = g_source;
    const char *end;
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
    if (end > p)
        fprintf(stderr, "%6d | %.*s\n", line, (int)(end - p), p);
}

void perry_fail(int exit_code, int line, const char *fmt, ...)
{
    va_list args;

    fflush(stdout);
    fprintf(stderr, "%s:%d: error: ", g_file_name, line);
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
    print_source_line(line);
    exit(exit_code);
}
