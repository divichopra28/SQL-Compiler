-- Stage 7: UNION, UNION ALL, INTERSECT, EXCEPT
SELECT name FROM students UNION SELECT name FROM faculty;
SELECT name FROM students UNION ALL SELECT name FROM faculty;
SELECT name FROM students INTERSECT SELECT name FROM faculty;
SELECT name FROM students EXCEPT SELECT name FROM faculty ORDER BY name;
SELECT id FROM students EXCEPT SELECT student_id FROM courses;
