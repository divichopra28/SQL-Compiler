-- Stage 3: DML - INSERT (full row, column list, multiple rows, defaults, INSERT ... SELECT)
INSERT INTO students VALUES (7, 'Kavya', 19, 8.2, 'kavya@college.edu', 'IT');
INSERT INTO students (id, name, age, cgpa) VALUES (8, 'Rohan', 21, 7.9);
INSERT INTO students VALUES
    (9, 'Isha', 20, 9.0, NULL, 'CSE'),
    (10, 'Dev', 23, 6.5, NULL, 'ECE');
INSERT INTO courses (cid, student_id, course_name) VALUES (107, 7, 'Operating Systems');
SELECT * FROM students WHERE id >= 7;
SELECT * FROM courses WHERE cid = 107;
CREATE TABLE honours (hid INT PRIMARY KEY, hname VARCHAR(50));
INSERT INTO honours SELECT id, name FROM students WHERE cgpa >= 9;
SELECT * FROM honours ORDER BY hid;
