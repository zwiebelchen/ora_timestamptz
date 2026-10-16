--
-- ora_timestamptz regression tests
--
SET TimeZone = 'Europe/Berlin';
CREATE EXTENSION ora_timestamptz;
SET ora_timestamptz.nls_timestamp_tz_format = 'DD-MON-RR HH.MI.SSXFF AM TZR';

-- input forms: region, offset, RFC 9557, region + TZD, session zone
SELECT '2026-10-16 14:30:00 Europe/Berlin'::ora_timestamptz;
SELECT '2026-10-16 14:30:00.123 +05:30'::ora_timestamptz;
SELECT '2026-10-16T14:30:00+02:00[Europe/Berlin]'::ora_timestamptz;
SELECT '2026-10-16T14:30:00Z'::ora_timestamptz;
SELECT '2026-10-16 14:30:00 UTC'::ora_timestamptz;
SELECT '2026-10-16 14:30'::ora_timestamptz;
SELECT '2026-10-16'::ora_timestamptz;
SELECT '2026-10-16 14:30 europe/berlin'::ora_timestamptz;
SELECT '2026-10-16 14:30:00-0330'::ora_timestamptz;
SELECT '2026-10-16 14:30:00 +02:00 Europe/Berlin'::ora_timestamptz;

-- invalid input
SELECT '2026-10-16 14:30 Mars/Olympus'::ora_timestamptz;
SELECT '2026-02-29 14:30 UTC'::ora_timestamptz;
SELECT '2026-10-16 24:00 UTC'::ora_timestamptz;
SELECT '2026-10-16 14:30 +15:00'::ora_timestamptz;
SELECT '2026-10-16 14:30:00.1234567891 UTC'::ora_timestamptz;
SELECT pg_input_is_valid('garbage', 'ora_timestamptz');
SELECT pg_input_is_valid('2026-10-16 14:30 UTC', 'ora_timestamptz');

-- Oracle: values are equal if they denote the same instant
SELECT '1999-01-15 8:00:00 -8:00'::ora_timestamptz = '1999-01-15 11:00:00 -5:00'::ora_timestamptz;
SELECT ora_timestamptz_identical('1999-01-15 8:00:00 -8:00', '1999-01-15 11:00:00 -5:00');
SELECT '2026-10-16 12:00 UTC'::ora_timestamptz < '2026-10-16 14:00:01 Europe/Berlin'::ora_timestamptz;

-- DST gap (ORA-01878) and overlap
SELECT '2026-03-29 02:30:00 Europe/Berlin'::ora_timestamptz;
SELECT '2026-10-25 02:30:00 Europe/Berlin'::ora_timestamptz;
SELECT '2026-10-25 02:30:00 Europe/Berlin CEST'::ora_timestamptz;
SELECT '2026-10-25 02:30:00 Europe/Berlin CET'::ora_timestamptz;
SELECT '2026-10-25 02:30:00+02:00[Europe/Berlin]'::ora_timestamptz;
SELECT '2026-10-25 02:30:00 Europe/Berlin PDT'::ora_timestamptz;
SELECT '2026-10-25 02:30:00+05:00[Europe/Berlin]'::ora_timestamptz;
SET ora_timestamptz.error_on_overlap_time = on;
SELECT '2026-10-25 02:30:00 Europe/Berlin'::ora_timestamptz;
SELECT '2026-10-25 02:30:00 Europe/Berlin CEST'::ora_timestamptz;
RESET ora_timestamptz.error_on_overlap_time;

-- precision (typmod), rounding like Oracle
SELECT '2026-10-16 14:30:00.123456789 UTC'::ora_timestamptz;
SELECT '2026-10-16 14:30:00.123456789 UTC'::ora_timestamptz(9);
SELECT '2026-10-16 14:30:00.123456789 UTC'::ora_timestamptz(3);
SELECT '2026-10-16 23:59:59.9999 UTC'::ora_timestamptz(0);
SELECT '2026-10-16 14:30:00.5 UTC'::ora_timestamptz::ora_timestamptz(0);
SELECT 'x'::text::ora_timestamptz(10);
CREATE TABLE t (id int, ts ora_timestamptz(3));
\d t
INSERT INTO t VALUES (1, '2026-10-16 14:30:00.123456 Europe/Berlin');
SELECT * FROM t;

-- arithmetic in UTC (Oracle doc example 4-6: DST end in New York)
SELECT '2000-10-28 23:24:54 America/New_York'::ora_timestamptz(0) + interval '8 hours';
SELECT '2026-03-28 12:00 Europe/Berlin'::ora_timestamptz + interval '1 day';
SELECT interval '1 hour' + '2026-10-16 14:30 +05:30'::ora_timestamptz;
SELECT '2026-10-16 14:30 Europe/Berlin'::ora_timestamptz - interval '1 month';
SELECT '2026-01-31 10:00 UTC'::ora_timestamptz + interval '1 month';
SELECT '2026-10-16 14:30 Europe/Berlin'::ora_timestamptz - '2026-10-10 10:00 UTC'::ora_timestamptz;
SELECT '2026-10-10 10:00 UTC'::ora_timestamptz - '2026-10-16 14:30:00.5 Europe/Berlin'::ora_timestamptz;
SELECT '2026-10-16 14:30 UTC'::ora_timestamptz + interval '0.000001 sec';
SELECT '2026-10-16 14:30 UTC'::ora_timestamptz + 'infinity'::interval;

-- AT TIME ZONE / AT LOCAL
SELECT '2026-10-16 14:30 Europe/Berlin'::ora_timestamptz AT TIME ZONE 'America/New_York';
SELECT '2026-10-16 14:30 Europe/Berlin'::ora_timestamptz AT TIME ZONE '-03:30';
SELECT '2026-10-16 14:30 Europe/Berlin'::ora_timestamptz AT TIME ZONE INTERVAL '05:45';
SELECT '2026-10-16 14:30 Europe/Berlin'::ora_timestamptz AT TIME ZONE 'SESSIONTIMEZONE';
SELECT '2026-10-16 14:30 +00:00'::ora_timestamptz AT LOCAL;
SELECT '2026-10-16 14:30 +00:00'::ora_timestamptz AT TIME ZONE 'Nowhere/City';

-- EXTRACT: Oracle returns UTC fields
SELECT EXTRACT(YEAR FROM x), EXTRACT(MONTH FROM x), EXTRACT(DAY FROM x),
       EXTRACT(HOUR FROM x), EXTRACT(MINUTE FROM x), EXTRACT(SECOND FROM x),
       EXTRACT(TIMEZONE_HOUR FROM x), EXTRACT(TIMEZONE_MINUTE FROM x)
FROM (SELECT '2026-10-17 01:30:05.25 Europe/Berlin'::ora_timestamptz AS x) s;
SELECT EXTRACT(TIMEZONE_HOUR FROM x), EXTRACT(TIMEZONE_MINUTE FROM x),
       ora_extract('timezone_region', x), ora_extract('timezone_abbr', x),
       ora_extract('timezone_offset', x)
FROM (SELECT '2026-10-16 14:30 -03:30'::ora_timestamptz AS x) s;
SELECT ora_extract('timezone_region', x), ora_extract('timezone_abbr', x)
FROM (SELECT '2026-01-16 14:30 Europe/Berlin'::ora_timestamptz AS x) s;
SELECT EXTRACT(TIMEZONE_REGION FROM '2026-10-16 14:30 UTC'::ora_timestamptz);
SELECT EXTRACT(EPOCH FROM '1970-01-01 01:00:00.5 Europe/Berlin'::ora_timestamptz);
SELECT EXTRACT(YEAR FROM '0000-06-01 00:00 UTC'::ora_timestamptz);

-- TO_CHAR / TO_TIMESTAMP_TZ
SELECT to_char('2026-10-16 14:30:05.123456 Europe/Berlin'::ora_timestamptz);
SELECT to_char('2026-10-16 14:30:05.123456 Europe/Berlin'::ora_timestamptz,
               'YYYY-MM-DD HH24:MI:SS.FF3 TZH:TZM TZR TZD');
SELECT to_char('2026-10-16 14:30:05.123456 Europe/Berlin'::ora_timestamptz,
               'FMDay, DD. Month YYYY "um" HH24:MI tzr');
SELECT to_char('2026-10-16 14:30:05 -03:30'::ora_timestamptz, 'Dy DDD D J SSSSS SYYYY a.m. TZR "/" TZD');
SELECT to_char('2026-10-16 00:05:00 UTC'::ora_timestamptz(0), 'HH12:MI AM FF');
SELECT to_char('2026-10-16 14:30 UTC'::ora_timestamptz, 'YYYY-MM-DD Q');
SELECT to_timestamp_tz('16-OCT-26 02.30.05.5 PM Europe/Berlin');
SELECT to_timestamp_tz('2026-10-16 14:30 +02:00', 'YYYY-MM-DD HH24:MI TZH:TZM');
SELECT to_timestamp_tz('2026-10-16 14:30 -3:30', 'YYYY-MM-DD HH24:MI TZH:TZM');
SELECT to_timestamp_tz('25.10.2026 02:30 Europe/Berlin CEST', 'DD.MM.YYYY HH24:MI TZR TZD');
SELECT to_timestamp_tz('16-Oct-1999 08:00:00.123456789 US/Pacific', 'DD-MON-YYYY HH24:MI:SS.FF TZR');
SELECT to_timestamp_tz('1999-10-16 08:00 +05:00', 'YYYY-MM-DD HH24:MI TZR');
SELECT to_timestamp_tz('2026-10-16 14:30 trailing', 'YYYY-MM-DD HH24:MI');
-- implicit conversion via NLS format, like Oracle
SELECT '16-OCT-26 02.30.05.000000 PM US/Pacific'::ora_timestamptz;
SET ora_timestamptz.output_format = 'nls';
SELECT '2026-10-16 14:30:05.5 Europe/Berlin'::ora_timestamptz;
RESET ora_timestamptz.output_format;

-- Oracle functions
SELECT from_tz(timestamp '2026-10-16 14:30', 'Europe/Berlin');
SELECT from_tz(timestamp '2026-10-25 02:30', 'Europe/Berlin CEST');
SELECT from_tz(timestamp '2026-10-16 14:30', '+09:00');
SELECT sys_extract_utc('2026-10-16 14:30:00.123456789 Europe/Berlin'::ora_timestamptz);
SELECT tz_offset('Asia/Kolkata'), tz_offset('-07:00'), sessiontimezone();
SELECT systimestamp() IS NOT NULL, ora_current_timestamp(3) IS NOT NULL,
       ora_extract('timezone_region', ora_current_timestamp());

-- casts
SELECT '2026-10-16 14:30 Asia/Tokyo'::ora_timestamptz::timestamptz;
SELECT '2026-10-16 14:30 Asia/Tokyo'::ora_timestamptz::timestamp;
SELECT '2026-10-16 23:30 -05:00'::ora_timestamptz::date;
SELECT timestamptz '2026-10-16 14:30+00'::ora_timestamptz;
SELECT timestamp '2026-10-16 14:30'::ora_timestamptz;
SELECT date '2026-10-16'::ora_timestamptz;
SELECT timestamp '2026-03-29 02:30'::ora_timestamptz;
BEGIN;
INSERT INTO t VALUES (2, now());
SELECT ts = now()::ora_timestamptz(3) FROM t WHERE id = 2;
COMMIT;
SELECT ora_timestamptz_iso(ts) = ts::text FROM t WHERE id = 1;

-- year range and BC (ISO notation: 0000 = 1 BC)
SELECT '-4711-01-01 00:00 UTC'::ora_timestamptz, to_char('-4711-01-01 00:00 UTC'::ora_timestamptz, 'SYYYY-MM-DD');
SELECT '-4712-12-31 00:00 UTC'::ora_timestamptz;
SELECT '9999-12-31 23:59:59 UTC'::ora_timestamptz + interval '1 second';

-- indexes, aggregates, joins
CREATE TABLE ev (id int, ts ora_timestamptz);
INSERT INTO ev SELECT g, ('2026-01-01 00:00 UTC'::ora_timestamptz + make_interval(mins => g))
	AT TIME ZONE (ARRAY['Europe/Berlin','America/New_York','+05:30','Asia/Tokyo'])[1 + g % 4]
	FROM generate_series(1, 10000) g;
CREATE INDEX ev_bt ON ev (ts);
CREATE INDEX ev_h ON ev USING hash (ts);
CREATE INDEX ev_br ON ev USING brin (ts);
ANALYZE ev;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
EXPLAIN (COSTS OFF) SELECT * FROM ev WHERE ts >= '2026-01-02 00:00 Europe/Berlin' AND ts < '2026-01-02 01:00 Europe/Berlin';
SELECT id, ts FROM ev WHERE ts >= '2026-01-02 00:00 Europe/Berlin' AND ts < '2026-01-02 00:05 Europe/Berlin' ORDER BY ts;
SELECT id FROM ev WHERE ts = '2026-01-01 05:31 +05:30';
SET enable_indexscan = off;
SET enable_bitmapscan = on;
SELECT count(*) FROM ev WHERE ts BETWEEN '2026-01-03 00:00 UTC' AND '2026-01-04 00:00 UTC';
RESET enable_seqscan;
RESET enable_indexscan;
RESET enable_bitmapscan;
SELECT min(ts), max(ts) FROM ev;
SELECT count(*) FROM ev a JOIN ev b ON a.ts = b.ts;
SELECT count(DISTINCT ts) FROM ev;
CREATE TABLE u (ts ora_timestamptz PRIMARY KEY);
INSERT INTO u VALUES ('2026-10-16 12:00 UTC');
INSERT INTO u VALUES ('2026-10-16 14:00 Europe/Berlin');

-- binary send
SELECT ora_timestamptz_send('2026-10-16 14:30:00.5 Europe/Berlin'::ora_timestamptz(1));
SELECT ora_timestamptz_identical(x, x) FROM (SELECT '2026-10-16 14:30:00.123456789 Europe/Berlin'::ora_timestamptz(9) AS x) s;

-- tzdata change support (no change expected here)
SELECT ora_timestamptz_tzdata_changed('2026-10-16 14:30 Europe/Berlin'),
       ora_timestamptz_tzdata_fix('2026-10-16 14:30 Europe/Berlin');

DROP EXTENSION ora_timestamptz CASCADE;
