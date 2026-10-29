/*
 * ast.h - the syntax tree the parser builds.
 *
 * Every piece of a program becomes a Node. For example
 *
 *     check_if_ total is_greater_than_ budget:
 *         show_me_results("Over budget")
 *
 * becomes an N_IF node whose 'a' is the condition (an N_COMPARE node)
 * and whose 'b' is the block of statements to run.
 */
#ifndef PERRY_AST_H
#define PERRY_AST_H

#include "tokens.h"
#include "value.h"

typedef enum {
    /* values */
    N_LITERAL,     /* 12, 15%, 199.99 ZAR, "hi", yes_, nothing_ : value */
    N_VAR,         /* price                      : name              */

    /* expressions */
    N_NEGATE,      /* -a                                             */
    N_BINARY,      /* a + b, a * b, a of b       : op, a, b          */
    N_COMPARE,     /* a is_greater_than_ b       : op, a, b          */
    N_SIMILAR,     /* a is_similar_to_ b within_ c : op, a, b, c     */
    N_AND,         /* a and_ b                                       */
    N_OR,          /* a or_ b                                        */
    N_NOT,         /* not_ a                                         */

    /* statements */
    N_BLOCK,       /* several statements         : items             */
    N_ASSIGN,      /* name = a                                       */
    N_CONSTANT,    /* never_changes_ name = a                        */
    N_SHOW,        /* show_me_results(items...)                      */
    N_IF,          /* check_if_ a: b  (c = thats_if_/since_then_)    */
    N_REPEAT,      /* exactly_ a times: b                            */
    N_WHILE,       /* keep_going_while_ a: b                         */
    N_REQUIRE,     /* not_unless_ a                                  */
    N_STOP,        /* stop_now_                                      */
    N_SKIP,        /* skip_this_one_                                 */

    /* lists */
    N_LIST,        /* [a, b, c]                  : items             */
    N_SPLIT,       /* split a into b                                 */
    N_TOTAL,       /* total_of_ a                                    */
    N_COUNT,       /* count_of_ a                                    */
    N_FOR,         /* go_through_ a as_ name: b                      */

    /* functions */
    N_FUNCTION,    /* define_ name(items): b     (items are N_VAR)   */
    N_CALL,        /* name(items)                                    */
    N_RETURN,      /* give_back_ a                                   */

    /* explaining */
    N_BECAUSE,     /* because_is_ name                               */

    /* v0.4: tables, rates, rounding, tax */
    N_FIELD,       /* a.name        (a row's cell, or a table's column) */
    N_PER,         /* a per year / a per month   : name = unit       */
    N_ROUND,       /* round_to_ a b decimals [op = mode]             */
    N_LOAD,        /* go_get_ a [as_ name]  (name = currency)        */
    N_FILTER,      /* go_get_in_ a [by_ name] op b [within_ c]       */
    N_REMOVE,      /* remove_where_ a [by_ name] op b [within_ c]    */
    N_SORT,        /* sort_ a [by_ name] highest_first_/lowest_first_ */
    N_TAX_TABLE,   /* tax_table_ name: items (N_BRACKET)             */
    N_BRACKET,     /* up_to_ a taxed_at_ b   (a = NULL for above_)   */
    N_TAX_APPLY,   /* name on_ a                                     */

    /* v0.5: dates and errors */
    N_TODAY,       /* today_                                         */
    N_DURATION,    /* a days / a months / a business days : op = DUR_ unit */
    N_BETWEEN,     /* days_between_ a and_ b   (op = which keyword)  */
    N_END_OF_MONTH,/* end_of_month_ a                                */
    N_IMPORT,      /* steal_from_ name                               */
    N_TRY,         /* try_this_: a   items = handlers   c = but_then_ */
    N_HANDLER,     /* even_if_ op: b                                 */
    N_ALARM,       /* raise_alarm_ a                                 */

    /* v0.6: bookkeeping, saving, currency conversion */
    N_BOOK,        /* book_ a: items (N_ENTRY)                       */
    N_ENTRY,       /* debit_/credit_ (op) account (name) amount (a)  */
    N_SAVE,        /* save_to_ a b    (a = file name, b = value)     */
    N_CONVERT,     /* convert_ a to_ name at_rate_ b [on_ c]         */

    /* v0.7 */
    N_STAT,        /* average_of_/biggest_of_/smallest_of_ a  (op)   */
    N_GROUP,       /* group_by_ a [by_ name]                         */
    N_VAT,         /* vat_on_ a / vat_inside_ a  (op)                */
    N_ADD,         /* add_where_ a b   (a = N_VAR list, b = value)   */
    N_ERASE,       /* erase_ name                                    */

    /* v0.8 */
    N_BLUEPRINT,   /* blueprint_ name: items = fields (N_VAR, a = type N_VAR) */
    N_SHARE,       /* share_ items (N_VAR names)                     */
    N_DUPLICATES,  /* duplicate_where_ a [by_ name]                  */
    N_ASK          /* ask_user_ a                                    */
} NodeKind;
/* N_IMPORT: steal_from_ name, or in_that_ name steal_from_ items */

typedef struct Node Node;

struct Node {
    NodeKind    kind;
    int         line;
    TokenKind   op;         /* which operator or comparison      */
    Value       value;      /* N_LITERAL                          */
    const char *name;       /* N_VAR, N_ASSIGN, N_CONSTANT        */
    int         name_len;
    Node       *a, *b, *c;  /* children                           */
    Node      **items;      /* N_BLOCK statements, N_SHOW values  */
    int         count;
};

#endif
