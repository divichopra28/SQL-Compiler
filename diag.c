/*
 * diag.c - error reporting with source excerpts
 */
#include "diag.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *src_text = NULL;
static int src_base = 1;
static int counts[4];
static int warns;

void diag_set_source(const char *text, int base_line) { src_text = text; src_base = base_line; }
void diag_reset(void) { memset(counts, 0, sizeof counts); warns = 0; }
int  diag_count(ErrKind k) { return counts[k]; }
int  diag_total(void) { return counts[0] + counts[1] + counts[2] + counts[3]; }
int  diag_warnings(void) { return warns; }

const char *diag_kind_name(ErrKind k) {
    switch (k) {
    case ERR_LEXICAL: return "LEXICAL ERROR";
    case ERR_SYNTAX: return "SYNTAX ERROR";
    case ERR_SEMANTIC: return "SEMANTIC ERROR";
    default: return "EXECUTION ERROR";
    }
}

/* print source line `line` (absolute) with a caret at `col` */
static void show_excerpt(int line, int col) {
    if (!src_text || line <= 0) return;
    int rel = line - src_base;          /* 0-based line inside statement */
    const char *p = src_text;
    for (int i = 0; i < rel && p; i++) {
        p = strchr(p, '\n');
        if (p) p++;
    }
    if (!p || !*p) return;
    const char *e = strchr(p, '\n');
    int len = e ? (int)(e - p) : (int)strlen(p);
    printf("    %s%4d |%s ", COL_DIM, line, COL_RESET);
    for (int i = 0; i < len; i++) putchar(p[i] == '\t' ? ' ' : p[i]);
    putchar('\n');
    if (col > 0) {
        printf("         %s|%s ", COL_DIM, COL_RESET);
        for (int i = 1; i < col && i <= len; i++) putchar(' ');
        printf("%s^%s\n", COL_RED, COL_RESET);
    }
}

void diag_error(ErrKind k, int line, int col, const char *fmt, ...) {
    counts[k]++;
    va_list ap;
    va_start(ap, fmt);
    char msg[2048];
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    printf("%s[%s]%s", COL_RED, diag_kind_name(k), COL_RESET);
    if (line > 0 && col > 0) printf(" line %d, column %d: ", line, col);
    else if (line > 0) printf(" line %d: ", line);
    else printf(" ");
    printf("%s\n", msg);
    show_excerpt(line, col);
}

void diag_warning(int line, int col, const char *fmt, ...) {
    warns++;
    va_list ap;
    va_start(ap, fmt);
    char msg[2048];
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    printf("%s[WARNING]%s", COL_YELLOW, COL_RESET);
    if (line > 0 && col > 0) printf(" line %d, column %d:", line, col);
    printf(" %s\n", msg);
}
