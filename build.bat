@echo off
rem Build Perry on Windows without make
gcc -std=c99 -Wall -Wextra -pedantic -O2 -o PERRY.exe src\main.c src\lexer.c src\parser.c src\interp.c src\value.c src\errors.c src\csv.c src\dates.c src\finance.c
if errorlevel 1 (
    echo Build failed.
) else (
    echo Built PERRY.exe
)
