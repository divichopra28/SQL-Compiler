-- Stage 6: JOINs
SELECT students.name, courses.course_name FROM students INNER JOIN courses ON students.id = courses.student_id;
SELECT students.name, courses.course_name FROM students LEFT JOIN courses ON students.id = courses.student_id;
SELECT students.name, courses.course_name FROM students RIGHT JOIN courses ON students.id = courses.student_id;
SELECT * FROM students FULL OUTER JOIN courses ON students.id = courses.student_id;
SELECT s.name, f.name FROM students s CROSS JOIN faculty f WHERE s.id = 1;
SELECT a.name AS student, b.name AS classmate FROM students a JOIN students b ON a.branch = b.branch AND a.id < b.id;
SELECT s.name, c.course_name FROM students s JOIN courses c ON s.id = c.student_id WHERE s.cgpa > 8.6 AND c.credits = 4;
SELECT s.name, COUNT(c.cid) AS courses FROM students s LEFT JOIN courses c ON s.id = c.student_id GROUP BY s.name ORDER BY courses DESC, s.name;
SELECT s.name, c.course_name FROM students s, courses c WHERE s.id = c.student_id AND s.branch = 'CSE';
