-- Monitor schema 1, application_id 1297042993 (MON1), aggregation version 1.
-- UTC values are signed SQLite INTEGER FILETIME ticks (100ns since 1601).
PRAGMA application_id=1297042993;
PRAGMA user_version=1;
CREATE TABLE metadata(synthetic INTEGER NOT NULL CHECK(synthetic IN(0,1)));
CREATE TABLE session(id INTEGER PRIMARY KEY, started INTEGER NOT NULL, ended INTEGER);
CREATE INDEX session_time ON session(started);
CREATE TABLE program(id INTEGER PRIMARY KEY, path TEXT NOT NULL COLLATE BINARY UNIQUE,
 name TEXT NOT NULL, first_seen INTEGER NOT NULL, last_seen INTEGER NOT NULL);
CREATE INDEX program_last_seen ON program(last_seen);
CREATE TABLE collection_run(id INTEGER PRIMARY KEY, session_id INTEGER NOT NULL REFERENCES session(id),
 started INTEGER NOT NULL, finished INTEGER NOT NULL, status INTEGER NOT NULL,
 row_count INTEGER NOT NULL, cpu_count INTEGER NOT NULL,
 foreground_utc INTEGER, foreground_pid INTEGER);
CREATE INDEX run_time ON collection_run(finished);
CREATE INDEX run_session ON collection_run(session_id);
CREATE TABLE raw(id INTEGER PRIMARY KEY, run_id INTEGER NOT NULL REFERENCES collection_run(id) ON DELETE CASCADE,
 utc INTEGER NOT NULL, pid INTEGER NOT NULL, creation INTEGER, program_id INTEGER REFERENCES program(id),
 valid INTEGER NOT NULL, cpu REAL, ram INTEGER, io_read REAL, io_write REAL, elapsed REAL,
 observation_interval REAL, cpu_interval REAL, io_interval REAL, foreground INTEGER,
 cpu_status INTEGER NOT NULL, read_status INTEGER NOT NULL, write_status INTEGER NOT NULL,
 open_error INTEGER NOT NULL, times_error INTEGER NOT NULL, memory_error INTEGER NOT NULL,
 io_error INTEGER NOT NULL, name_error INTEGER NOT NULL, path_error INTEGER NOT NULL, name TEXT);
CREATE INDEX raw_time ON raw(utc);
CREATE INDEX raw_run ON raw(run_id);
CREATE INDEX raw_program_time ON raw(program_id,utc);
CREATE TABLE aggregate_window(id INTEGER PRIMARY KEY, run_id INTEGER NOT NULL REFERENCES collection_run(id),
 program_id INTEGER NOT NULL REFERENCES program(id), pid INTEGER NOT NULL, creation INTEGER NOT NULL,
 start_utc INTEGER NOT NULL, end_utc INTEGER NOT NULL, duration REAL NOT NULL, version INTEGER NOT NULL);
CREATE INDEX aggregate_time ON aggregate_window(end_utc);
CREATE INDEX aggregate_run ON aggregate_window(run_id);
CREATE INDEX aggregate_program_time ON aggregate_window(program_id,end_utc);
CREATE TABLE aggregate_metric(window_id INTEGER NOT NULL REFERENCES aggregate_window(id) ON DELETE CASCADE,
 metric INTEGER NOT NULL CHECK(metric BETWEEN 0 AND 3), value REAL, covered REAL NOT NULL,
 valid_count INTEGER NOT NULL, eligible INTEGER NOT NULL, state INTEGER NOT NULL, reason TEXT NOT NULL,
 n INTEGER NOT NULL, days INTEGER NOT NULL, minutes INTEGER NOT NULL, median REAL, mad REAL, threshold REAL,
 streak INTEGER NOT NULL, PRIMARY KEY(window_id,metric)) WITHOUT ROWID;
CREATE INDEX metric_baseline ON aggregate_metric(metric,eligible,window_id);
CREATE TABLE feedback(id INTEGER PRIMARY KEY, program_id INTEGER NOT NULL REFERENCES program(id),
 kind INTEGER NOT NULL CHECK(kind IN(0,1,2)), created INTEGER NOT NULL, expires INTEGER NOT NULL,
 cancelled INTEGER NOT NULL DEFAULT 0);
CREATE INDEX feedback_active ON feedback(program_id,cancelled,expires);
CREATE INDEX feedback_expiry ON feedback(expires);
