-- Stage 3: DML - DELETE (with WHERE, all rows)
DELETE FROM courses WHERE credits < 3;
DELETE FROM students WHERE id = 5;
SELECT id, name FROM students;
DELETE FROM courses;
SELECT COUNT(*) FROM courses;
