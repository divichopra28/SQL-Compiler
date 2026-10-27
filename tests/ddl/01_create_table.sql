-- Stage 2: DDL - CREATE TABLE with every constraint kind
CREATE TABLE departments (
    dept_id   INT PRIMARY KEY,
    dname     VARCHAR(30) NOT NULL UNIQUE,
    budget    DECIMAL(12,2) DEFAULT 100000,
    founded   DATE,
    active    BOOLEAN DEFAULT TRUE,
    CHECK (budget >= 0)
);
CREATE TABLE staff (
    sid      INT,
    dept_id  INT REFERENCES departments(dept_id),
    sname    VARCHAR(40) NOT NULL,
    code     CHAR(4),
    rating   DOUBLE,
    big      BIGINT,
    small    SMALLINT,
    notes    TEXT,
    login_at DATETIME,
    shift    TIME,
    PRIMARY KEY (sid),
    UNIQUE (sname, code)
);
CREATE TABLE IF NOT EXISTS staff (x INT);
DESCRIBE departments;
DESCRIBE staff;
SHOW TABLES;
