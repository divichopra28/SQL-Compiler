-- Stage 5: DISTINCT, ORDER BY, GROUP BY, HAVING, aggregates, LIMIT
SELECT DISTINCT branch FROM students;
SELECT * FROM students ORDER BY cgpa;
SELECT * FROM students ORDER BY cgpa DESC;
SELECT name, branch, cgpa FROM students ORDER BY branch ASC, cgpa DESC;
SELECT branch, COUNT(*) FROM students GROUP BY branch;
SELECT branch, AVG(cgpa) FROM students GROUP BY branch HAVING AVG(cgpa) > 8;
SELECT COUNT(*) FROM students;
SELECT AVG(cgpa) FROM students;
SELECT branch, MAX(cgpa) FROM students GROUP BY branch;
SELECT MIN(age), MAX(age), SUM(age), COUNT(email), COUNT(DISTINCT branch) FROM students;
SELECT branch, COUNT(*) AS n, ROUND(AVG(cgpa), 2) AS avg_cgpa FROM students GROUP BY branch ORDER BY n DESC, branch;
SELECT name, cgpa FROM students ORDER BY cgpa DESC LIMIT 3;
SELECT name FROM students ORDER BY name LIMIT 2 OFFSET 2;
SELECT name AS student, cgpa * 10 AS percentage FROM students ORDER BY percentage DESC;
