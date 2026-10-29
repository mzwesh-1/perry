/*
 * finance.h - Perry for Universities: finance, actuarial maths and statistics.
 */
#ifndef PERRY_FINANCE_H
#define PERRY_FINANCE_H

#include "value.h"

/* Which built-in function this name is (npv_, irr_, ...), or -1. */
int         finance_lookup(const char *name, int len);
const char *finance_name(int index);

/* Runs it with the values already worked out. */
Value       finance_call(int fn, const Value *args, int count, int line);

#endif
