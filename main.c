/*
 * main.c - SQL-C command-line interface
 *
 * Reads SQL statements (interactively or from files) and runs each one
 * through the compiler pipeline, printing every phase:
 *
 *   Lexical Analysis -> Syntax Analysis -> AST -> Semantic Analysis ->
 *   Symbol Table -> Intermediate Code -> Optimization -> Execution
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include "ast.h"
#include "diag.h"
#include "lexer.h"
#include "parser.tab.h"
#include "symbol_table.h"
#include "semantic.h"
#include "ir.h"
#include "optimizer.h"
#include "executor.h"
#include "transaction.h"

extern char *yytext;
extern Node *g_parse_result;
int yylex(void);

/* which phases are displayed */
enum { PH_LEX = 1, PH_SYNTAX = 2, PH_AST = 4, PH_SEM = 8, PH_SYM = 16, PH_IR = 32, PH_OPT = 64 };
static int g_phases = PH_LEX | PH_SYNTAX | PH_AST | PH_SEM | PH_SYM | PH_IR | PH_OPT;
static int g_echo = 1;
static int g_stats_ok = 0, g_stats_err = 0;

#define VERSION "1.0"

/* ======================================================================
 *  Output helpers
 * ====================================================================== */
static void section(const char *title) {
    printf("\n%s========== %s ==========%s\n\n", COL_BLUE, title, COL_RESET);
}
static void subsection(const char *title) {
    printf("%s===== %s =====%s\n", COL_CYAN, title, COL_RESET);
}

static void print_banner(void) {
    printf("%s", COL_BOLD);
    printf("=========================================================\n");
    printf("                      SQL COMPILER\n");
    printf("        SQL-C: A Lex and Yacc Based SQL Compiler\n");
    printf("=========================================================%s\n", COL_RESET);
    printf("Version %s  |  Flex + Bison + C  |  storage engine: SQLite %s\n", VERSION, sqlite3_libversion());
    printf("Database: %s%s%s   User: %s%s%s\n", COL_BOLD, g_dbname, COL_RESET, COL_BOLD, g_current_user, COL_RESET);
    printf("Type \\help for commands, \\q to quit. End every statement with ';'\n");
}

static void print_help(void) {
    printf("\n%sSQL statements%s (end with ';')\n", COL_BOLD, COL_RESET);
    printf("  DDL  CREATE DATABASE|TABLE|VIEW|INDEX, ALTER TABLE, DROP TABLE|VIEW|INDEX|DATABASE, TRUNCATE\n");
    printf("  DML  INSERT, UPDATE, DELETE\n");
    printf("  DQL  SELECT (WHERE, DISTINCT, JOINs, GROUP BY, HAVING, ORDER BY, LIMIT,\n");
    printf("       subqueries, UNION [ALL] / INTERSECT / EXCEPT, aggregates)\n");
    printf("  DCL  GRANT, REVOKE, CREATE USER, DROP USER, CONNECT <user>\n");
    printf("  TCL  START TRANSACTION, COMMIT, ROLLBACK, SAVEPOINT, ROLLBACK TO SAVEPOINT, RELEASE\n");
    printf("  Utility  USE <db>, SHOW TABLES|VIEWS|INDEXES|DATABASES|USERS|GRANTS [FOR u]|TRANSACTION,\n");
    printf("           DESCRIBE <table>, EXPLAIN <statement>\n");
    printf("\n%sMeta commands%s\n", COL_BOLD, COL_RESET);
    printf("  \\help            this help\n");
    printf("  \\q               quit\n");
    printf("  \\i <file>        run the SQL statements in a file\n");
    printf("  \\full            show every compiler phase (default)\n");
    printf("  \\compact         show only results and errors\n");
    printf("  \\phase <p> on|off  toggle one phase: tokens syntax ast semantic symtab ir optimize\n");
    printf("  \\symtab          dump the whole symbol table (catalog)\n");
    printf("  \\keywords        list the lexer's keyword table\n");
    printf("  \\txn             show the transaction manager state\n");
    printf("  \\ascii on|off    draw trees with ASCII characters\n");
    printf("  \\color on|off    coloured output\n\n");
}

static void print_keywords(void) {
    int n = lex_keyword_count();
    printf("Keyword table (%d entries, sorted, looked up with binary search):\n", n);
    for (int i = 0; i < n; i++) {
        printf("  %-12s %-10s", lex_keyword_name(i), lex_keyword_class(i));
        if (i % 3 == 2) printf("\n");
    }
    printf("\n");
}

/* ======================================================================
 *  The compiler pipeline for one statement
 * ====================================================================== */
static void echo_statement(const char *sql) {
    const char *p = sql;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    printf("\n%sSQL>%s ", COL_BOLD, COL_RESET);
    int first = 1;
    while (*p) {
        const char *e = strchr(p, '\n');
        int len = e ? (int)(e - p) : (int)strlen(p);
        if (!first) printf("     ");
        printf("%.*s\n", len, p);
        first = 0;
        if (!e) break;
        p = e + 1;
        while (*p == '\n' && !p[1]) p++;
    }
}

static int lexical_phase(const char *sql, int line) {
    int show = g_phases & PH_LEX;
    if (show) {
        section("LEXICAL ANALYSIS");
        printf("  %-4s %-20s %-14s %-14s %s\n", "#", "LEXEME", "CATEGORY", "TOKEN", "LINE:COL");
        printf("  %-4s %-20s %-14s %-14s %s\n", "----", "--------------------", "--------------", "--------------", "--------");
    }
    g_lex_report = 1;
    lex_begin(sql, line);
    int tok, n = 0;
    char buf[64];
    while ((tok = yylex()) != 0) {
        n++;
        if (show && tok != LEX_ERROR) {
            char lex[64];
            snprintf(lex, sizeof lex, "%s", yytext);
            if (strlen(yytext) > 20) { lex[17] = '.'; lex[18] = '.'; lex[19] = '.'; lex[20] = 0; }
            printf("  %-4d %-20s %-14s %-14s %d:%d\n", n, lex, lex_token_class(tok), lex_token_name(tok, buf, sizeof buf),
                   yylloc.first_line, yylloc.first_column);
        }
    }
    lex_end();
    int errs = diag_count(ERR_LEXICAL);
    if (errs) {
        printf("\n%sLexical Analysis Failed%s - %d lexical error%s\n", COL_RED, COL_RESET, errs, errs == 1 ? "" : "s");
        return -1;
    }
    if (n == 0) return 1;                     /* nothing but comments */
    if (show) printf("\nLexical Analysis Successful - %d token%s\n", n, n == 1 ? "" : "s");
    return 0;
}

static void run_statement(const char *sql, int line) {
    diag_reset();
    diag_set_source(sql, line);
    int full = g_phases != 0;
    if (g_echo) echo_statement(sql);

    /* 1. lexical analysis */
    int lr = lexical_phase(sql, line);
    if (lr == 1) return;
    if (lr < 0) { g_stats_err++; return; }

    /* 2. syntax analysis */
    if (g_phases & PH_SYNTAX) section("SYNTAX ANALYSIS");
    g_lex_report = 0;
    g_parse_result = NULL;
    lex_begin(sql, line);
    int rc = yyparse();
    lex_end();
    g_lex_report = 1;
    Node *ast = g_parse_result;
    if (rc != 0 || !ast) {
        printf("\n%sSyntax Analysis Failed%s\n", COL_RED, COL_RESET);
        g_stats_err++;
        return;
    }
    if (g_phases & PH_SYNTAX) {
        Node *inner = ast->type == N_EXPLAIN ? ast->kids[0] : ast;
        printf("Syntax Analysis Successful\n");
        printf("Statement: %s  (category: %s)\n", stmt_name(inner), stmt_category(inner));
    }

    /* 3. abstract syntax tree */
    if (g_phases & PH_AST) {
        section("ABSTRACT SYNTAX TREE");
        ast_print(ast);
    }

    /* 4. semantic analysis */
    if (g_phases & PH_SEM) section("SEMANTIC ANALYSIS");
    g_sem_verbose = (g_phases & PH_SEM) != 0;
    symlog_reset();
    g_symlog = 1;
    int serr = sem_analyze(ast);
    g_symlog = 0;
    if (serr) {
        printf("\n%sSemantic Analysis Failed%s - %d semantic error%s\n", COL_RED, COL_RESET, serr, serr == 1 ? "" : "s");
        g_stats_err++;
        return;
    }
    if (g_phases & PH_SEM) {
        int w = diag_warnings();
        printf("\nSemantic Analysis Successful%s", w ? "" : "\n");
        if (w) printf(" (%d warning%s)\n", w, w == 1 ? "" : "s");
    }

    /* 5. symbol table */
    if (g_phases & PH_SYM) {
        section("SYMBOL TABLE");
        if (!g_symlog_lines.n) printf("  (no symbol table lookups needed)\n");
        for (int i = 0; i < g_symlog_lines.n; i++) printf("  %s\n", (char *)g_symlog_lines.items[i]);
    }

    /* 6. intermediate code */
    Plan *plan = ir_generate(ast);
    if (g_phases & PH_IR) {
        section("INTERMEDIATE CODE");
        ir_print(plan);
    }

    /* 7. optimization */
    if (g_phases & PH_OPT) {
        section("OPTIMIZATION");
        if (plan->root->op == IR_COMMAND) {
            printf("No optimization applies to %s statements (single catalog/transaction operation).\n", stmt_category(plan->root->ast));
            g_opt_verbose = 0;
            optimize_plan(plan);
        } else {
            subsection("BEFORE OPTIMIZATION");
            ir_print(plan);
            printf("\n");
            subsection("OPTIMIZATION PASSES");
            g_opt_verbose = 1;
            int n = optimize_plan(plan);
            if (!n) printf("  (no rewrite applicable - the plan is already optimal)\n");
            printf("\n");
            subsection("AFTER OPTIMIZATION");
            ir_print(plan);
            printf("\nOptimization completed (%d rewrite%s).\n", n, n == 1 ? "" : "s");
        }
    } else {
        g_opt_verbose = 0;
        optimize_plan(plan);
    }

    /* 8. execution */
    if (plan->explain) {
        if (full) section("EXECUTION");
        printf("EXPLAIN: the statement was compiled but not executed.\n");
        if (!(g_phases & PH_IR)) { printf("Optimized plan:\n"); ir_print(plan); }
        g_stats_ok++;
        return;
    }
    if (full) section("EXECUTION");
    symlog_reset();
    g_symlog = 1;
    int er = execute_plan(plan);
    g_symlog = 0;
    if (er) { g_stats_err++; return; }
    g_stats_ok++;
    if ((g_phases & PH_SYM) && g_symlog_lines.n) {
        int changed = 0;
        for (int i = 0; i < g_symlog_lines.n; i++) {
            const char *l = g_symlog_lines.items[i];
            if (strncmp(l, "lookup", 6) != 0) {
                if (!changed++) printf("\n%sSymbol table updated:%s\n", COL_CYAN, COL_RESET);
                printf("  %s\n", l);
            }
        }
        if (changed && (ast->type == N_CREATE_TABLE || ast->type == N_ALTER_TABLE)) {
            const char *name = ast->type == N_ALTER_TABLE && ast->kids[0]->op == A_RENAME_TABLE ? ast->kids[0]->s2 : ast->s1;
            TableSym *t = ht_get(g_cat.tables, name);
            if (t) { printf("\n"); symtab_print_table(t); }
        }
    }
}

/* ======================================================================
 *  Splitting input into statements
 * ====================================================================== */
/* returns length of the next statement (up to and including ';'), or -1 if incomplete */
static int stmt_length(const char *s) {
    int in_sq = 0, in_dq = 0, in_bq = 0, in_lc = 0, in_bc = 0;
    for (int i = 0; s[i]; i++) {
        char c = s[i], n = s[i + 1];
        if (in_lc) { if (c == '\n') in_lc = 0; continue; }
        if (in_bc) { if (c == '*' && n == '/') { in_bc = 0; i++; } continue; }
        if (in_sq) { if (c == '\'') { if (n == '\'') i++; else in_sq = 0; } else if (c == '\n') in_sq = 0; continue; }
        if (in_dq) { if (c == '"' || c == '\n') in_dq = 0; continue; }
        if (in_bq) { if (c == '`' || c == '\n') in_bq = 0; continue; }
        if (c == '-' && n == '-') { in_lc = 1; i++; continue; }
        if (c == '/' && n == '*') { in_bc = 1; i++; continue; }
        if (c == '\'') in_sq = 1;
        else if (c == '"') in_dq = 1;
        else if (c == '`') in_bq = 1;
        else if (c == ';') return i + 1;
    }
    return -1;
}

/* skip whitespace and comments; counts newlines */
static const char *skip_blank(const char *p, int *line) {
    for (;;) {
        while (*p && isspace((unsigned char)*p)) { if (*p == '\n') (*line)++; p++; }
        if (p[0] == '-' && p[1] == '-') { while (*p && *p != '\n') p++; continue; }
        if (p[0] == '/' && p[1] == '*') {
            const char *e = strstr(p + 2, "*/");
            if (!e) return p;          /* unterminated: let the lexer report it */
            for (const char *q = p; q < e; q++) if (*q == '\n') (*line)++;
            p = e + 2;
            continue;
        }
        return p;
    }
}

static void meta_command(char *line);

/* run every complete statement in `text`; returns the unconsumed remainder */
static char *run_text(char *text, int *line, int final) {
    char *p = text;
    for (;;) {
        const char *q = skip_blank(p, line);
        p = (char *)q;
        if (!*p) return p;
        if (*p == '\\') {                     /* meta command inside a script */
            char *e = strchr(p, '\n');
            if (!e && !final) return p;
            char *cmd = e ? xstrndup(p, (size_t)(e - p)) : xstrdup(p);
            meta_command(cmd);
            free(cmd);
            p = e ? e : p + strlen(p);
            continue;
        }
        int len = stmt_length(p);
        if (len < 0) {
            if (!final) return p;
            len = (int)strlen(p);
        }
        char *stmt = xstrndup(p, (size_t)len);
        int start_line = *line;
        for (int i = 0; i < len; i++) if (p[i] == '\n') (*line)++;
        int only_semi = 1;
        for (int i = 0; i < len; i++) if (!isspace((unsigned char)stmt[i]) && stmt[i] != ';') only_semi = 0;
        if (!only_semi) run_statement(stmt, start_line);
        free(stmt);
        p += len;
    }
}

static int run_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { printf("%scannot open file '%s'%s\n", COL_RED, path, COL_RESET); return -1; }
    SB b; sb_init(&b);
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf - 1, f)) > 0) { buf[n] = 0; sb_add(&b, "%s", buf); }
    fclose(f);
    char *text = sb_take(&b);
    int line = 1;
    run_text(text, &line, 1);
    free(text);
    return 0;
}

/* ======================================================================
 *  Meta commands
 * ====================================================================== */
static int onoff(const char *s) { return s && (!strcmp(s, "on") || !strcmp(s, "1") || !strcmp(s, "true")); }

static void meta_command(char *line) {
    char *cmd = strtok(line, " \t\r\n");
    char *arg = strtok(NULL, " \t\r\n");
    char *arg2 = strtok(NULL, " \t\r\n");
    if (!cmd) return;
    if (!strcmp(cmd, "\\q") || !strcmp(cmd, "\\quit") || !strcmp(cmd, "\\exit")) {
        executor_close();
        printf("Bye.\n");
        exit(0);
    } else if (!strcmp(cmd, "\\help") || !strcmp(cmd, "\\h") || !strcmp(cmd, "\\?")) print_help();
    else if (!strcmp(cmd, "\\i") || !strcmp(cmd, "\\include")) {
        if (!arg) printf("usage: \\i <file.sql>\n");
        else run_file(arg);
    } else if (!strcmp(cmd, "\\full")) { g_phases = PH_LEX | PH_SYNTAX | PH_AST | PH_SEM | PH_SYM | PH_IR | PH_OPT; g_echo = 1; printf("Showing every compiler phase.\n"); }
    else if (!strcmp(cmd, "\\compact")) { g_phases = 0; printf("Compact mode: only results and errors.\n"); }
    else if (!strcmp(cmd, "\\phase")) {
        struct { const char *n; int f; } ph[] = { {"tokens", PH_LEX}, {"lexical", PH_LEX}, {"syntax", PH_SYNTAX}, {"ast", PH_AST},
            {"semantic", PH_SEM}, {"symtab", PH_SYM}, {"ir", PH_IR}, {"optimize", PH_OPT}, {"opt", PH_OPT} };
        int done = 0;
        for (size_t i = 0; arg && i < sizeof ph / sizeof ph[0]; i++)
            if (!strcmp(arg, ph[i].n)) {
                if (onoff(arg2)) g_phases |= ph[i].f; else g_phases &= ~ph[i].f;
                printf("Phase '%s' %s.\n", arg, onoff(arg2) ? "shown" : "hidden");
                done = 1;
            }
        if (!done) printf("usage: \\phase tokens|syntax|ast|semantic|symtab|ir|optimize on|off\n");
    } else if (!strcmp(cmd, "\\symtab")) symtab_dump();
    else if (!strcmp(cmd, "\\keywords")) print_keywords();
    else if (!strcmp(cmd, "\\txn")) txn_print_status();
    else if (!strcmp(cmd, "\\ascii")) { g_ascii = onoff(arg); printf("ASCII trees %s.\n", g_ascii ? "on" : "off"); }
    else if (!strcmp(cmd, "\\color")) { g_color = onoff(arg); printf("Colours %s.\n", g_color ? "on" : "off"); }
    else if (!strcmp(cmd, "\\echo")) { g_echo = onoff(arg); }
    else printf("unknown command %s (try \\help)\n", cmd);
}

/* ======================================================================
 *  Interactive loop
 * ====================================================================== */
static void repl(void) {
    SB buf; sb_init(&buf);
    char line[8192];
    int lineno = 1;
    int save_echo = g_echo;
    g_echo = 0;                               /* the user just typed it */
    for (;;) {
        printf("%s", buf.len ? "  -> " : "SQL> ");
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin)) { printf("\n"); break; }
        if (!buf.len) {
            char *t = line;
            while (isspace((unsigned char)*t)) t++;
            if (*t == '\\') { meta_command(t); continue; }
            if (!strncasecmp(t, "exit", 4) || !strncasecmp(t, "quit", 4)) {
                char c = t[4];
                if (!c || isspace((unsigned char)c) || c == ';') break;
            }
            if (!*t) continue;
        }
        sb_add(&buf, "%s", line);
        char *text = sb_take(&buf);
        int ln = lineno;
        char *rest = run_text(text, &ln, 0);
        lineno = 1;
        sb_add(&buf, "%s", rest);
        /* keep only non-blank remainder */
        int blank = 1;
        for (char *q = buf.s; q && *q; q++) if (!isspace((unsigned char)*q)) blank = 0;
        if (blank) { free(sb_take(&buf)); }
        free(text);
    }
    g_echo = save_echo;
}

static void usage(const char *prog) {
    printf("usage: %s [options] [file.sql ...]\n\n", prog);
    printf("  -c, --compact     show only results and errors (no compiler phases)\n");
    printf("  -d <dir>          directory that holds the database files (default: database)\n");
    printf("  -D <name>         database to open (default: sqlcompiler)\n");
    printf("  -i                interactive prompt (also after running files, or with piped input)\n");
    printf("  --ascii           draw trees with ASCII characters\n");
    printf("  --no-color        disable colours\n");
    printf("  -h, --help        this help\n\n");
    printf("Without files the compiler starts an interactive SQL> prompt.\n");
}

int main(int argc, char **argv) {
    const char *dir = "database", *db = "sqlcompiler";
    PtrVec files = {0};
    int interactive_after = 0;
    g_color = isatty(STDOUT_FILENO);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c") || !strcmp(argv[i], "--compact") || !strcmp(argv[i], "-q")) g_phases = 0;
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) dir = argv[++i];
        else if (!strcmp(argv[i], "-D") && i + 1 < argc) db = argv[++i];
        else if (!strcmp(argv[i], "-i")) interactive_after = 1;
        else if (!strcmp(argv[i], "--ascii")) g_ascii = 1;
        else if (!strcmp(argv[i], "--no-color")) g_color = 0;
        else if (!strcmp(argv[i], "--color")) g_color = 1;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(argv[0]); return 0; }
        else if (argv[i][0] == '-') { usage(argv[0]); return 1; }
        else pv_push(&files, argv[i]);
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (executor_open(dir, db)) return 1;

    if (files.n) {
        for (int i = 0; i < files.n; i++) run_file(files.items[i]);
        if (!interactive_after) {
            if (g_phases) printf("\n%s%d statement(s) succeeded, %d failed.%s\n", COL_BOLD, g_stats_ok, g_stats_err, COL_RESET);
            executor_close();
            return g_stats_err ? 2 : 0;
        }
    } else if (!isatty(STDIN_FILENO) && !interactive_after) {
        SB b; sb_init(&b);
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf - 1, stdin)) > 0) { buf[n] = 0; sb_add(&b, "%s", buf); }
        char *text = sb_take(&b);
        int line = 1;
        run_text(text, &line, 1);
        free(text);
        executor_close();
        return g_stats_err ? 2 : 0;
    }
    print_banner();
    repl();
    executor_close();
    printf("Bye.\n");
    return 0;
}
