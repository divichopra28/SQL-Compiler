-- Stage 2: DDL - CREATE / DROP VIEW and INDEX
CREATE VIEW toppers AS SELECT name, cgpa, branch FROM students WHERE cgpa >= 8.5;
CREATE VIEW cse_toppers AS SELECT name, cgpa FROM toppers WHERE branch = 'CSE';
SELECT * FROM toppers ORDER BY cgpa DESC;
SELECT * FROM cse_toppers;
DESCRIBE toppers;
CREATE INDEX idx_branch ON students (branch);
CREATE UNIQUE INDEX idx_name ON students (name);
SHOW INDEXES;
SHOW VIEWS;
DROP INDEX idx_branch;
DROP VIEW cse_toppers;
DROP VIEW toppers;
SHOW VIEWS;
