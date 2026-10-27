-- =====================================================================
--  SQL-C demo script   (run with:  make demo   or   ./sqlc tests/demo.sql)
--
--  Loads the sample data quietly, then shows every compiler phase for a
--  tour of DDL, DML, DQL, DCL and TCL statements plus one example of each
--  error category.
-- =====================================================================
\compact
\i tests/sample_data.sql
DROP TABLE IF EXISTS demo_students;
\full

-- ---------- DQL: the example from the project specification ----------
SELECT name, cgpa
FROM students
WHERE cgpa > 8
ORDER BY cgpa DESC;

-- ---------- DDL: CREATE TABLE with constraints ----------
CREATE TABLE demo_students (
    id INT PRIMARY KEY,
    name VARCHAR(50)
);

-- ---------- DML ----------
INSERT INTO demo_students VALUES (1, 'Rahul'), (2, 'Priya');
UPDATE demo_students SET name = 'Priya S' WHERE id = 2;
DELETE FROM demo_students WHERE id = 1;

-- ---------- Optimizer: constant folding + redundant PROJECT ----------
SELECT * FROM students WHERE age > 10 + 5;

-- ---------- JOIN: selection pushdown, hash join, column pruning ----------
SELECT s.name, c.course_name
FROM students s
JOIN courses c ON s.id = c.student_id
WHERE s.cgpa > 8.6 AND c.credits = 4;

-- ---------- GROUP BY / HAVING ----------
SELECT branch, AVG(cgpa)
FROM students
GROUP BY branch
HAVING AVG(cgpa) > 8;

-- ---------- Subquery ----------
SELECT name
FROM students
WHERE cgpa > (
    SELECT AVG(cgpa)
    FROM students
);

-- ---------- Set operation ----------
SELECT name FROM students
UNION
SELECT name FROM faculty;

-- ---------- DCL ----------
GRANT SELECT, INSERT ON students TO demo_user;
REVOKE INSERT ON students FROM demo_user;

-- ---------- TCL ----------
START TRANSACTION;
SAVEPOINT sp1;
UPDATE students SET cgpa = 9.0 WHERE id = 1;
ROLLBACK TO SAVEPOINT sp1;
COMMIT;

-- ---------- The four error categories ----------
\compact
SELECT @name FROM students;
SELECT FROM students;
SELECT salary FROM students;
INSERT INTO students VALUES (1, 'Duplicate', 20, 7.5, NULL, 'IT');
\full
