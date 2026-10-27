/*
 * util.c - helper routines
 */
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

int g_color = 1;
int g_ascii = 0;

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) { fprintf(stderr, "fatal: out of memory\n"); exit(2); }
    return p;
}
void *xcalloc(size_t n, size_t sz) {
    void *p = calloc(n ? n : 1, sz ? sz : 1);
    if (!p) { fprintf(stderr, "fatal: out of memory\n"); exit(2); }
    return p;
}
void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) { fprintf(stderr, "fatal: out of memory\n"); exit(2); }
    return q;
}
char *xstrdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *d = xmalloc(n + 1);
    memcpy(d, s, n + 1);
    return d;
}
char *xstrndup(const char *s, size_t n) {
    char *d = xmalloc(n + 1);
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}
char *xsprintf(const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    char *s = xmalloc((size_t)n + 1);
    vsnprintf(s, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    return s;
}

int ci_cmp(const char *a, const char *b) {
    if (!a || !b) return (a ? 1 : 0) - (b ? 1 : 0);
    while (*a && *b) {
        int d = tolower((unsigned char)*a) - tolower((unsigned char)*b);
        if (d) return d;
        a++; b++;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}
int ci_eq(const char *a, const char *b) {
    if (!a || !b) return a == b;
    return ci_cmp(a, b) == 0;
}
char *str_upper_dup(const char *s) {
    char *d = xstrdup(s);
    for (char *p = d; *p; p++) *p = (char)toupper((unsigned char)*p);
    return d;
}

void sb_init(SB *b) { b->s = NULL; b->len = b->cap = 0; }
static void sb_reserve(SB *b, size_t extra) {
    if (b->len + extra + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 64;
        while (nc < b->len + extra + 1) nc *= 2;
        b->s = xrealloc(b->s, nc);
        b->cap = nc;
    }
}
void sb_add(SB *b, const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    sb_reserve(b, (size_t)n);
    vsnprintf(b->s + b->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    b->len += (size_t)n;
}
void sb_addc(SB *b, char c) {
    sb_reserve(b, 1);
    b->s[b->len++] = c;
    b->s[b->len] = 0;
}
char *sb_take(SB *b) {
    char *s = b->s ? b->s : xstrdup("");
    b->s = NULL; b->len = b->cap = 0;
    return s;
}

void pv_push(PtrVec *v, void *p) {
    if (v->n == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 8;
        v->items = xrealloc(v->items, sizeof(void *) * (size_t)v->cap);
    }
    v->items[v->n++] = p;
}
int pv_contains_str(PtrVec *v, const char *s) {
    for (int i = 0; i < v->n; i++)
        if (strcmp((char *)v->items[i], s) == 0) return 1;
    return 0;
}

void fmt_double(double d, char *buf, size_t n) {
    snprintf(buf, n, "%.10f", d);
    /* trim trailing zeros but keep one digit after the point */
    char *dot = strchr(buf, '.');
    if (dot) {
        char *end = buf + strlen(buf) - 1;
        while (end > dot + 1 && *end == '0') *end-- = 0;
    }
    if (strcmp(buf, "-0.0") == 0) snprintf(buf, n, "0.0");
}

const char *tree_branch(int last) {
    if (g_ascii) return last ? "`-- " : "|-- ";
    return last ? "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x80 " : "\xe2\x94\x9c\xe2\x94\x80\xe2\x94\x80 ";
}
const char *tree_indent(int last) {
    if (g_ascii) return last ? "    " : "|   ";
    return last ? "    " : "\xe2\x94\x82   ";
}
