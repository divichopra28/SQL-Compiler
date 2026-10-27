/*
 * diag.h - diagnostics (LEXICAL / SYNTAX / SEMANTIC / EXECUTION errors)
 *
 * Errors are printed with the offending source line and a caret under
 * the column that caused them.
 */
#ifndef DIAG_H
#define DIAG_H

typedef enum { ERR_LEXICAL, ERR_SYNTAX, ERR_SEMANTIC, ERR_EXECUTION } ErrKind;

void diag_set_source(const char *text, int base_line);  /* statement text */
void diag_reset(void);
void diag_error(ErrKind k, int line, int col, const char *fmt, ...);
void diag_warning(int line, int col, const char *fmt, ...);
int  diag_count(ErrKind k);
int  diag_total(void);
int  diag_warnings(void);
const char *diag_kind_name(ErrKind k);

#endif
