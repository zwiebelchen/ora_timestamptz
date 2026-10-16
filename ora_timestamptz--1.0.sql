/* ora_timestamptz/ora_timestamptz--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION ora_timestamptz" to load this file. \quit

--
-- The type
--
CREATE TYPE ora_timestamptz;

CREATE FUNCTION ora_timestamptz_in(cstring, oid, integer)
RETURNS ora_timestamptz AS 'MODULE_PATHNAME' LANGUAGE C STABLE STRICT PARALLEL SAFE;

CREATE FUNCTION ora_timestamptz_out(ora_timestamptz)
RETURNS cstring AS 'MODULE_PATHNAME' LANGUAGE C STABLE STRICT PARALLEL SAFE;

CREATE FUNCTION ora_timestamptz_recv(internal, oid, integer)
RETURNS ora_timestamptz AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION ora_timestamptz_send(ora_timestamptz)
RETURNS bytea AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION ora_timestamptz_typmod_in(cstring[])
RETURNS integer AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION ora_timestamptz_typmod_out(integer)
RETURNS cstring AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE TYPE ora_timestamptz (
	INPUT = ora_timestamptz_in,
	OUTPUT = ora_timestamptz_out,
	RECEIVE = ora_timestamptz_recv,
	SEND = ora_timestamptz_send,
	TYPMOD_IN = ora_timestamptz_typmod_in,
	TYPMOD_OUT = ora_timestamptz_typmod_out,
	INTERNALLENGTH = VARIABLE,
	ALIGNMENT = double,
	STORAGE = plain,
	CATEGORY = 'D',
	PREFERRED = false
);

COMMENT ON TYPE ora_timestamptz IS
	'Oracle-compatible TIMESTAMP WITH TIME ZONE: keeps the offset or region of each value';

-- length coercion (fractional seconds precision)
CREATE FUNCTION ora_timestamptz(ora_timestamptz, integer, boolean)
RETURNS ora_timestamptz AS 'MODULE_PATHNAME', 'ora_timestamptz_scale'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE CAST (ora_timestamptz AS ora_timestamptz)
	WITH FUNCTION ora_timestamptz(ora_timestamptz, integer, boolean) AS IMPLICIT;

--
-- Comparison: on the UTC instant, like Oracle
--
CREATE FUNCTION ora_timestamptz_eq(ora_timestamptz, ora_timestamptz) RETURNS boolean
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE LEAKPROOF;
CREATE FUNCTION ora_timestamptz_ne(ora_timestamptz, ora_timestamptz) RETURNS boolean
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE LEAKPROOF;
CREATE FUNCTION ora_timestamptz_lt(ora_timestamptz, ora_timestamptz) RETURNS boolean
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE LEAKPROOF;
CREATE FUNCTION ora_timestamptz_le(ora_timestamptz, ora_timestamptz) RETURNS boolean
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE LEAKPROOF;
CREATE FUNCTION ora_timestamptz_gt(ora_timestamptz, ora_timestamptz) RETURNS boolean
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE LEAKPROOF;
CREATE FUNCTION ora_timestamptz_ge(ora_timestamptz, ora_timestamptz) RETURNS boolean
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE LEAKPROOF;
CREATE FUNCTION ora_timestamptz_cmp(ora_timestamptz, ora_timestamptz) RETURNS integer
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE LEAKPROOF;
CREATE FUNCTION ora_timestamptz_sortsupport(internal) RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION ora_timestamptz_hash(ora_timestamptz) RETURNS integer
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE LEAKPROOF;
CREATE FUNCTION ora_timestamptz_hash_extended(ora_timestamptz, bigint) RETURNS bigint
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE LEAKPROOF;

CREATE OPERATOR = (
	LEFTARG = ora_timestamptz, RIGHTARG = ora_timestamptz,
	PROCEDURE = ora_timestamptz_eq, COMMUTATOR = =, NEGATOR = <>,
	RESTRICT = eqsel, JOIN = eqjoinsel, HASHES, MERGES
);
CREATE OPERATOR <> (
	LEFTARG = ora_timestamptz, RIGHTARG = ora_timestamptz,
	PROCEDURE = ora_timestamptz_ne, COMMUTATOR = <>, NEGATOR = =,
	RESTRICT = neqsel, JOIN = neqjoinsel
);
CREATE OPERATOR < (
	LEFTARG = ora_timestamptz, RIGHTARG = ora_timestamptz,
	PROCEDURE = ora_timestamptz_lt, COMMUTATOR = >, NEGATOR = >=,
	RESTRICT = scalarltsel, JOIN = scalarltjoinsel
);
CREATE OPERATOR <= (
	LEFTARG = ora_timestamptz, RIGHTARG = ora_timestamptz,
	PROCEDURE = ora_timestamptz_le, COMMUTATOR = >=, NEGATOR = >,
	RESTRICT = scalarlesel, JOIN = scalarlejoinsel
);
CREATE OPERATOR > (
	LEFTARG = ora_timestamptz, RIGHTARG = ora_timestamptz,
	PROCEDURE = ora_timestamptz_gt, COMMUTATOR = <, NEGATOR = <=,
	RESTRICT = scalargtsel, JOIN = scalargtjoinsel
);
CREATE OPERATOR >= (
	LEFTARG = ora_timestamptz, RIGHTARG = ora_timestamptz,
	PROCEDURE = ora_timestamptz_ge, COMMUTATOR = <=, NEGATOR = <,
	RESTRICT = scalargesel, JOIN = scalargejoinsel
);

CREATE OPERATOR CLASS ora_timestamptz_ops
DEFAULT FOR TYPE ora_timestamptz USING btree AS
	OPERATOR 1 <,
	OPERATOR 2 <=,
	OPERATOR 3 =,
	OPERATOR 4 >=,
	OPERATOR 5 >,
	FUNCTION 1 ora_timestamptz_cmp(ora_timestamptz, ora_timestamptz),
	FUNCTION 2 ora_timestamptz_sortsupport(internal);

CREATE OPERATOR CLASS ora_timestamptz_ops
DEFAULT FOR TYPE ora_timestamptz USING hash AS
	OPERATOR 1 =,
	FUNCTION 1 ora_timestamptz_hash(ora_timestamptz),
	FUNCTION 2 ora_timestamptz_hash_extended(ora_timestamptz, bigint);

CREATE OPERATOR CLASS ora_timestamptz_minmax_ops
DEFAULT FOR TYPE ora_timestamptz USING brin AS
	OPERATOR 1 <,
	OPERATOR 2 <=,
	OPERATOR 3 =,
	OPERATOR 4 >=,
	OPERATOR 5 >,
	FUNCTION 1 brin_minmax_opcinfo(internal),
	FUNCTION 2 brin_minmax_add_value(internal, internal, internal, internal),
	FUNCTION 3 brin_minmax_consistent(internal, internal, internal),
	FUNCTION 4 brin_minmax_union(internal, internal, internal);

-- same instant AND same zone
CREATE FUNCTION ora_timestamptz_identical(ora_timestamptz, ora_timestamptz) RETURNS boolean
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
COMMENT ON FUNCTION ora_timestamptz_identical(ora_timestamptz, ora_timestamptz) IS
	'true if both values denote the same instant and carry the same zone';

--
-- min / max
--
CREATE FUNCTION ora_timestamptz_larger(ora_timestamptz, ora_timestamptz) RETURNS ora_timestamptz
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION ora_timestamptz_smaller(ora_timestamptz, ora_timestamptz) RETURNS ora_timestamptz
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE AGGREGATE max(ora_timestamptz) (
	SFUNC = ora_timestamptz_larger, STYPE = ora_timestamptz,
	SORTOP = >, COMBINEFUNC = ora_timestamptz_larger, PARALLEL = SAFE
);
CREATE AGGREGATE min(ora_timestamptz) (
	SFUNC = ora_timestamptz_smaller, STYPE = ora_timestamptz,
	SORTOP = <, COMBINEFUNC = ora_timestamptz_smaller, PARALLEL = SAFE
);

--
-- Arithmetic (Oracle: evaluated in UTC, zone of the operand is kept)
--
CREATE FUNCTION ora_timestamptz_pl_interval(ora_timestamptz, interval) RETURNS ora_timestamptz
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION ora_timestamptz_interval_pl(interval, ora_timestamptz) RETURNS ora_timestamptz
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION ora_timestamptz_mi_interval(ora_timestamptz, interval) RETURNS ora_timestamptz
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION ora_timestamptz_mi(ora_timestamptz, ora_timestamptz) RETURNS interval
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OPERATOR + (
	LEFTARG = ora_timestamptz, RIGHTARG = interval,
	PROCEDURE = ora_timestamptz_pl_interval, COMMUTATOR = +
);
CREATE OPERATOR + (
	LEFTARG = interval, RIGHTARG = ora_timestamptz,
	PROCEDURE = ora_timestamptz_interval_pl, COMMUTATOR = +
);
CREATE OPERATOR - (
	LEFTARG = ora_timestamptz, RIGHTARG = interval,
	PROCEDURE = ora_timestamptz_mi_interval
);
CREATE OPERATOR - (
	LEFTARG = ora_timestamptz, RIGHTARG = ora_timestamptz,
	PROCEDURE = ora_timestamptz_mi
);

--
-- Casts
--
CREATE FUNCTION ora_timestamptz_to_timestamptz(ora_timestamptz) RETURNS timestamptz
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION ora_timestamptz_to_timestamp(ora_timestamptz) RETURNS timestamp
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION ora_timestamptz_to_date(ora_timestamptz) RETURNS date
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION ora_timestamptz(timestamptz) RETURNS ora_timestamptz
AS 'MODULE_PATHNAME', 'timestamptz_to_ora_timestamptz' LANGUAGE C STABLE STRICT PARALLEL SAFE;
CREATE FUNCTION ora_timestamptz(timestamp) RETURNS ora_timestamptz
AS 'MODULE_PATHNAME', 'timestamp_to_ora_timestamptz' LANGUAGE C STABLE STRICT PARALLEL SAFE;
CREATE FUNCTION ora_timestamptz(date) RETURNS ora_timestamptz
AS 'MODULE_PATHNAME', 'date_to_ora_timestamptz' LANGUAGE C STABLE STRICT PARALLEL SAFE;

-- Oracle converts DATE/TIMESTAMP/TSLTZ implicitly to TSTZ (session time zone)
CREATE CAST (timestamptz AS ora_timestamptz)
	WITH FUNCTION ora_timestamptz(timestamptz) AS IMPLICIT;
CREATE CAST (timestamp AS ora_timestamptz)
	WITH FUNCTION ora_timestamptz(timestamp) AS IMPLICIT;
CREATE CAST (date AS ora_timestamptz)
	WITH FUNCTION ora_timestamptz(date) AS IMPLICIT;
-- the other direction loses information: assignment only
CREATE CAST (ora_timestamptz AS timestamptz)
	WITH FUNCTION ora_timestamptz_to_timestamptz(ora_timestamptz) AS ASSIGNMENT;
CREATE CAST (ora_timestamptz AS timestamp)
	WITH FUNCTION ora_timestamptz_to_timestamp(ora_timestamptz) AS ASSIGNMENT;
CREATE CAST (ora_timestamptz AS date)
	WITH FUNCTION ora_timestamptz_to_date(ora_timestamptz) AS ASSIGNMENT;

--
-- SQL syntax hooks.  The grammar maps these to pg_catalog functions:
--   x AT TIME ZONE z   -> pg_catalog.timezone(z, x)
--   x AT LOCAL         -> pg_catalog.timezone(x)
--   EXTRACT(f FROM x)  -> pg_catalog.extract(f, x)
--
CREATE FUNCTION pg_catalog.timezone(text, ora_timestamptz) RETURNS ora_timestamptz
AS 'MODULE_PATHNAME', 'ora_timestamptz_at_zone' LANGUAGE C STABLE STRICT PARALLEL SAFE;
CREATE FUNCTION pg_catalog.timezone(interval, ora_timestamptz) RETURNS ora_timestamptz
AS 'MODULE_PATHNAME', 'ora_timestamptz_at_interval' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION pg_catalog.timezone(ora_timestamptz) RETURNS ora_timestamptz
AS 'MODULE_PATHNAME', 'ora_timestamptz_at_local' LANGUAGE C STABLE STRICT PARALLEL SAFE;
CREATE FUNCTION pg_catalog.extract(text, ora_timestamptz) RETURNS numeric
AS 'MODULE_PATHNAME', 'ora_timestamptz_extract' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION ora_extract(text, ora_timestamptz) RETURNS text
AS 'MODULE_PATHNAME', 'ora_timestamptz_extract_text' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
COMMENT ON FUNCTION ora_extract(text, ora_timestamptz) IS
	'EXTRACT returning text, also for TIMEZONE_REGION, TIMEZONE_ABBR and TIMEZONE_OFFSET';

--
-- Oracle functions
--
CREATE FUNCTION from_tz(timestamp, text) RETURNS ora_timestamptz
AS 'MODULE_PATHNAME', 'ora_from_tz' LANGUAGE C STABLE STRICT PARALLEL SAFE;

CREATE FUNCTION sys_extract_utc(ora_timestamptz) RETURNS timestamp
AS 'MODULE_PATHNAME', 'ora_sys_extract_utc' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION tz_offset(text) RETURNS text
AS 'MODULE_PATHNAME', 'ora_tz_offset' LANGUAGE C STABLE STRICT PARALLEL SAFE;

CREATE FUNCTION sessiontimezone() RETURNS text
AS 'MODULE_PATHNAME', 'ora_sessiontimezone' LANGUAGE C STABLE STRICT PARALLEL SAFE;

CREATE FUNCTION dbtimezone() RETURNS text
AS 'MODULE_PATHNAME', 'ora_dbtimezone' LANGUAGE C STABLE STRICT PARALLEL SAFE;

CREATE FUNCTION systimestamp() RETURNS ora_timestamptz
AS 'MODULE_PATHNAME', 'ora_systimestamp' LANGUAGE C STABLE STRICT PARALLEL SAFE;
CREATE FUNCTION systimestamp(integer) RETURNS ora_timestamptz
AS 'MODULE_PATHNAME', 'ora_systimestamp' LANGUAGE C STABLE STRICT PARALLEL SAFE;

CREATE FUNCTION ora_current_timestamp() RETURNS ora_timestamptz
AS 'MODULE_PATHNAME', 'ora_current_timestamp' LANGUAGE C STABLE STRICT PARALLEL SAFE;
CREATE FUNCTION ora_current_timestamp(integer) RETURNS ora_timestamptz
AS 'MODULE_PATHNAME', 'ora_current_timestamp' LANGUAGE C STABLE STRICT PARALLEL SAFE;

CREATE FUNCTION to_char(ora_timestamptz) RETURNS text
AS 'MODULE_PATHNAME', 'ora_timestamptz_to_char' LANGUAGE C STABLE STRICT PARALLEL SAFE;
CREATE FUNCTION to_char(ora_timestamptz, text) RETURNS text
AS 'MODULE_PATHNAME', 'ora_timestamptz_to_char' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION to_timestamp_tz(text) RETURNS ora_timestamptz
AS 'MODULE_PATHNAME', 'ora_to_timestamp_tz' LANGUAGE C STABLE STRICT PARALLEL SAFE;
CREATE FUNCTION to_timestamp_tz(text, text) RETURNS ora_timestamptz
AS 'MODULE_PATHNAME', 'ora_to_timestamp_tz' LANGUAGE C STABLE STRICT PARALLEL SAFE;

-- lossless ISO / RFC 9557 text, independent of ora_timestamptz.output_format
CREATE FUNCTION ora_timestamptz_iso(ora_timestamptz) RETURNS text
AS 'MODULE_PATHNAME', 'ora_timestamptz_iso' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

--
-- Time zone database updates (cf. Oracle DBMS_DST)
--
CREATE FUNCTION ora_timestamptz_tzdata_changed(ora_timestamptz) RETURNS boolean
AS 'MODULE_PATHNAME', 'ora_timestamptz_tzdata_changed' LANGUAGE C STABLE STRICT PARALLEL SAFE;
CREATE FUNCTION ora_timestamptz_tzdata_fix(ora_timestamptz) RETURNS ora_timestamptz
AS 'MODULE_PATHNAME', 'ora_timestamptz_tzdata_fix' LANGUAGE C STABLE STRICT PARALLEL SAFE;
