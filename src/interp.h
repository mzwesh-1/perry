/*
 * interp.h - runs a Perry syntax tree.
 */
#ifndef PERRY_INTERP_H
#define PERRY_INTERP_H

#include "ast.h"

/* Runs the program. On a runtime error it reports and exits. */
void interpret(const Node *program);

/* Reports a runtime error that try_this_ can catch. Never returns. */
void perry_runtime_fail(int line, const char *fmt, ...);

#endif
