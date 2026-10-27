-- Stage 3: DML - UPDATE (single column, several columns, expressions, subquery)
UPDATE students SET age = 21 WHERE id = 1;
UPDATE students SET age = 22, cgpa = 8.9 WHERE id = 3;
UPDATE students SET cgpa = cgpa + 0.1 WHERE branch = 'ECE';
UPDATE students SET branch = 'AI' WHERE cgpa > (SELECT AVG(cgpa) FROM students) AND branch = 'CSE';
SELECT id, name, age, cgpa, branch FROM students ORDER BY id;
