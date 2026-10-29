/*
 * errors.h - reporting errors with the file name, line and the line of code.
 */
#ifndef PERRY_ERRORS_H
#define PERRY_ERRORS_H

#define EXIT_PARSE_ERROR   65
#define EXIT_RUNTIME_ERROR 70

extern const char *g_file_name;
extern const char *g_source;

/* Prints the error and the offending line, then exits the program. */
void perry_fail(int exit_code, int line, const char *fmt, ...);

#endif
