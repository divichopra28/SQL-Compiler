-- Stage 2: DDL - ALTER TABLE (ADD / DROP COLUMN / MODIFY / RENAME)
ALTER TABLE students ADD phone VARCHAR(15);
ALTER TABLE students ADD COLUMN hostel BOOLEAN DEFAULT FALSE;
SELECT id, name, phone, hostel FROM students WHERE id <= 2;
ALTER TABLE students DROP COLUMN phone;
ALTER TABLE students MODIFY age SMALLINT;
ALTER TABLE students MODIFY cgpa DECIMAL(4,1);
ALTER TABLE students RENAME COLUMN name TO student_name;
ALTER TABLE faculty RENAME TO teachers;
SELECT * FROM teachers;
DESCRIBE students;
