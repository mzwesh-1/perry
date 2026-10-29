/*
 * check.h - PERRY --check: finds mistakes without running the program.
 */
#ifndef PERRY_CHECK_H
#define PERRY_CHECK_H

#include "ast.h"

/* Prints every mistake found (as file:line: error: ...) and returns how many. */
int check_program(const Node *program);

#endif
