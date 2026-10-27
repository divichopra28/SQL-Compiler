-- Expressions, precedence, functions and data types
SELECT salary * 12 FROM employees;
SELECT ename, salary * 12 AS annual FROM employees WHERE salary > 40000;
SELECT 10 + 5 * 2, (10 + 5) * 2, 17 % 5, 7 / 2, 7.0 / 2, -3 + 1;
SELECT UPPER(name), LOWER(branch), LENGTH(name) FROM students WHERE id = 1;
SELECT name, COALESCE(email, 'not given') AS email FROM students WHERE id IN (3, 4);
SELECT ename, joined FROM employees WHERE joined > '2020-01-01';
SELECT ename FROM employees WHERE joined BETWEEN DATE '2019-01-01' AND DATE '2021-12-31';
SELECT name FROM students WHERE age > 10 + 5 AND cgpa > 9;
