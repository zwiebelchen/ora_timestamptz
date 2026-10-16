--
-- DDL rewriting: TIMESTAMP WITH TIME ZONE -> ora_timestamptz
--
SET TimeZone = 'Europe/Berlin';
CREATE EXTENSION ora_timestamptz;

CREATE FUNCTION coltypes(rel regclass) RETURNS TABLE (col name, typ text)
LANGUAGE sql AS $$
  SELECT attname, format_type(atttypid, atttypmod) FROM pg_attribute
   WHERE attrelid = rel AND attnum > 0 AND NOT attisdropped ORDER BY attnum $$;

-- default: off
SHOW ora_timestamptz.replace_timestamptz;
CREATE TABLE t_off (a TIMESTAMP WITH TIME ZONE);
SELECT * FROM coltypes('t_off');

-- with_time_zone: only the Oracle/SQL-standard spelling is rewritten
SET ora_timestamptz.replace_timestamptz = with_time_zone;
CREATE TABLE t_wtz (
  a TIMESTAMP WITH TIME ZONE,
  b timestamp(3) with time zone DEFAULT now(),
  c TIMESTAMP(9) WITH TIME ZONE,
  d timestamptz,
  e pg_catalog.timestamptz,
  f TIMESTAMP WITH TIME ZONE[],
  g TIMESTAMP,
  h TIMESTAMP(0) WITHOUT TIME ZONE
);
SELECT * FROM coltypes('t_wtz');
INSERT INTO t_wtz (a, c) VALUES ('2026-10-16 14:30:00 Europe/Berlin', '2026-10-16 14:30:00.123456789 +05:30');
SELECT a, c, b IS NOT NULL AS has_default FROM t_wtz;

ALTER TABLE t_wtz ADD COLUMN i TIMESTAMP WITH TIME ZONE, ADD COLUMN j timestamptz;
ALTER TABLE t_off ALTER COLUMN a TYPE TIMESTAMP(3) WITH TIME ZONE;
SELECT * FROM coltypes('t_wtz') WHERE col IN ('i', 'j');
SELECT * FROM coltypes('t_off');

CREATE DOMAIN d_event AS TIMESTAMP WITH TIME ZONE NOT NULL;
SELECT format_type(typbasetype, typtypmod) FROM pg_type WHERE typname = 'd_event';
CREATE TYPE ty_event AS (at TIMESTAMP WITH TIME ZONE, label text);
SELECT * FROM coltypes('ty_event');

CREATE TABLE t_part (id int, at TIMESTAMP WITH TIME ZONE) PARTITION BY RANGE (at);
CREATE TABLE t_part_2026 PARTITION OF t_part
  FOR VALUES FROM ('2026-01-01 00:00 UTC') TO ('2027-01-01 00:00 UTC');
INSERT INTO t_part VALUES (1, '2026-06-01 12:00 America/New_York');
SELECT * FROM coltypes('t_part');
SELECT tableoid::regclass, * FROM t_part;

CREATE TEMP TABLE t_temp (a TIMESTAMP WITH TIME ZONE);
SELECT * FROM coltypes('t_temp');

-- dynamic SQL (statement locations refer to the inner query string)
DO $$
BEGIN
  EXECUTE 'CREATE TABLE t_dyn (x int, a TIMESTAMP WITH TIME ZONE, b timestamptz)';
END $$;
SELECT * FROM coltypes('t_dyn');

-- all: timestamptz is rewritten too
SET ora_timestamptz.replace_timestamptz = 'all';
CREATE TABLE t_all (a TIMESTAMP WITH TIME ZONE, b timestamptz, c timestamptz(2));
SELECT * FROM coltypes('t_all');

-- schema filter
CREATE SCHEMA app;
CREATE SCHEMA other;
SET ora_timestamptz.replace_schemas = 'app';
CREATE TABLE app.t (a TIMESTAMP WITH TIME ZONE);
CREATE TABLE other.t (a TIMESTAMP WITH TIME ZONE);
SET search_path = app, public;
CREATE TABLE t_sp (a TIMESTAMP WITH TIME ZONE);
RESET search_path;
SELECT * FROM coltypes('app.t');
SELECT * FROM coltypes('other.t');
SELECT * FROM coltypes('app.t_sp');
ALTER TABLE other.t ADD COLUMN b TIMESTAMP WITH TIME ZONE;
SELECT * FROM coltypes('other.t');
SET ora_timestamptz.replace_schemas = 'app, "Other"';
SHOW ora_timestamptz.replace_schemas;
SET ora_timestamptz.replace_schemas = 'app, "unterminated';
SET ora_timestamptz.replace_schemas = other, app;
SHOW ora_timestamptz.replace_schemas;
CREATE TABLE app.t2 (a TIMESTAMP WITH TIME ZONE);
CREATE TABLE other.t2 (a TIMESTAMP WITH TIME ZONE);
SELECT * FROM coltypes('app.t2');
SELECT * FROM coltypes('other.t2');
RESET ora_timestamptz.replace_schemas;

-- invalid value
SET ora_timestamptz.replace_timestamptz = 'sometimes';

-- dump-relevant: the column type is a real ora_timestamptz
SELECT pg_typeof(a) FROM t_wtz LIMIT 1;

-- without the extension installed nothing is rewritten
DROP SCHEMA app, other CASCADE;
DROP TABLE t_off, t_wtz, t_part, t_temp, t_dyn, t_all;
DROP TYPE ty_event;
DROP DOMAIN d_event;
DROP EXTENSION ora_timestamptz;
CREATE TABLE t_noext (a TIMESTAMP WITH TIME ZONE);
SELECT * FROM coltypes('t_noext');
DROP TABLE t_noext;
DROP FUNCTION coltypes(regclass);
