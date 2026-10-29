# Perry - build with:  make
# The command is called PERRY (capital letters), so it never clashes with
# other programs called "perry".
CC     = gcc
CFLAGS = -std=c99 -Wall -Wextra -pedantic -O2

SRC = src/main.c src/lexer.c src/parser.c src/interp.c src/value.c src/errors.c src/csv.c src/dates.c src/finance.c src/check.c
HDR = src/tokens.h src/lexer.h src/ast.h src/parser.h src/interp.h src/value.h src/errors.h src/csv.h src/dates.h src/finance.h src/check.h

PERRY: $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o PERRY $(SRC)

# A PERRY that runs on any 64-bit Linux or WSL, with nothing else needed
# (this is the one packed inside the VS Code extension).
static: $(SRC) $(HDR)
	$(CC) $(CFLAGS) -static -s -o PERRY-linux-x64 $(SRC)

test: PERRY
	./PERRY test.perry
	sh tests/check/run.sh

# Install PERRY so it can be run from anywhere (and found by VS Code):
#   sudo make install
PREFIX = /usr/local

install: PERRY
	install -m 755 PERRY $(PREFIX)/bin/PERRY

uninstall:
	rm -f $(PREFIX)/bin/PERRY

clean:
	rm -f PERRY PERRY.exe PERRY-linux-x64 perry perry.exe

.PHONY: test clean install uninstall static
