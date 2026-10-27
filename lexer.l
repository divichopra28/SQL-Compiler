/*
 * lexer.l - Flex specification for SQL-C
 *
 * Recognises SQL keywords (via a sorted keyword table + binary search),
 * identifiers, integer / float / string / date literals, operators and
 * punctuation. Every token carries its line and column (yylloc) so that
 * later phases can point at the exact source position.
 *
 * Lexical errors detected here:
 *   - invalid characters            ( @  #  $  ! ... )
 *   - unterminated string literals  ( 'abc )
 *   - unterminated comments         ( slash-star without star-slash )
 *   - malformed numbers             ( 12abc )
 *   - integer overflow, over-long identifiers, invalid dates
 */
%option noyywrap nounput noinput caseless

%{
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "ast.h"
#include "diag.h"
#include "lexer.h"
#include "parser.tab.h"

int g_lex_report = 1;

static int cur_line = 1, cur_col = 1;
static int last_tok = 0;
static char last_text[256];
static YY_BUFFER_STATE cur_buf = NULL;

static void update_loc(void) {
    yylloc.first_line = cur_line;
    yylloc.first_column = cur_col;
    for (int i = 0; i < yyleng; i++) {
        if (yytext[i] == '\n') { cur_line++; cur_col = 1; }
        else cur_col++;
    }
    yylloc.last_line = cur_line;
    yylloc.last_column = cur_col - 1;
}
#define YY_USER_ACTION update_loc();

static void remember(int tok) {
    last_tok = tok;
    snprintf(last_text, sizeof last_text, "%s", yytext);
}
#define RET(t) do { remember(t); return (t); } while (0)

static void lex_err(const char *fmt, const char *arg) {
    if (!g_lex_report) return;
    char msg[512];
    snprintf(msg, sizeof msg, fmt, arg);
    diag_error(ERR_LEXICAL, yylloc.first_line, yylloc.first_column, "%s", msg);
}

/* ---------------- keyword table ---------------- */
typedef struct { const char *word; int token; const char *cls; } Keyword;

#define KW(w, c) { #w, K_##w, c }
static Keyword keywords[] = {
    KW(ADD, "KEYWORD"), KW(ALL, "KEYWORD"), KW(ALTER, "KEYWORD"), KW(AND, "KEYWORD"),
    KW(AS, "KEYWORD"), KW(ASC, "KEYWORD"), KW(AVG, "AGGREGATE"), KW(BEGIN, "KEYWORD"),
    KW(BETWEEN, "KEYWORD"), KW(BIGINT, "DATA TYPE"), KW(BOOL, "DATA TYPE"),
    KW(BOOLEAN, "DATA TYPE"), KW(BY, "KEYWORD"), KW(CHAR, "DATA TYPE"), KW(CHECK, "KEYWORD"),
    KW(COLUMN, "KEYWORD"), KW(COMMIT, "KEYWORD"), KW(CONNECT, "KEYWORD"),
    KW(CONSTRAINT, "KEYWORD"), KW(COUNT, "AGGREGATE"), KW(CREATE, "KEYWORD"),
    KW(CROSS, "KEYWORD"), KW(DATABASE, "KEYWORD"), KW(DATABASES, "KEYWORD"),
    KW(DATE, "DATA TYPE"), KW(DATETIME, "DATA TYPE"), KW(DECIMAL, "DATA TYPE"),
    KW(DEFAULT, "KEYWORD"), KW(DELETE, "KEYWORD"), KW(DESC, "KEYWORD"),
    KW(DESCRIBE, "KEYWORD"), KW(DISTINCT, "KEYWORD"), KW(DOUBLE, "DATA TYPE"),
    KW(DROP, "KEYWORD"), KW(EXCEPT, "KEYWORD"), KW(EXISTS, "KEYWORD"), KW(EXPLAIN, "KEYWORD"),
    KW(FALSE, "BOOLEAN"), KW(FLOAT, "DATA TYPE"), KW(FOR, "KEYWORD"), KW(FOREIGN, "KEYWORD"),
    KW(FROM, "KEYWORD"), KW(FULL, "KEYWORD"), KW(GRANT, "KEYWORD"), KW(GRANTS, "KEYWORD"),
    KW(GROUP, "KEYWORD"), KW(HAVING, "KEYWORD"), KW(IF, "KEYWORD"), KW(IN, "KEYWORD"),
    KW(INDEX, "KEYWORD"), KW(INDEXES, "KEYWORD"), KW(INNER, "KEYWORD"), KW(INSERT, "KEYWORD"),
    KW(INT, "DATA TYPE"), KW(INTEGER, "DATA TYPE"), KW(INTERSECT, "KEYWORD"),
    KW(INTO, "KEYWORD"), KW(IS, "KEYWORD"), KW(JOIN, "KEYWORD"), KW(KEY, "KEYWORD"),
    KW(LEFT, "KEYWORD"), KW(LIKE, "KEYWORD"), KW(LIMIT, "KEYWORD"), KW(MAX, "AGGREGATE"),
    KW(MIN, "AGGREGATE"), KW(MODIFY, "KEYWORD"), KW(NOT, "KEYWORD"), KW(NULL, "NULL"),
    KW(NUMERIC, "DATA TYPE"), KW(OFFSET, "KEYWORD"), KW(ON, "KEYWORD"), KW(OR, "KEYWORD"),
    KW(ORDER, "KEYWORD"), KW(OUTER, "KEYWORD"), KW(PRIMARY, "KEYWORD"),
    KW(PRIVILEGES, "KEYWORD"), KW(REAL, "DATA TYPE"), KW(REFERENCES, "KEYWORD"),
    KW(RELEASE, "KEYWORD"), KW(RENAME, "KEYWORD"), KW(REVOKE, "KEYWORD"), KW(RIGHT, "KEYWORD"),
    KW(ROLLBACK, "KEYWORD"), KW(SAVEPOINT, "KEYWORD"), KW(SELECT, "KEYWORD"), KW(SET, "KEYWORD"),
    KW(SHOW, "KEYWORD"), KW(SMALLINT, "DATA TYPE"), KW(START, "KEYWORD"), KW(SUM, "AGGREGATE"),
    KW(TABLE, "KEYWORD"), KW(TABLES, "KEYWORD"), KW(TEXT, "DATA TYPE"), KW(TIME, "DATA TYPE"),
    KW(TO, "KEYWORD"), KW(TRANSACTION, "KEYWORD"), KW(TRUE, "BOOLEAN"), KW(TRUNCATE, "KEYWORD"),
    KW(UNION, "KEYWORD"), KW(UNIQUE, "KEYWORD"), KW(UPDATE, "KEYWORD"), KW(USE, "KEYWORD"),
    KW(USER, "KEYWORD"), KW(USERS, "KEYWORD"), KW(VALUES, "KEYWORD"), KW(VARCHAR, "DATA TYPE"),
    KW(VIEW, "KEYWORD"), KW(VIEWS, "KEYWORD"), KW(WHERE, "KEYWORD"), KW(WORK, "KEYWORD"),
};
#define NKEYWORDS ((int)(sizeof keywords / sizeof keywords[0]))
static int kw_sorted = 0;

static int kw_cmp(const void *a, const void *b) {
    return ci_cmp(((const Keyword *)a)->word, ((const Keyword *)b)->word);
}
static void kw_init(void) {
    if (!kw_sorted) { qsort(keywords, NKEYWORDS, sizeof(Keyword), kw_cmp); kw_sorted = 1; }
}
static const Keyword *kw_lookup(const char *s) {
    kw_init();
    Keyword key = { s, 0, NULL };
    return bsearch(&key, keywords, NKEYWORDS, sizeof(Keyword), kw_cmp);
}
static const Keyword *kw_by_token(int tok) {
    for (int i = 0; i < NKEYWORDS; i++) if (keywords[i].token == tok) return &keywords[i];
    return NULL;
}

static int valid_date(const char *s) {   /* s = YYYY-MM-DD */
    int y, m, d;
    if (sscanf(s, "%4d-%2d-%2d", &y, &m, &d) != 3) return 0;
    static const int mdays[] = {31,29,31,30,31,30,31,31,30,31,30,31};
    if (m < 1 || m > 12 || d < 1 || d > mdays[m - 1]) return 0;
    if (m == 2 && d == 29 && !((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 0;
    return 1;
}

static char *unquote(const char *s, int len) {   /* 'it''s' -> it's */
    char *out = xmalloc((size_t)len + 1);
    int j = 0;
    for (int i = 1; i < len - 1; i++) {
        out[j++] = s[i];
        if (s[i] == '\'' && s[i + 1] == '\'') i++;
    }
    out[j] = 0;
    return out;
}
%}

DIGIT     [0-9]
LETTER    [A-Za-z_]
IDCHAR    [A-Za-z0-9_]
EXP       [eE][+-]?{DIGIT}+

%x COMMENT

%%

[ \t\r\n]+                  { /* skip whitespace */ }
"--"[^\n]*                  { /* line comment */ }
"/*"                        { BEGIN(COMMENT); }
<COMMENT>"*/"               { BEGIN(INITIAL); }
<COMMENT>\n                 { }
<COMMENT>.                  { }
<COMMENT><<EOF>>            { lex_err("unterminated comment%s", ""); BEGIN(INITIAL); RET(LEX_ERROR); }

"DATE"[ \t]*'[0-9]{4}-[0-9]{2}-[0-9]{2}'  {
                                char *q = strchr(yytext, '\'');
                                char *v = xstrndup(q + 1, 10);
                                if (!valid_date(v)) { lex_err("invalid date literal %s", yytext); free(v); RET(LEX_ERROR); }
                                yylval.sval = v;
                                RET(DATE_LIT);
                            }

{DIGIT}+"."{DIGIT}*{EXP}?   { yylval.fval = strtod(yytext, NULL); RET(FLOATNUM); }
"."{DIGIT}+{EXP}?           { yylval.fval = strtod(yytext, NULL); RET(FLOATNUM); }
{DIGIT}+{EXP}               { yylval.fval = strtod(yytext, NULL); RET(FLOATNUM); }
{DIGIT}+                    {
                                errno = 0;
                                yylval.ival = strtoll(yytext, NULL, 10);
                                if (errno == ERANGE) { lex_err("integer literal '%s' is out of range", yytext); RET(LEX_ERROR); }
                                RET(INTNUM);
                            }
{DIGIT}+{LETTER}{IDCHAR}*   { lex_err("malformed number '%s' (identifiers cannot start with a digit)", yytext); RET(LEX_ERROR); }

'([^'\n]|'')*'              { yylval.sval = unquote(yytext, yyleng); RET(STRING); }
'([^'\n]|'')*               { lex_err("unterminated string literal %s", yytext); RET(LEX_ERROR); }

\"[^\"\n]+\"                { yylval.sval = xstrndup(yytext + 1, (size_t)yyleng - 2); RET(IDENT); }
`[^`\n]+`                   { yylval.sval = xstrndup(yytext + 1, (size_t)yyleng - 2); RET(IDENT); }

{LETTER}{IDCHAR}*           {
                                const Keyword *k = kw_lookup(yytext);
                                if (k) RET(k->token);
                                if (yyleng >= MAX_NAME) { lex_err("identifier '%s' is too long (max 63 characters)", yytext); RET(LEX_ERROR); }
                                yylval.sval = xstrdup(yytext);
                                RET(IDENT);
                            }

"<>"                        { RET(T_NE); }
"!="                        { RET(T_NE); }
"<="                        { RET(T_LE); }
">="                        { RET(T_GE); }
"="                         { RET('='); }
"<"                         { RET('<'); }
">"                         { RET('>'); }
"+"                         { RET('+'); }
"-"                         { RET('-'); }
"*"                         { RET('*'); }
"/"                         { RET('/'); }
"%"                         { RET('%'); }
"("                         { RET('('); }
")"                         { RET(')'); }
","                         { RET(','); }
";"                         { RET(';'); }
"."                         { RET('.'); }

.                           {
                                char c[8];
                                snprintf(c, sizeof c, "%s", yytext);
                                if (yytext[0] == '"') lex_err("unterminated quoted identifier starting with %s", c);
                                else lex_err("invalid character '%s'", c);
                                RET(LEX_ERROR);
                            }

%%

void lex_begin(const char *text, int base_line) {
    if (cur_buf) { yy_delete_buffer(cur_buf); cur_buf = NULL; }
    BEGIN(INITIAL);
    cur_buf = yy_scan_string(text);
    cur_line = base_line;
    cur_col = 1;
    last_tok = 0;
    last_text[0] = 0;
    yylloc.first_line = yylloc.last_line = base_line;
    yylloc.first_column = yylloc.last_column = 1;
}

void lex_end(void) {
    if (cur_buf) { yy_delete_buffer(cur_buf); cur_buf = NULL; }
}

int lex_last_token(void) { return last_tok; }
const char *lex_last_text(void) { return last_text; }

const char *lex_token_class(int tok) {
    const Keyword *k = kw_by_token(tok);
    if (k) return k->cls;
    switch (tok) {
    case IDENT: return "IDENTIFIER";
    case INTNUM: return "INTEGER";
    case FLOATNUM: return "FLOAT";
    case STRING: return "STRING";
    case DATE_LIT: return "DATE";
    case T_NE: case T_LE: case T_GE: case '=': case '<': case '>':
        return "REL OPERATOR";
    case '+': case '-': case '*': case '/': case '%':
        return "ARITH OPERATOR";
    case '(': case ')': case ',': case ';': case '.':
        return "PUNCTUATION";
    case LEX_ERROR: return "ERROR";
    case 0: return "END";
    }
    return "TOKEN";
}

const char *lex_token_name(int tok, char *buf, size_t n) {
    const Keyword *k = kw_by_token(tok);
    if (k) { snprintf(buf, n, "K_%s", k->word); return buf; }
    const char *s = "?";
    switch (tok) {
    case IDENT: s = "IDENT"; break;
    case INTNUM: s = "INTNUM"; break;
    case FLOATNUM: s = "FLOATNUM"; break;
    case STRING: s = "STRING"; break;
    case DATE_LIT: s = "DATE_LIT"; break;
    case T_NE: s = "NE"; break;
    case T_LE: s = "LE"; break;
    case T_GE: s = "GE"; break;
    case '=': s = "EQ"; break;
    case '<': s = "LT"; break;
    case '>': s = "GT"; break;
    case '+': s = "PLUS"; break;
    case '-': s = "MINUS"; break;
    case '*': s = "STAR"; break;
    case '/': s = "SLASH"; break;
    case '%': s = "PERCENT"; break;
    case '(': s = "LPAREN"; break;
    case ')': s = "RPAREN"; break;
    case ',': s = "COMMA"; break;
    case ';': s = "SEMICOLON"; break;
    case '.': s = "DOT"; break;
    case LEX_ERROR: s = "LEX_ERROR"; break;
    }
    snprintf(buf, n, "%s", s);
    return buf;
}

int lex_keyword_count(void) { kw_init(); return NKEYWORDS; }
const char *lex_keyword_name(int i) { kw_init(); return keywords[i].word; }
const char *lex_keyword_class(int i) { kw_init(); return keywords[i].cls; }
