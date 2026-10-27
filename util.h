/*
 * util.h - small helpers shared by every compiler phase
 * (memory, strings, growable buffers, terminal colours).
 */
#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>
#include <stdarg.h>

extern int g_color;   /* ANSI colours on/off                     */
extern int g_ascii;   /* draw trees with ASCII instead of UTF-8  */

#define CLR(c)      (g_color ? (c) : "")
#define COL_RESET   CLR("\033[0m")
#define COL_BOLD    CLR("\033[1m")
#define COL_DIM     CLR("\033[2m")
#define COL_RED     CLR("\033[1;31m")
#define COL_GREEN   CLR("\033[1;32m")
#define COL_YELLOW  CLR("\033[1;33m")
#define COL_BLUE    CLR("\033[1;34m")
#define COL_MAGENTA CLR("\033[1;35m")
#define COL_CYAN    CLR("\033[1;36m")

#define MAX_NAME 64

void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t sz);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
char *xsprintf(const char *fmt, ...);

int  ci_eq(const char *a, const char *b);      /* case-insensitive equality */
int  ci_cmp(const char *a, const char *b);
char *str_upper_dup(const char *s);

/* growable string buffer */
typedef struct { char *s; size_t len, cap; } SB;
void  sb_init(SB *b);
void  sb_add(SB *b, const char *fmt, ...);
void  sb_addc(SB *b, char c);
char *sb_take(SB *b);          /* returns malloc'd string, resets buffer */

/* growable pointer vector */
typedef struct { void **items; int n, cap; } PtrVec;
void pv_push(PtrVec *v, void *p);
int  pv_contains_str(PtrVec *v, const char *s);

/* format a double the SQL way: 8.5, 9.0, 0.333333 */
void fmt_double(double d, char *buf, size_t n);

/* tree drawing glyphs */
const char *tree_branch(int last);   /* "├── " or "└── " */
const char *tree_indent(int last);   /* "│   " or "    " */

#endif
