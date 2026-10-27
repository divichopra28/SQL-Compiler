# =====================================================================
#  SQL-C : A Lex and Yacc Based SQL Compiler
# =====================================================================
#
#   make            build ./sqlc
#   make run        start the interactive compiler
#   make demo       run the demo script (creates sample tables)
#   make test       run the regression tests in tests/
#   make clean      remove build products
#   make reset-db   delete the database files
#
# Requirements: gcc, flex, bison (>= 3.6), SQLite3 development files
#   Ubuntu/WSL : sudo apt install build-essential flex bison libsqlite3-dev
#   macOS      : brew install flex bison sqlite   (use brew's bison)

CC      ?= gcc
CFLAGS  ?= -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -O2 -g
FLEX    ?= flex
BISON   ?= bison
LDLIBS  = -lsqlite3 -lm

TARGET  = sqlc
OBJS    = parser.tab.o lex.yy.o main.o ast.o util.o diag.o symbol_table.o \
          semantic.o eval.o ir.o optimizer.o executor.o transaction.o

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

# ---- generated scanner / parser ------------------------------------
parser.tab.c parser.tab.h: parser.y
	$(BISON) -d -Wall -o parser.tab.c parser.y

lex.yy.c: lexer.l parser.tab.h
	$(FLEX) -o lex.yy.c lexer.l

lex.yy.o: lex.yy.c
	$(CC) $(CFLAGS) -Wno-sign-compare -c lex.yy.c

parser.tab.o: parser.tab.c
	$(CC) $(CFLAGS) -c parser.tab.c

# every C module depends on the generated token header
%.o: %.c parser.tab.h
	$(CC) $(CFLAGS) -c $<

# ---- convenience targets -------------------------------------------
run: $(TARGET)
	./$(TARGET)

demo: $(TARGET)
	./$(TARGET) tests/demo.sql || true

test: $(TARGET)
	@sh tests/run_tests.sh

reset-db:
	rm -f database/*.db

clean:
	rm -f $(TARGET) $(OBJS) parser.tab.c parser.tab.h lex.yy.c parser.output

.PHONY: all run demo test clean reset-db
