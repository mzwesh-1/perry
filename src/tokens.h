/*
 * tokens.h - every kind of token Perry knows about.
 *
 * The keyword list uses the old "X-macro" trick: the list is written
 * once, and then expanded in different ways (to make the enum, to make
 * the lookup table, to make printable names). Add a keyword here and
 * the whole lexer picks it up.
 */
#ifndef PERRY_TOKENS_H
#define PERRY_TOKENS_H

#define PERRY_KEYWORDS(X)                                        \
    /* modules */                                                \
    X(STEAL_FROM,              "steal_from_")                    \
    X(IN_THAT,                 "in_that_")                       \
    X(SHARE,                   "share_")                         \
    /* conditions */                                             \
    X(CHECK_IF,                "check_if_")                      \
    X(THATS_IF,                "thats_if_")                      \
    X(SINCE_THEN,              "since_then_")                    \
    X(IF_THEN,                 "if_then_")                       \
    X(NOT_UNLESS,              "not_unless_")                    \
    /* loops */                                                  \
    X(EXACTLY,                 "exactly_")                       \
    X(TIMES,                   "times")                          \
    X(GO_THROUGH,              "go_through_")                    \
    X(AS,                      "as_")                            \
    X(KEEP_GOING_WHILE,        "keep_going_while_")              \
    X(STOP_NOW,                "stop_now_")                      \
    X(SKIP_THIS_ONE,           "skip_this_one_")                 \
    /* errors */                                                 \
    X(TRY_THIS,                "try_this_")                      \
    X(EVEN_IF,                 "even_if_")                       \
    X(THERES_AN_ERROR,         "theres_an_error_")               \
    X(THERES_A_CURRENCY_ERROR, "theres_a_currency_error_")       \
    X(THERES_A_FILE_ERROR,     "theres_a_file_error_")           \
    X(THERES_A_BALANCE_ERROR,  "theres_a_balance_error_")        \
    X(BUT_THEN,                "but_then_")                      \
    X(RAISE_ALARM,             "raise_alarm_")                   \
    /* functions, records, constants */                          \
    X(DEFINE,                  "define_")                        \
    X(GIVE_BACK,               "give_back_")                     \
    X(BLUEPRINT,               "blueprint_")                     \
    X(NEVER_CHANGES,           "never_changes_")                 \
    /* comparing two values */                                   \
    X(IS_EQUAL_TO,             "is_equal_to_")                   \
    X(IS_NOT_EQUAL_TO,         "is_not_equal_to_")               \
    X(IS_GREATER_THAN,         "is_greater_than_")               \
    X(IS_LESS_THAN,            "is_less_than_")                  \
    X(IS_GREATER_EQUAL_TO,     "is_greater_equal_to_")           \
    X(IS_LESS_EQUAL_TO,        "is_less_equal_to_")              \
    X(IS_SIMILAR_TO,           "is_similar_to_")                 \
    X(IS_REALLY_NOT_CLOSE_TO,  "is_really_not_close_to_")        \
    X(WITHIN,                  "within_")                        \
    /* filtering lists */                                        \
    X(THAT_IS_EQUAL_TO,        "that_is_equal_to_")              \
    X(THAT_IS_NOT_EQUAL_TO,    "that_is_not_equal_to_")          \
    X(THAT_IS_GREATER_THAN,    "that_is_greater_than_")          \
    X(THAT_IS_LESS_THAN,       "that_is_less_than_")             \
    X(THAT_IS_GREATER_EQUAL_TO,"that_is_greater_equal_to_")      \
    X(THAT_IS_LESS_EQUAL_TO,   "that_is_less_equal_to_")         \
    X(THAT_IS_SIMILAR_TO,      "that_is_similar_to_")            \
    X(THAT_IS_REALLY_NOT_CLOSE_TO, "that_is_really_not_close_to_") \
    /* logic and constants */                                    \
    X(AND,                     "and_")                           \
    X(OR,                      "or_")                            \
    X(NOT,                     "not_")                           \
    X(YES,                     "yes_")                           \
    X(NO,                      "no_")                            \
    X(NOTHING,                 "nothing_")                       \
    X(TODAY,                   "today_")                         \
    /* lists */                                                  \
    X(ADD_WHERE,               "add_where_")                     \
    X(REMOVE_WHERE,            "remove_where_")                  \
    X(DUPLICATE_WHERE,         "duplicate_where_")               \
    X(ERASE,                   "erase_")                         \
    X(GO_GET,                  "go_get_")                        \
    X(GO_GET_IN,               "go_get_in_")                     \
    X(TOTAL_OF,                "total_of_")                      \
    X(COUNT_OF,                "count_of_")                      \
    X(AVERAGE_OF,              "average_of_")                    \
    X(BIGGEST_OF,              "biggest_of_")                    \
    X(SMALLEST_OF,             "smallest_of_")                   \
    X(SORT,                    "sort_")                          \
    X(BY,                      "by_")                            \
    X(HIGHEST_FIRST,           "highest_first_")                 \
    X(LOWEST_FIRST,            "lowest_first_")                  \
    X(GROUP_BY,                "group_by_")                      \
    /* input and output */                                       \
    X(SHOW_ME_RESULTS,         "show_me_results")                \
    X(ASK_USER,                "ask_user_")                      \
    X(SAVE_TO,                 "save_to_")                       \
    /* money */                                                  \
    X(BECAUSE_IS,              "because_is_")                    \
    X(SPLIT,                   "split")                          \
    X(INTO,                    "into")                           \
    X(PER,                     "per")                            \
    X(OF,                      "of")                             \
    X(ROUND_TO,                "round_to_")                      \
    X(THE_BANK_WAY,            "the_bank_way_")                  \
    X(UP,                      "up_")                            \
    X(DOWN,                    "down_")                          \
    X(CONVERT,                 "convert_")                       \
    X(TO,                      "to_")                            \
    X(AT_RATE,                 "at_rate_")                       \
    X(ON,                      "on_")                            \
    X(VAT_ON,                  "vat_on_")                        \
    X(VAT_INSIDE,              "vat_inside_")                    \
    X(TAX_TABLE,               "tax_table_")                     \
    X(UP_TO,                   "up_to_")                         \
    X(TAXED_AT,                "taxed_at_")                      \
    X(ABOVE,                   "above_")                         \
    /* dates */                                                  \
    X(DAYS_BETWEEN,            "days_between_")                  \
    X(BUSINESS_DAYS_BETWEEN,   "business_days_between_")         \
    X(END_OF_MONTH,            "end_of_month_")                  \
    /* bookkeeping */                                            \
    X(BOOK,                    "book_")                          \
    X(DEBIT,                   "debit_")                         \
    X(CREDIT,                  "credit_")

typedef enum {
    /* layout */
    TOK_EOF,
    TOK_NEWLINE,
    TOK_INDENT,
    TOK_DEDENT,

    /* values */
    TOK_IDENT,      /* balance, late_fee, tx          */
    TOK_NUMBER,     /* 12, 3.5                        */
    TOK_MONEY,      /* 199.99 ZAR                     */
    TOK_PERCENT,    /* 15%, 11.75%                    */
    TOK_DATE,       /* 2026-12-31                     */
    TOK_STRING,     /* "hello"                        */

    /* symbols */
    TOK_ASSIGN,     /* =  */
    TOK_PLUS,       /* +  */
    TOK_MINUS,      /* -  */
    TOK_STAR,       /* *  */
    TOK_SLASH,      /* /  */
    TOK_LPAREN,     /* (  */
    TOK_RPAREN,     /* )  */
    TOK_LBRACKET,   /* [  */
    TOK_RBRACKET,   /* ]  */
    TOK_COMMA,      /* ,  */
    TOK_COLON,      /* :  */
    TOK_DOT,        /* .  */

    TOK_ERROR,

    /* one token kind per keyword: TOK_CHECK_IF, TOK_SINCE_THEN, ... */
#define X(name, text) TOK_##name,
    PERRY_KEYWORDS(X)
#undef X

    TOK_KIND_COUNT
} TokenKind;

typedef struct {
    TokenKind   kind;
    const char *start;   /* points into the source text (not copied) */
    int         length;
    int         line;
} Token;

const char *token_kind_name(TokenKind kind);
const char *token_keyword_text(TokenKind kind);   /* e.g. "is_greater_than_" */
int         token_is_keyword(TokenKind kind);

#endif
