-- LEXICAL ERRORS
SELECT @name FROM students;
SELECT name FROM students WHERE id = 12abc;
SELECT # FROM students;
SELECT DATE '2026-02-30';
SELECT name, $salary FROM students WHERE age ! 20;
SELECT name FROM students WHERE branch = 'CSE;
