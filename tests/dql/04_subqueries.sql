-- Stage 7: Subqueries (scalar, IN, EXISTS, correlated, derived tables)
SELECT name FROM students WHERE cgpa > (SELECT AVG(cgpa) FROM students);
SELECT * FROM students WHERE id IN (SELECT student_id FROM courses);
SELECT name FROM students WHERE id NOT IN (SELECT student_id FROM courses WHERE student_id IS NOT NULL);
SELECT name FROM students s WHERE EXISTS (SELECT 1 FROM courses c WHERE c.student_id = s.id);
SELECT name, (SELECT COUNT(*) FROM courses c WHERE c.student_id = s.id) AS n FROM students s ORDER BY n DESC, name;
SELECT name, cgpa FROM students s WHERE cgpa = (SELECT MAX(cgpa) FROM students t WHERE t.branch = s.branch) ORDER BY name;
SELECT t.branch, t.total FROM (SELECT branch, COUNT(*) AS total FROM students GROUP BY branch) AS t WHERE t.total > 1;
