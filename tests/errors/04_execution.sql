-- EXECUTION (run-time / data) ERRORS - each statement is rolled back
INSERT INTO students VALUES (1, 'Duplicate id', 20, 7.0, NULL, 'IT');
INSERT INTO students VALUES (60, 'Copy', 20, 7.0, 'rahul@college.edu', 'IT');
INSERT INTO courses VALUES (200, 99, 'Ghost course', 3);
DELETE FROM students WHERE id = 1;
UPDATE students SET id = 100 WHERE id = 2;
UPDATE students SET age = age - 10 WHERE id = 6;
UPDATE students SET email = 'priya@college.edu' WHERE id = 1;
SELECT name FROM students WHERE cgpa > (SELECT cgpa FROM students);
SELECT 1 / 0;
TRUNCATE TABLE students;
ALTER TABLE students MODIFY name INT;
ALTER TABLE students ADD roll INT NOT NULL;
SELECT COUNT(*) FROM students;
