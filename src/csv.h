/*
 * csv.h - reading a CSV file (like a bank statement) into a table.
 */
#ifndef PERRY_CSV_H
#define PERRY_CSV_H

#include "value.h"

/*
 * Loads a CSV file. The first line must hold the column names.
 * If currency is not NULL, plain numbers become money in that currency.
 * Returns NULL and fills 'error' (and *error_line) if something is wrong.
 */
PerryTable *csv_load(const char *path, const char *currency,
                     char *error, size_t error_size, int *error_line);

/*
 * Saves a value as a CSV file: a table with its column names, a list as
 * one column, a row as one line, anything else as a single value.
 * Money is written as a plain number (e.g. -8000.00) so spreadsheets can
 * add it up; read it back with go_get_ "file.csv" as_ ZAR.
 * Returns 0 and fills 'error' if the file can't be written.
 */
int csv_save(const char *path, Value v, char *error, size_t error_size);

#endif
