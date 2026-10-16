# ora_timestamptz – Oracle `TIMESTAMP WITH TIME ZONE` for PostgreSQL 17

**English** | [Deutsch](README.de.md)

PostgreSQL's `timestamptz` converts values to UTC on input and **discards the
time zone**. Oracle's `TIMESTAMP WITH TIME ZONE` keeps it. This extension
provides the data type `ora_timestamptz`, which reproduces Oracle's behavior:
every value stores the instant (UTC, nanoseconds) **and** its zone, either as a
fixed offset (`+02:00`) or as a region (`Europe/Berlin`).

```sql
CREATE EXTENSION ora_timestamptz;

CREATE TABLE booking (id bigint PRIMARY KEY, created ora_timestamptz(6));
INSERT INTO booking VALUES
  (1, '2026-10-16 14:30:00 Europe/Berlin'),
  (2, '2026-10-16 08:30:00 -04:00'),
  (3, '2026-10-16 21:30:00+09:00[Asia/Tokyo]');

SELECT * FROM booking;
--  1 | 2026-10-16 14:30:00.000000+02:00[Europe/Berlin]
--  2 | 2026-10-16 08:30:00.000000-04:00
--  3 | 2026-10-16 21:30:00.000000+09:00[Asia/Tokyo]

SELECT count(DISTINCT created) FROM booking;   -- 1: same instant (Oracle semantics)
```

## Building and installing

You need PostgreSQL **17** with the server development files and a C compiler.
A patched PostgreSQL is **not** required: the extension is built with PGXS
against a regular installation.

### Debian / Ubuntu (PGDG or distribution packages)

```sh
sudo apt install build-essential postgresql-server-dev-17
git clone https://github.com/zwiebelchen/ora_timestamptz.git
cd ora_timestamptz
make PG_CONFIG=/usr/lib/postgresql/17/bin/pg_config
sudo make PG_CONFIG=/usr/lib/postgresql/17/bin/pg_config install
```

### RHEL / AlmaLinux / Rocky (PGDG packages)

```sh
sudo dnf install gcc make redhat-rpm-config postgresql17-devel
git clone https://github.com/zwiebelchen/ora_timestamptz.git
cd ora_timestamptz
make PG_CONFIG=/usr/pgsql-17/bin/pg_config
sudo make PG_CONFIG=/usr/pgsql-17/bin/pg_config install
```

If the build complains about a missing `clang`/LLVM (JIT bitcode), either
install `clang` and `llvm`, or build without bitcode: `make with_llvm=no ...`
and `sudo make with_llvm=no ... install`.

### What gets installed

| File | Destination |
|---|---|
| `ora_timestamptz.so` | `$(pg_config --pkglibdir)` |
| `ora_timestamptz.control`, `ora_timestamptz--1.0.sql` | `$(pg_config --sharedir)/extension/` |
| `README*.md` | `$(pg_config --docdir)/extension/` |

## Enabling it in PostgreSQL

```sql
-- as superuser, in every database that needs the type
CREATE EXTENSION ora_timestamptz;

-- check
SELECT extversion FROM pg_extension WHERE extname = 'ora_timestamptz';
SELECT '2026-10-16 14:30 Europe/Berlin'::ora_timestamptz;
```

- No restart and no `shared_preload_libraries` entry are needed.
- `CREATE EXTENSION` requires superuser privileges because the extension
  creates functions in `pg_catalog`. That is the only way to make `AT TIME
  ZONE`, `AT LOCAL` and `EXTRACT` work with the regular SQL syntax.
- To have it in every new database: run it once in `template1`.
- Removal: `DROP EXTENSION ora_timestamptz;` (`CASCADE` also drops columns of
  this type).

### Operations

- **Every server needs the library:** primary, streaming replicas and restore
  targets must have the extension installed, otherwise the data cannot be read
  there.
- **`pg_dump`/`pg_restore`:** the dump only contains `CREATE EXTENSION`; the
  target server must have the extension installed. The text format is lossless.
- **Minor updates (17.x):** no rebuild needed. **Major upgrade** (e.g. to 18):
  build and install the extension for the new version before running
  `pg_upgrade`. So far only PostgreSQL 17 has been tested.
- **Managed services** (AWS RDS/Aurora, Azure, Cloud SQL) do not allow custom C
  extensions, so the extension cannot be used there.

### Tests

```sh
make PG_CONFIG=... installcheck   # against the running, installed instance
```

## Behavior (Oracle 1:1)

| Topic | Behavior |
|---|---|
| Storage | UTC instant + offset **or** region (IANA name, canonicalized: `europe/berlin` → `Europe/Berlin`) |
| Values without a zone | get the session time zone (`TimeZone`), as region or offset, like Oracle's `SESSIONTIMEZONE` |
| Equality, ordering, indexes | by UTC instant: `'08:00 -08:00' = '11:00 -05:00'` is `true` |
| Precision | `ora_timestamptz(0..9)`, default 6, values are **rounded** |
| Range | year -4712 to 9999 (ORA-01841) |
| DST gap (e.g. 29 Mar 02:30 Berlin) | error, like ORA-01878 |
| DST overlap (25 Oct 02:30 Berlin) | **standard time**, like Oracle with `ERROR_ON_OVERLAP_TIME=FALSE`; disambiguate with TZD (`Europe/Berlin CEST`) or an offset (`+02:00[Europe/Berlin]`) |
| `ora_timestamptz.error_on_overlap_time = on` | an overlap raises an error (ORA-01883) |
| Offset range | -12:59 to +14:59 (ORA-01874) |
| Arithmetic | **always in UTC**, the operand's zone is kept: `x + interval '1 day'` = +86400 s |
| Month arithmetic | in UTC; an invalid day raises an error (ORA-01839) instead of clamping to the last day of the month |
| `a - b` | `interval` with days + time of day (like `INTERVAL DAY TO SECOND`) |
| `EXTRACT(YEAR/MONTH/DAY/HOUR/MINUTE/SECOND ...)` | returns **UTC fields** (like Oracle) |
| `EXTRACT(TIMEZONE_HOUR/TIMEZONE_MINUTE ...)` | from the value's offset (negative for western zones: `-03:30` gives -3 / -30) |

Example from the Oracle documentation (Example 4-6), identical result:

```sql
SELECT '2000-10-28 23:24:54 America/New_York'::ora_timestamptz(0) + interval '8 hours';
-- 2000-10-29 06:24:54-05:00[America/New_York]
```

## Input and output format

**Input** (casts, `INSERT`, JDBC):

```
YYYY-MM-DD[ |T]HH24:MI[:SS[.F{1..9}]] [zone]
zone:  (empty)              -> session time zone
       Z | +HH | +HH:MI | +HHMI
       Region [TZD]           Europe/Berlin CEST
       [Region]               RFC 9557
       +HH:MI[Region]         RFC 9557 / java.time (offset disambiguates)
       +HH:MI Region
```

If the text does not match, `ora_timestamptz.nls_timestamp_tz_format` is tried,
like Oracle's implicit conversion, e.g.
`'16-OCT-26 02.30.05.000000 PM US/Pacific'`.

**Output:** by default `ora_timestamptz.output_format = 'iso'`:
`2026-10-16 14:30:00.123456+02:00[Europe/Berlin]`. This format is lossless
(including the DST overlap) and suitable for `pg_dump`/`COPY` and Java. With
`'nls'`, `nls_timestamp_tz_format` is used instead. That looks like Oracle but
is **lossy** (e.g. `RR`, overlap) and must not be used for dumps.

## Functions and syntax

| Oracle | PostgreSQL with ora_timestamptz |
|---|---|
| `x AT TIME ZONE 'Europe/Paris'` | identical (also `'+05:30'`, `'SESSIONTIMEZONE'`, `'DBTIMEZONE'`, `INTERVAL '05:45'`) |
| `x AT LOCAL` | identical |
| `EXTRACT(HOUR FROM x)` | identical (UTC!) |
| `EXTRACT(TIMEZONE_REGION FROM x)` / `TIMEZONE_ABBR` | `ora_extract('timezone_region', x)`, `ora_extract('timezone_abbr', x)`: PostgreSQL's `EXTRACT` can only return `numeric`. For offset values `UNKNOWN` / `UNK` are returned, as in Oracle. |
| `FROM_TZ(ts, 'Europe/Berlin')` | `from_tz(ts, 'Europe/Berlin')`, also `'Europe/Berlin CEST'` |
| `SYS_EXTRACT_UTC(x)` | `sys_extract_utc(x)` → `timestamp` |
| `TZ_OFFSET('Europe/Berlin')` | `tz_offset('Europe/Berlin')` |
| `SESSIONTIMEZONE`, `DBTIMEZONE` | `sessiontimezone()`, `dbtimezone()` |
| `SYSTIMESTAMP` | `systimestamp()` (offset of the server time zone `log_timezone`) |
| `CURRENT_TIMESTAMP` | `ora_current_timestamp([p])` (session zone, region if applicable) |
| `TO_CHAR(x[, fmt])` | `to_char(x[, fmt])` |
| `TO_TIMESTAMP_TZ(s[, fmt])` | `to_timestamp_tz(s[, fmt])` |
| `CAST(x AS TIMESTAMP)` / `AS DATE` | `x::timestamp` (local wall-clock time) / `x::date` |

Format elements for `to_char`/`to_timestamp_tz`: `SYYYY YYYY YYY YY Y RRRR RR
MM MON MONTH DD DDD D DY DAY J HH HH12 HH24 MI SS SSSSS FF FF1..FF9 X AM PM
A.M. P.M. TZH TZM TZR TZD FM "text"` and punctuation. The spelling controls
upper/lower case as in Oracle (`MON` → `OCT`, `Mon` → `Oct`; `TZR` →
`EUROPE/BERLIN`, `tzr` → `Europe/Berlin`). Month and day names are English
(`NLS_DATE_LANGUAGE=AMERICAN`). `X` always prints `.`.

More functions:

- `ora_timestamptz_identical(a, b)` checks for the same instant **and** the
  same zone.
- `ora_timestamptz_iso(x)` returns lossless text regardless of `output_format`.
- `min()`, `max()`, and B-tree, hash and BRIN operator classes are available.
  Primary keys and UNIQUE constraints work with Oracle semantics: a second value
  with the same instant in another zone is a duplicate.

### Casts

| from → to | kind | meaning |
|---|---|---|
| `timestamptz` → `ora_timestamptz` | implicit | instant + session zone |
| `timestamp` → `ora_timestamptz` | implicit | wall-clock time in the session zone (like Oracle) |
| `date` → `ora_timestamptz` | implicit | midnight in the session zone |
| `ora_timestamptz` → `timestamptz` | assignment | instant, the zone is lost |
| `ora_timestamptz` → `timestamp` / `date` | assignment | local wall-clock time / date in the stored zone |

This makes `WHERE col >= now()` and `INSERT ... VALUES (now())` work,
including index usage.

## Configuration parameters

| Parameter | Default | Oracle equivalent |
|---|---|---|
| `ora_timestamptz.output_format` | `iso` | – |
| `ora_timestamptz.nls_timestamp_tz_format` | `DD-MON-RR HH.MI.SSXFF AM TZR` | `NLS_TIMESTAMP_TZ_FORMAT` |
| `ora_timestamptz.error_on_overlap_time` | `off` | `ERROR_ON_OVERLAP_TIME` |

The session time zone is PostgreSQL's `TimeZone`. A PostgreSQL pitfall:
`SET TIME ZONE '+03:00'` is read POSIX-style as **UTC−3**. Use a region or
`SET TIME ZONE INTERVAL '+03:00' HOUR TO MINUTE` instead.

## Time zone database updates (equivalent of DBMS_DST)

Region values store the UTC instant. If new tzdata retroactively changes a
region's rules, the displayed local time shifts, exactly as in Oracle before a
DST upgrade. Each value additionally stores the offset at creation time, so
affected values can be found and repaired. The repair preserves the original
local time:

```sql
SELECT count(*) FROM booking WHERE ora_timestamptz_tzdata_changed(created);
UPDATE booking SET created = ora_timestamptz_tzdata_fix(created)
 WHERE ora_timestamptz_tzdata_changed(created);
```

## Migrating from Oracle

- Replace `TIMESTAMP(n) WITH TIME ZONE` with `ora_timestamptz(n)` in DDL. The
  PostgreSQL parser maps the SQL-standard syntax to `timestamptz` and cannot be
  hooked. With ora2pg, for example:
  `DATA_TYPE TIMESTAMP WITH TIME ZONE:ora_timestamptz`.
- For a lossless export from Oracle use
  `TO_CHAR(col, 'YYYY-MM-DD HH24:MI:SS.FF9 TZH:TZM TZR')`. The extension reads
  the `offset region` format directly.
- `SYSTIMESTAMP`/`SESSIONTIMEZONE` need `()`.
- `tstz + NUMBER` (days) is intentionally not implemented (Oracle converts to
  `DATE` there and loses the zone). Use `+ interval '1 day'` instead.
- Calendar: proleptic Gregorian (like PostgreSQL, Joda-Time and java.time).
  Oracle uses the Julian calendar before 15 Oct 1582.
- Text input/output uses ISO year numbering (`0000` = 1 BC);
  `to_char(..., 'SYYYY')` and `EXTRACT(YEAR ...)` use Oracle numbering.

## Java: Joda-Time + pgJDBC

The [`java/`](java/) directory contains a Maven module
`io.github.zwiebelchen:ora-timestamptz-jdbc` (Java 8+, Joda-Time 2.14,
pgJDBC 42.7).

```java
// optional, per connection: getObject() then returns PGOraTimestampTz
PGOraTimestampTz.register(connection);

// write: the region or offset of the Joda DateTime is preserved
DateTime dt = new DateTime(DateTimeZone.forID("Europe/Berlin"));
try (PreparedStatement ps = connection.prepareStatement(
        "INSERT INTO booking (id, created) VALUES (?, ?)")) {
    ps.setLong(1, 4);
    OraTimestampTzJdbc.setDateTime(ps, 2, dt);
    ps.executeUpdate();
}

// read
DateTime read = OraTimestampTzJdbc.getDateTime(rs, "created");     // zone = Europe/Berlin
OraTimestampTz full = OraTimestampTzJdbc.get(rs, "created");      // incl. nanoseconds
ZonedDateTime z = full.toZonedDateTime();
```

Using it: build the module once and put it into your local repository or your
artifact repository (`cd java && mvn install`), then add the dependency:

```xml
<dependency>
  <groupId>io.github.zwiebelchen</groupId>
  <artifactId>ora-timestamptz-jdbc</artifactId>
  <version>1.0.0-SNAPSHOT</version>
</dependency>
```

Important:

- Do **not** use `setTimestamp(...)` or `setObject(i, OffsetDateTime)` for
  `ora_timestamptz` parameters. pgJDBC sends those as
  `timestamp`/`timestamptz`; the server then applies the session zone and the
  original region or offset is lost. With pgJDBC the session zone is the JVM
  default time zone at connect time. For plain comparisons (`WHERE ts = ?`)
  this does not matter, because comparison is by instant.
- Registration is optional. Without it pgJDBC returns a `PGobject`, which
  `OraTimestampTzJdbc` handles as well. This is handy with connection pools that
  have no init hook.
- Joda-Time has millisecond resolution. For 7 to 9 fractional digits use
  `OraTimestampTz.toZonedDateTime()`.
- On write, `localtime+offset[region]` is sent. The server checks the offset
  against **its own** tzdata. If JVM/Joda tzdata and server tzdata disagree for
  that instant, the insert fails instead of silently storing a different
  instant. So keep the tzdata of PostgreSQL (or of the OS with
  `--with-system-tzdata`), the JDK and Joda-Time current and in sync.
- If Joda does not know a region, reading falls back to the stored offset.
- `OraTimestampTz.equals()` means same instant **and** same zone.
  `isSameInstant()`/`compareTo()` compare only the instant, like the database.
- Hibernate/JPA: no `UserType` is included, but one can be built directly on
  `OraTimestampTzJdbc.get`/`set`.

Tests: `mvn test` runs the unit tests. The integration tests against a database
run with
`mvn test -Doratstz.jdbcUrl='jdbc:postgresql://localhost/test?user=test'`.

## Internal representation

Varlena, 22 bytes + length of the region name: `int64` seconds since
1970-01-01 UTC, `int32` nanoseconds, `int32` offset (for regions the offset at
creation time), flags, precision, and the region name. The binary format
(`send`/`recv`) is the same in network byte order, with the region name
NUL-terminated.
