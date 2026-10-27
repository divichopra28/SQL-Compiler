-- ------------------------------------------------------------------
--  Sample schema and data used by the demo and the test suite.
--  Safe to run repeatedly: existing objects are dropped first.
--  Usage inside the compiler:   \i tests/sample_data.sql
-- ------------------------------------------------------------------

DROP VIEW IF EXISTS toppers;
DROP TABLE IF EXISTS courses;
DROP TABLE IF EXISTS students;
DROP TABLE IF EXISTS faculty;
DROP TABLE IF EXISTS employees;

CREATE TABLE students (
    id      INT PRIMARY KEY,
    name    VARCHAR(50) NOT NULL,
    age     INT CHECK (age >= 17),
    cgpa    FLOAT,
    email   VARCHAR(100) UNIQUE,
    branch  VARCHAR(20) DEFAULT 'CSE'
);

CREATE TABLE courses (
    cid         INT PRIMARY KEY,
    student_id  INT,
    course_name VARCHAR(40) NOT NULL,
    credits     INT DEFAULT 4,
    FOREIGN KEY (student_id) REFERENCES students (id)
);

CREATE TABLE faculty (
    fid     INT PRIMARY KEY,
    name    VARCHAR(50) NOT NULL,
    dept    VARCHAR(20)
);

CREATE TABLE employees (
    eid     INT PRIMARY KEY,
    ename   VARCHAR(50),
    salary  DECIMAL(10,2),
    joined  DATE
);

INSERT INTO students VALUES
    (1, 'Rahul',  20, 8.5, 'rahul@college.edu',  'CSE'),
    (2, 'Priya',  21, 9.1, 'priya@college.edu',  'CSE'),
    (3, 'Aarav',  19, 7.4, NULL,                 'IT'),
    (4, 'Ananya', 22, 8.9, 'ananya@college.edu', 'ECE'),
    (5, 'Vikram', 20, 6.8, NULL,                 'IT'),
    (6, 'Sneha',  18, 9.4, 'sneha@college.edu',  'ECE');

INSERT INTO courses (cid, student_id, course_name, credits) VALUES
    (101, 1, 'Compiler Design', 4),
    (102, 1, 'DBMS',            3),
    (103, 2, 'Compiler Design', 4),
    (104, 4, 'VLSI',            3),
    (105, 6, 'Signals',         4),
    (106, NULL, 'Ethics',       2);

INSERT INTO faculty VALUES
    (1, 'Dr. Mehta', 'CSE'),
    (2, 'Priya',     'IT'),
    (3, 'Dr. Rao',   'ECE');

INSERT INTO employees VALUES
    (1, 'Kiran', 42000.50, '2021-07-01'),
    (2, 'Meera', 55000,    '2019-03-15'),
    (3, 'Arjun', 38000,    '2023-01-10');
