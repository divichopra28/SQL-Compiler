# SQL-C: A Lex and Yacc Based SQL Compiler

A complete SQL compiler written in **C** with **Flex** (lexer) and **Bison** (parser), built as a
Compiler Design project. It is a terminal application: every SQL statement you type goes through
all the classic compiler phases, and each phase is printed so you can see exactly what the
compiler did.

```
Lexical Analysis  ->  Syntax Analysis  ->  Abstract Syntax Tree  ->  Semantic Analysis
      ->  Symbol Table  ->  Intermediate Code  ->  Code Optimization  ->  Execution
```

SQLite is used **only as the storage engine** (it stores the rows on disk). It is never used to
parse or run your SQL: parsing is done by our Bison grammar, checking by our semantic analyzer,
and queries are executed by our own interpreter of the optimized intermediate code.

---

## 1. Features at a glance

| Category | Statements |
|---|---|
| **DDL** | `CREATE DATABASE`, `CREATE TABLE` (PRIMARY KEY, FOREIGN KEY, NOT NULL, UNIQUE, DEFAULT, CHECK), `CREATE VIEW`, `CREATE [UNIQUE] INDEX`, `ALTER TABLE ... ADD / DROP COLUMN / MODIFY / RENAME COLUMN / RENAME TO`, `DROP TABLE / VIEW / INDEX / DATABASE [IF EXISTS]`, `TRUNCATE TABLE` |
| **DML** | `INSERT` (full row, column list, multiple rows, `INSERT ... SELECT`), `UPDATE` (several columns, expressions, subqueries), `DELETE` |
| **DQL** | `SELECT` with `WHERE` (`= != <> < > <= >=`, `AND OR NOT`, `BETWEEN`, `IN`, `LIKE` with `%` `_`, `IS [NOT] NULL`), `DISTINCT`, `ORDER BY` (multi-column, ASC/DESC, alias or position), `GROUP BY`, `HAVING`, `COUNT SUM AVG MIN MAX` (incl. `DISTINCT`), `LIMIT/OFFSET`, `INNER / LEFT / RIGHT / FULL OUTER / CROSS` joins, self joins, scalar / `IN` / `EXISTS` / correlated subqueries, subqueries in `FROM`, `UNION [ALL]`, `INTERSECT`, `EXCEPT`, views |
| **DCL** | `GRANT`, `REVOKE`, `CREATE USER`, `DROP USER`, `CONNECT <user>` with an internal privilege model |
| **TCL** | `START TRANSACTION` / `BEGIN`, `COMMIT`, `ROLLBACK`, `SAVEPOINT`, `ROLLBACK TO SAVEPOINT`, `RELEASE SAVEPOINT` |
| Utility | `USE`, `SHOW TABLES / VIEWS / INDEXES / DATABASES / USERS / GRANTS / TRANSACTION`, `DESCRIBE`, `EXPLAIN` |

Data types: `INT INTEGER SMALLINT BIGINT FLOAT REAL DOUBLE DECIMAL(p,s) NUMERIC CHAR(n) VARCHAR(n) TEXT DATE TIME DATETIME BOOLEAN`.
Scalar functions: `UPPER LOWER LENGTH ABS ROUND COALESCE IFNULL CONCAT`.

---

## 2. Building

### Requirements
* gcc (or clang), make
* flex
* bison **3.6 or newer** (uses `%define parse.error custom`)
* SQLite 3 development library

**Ubuntu / Debian / WSL**
```bash
sudo apt install build-essential flex bison libsqlite3-dev
```
**macOS (Homebrew)** – the bison shipped with macOS is too old:
```bash
brew install flex bison sqlite
make BISON=$(brew --prefix bison)/bin/bison
```
**Windows** – use WSL (Ubuntu) and follow the Ubuntu instructions.

### Build and run
```bash
make            # builds ./sqlc
./sqlc          # interactive SQL> prompt, shows every compiler phase
make demo       # guided tour of all statement categories
make test       # regression test suite (21 test files)
```

---

## 3. Using the compiler

```text
./sqlc [options] [file.sql ...]

  -c, --compact     show only results and errors (hide the compiler phases)
  -d <dir>          directory for database files       (default: database)
  -D <name>         database to open                   (default: sqlcompiler)
  -i                interactive prompt (also after running files)
  --ascii           draw trees with ASCII characters instead of box drawing
  --no-color        disable colours
```

Statements end with `;` and may span several lines. Commands that start with a backslash are
handled by the command-line interface, not the compiler:

| Command | Meaning |
|---|---|
| `\help` | list statements and commands |
| `\i file.sql` | run a script (e.g. `\i tests/sample_data.sql`) |
| `\full` / `\compact` | show all phases / only results |
| `\phase ast off` | hide one phase (`tokens syntax ast semantic symtab ir optimize`) |
| `\symtab` | dump the whole symbol table |
| `\keywords` | print the lexer's keyword table |
| `\txn` | show the transaction manager state |
| `\q` | quit |

### Example session

```text
SQL> SELECT name, cgpa FROM students WHERE cgpa > 8 ORDER BY cgpa DESC;

========== LEXICAL ANALYSIS ==========

  #    LEXEME     CATEGORY       TOKEN       LINE:COL
  1    SELECT     KEYWORD        K_SELECT    1:1
  2    name       IDENTIFIER     IDENT       1:8
  3    ,          PUNCTUATION    COMMA       1:12
  ...
  10   8          INTEGER        INTNUM      1:47

Lexical Analysis Successful - 15 tokens

========== SYNTAX ANALYSIS ==========

Syntax Analysis Successful
Statement: SELECT  (category: DQL)

========== ABSTRACT SYNTAX TREE ==========

SELECT
├── COLUMNS
│   ├── name
│   └── cgpa
├── FROM
│   └── students
├── WHERE
│   └── >
│       ├── cgpa
│       └── 8
└── ORDER BY
    └── cgpa DESC

========== SEMANTIC ANALYSIS ==========

  [OK]   Table 'students' found (6 columns)
  [OK]   Privilege: user 'admin' may SELECT on 'students'
  [OK]   Column 'cgpa' -> students.cgpa : FLOAT
  [OK]   WHERE clause is a valid BOOLEAN condition
  [OK]   Column 'name' -> students.name : VARCHAR(50)

Semantic Analysis Successful

========== SYMBOL TABLE ==========

  lookup_table(students) -> FOUND
  lookup_column(students, cgpa) -> FLOAT
  lookup_column(students, name) -> VARCHAR(50)

========== INTERMEDIATE CODE ==========

T1 = LOAD students
T2 = FILTER T1 WHERE cgpa > 8
T3 = PROJECT T2 (name, cgpa)
T4 = SORT T3 BY cgpa DESC

========== OPTIMIZATION ==========

===== BEFORE OPTIMIZATION =====
  ...
===== OPTIMIZATION PASSES =====
  [PROJECTION REDUCTION] LOAD students reads only 2 of 6 column(s) (name, cgpa)
  [REDUNDANT PROJECT] PROJECT keeps every input column unchanged - removed

===== AFTER OPTIMIZATION =====
T1 = LOAD students (name, cgpa)
T2 = FILTER T1 WHERE cgpa > 8
T3 = SORT T2 BY cgpa DESC

========== EXECUTION ==========

+--------+------+
| name   | cgpa |
+--------+------+
| Sneha  |  9.4 |
| Priya  |  9.1 |
| Ananya |  8.9 |
| Rahul  |  8.5 |
+--------+------+
4 rows in set (0.000 sec)
```

---

## 4. Project structure

```text
SQLCompiler/
├── lexer.l            Flex scanner: keyword table, literals, operators, lexical errors
├── lexer.h
├── parser.y           Bison grammar: DDL / DML / DQL / DCL / TCL, precedence, syntax errors
├── main.c             command-line interface and the phase-by-phase pipeline
├── ast.h / ast.c      AST node types, constructors, tree printer, AST -> SQL un-parser
├── symbol_table.h/.c  hash-table symbol table (tables, columns, views, indexes, users, privileges)
├── semantic.h/.c      semantic analyzer (scopes, types, aggregates, constraints, privileges)
├── ir.h / ir.c        intermediate representation (relational three-address code)
├── optimizer.h/.c     IR optimizer (folding, pushdown, pruning, redundancy removal, ...)
├── executor.h/.c      IR interpreter + storage layer (SQLite used only for storage)
├── eval.h / eval.c    run-time values, relations and expression evaluator
├── transaction.h/.c   transaction manager (ids, states, savepoints, pending operations)
├── diag.h / diag.c    LEXICAL / SYNTAX / SEMANTIC / EXECUTION error reporting with carets
├── util.h / util.c    strings, buffers, colours
├── database/          database files (created at run time, e.g. sqlcompiler.db)
├── tests/
│   ├── sample_data.sql   sample schema + rows used by demo and tests
│   ├── demo.sql          guided demo (make demo)
│   ├── run_tests.sh      regression test runner (make test)
│   ├── ddl/ dml/ dql/ dcl/ tcl/ errors/   test scripts + expected outputs
├── Makefile
└── README.md
```

---

## 5. How each phase works

### 5.1 Lexical analysis (`lexer.l`)
* 110 keywords are kept in a **keyword table** that is sorted once and searched with **binary
  search** (`\keywords` prints it). Each keyword has a category: KEYWORD, DATA TYPE, AGGREGATE,
  BOOLEAN or NULL. Keywords are case-insensitive.
* Token classes: `IDENTIFIER` (also `"quoted"` and `` `quoted` ``), `INTEGER`, `FLOAT`
  (`8.5`, `.5`, `1e3`), `STRING` (`'it''s'`), `DATE` (`DATE '2024-05-01'`), relational and
  arithmetic operators, punctuation.
* Every token carries its line and column (`yylloc`), used for error carets.
* Lexical errors: invalid characters (`@ # $ !`), unterminated strings / comments, malformed
  numbers (`12abc`), integer overflow, identifiers longer than 63 characters, invalid dates
  (`DATE '2026-02-30'`).

### 5.2 Syntax analysis (`parser.y`)
The grammar is split by command category exactly as in SQL:
```
sql_statement : ddl_statement | dml_statement | dql_statement
              | dcl_statement | tcl_statement | utility_statement ;
```
Expression precedence (lowest to highest):
`OR` < `AND` < `NOT` < comparisons, `LIKE`, `IN`, `IS`, `BETWEEN` < `+ -` < `* / %` < unary `-`,
so `age > 18 AND cgpa > 8 OR branch = 'CSE'` parses as `((age > 18) AND (cgpa > 8)) OR (branch = 'CSE')`.
`BETWEEN` takes a restricted operand (`b_expr`, arithmetic only) so that
`x BETWEEN 1 AND 5 AND y > 2` is unambiguous. The grammar has **zero shift/reduce and zero
reduce/reduce conflicts**. Syntax errors report the unexpected token, what was expected, and a
hint for common mistakes.

### 5.3 Abstract syntax tree (`ast.c`)
A generic n-ary `Node` (type, operator, names, literal value, children, source position). The
printer draws the tree with box characters; the un-parser turns trees back into SQL, which is used
for IR printing, view definitions and the persisted catalog.

### 5.4 Semantic analysis (`semantic.c`)
* **Existence**: tables, views, columns, users, savepoints, databases (with *did you mean ...?*
  suggestions using edit distance).
* **Name resolution and scopes**: aliases, `t.*`, ambiguous columns in joins, self joins,
  correlated sub-queries (outer references are detected and the sub-query is marked correlated).
* **Types**: arithmetic needs numbers, comparisons need compatible types, `LIKE` needs strings,
  conditions must be BOOLEAN, date literals are validated, `AVG(name)` is rejected.
* **Aggregates**: not allowed in `WHERE`/`GROUP BY`/`ON`, no nesting, every non-aggregated column
  must be in `GROUP BY`, `DISTINCT` + `ORDER BY` rules, set-operation column counts and types.
* **Constraints (compile time)**: duplicate columns, multiple primary keys, foreign key targets
  must be a PRIMARY KEY/UNIQUE of the referenced table with compatible types, `CHECK` must be
  boolean and may not contain sub-queries, `NULL` into `NOT NULL`, values too long for
  `VARCHAR(n)`, and for constant `INSERT` rows the `CHECK` constraints and duplicate primary keys
  are evaluated already during analysis.
* **Privileges**: the current user needs SELECT / INSERT / UPDATE / DELETE on each object; DDL and
  DCL need an administrator.

### 5.5 Symbol table (`symbol_table.c`)
Chained hash tables (case-insensitive keys) for **tables, views, indexes and users**. Each table
entry holds its columns (type, length, NOT NULL, DEFAULT) and its key, foreign-key and CHECK
constraints; each user holds a list of *(object, privilege mask)* pairs:
```
user1
 ├── SELECT -> students
 ├── INSERT -> students
 └── UPDATE -> students
```
API: `insert_table lookup_table delete_table insert_column lookup_column insert_view lookup_view
insert_index lookup_index insert_user lookup_user grant_privilege revoke_privilege has_privilege`.
Every call made for a statement is shown in the SYMBOL TABLE phase. The catalog is persisted inside
the database file as canonical SQL (`__sqlc_catalog`) and is **re-parsed by our own parser** at
start-up, after `USE` and after a rollback.

### 5.6 Intermediate code (`ir.c`)
The AST is translated into relational three-address code. Operations:

| IR | Meaning |
|---|---|
| `LOAD t [(cols)]` | read a base table (only the listed columns after pruning) |
| `VIEW v (Tn)`, `DERIVED (Tn) AS x` | expand a view / sub-query in FROM |
| `FILTER Tn WHERE/HAVING cond` | selection |
| `[LEFT/RIGHT/FULL/CROSS] JOIN Ta AND Tb ON cond` | join |
| `GROUP Tn BY keys`, `AGGREGATE Tn aggs` | grouping and aggregation |
| `PROJECT Tn (exprs)` | projection / computed columns |
| `DISTINCT`, `SORT ... BY`, `LIMIT` | duplicate removal, ordering, limiting |
| `UNION / INTERSECT / EXCEPT Ta AND Tb` | set operations |
| `VALUES`, `INSERT`, `UPDATE`, `DELETE` | data manipulation |
| `CREATE_TABLE`, `GRANT`, `TXN_BEGIN`, ... | catalog / transaction operations |

Sub-queries inside expressions are compiled to their own blocks (`SUBQ#1`, `SUBQ#2`, ...).

### 5.7 Optimization (`optimizer.c`)
The plan is printed **before** and **after**, with a log line for every rewrite:

1. **Constant folding** – `age > 10 + 5` becomes `age > 15`; `x AND TRUE` becomes `x`.
2. **Redundant filter removal** – `WHERE 1 = 1` disappears; always-false conditions are flagged.
3. **Selection pushdown** – WHERE (and ON) conjuncts that use only one table move below the join
   (respecting outer-join semantics); `CROSS JOIN` + equality in WHERE becomes an `INNER JOIN`.
4. **Join algorithm selection** – equi-joins run as a **hash join**, others as nested loops.
5. **Projection reduction** – each `LOAD` reads only the columns the query needs
   (`SELECT COUNT(*)` reads none).
6. **Redundant operation removal** – a `PROJECT` that keeps every column unchanged (e.g.
   `SELECT *`) is dropped; `DISTINCT` is dropped when the output contains the primary key.
7. **Sub-query caching** – uncorrelated sub-queries are evaluated once and cached.

### 5.8 Execution (`executor.c`, `eval.c`)
The optimized IR tree is interpreted operator by operator over in-memory relations (three-valued
logic for NULL). SQLite is used only to read table rows (`SELECT rowid, cols FROM t`) and to write
the rows that `INSERT`, `UPDATE` and `DELETE` produce. Constraints are enforced by the executor
itself: PRIMARY KEY, UNIQUE (incl. unique indexes), NOT NULL, CHECK and FOREIGN KEY (both the
child side and the referenced side). `ALTER TABLE ... MODIFY` converts existing data and checks it.

### 5.9 Transactions (`transaction.c`)
The transaction manager keeps the **transaction id, state (ACTIVE / COMMITTED / ROLLED_BACK),
savepoint stack and pending-operation log** (`SHOW TRANSACTION`). Every data or schema change is
also atomic on its own: if a statement fails half-way, only that statement is undone and an open
transaction stays ACTIVE. `SAVEPOINT` outside a transaction starts one implicitly.

---

## 6. Error handling

Errors are always labelled with their phase and point at the source position:

```text
SQL> SELECT @name FROM students;
[LEXICAL ERROR] line 1, column 8: invalid character '@'
       1 | SELECT @name FROM students;
         |        ^

SQL> SELECT FROM students;
[SYNTAX ERROR] line 1, column 8: unexpected keyword 'FROM'; expected one of 32 possible tokens ...
    hint: SELECT needs a column list or '*' before FROM

SQL> SELECT salary FROM students;
[SEMANTIC ERROR] line 1, column 8: column 'salary' does not exist in table 'students'

SQL> INSERT INTO students VALUES (1, 'Duplicate', 20, 7.5, NULL, 'IT');
[EXECUTION ERROR] PRIMARY KEY violated on 'students': duplicate value (id = 1) (INSERT)
Statement rolled back - no changes were made.
```

`tests/errors/` contains many more examples of each category.

---

## 7. Tests

`make test` runs every script in `tests/ddl`, `dml`, `dql`, `dcl`, `tcl` and `errors` against a
fresh database loaded with `tests/sample_data.sql`, and compares the output with the stored
`.expected` file. `UPDATE=1 sh tests/run_tests.sh` re-records the expected outputs.

---

## 8. Development stages

The compiler was built and tested in the stages below; the test folders follow the same order.

| Stage | Work | Where |
|---|---|---|
| 1 Lexer | keywords, identifiers, literals, operators, lexical errors | `lexer.l`, `tests/errors/01_lexical.sql` |
| 2 DDL grammar | CREATE / ALTER / DROP / TRUNCATE | `parser.y`, `tests/ddl/` |
| 3 DML grammar | INSERT / UPDATE / DELETE | `parser.y`, `tests/dml/` |
| 4 SELECT | SELECT / FROM / WHERE | `tests/dql/01_select_where.sql` |
| 5 Advanced SELECT | DISTINCT, ORDER BY, GROUP BY, HAVING, aggregates | `tests/dql/02_advanced_select.sql` |
| 6 JOINs | INNER, LEFT, RIGHT, FULL, CROSS, self join | `tests/dql/03_joins.sql` |
| 7 Subqueries and set operations | scalar / IN / EXISTS / correlated, UNION, INTERSECT, EXCEPT | `tests/dql/04_*`, `05_*` |
| 8 AST + semantic analysis + symbol table | | `ast.c`, `semantic.c`, `symbol_table.c` |
| 9 IR + optimizer | | `ir.c`, `optimizer.c` |
| 10 Execution, DCL, TCL | | `executor.c`, `transaction.c`, `tests/dcl`, `tests/tcl` |

---

## 9. Design notes and limitations

* Typing is deliberately strict (like a compiler): `WHERE name > 5` or `INSERT` of `'ten'` into an
  `INT` column is a semantic error rather than a silent conversion.
* Integer division truncates (`7 / 2 = 3`, `7.0 / 2 = 3.5`); `LIKE` is case-insensitive;
  NULLs sort first in ascending order.
* Users have no passwords; `CONNECT <user>` switches the current user (the built-in `admin` has
  every privilege).
* The engine keeps each intermediate relation in memory, which is ideal for demonstrating the
  phases on classroom-sized data rather than millions of rows.
