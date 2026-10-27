/*
 * lexer.h - interface of the Flex scanner
 */
#ifndef LEXER_H
#define LEXER_H

#include <stddef.h>

extern int g_lex_report;      /* 1 = report lexical errors through diag */
extern int g_parse_report;    /* 1 = report syntax errors (parser.y)     */

void lex_begin(const char *text, int base_line);   /* scan this string   */
void lex_end(void);
int  lex_last_token(void);
const char *lex_last_text(void);

const char *lex_token_class(int tok);   /* KEYWORD, IDENTIFIER, OPERATOR ... */
const char *lex_token_name(int tok, char *buf, size_t n);  /* K_SELECT, IDENT ... */

int  lex_keyword_count(void);
const char *lex_keyword_name(int i);
const char *lex_keyword_class(int i);

struct Node;
struct Node *parse_string(const char *sql, int report);   /* defined in parser.y */
int yyparse(void);

#endif
