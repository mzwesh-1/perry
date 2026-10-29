/*
 * parser.h - turns tokens into a syntax tree.
 */
#ifndef PERRY_PARSER_H
#define PERRY_PARSER_H

#include "ast.h"

/* Parses a whole program. On a syntax error it reports and exits. */
Node *parse_program(const char *source);

/* Prints the tree, for "perry --tree file.perry". */
void ast_print(const Node *n, int depth);

#endif
