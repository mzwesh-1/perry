/*
 * main.c - the "PERRY" command.
 *
 *   PERRY test.perry            run a program
 *   PERRY --tokens test.perry   show the tokens the lexer finds
 *   PERRY --tree test.perry     show the tree the parser builds
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lexer.h"
#include "parser.h"
#include "interp.h"
#include "errors.h"
#include "check.h"

#define PERRY_VERSION "0.9.0"

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    char *buffer;
    long size;
    size_t got;

    if (f == NULL) {
        fprintf(stderr, "PERRY: can't open '%s'\n", path);
        return NULL;
    }
    fseek(f, 0L, SEEK_END);
    size = ftell(f);
    rewind(f);

    buffer = (char *)malloc((size_t)size + 1);
    if (buffer == NULL) {
        fprintf(stderr, "PERRY: not enough memory to read '%s'\n", path);
        fclose(f);
        return NULL;
    }
    got = fread(buffer, 1, (size_t)size, f);
    buffer[got] = '\0';
    fclose(f);
    return buffer;
}

static void show_tokens(const char *source)
{
    Lexer lx;
    Token t;
    int count = 0;

    printf(" line  token                       text\n");
    printf(" ----  -----                       ----\n");
    lexer_init(&lx, source);
    for (;;) {
        t = lexer_next(&lx);
        if (t.kind == TOK_ERROR)
            perry_fail(EXIT_PARSE_ERROR, t.line, "%s", lx.error);
        printf("%5d  %-28s", t.line, token_kind_name(t.kind));
        if (t.kind != TOK_NEWLINE && t.kind != TOK_INDENT &&
            t.kind != TOK_DEDENT && t.kind != TOK_EOF)
            printf("%.*s", t.length, t.start);
        printf("\n");
        count++;
        if (t.kind == TOK_EOF) break;
    }
    printf("\n%d tokens. No errors.\n", count);
}

static void usage(void)
{
    fprintf(stderr,
            "Perry %s\n"
            "usage: PERRY <file.perry>            run a program\n"
            "       PERRY --tokens <file.perry>   show the tokens\n"
            "       PERRY --tree <file.perry>     show the syntax tree\n"
            "       PERRY --check <file.perry>    check for mistakes without running\n",
            PERRY_VERSION);
}

int main(int argc, char **argv)
{
    const char *mode = "run";
    const char *path;
    char *source;

    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("Perry %s\n", PERRY_VERSION);
        return 0;
    }
    if (argc == 2) {
        path = argv[1];
    } else if (argc == 3 && (strcmp(argv[1], "--tokens") == 0 || strcmp(argv[1], "--tree") == 0 ||
                             strcmp(argv[1], "--check") == 0)) {
        mode = argv[1] + 2;
        path = argv[2];
    } else {
        usage();
        return 64;
    }

    source = read_file(path);
    if (source == NULL) return 74;
    g_file_name = path;
    g_source    = source;

    if (strcmp(mode, "tokens") == 0) {
        show_tokens(source);
    } else {
        Node *program = parse_program(source);
        if (strcmp(mode, "tree") == 0)       ast_print(program, 0);
        else if (strcmp(mode, "check") == 0) {
            int errors = check_program(program);
            if (errors > 0) {
                free(source);
                return EXIT_PARSE_ERROR;
            }
            printf("%s: no mistakes found\n", path);
        }
        else                                 interpret(program);
    }

    free(source);
    return 0;
}
