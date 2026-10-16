# ora_timestamptz – Oracle `TIMESTAMP WITH TIME ZONE` für PostgreSQL 17

[English](README.md) | **Deutsch**

PostgreSQLs `timestamptz` rechnet beim Speichern nach UTC um und **verwirft die
Zeitzone**. Oracles `TIMESTAMP WITH TIME ZONE` behält sie. Diese Extension
liefert den Datentyp `ora_timestamptz`, der das Oracle-Verhalten nachbildet:
Jeder Wert speichert den Zeitpunkt (UTC, Nanosekunden) **und** seine Zone,
entweder als festen Offset (`+02:00`) oder als Region (`Europe/Berlin`).

```sql
CREATE EXTENSION ora_timestamptz;

CREATE TABLE buchung (id bigint PRIMARY KEY, erstellt ora_timestamptz(6));
INSERT INTO buchung VALUES
  (1, '2026-10-16 14:30:00 Europe/Berlin'),
  (2, '2026-10-16 08:30:00 -04:00'),
  (3, '2026-10-16 21:30:00+09:00[Asia/Tokyo]');

SELECT * FROM buchung;
--  1 | 2026-10-16 14:30:00.000000+02:00[Europe/Berlin]
--  2 | 2026-10-16 08:30:00.000000-04:00
--  3 | 2026-10-16 21:30:00.000000+09:00[Asia/Tokyo]

SELECT count(DISTINCT erstellt) FROM buchung;   -- 1: gleicher Zeitpunkt (Oracle-Semantik)
```

## Bauen und installieren

Voraussetzung ist PostgreSQL **17** mit den Server-Entwicklungsdateien und ein
C-Compiler. Ein gepatchtes PostgreSQL ist **nicht** nötig. Die Extension wird
per PGXS gegen eine ganz normale Installation gebaut.

### Debian / Ubuntu (PGDG- oder Distributionspakete)

```sh
sudo apt install build-essential postgresql-server-dev-17
git clone https://github.com/zwiebelchen/ora_timestamptz.git
cd ora_timestamptz
make PG_CONFIG=/usr/lib/postgresql/17/bin/pg_config
sudo make PG_CONFIG=/usr/lib/postgresql/17/bin/pg_config install
```

### RHEL / AlmaLinux / Rocky (PGDG-Pakete)

```sh
sudo dnf install gcc make redhat-rpm-config postgresql17-devel
git clone https://github.com/zwiebelchen/ora_timestamptz.git
cd ora_timestamptz
make PG_CONFIG=/usr/pgsql-17/bin/pg_config
sudo make PG_CONFIG=/usr/pgsql-17/bin/pg_config install
```

Meldet der Build fehlendes `clang`/LLVM (JIT-Bitcode), entweder `clang` und
`llvm` installieren oder ohne Bitcode bauen: `make with_llvm=no ...` bzw.
`sudo make with_llvm=no ... install`.

### Was installiert wird

| Datei | Ziel |
|---|---|
| `ora_timestamptz.so` | `$(pg_config --pkglibdir)` |
| `ora_timestamptz.control`, `ora_timestamptz--1.0.sql` | `$(pg_config --sharedir)/extension/` |
| `README*.md` | `$(pg_config --docdir)/extension/` |

## In PostgreSQL einbinden

```sql
-- als Superuser, in jeder Datenbank, die den Typ braucht
CREATE EXTENSION ora_timestamptz;

-- prüfen
SELECT extversion FROM pg_extension WHERE extname = 'ora_timestamptz';
SELECT '2026-10-16 14:30 Europe/Berlin'::ora_timestamptz;
```

- Ein Neustart oder ein Eintrag in `shared_preload_libraries` ist **nicht**
  nötig.
- `CREATE EXTENSION` braucht Superuser-Rechte, weil die Extension Funktionen in
  `pg_catalog` anlegt. Nur so funktionieren `AT TIME ZONE`, `AT LOCAL` und
  `EXTRACT` mit der normalen SQL-Syntax.
- Für alle neuen Datenbanken: einmal in `template1` ausführen.
- Entfernen: `DROP EXTENSION ora_timestamptz;` (mit `CASCADE` werden auch
  Spalten dieses Typs gelöscht).

### Betrieb

- **Jeder Server braucht die Bibliothek:** Primary, Streaming-Replicas und
  Restore-Ziele müssen die Extension installiert haben, sonst lassen sich die
  Daten dort nicht lesen.
- **`pg_dump`/`pg_restore`:** Der Dump enthält nur `CREATE EXTENSION`; auf dem
  Zielserver muss die Extension installiert sein. Das Textformat ist
  verlustfrei.
- **Minor-Updates (17.x):** kein Neubau nötig. **Major-Upgrade** (z. B. auf 18):
  die Extension gegen die neue Version bauen und installieren, bevor
  `pg_upgrade` läuft. Getestet ist bisher nur PostgreSQL 17.
- **Managed-Dienste** (AWS RDS/Aurora, Azure, Cloud SQL) erlauben keine eigenen
  C-Extensions. Dort ist der Einsatz nicht möglich.

### Tests

```sh
make PG_CONFIG=... installcheck   # gegen die laufende, installierte Instanz
```

## Verhalten (Oracle 1:1)

| Thema | Verhalten |
|---|---|
| Speicherung | UTC-Zeitpunkt + Offset **oder** Region (IANA-Name, kanonisiert: `europe/berlin` → `Europe/Berlin`) |
| Werte ohne Zone | bekommen die Session-Zeitzone (`TimeZone`), als Region bzw. als Offset, wie Oracle `SESSIONTIMEZONE` |
| Gleichheit, Sortierung, Indizes | nach UTC-Zeitpunkt: `'08:00 -08:00' = '11:00 -05:00'` ist `true` |
| Präzision | `ora_timestamptz(0..9)`, Default 6, es wird **gerundet** |
| Wertebereich | Jahr -4712 bis 9999 (ORA-01841) |
| DST-Lücke (z. B. 29.03. 02:30 Berlin) | Fehler, wie ORA-01878 |
| DST-Überlappung (25.10. 02:30 Berlin) | **Standardzeit**, wie Oracle mit `ERROR_ON_OVERLAP_TIME=FALSE`; eindeutig per TZD (`Europe/Berlin CEST`) oder Offset (`+02:00[Europe/Berlin]`) |
| `ora_timestamptz.error_on_overlap_time = on` | Überlappung ergibt einen Fehler (ORA-01883) |
| Offset-Bereich | -12:59 bis +14:59 (ORA-01874) |
| Arithmetik | **immer in UTC**, die Zone des Operanden bleibt: `x + interval '1 day'` = +86400 s |
| Monatsarithmetik | in UTC; ungültiger Tag ergibt einen Fehler (ORA-01839), kein Kappen auf den Monatsletzten |
| `a - b` | `interval` mit Tagen + Uhrzeit (wie `INTERVAL DAY TO SECOND`) |
| `EXTRACT(YEAR/MONTH/DAY/HOUR/MINUTE/SECOND ...)` | liefert **UTC-Felder** (wie Oracle) |
| `EXTRACT(TIMEZONE_HOUR/TIMEZONE_MINUTE ...)` | aus dem Offset des Werts (negativ bei Westzonen: `-03:30` ergibt -3 / -30) |

Beispiel aus der Oracle-Doku (Example 4-6), identisches Ergebnis:

```sql
SELECT '2000-10-28 23:24:54 America/New_York'::ora_timestamptz(0) + interval '8 hours';
-- 2000-10-29 06:24:54-05:00[America/New_York]
```

## Ein- und Ausgabeformat

**Eingabe** (Typ-Cast, `INSERT`, JDBC):

```
YYYY-MM-DD[ |T]HH24:MI[:SS[.F{1..9}]] [Zone]
Zone:  (leer)              -> Session-Zeitzone
       Z | +HH | +HH:MI | +HHMI
       Region [TZD]          Europe/Berlin CEST
       [Region]              RFC 9557
       +HH:MI[Region]        RFC 9557 / java.time (Offset disambiguiert)
       +HH:MI Region
```

Passt der Text nicht, wird wie bei Oracles impliziter Konvertierung
`ora_timestamptz.nls_timestamp_tz_format` versucht, z. B.
`'16-OCT-26 02.30.05.000000 PM US/Pacific'`.

**Ausgabe:** Standard ist `ora_timestamptz.output_format = 'iso'`:
`2026-10-16 14:30:00.123456+02:00[Europe/Berlin]`. Das Format ist verlustfrei
(auch in der DST-Überlappung) und für `pg_dump`/`COPY` und Java geeignet.
Mit `'nls'` wird stattdessen `nls_timestamp_tz_format` verwendet. Das sieht aus
wie Oracle, ist aber **verlustbehaftet** (z. B. `RR`, Überlappung) und darf
nicht für Dumps verwendet werden.

## Funktionen und Syntax

| Oracle | PostgreSQL mit ora_timestamptz |
|---|---|
| `x AT TIME ZONE 'Europe/Paris'` | identisch (auch `'+05:30'`, `'SESSIONTIMEZONE'`, `'DBTIMEZONE'`, `INTERVAL '05:45'`) |
| `x AT LOCAL` | identisch |
| `EXTRACT(HOUR FROM x)` | identisch (UTC!) |
| `EXTRACT(TIMEZONE_REGION FROM x)` / `TIMEZONE_ABBR` | `ora_extract('timezone_region', x)`, `ora_extract('timezone_abbr', x)`: PostgreSQLs `EXTRACT` kann nur `numeric` liefern. Für Offset-Werte kommen wie in Oracle `UNKNOWN` bzw. `UNK` zurück. |
| `FROM_TZ(ts, 'Europe/Berlin')` | `from_tz(ts, 'Europe/Berlin')`, auch `'Europe/Berlin CEST'` |
| `SYS_EXTRACT_UTC(x)` | `sys_extract_utc(x)` → `timestamp` |
| `TZ_OFFSET('Europe/Berlin')` | `tz_offset('Europe/Berlin')` |
| `SESSIONTIMEZONE`, `DBTIMEZONE` | `sessiontimezone()`, `dbtimezone()` |
| `SYSTIMESTAMP` | `systimestamp()` (Offset der Server-Zeitzone `log_timezone`) |
| `CURRENT_TIMESTAMP` | `ora_current_timestamp([p])` (Session-Zone, ggf. Region) |
| `TO_CHAR(x[, fmt])` | `to_char(x[, fmt])` |
| `TO_TIMESTAMP_TZ(s[, fmt])` | `to_timestamp_tz(s[, fmt])` |
| `CAST(x AS TIMESTAMP)` / `AS DATE` | `x::timestamp` (lokale Wanduhrzeit) / `x::date` |

Formatelemente für `to_char`/`to_timestamp_tz`: `SYYYY YYYY YYY YY Y RRRR RR
MM MON MONTH DD DDD D DY DAY J HH HH12 HH24 MI SS SSSSS FF FF1..FF9 X AM PM
A.M. P.M. TZH TZM TZR TZD FM "Text"` und Satzzeichen. Die Schreibweise steuert
die Groß-/Kleinschreibung wie in Oracle (`MON` → `OCT`, `Mon` → `Oct`;
`TZR` → `EUROPE/BERLIN`, `tzr` → `Europe/Berlin`). Monats- und Tagesnamen
sind englisch (`NLS_DATE_LANGUAGE=AMERICAN`). `X` gibt immer `.` aus.

Weitere Funktionen:

- `ora_timestamptz_identical(a, b)` prüft gleichen Zeitpunkt **und** gleiche Zone.
- `ora_timestamptz_iso(x)` liefert verlustfreien Text unabhängig von `output_format`.
- `min()`, `max()`, B-Tree-, Hash- und BRIN-Operator-Klassen sind vorhanden.
  PK und UNIQUE sind möglich, mit Oracle-Semantik: ein zweiter Wert mit gleichem
  Zeitpunkt in anderer Zone ist ein Duplikat.

### Casts

| von → nach | Art | Bedeutung |
|---|---|---|
| `timestamptz` → `ora_timestamptz` | implizit | Zeitpunkt + Session-Zone |
| `timestamp` → `ora_timestamptz` | implizit | Wanduhrzeit in der Session-Zone (wie Oracle) |
| `date` → `ora_timestamptz` | implizit | Mitternacht in der Session-Zone |
| `ora_timestamptz` → `timestamptz` | Zuweisung | Zeitpunkt, Zone geht verloren |
| `ora_timestamptz` → `timestamp` / `date` | Zuweisung | lokale Wanduhrzeit / Datum der gespeicherten Zone |

Dadurch funktionieren `WHERE col >= now()` und `INSERT ... VALUES (now())`
inklusive Indexnutzung.

## GUCs

| Parameter | Default | Oracle-Entsprechung |
|---|---|---|
| `ora_timestamptz.output_format` | `iso` | – |
| `ora_timestamptz.nls_timestamp_tz_format` | `DD-MON-RR HH.MI.SSXFF AM TZR` | `NLS_TIMESTAMP_TZ_FORMAT` |
| `ora_timestamptz.error_on_overlap_time` | `off` | `ERROR_ON_OVERLAP_TIME` |

Die Session-Zeitzone ist PostgreSQLs `TimeZone`. Achtung bei
PostgreSQL: `SET TIME ZONE '+03:00'` wird POSIX-artig als **UTC−3** gelesen.
Besser ist eine Region oder `SET TIME ZONE INTERVAL '+03:00' HOUR TO MINUTE`.

## Zeitzonendaten-Updates (entspricht DBMS_DST)

Region-Werte speichern den UTC-Zeitpunkt. Ändern neue tzdata die Regeln einer
Region rückwirkend, verschiebt sich die angezeigte lokale Zeit, genau wie in
Oracle vor einem DST-Upgrade. Pro Wert wird zusätzlich der Offset beim Anlegen
gespeichert, damit sich betroffene Werte finden und reparieren lassen. Die
Reparatur erhält die ursprüngliche lokale Uhrzeit:

```sql
SELECT count(*) FROM buchung WHERE ora_timestamptz_tzdata_changed(erstellt);
UPDATE buchung SET erstellt = ora_timestamptz_tzdata_fix(erstellt)
 WHERE ora_timestamptz_tzdata_changed(erstellt);
```

## Migration aus Oracle

- In DDLs `TIMESTAMP(n) WITH TIME ZONE` durch `ora_timestamptz(n)` ersetzen.
  Der PostgreSQL-Parser bildet die SQL-Standard-Syntax fest auf `timestamptz`
  ab. In ora2pg geht das z. B. mit
  `DATA_TYPE TIMESTAMP WITH TIME ZONE:ora_timestamptz`.
- Für den Export aus Oracle verlustfrei
  `TO_CHAR(col, 'YYYY-MM-DD HH24:MI:SS.FF9 TZH:TZM TZR')` verwenden. Die
  Extension liest das Format `Offset Region` direkt ein.
- `SYSTIMESTAMP`/`SESSIONTIMEZONE` brauchen `()`.
- `tstz + NUMBER` (Tage) ist bewusst nicht implementiert (Oracle konvertiert
  dabei nach `DATE` und verliert die Zone). Stattdessen `+ interval '1 day'`
  verwenden.
- Kalender: proleptisch gregorianisch (wie PostgreSQL, Joda-Time und
  java.time). Oracle rechnet vor dem 15.10.1582 julianisch.
- Textein-/-ausgabe nutzt die ISO-Jahreszählung (`0000` = 1 v. Chr.),
  `to_char(..., 'SYYYY')` und `EXTRACT(YEAR ...)` die Oracle-Zählung.

## Java: Joda-Time + pgJDBC

Im Verzeichnis [`java/`](java/) liegt ein Maven-Modul
`io.github.zwiebelchen:ora-timestamptz-jdbc` (Java 8+, Joda-Time 2.14,
pgJDBC 42.7).

```java
// optional, pro Connection: getObject() liefert dann PGOraTimestampTz
PGOraTimestampTz.register(connection);

// schreiben: Region bzw. Offset der Joda-DateTime bleiben erhalten
DateTime dt = new DateTime(DateTimeZone.forID("Europe/Berlin"));
try (PreparedStatement ps = connection.prepareStatement(
        "INSERT INTO buchung (id, erstellt) VALUES (?, ?)")) {
    ps.setLong(1, 4);
    OraTimestampTzJdbc.setDateTime(ps, 2, dt);
    ps.executeUpdate();
}

// lesen
DateTime gelesen = OraTimestampTzJdbc.getDateTime(rs, "erstellt");   // Zone = Europe/Berlin
OraTimestampTz voll = OraTimestampTzJdbc.get(rs, "erstellt");        // inkl. Nanosekunden
ZonedDateTime z = voll.toZonedDateTime();
```

Wichtig:

- **Nicht** `setTimestamp(...)` oder `setObject(i, OffsetDateTime)` für
  `ora_timestamptz`-Parameter verwenden. pgJDBC sendet diese als
  `timestamp`/`timestamptz`; der Server setzt dann die Session-Zone ein, und
  die ursprüngliche Region bzw. der Offset geht verloren. Die Session-Zone ist
  bei pgJDBC die JVM-Default-Zeitzone beim Verbindungsaufbau. Für reine
  Vergleiche (`WHERE ts = ?`) ist das unkritisch, weil nach Zeitpunkt verglichen
  wird.
- Die Registrierung ist optional. Ohne sie liefert pgJDBC ein `PGobject`, das
  `OraTimestampTzJdbc` ebenso verarbeitet. Das ist praktisch bei
  Connection-Pools ohne Init-Hook.
- Joda-Time hat Millisekunden-Auflösung. Für 7 bis 9 Nachkommastellen
  `OraTimestampTz.toZonedDateTime()` verwenden.
- Beim Schreiben wird `Lokalzeit+Offset[Region]` gesendet. Der Server prüft den
  Offset gegen **seine** tzdata. Weichen JVM-/Joda-tzdata und Server-tzdata für
  diesen Zeitpunkt voneinander ab, schlägt das Insert fehl, statt still einen
  anderen Zeitpunkt zu speichern. Deshalb tzdata von PostgreSQL (bzw. dem OS bei
  `--with-system-tzdata`), JDK und Joda-Time aktuell und synchron halten.
- Kennt Joda eine Region nicht, wird beim Lesen auf den gespeicherten Offset
  zurückgefallen.
- `OraTimestampTz.equals()` bedeutet gleicher Zeitpunkt **und** gleiche Zone.
  `isSameInstant()`/`compareTo()` vergleichen wie die Datenbank nur den
  Zeitpunkt.
- Hibernate/JPA: Ein `UserType` ist nicht enthalten, lässt sich aber direkt auf
  `OraTimestampTzJdbc.get`/`set` aufbauen.

Einbinden: Modul einmal bauen und ins lokale Repository bzw. euer Artifactory
legen (`cd java && mvn install`), dann als Dependency eintragen:

```xml
<dependency>
  <groupId>io.github.zwiebelchen</groupId>
  <artifactId>ora-timestamptz-jdbc</artifactId>
  <version>1.0.0-SNAPSHOT</version>
</dependency>
```

Tests: `mvn test` führt die Unit-Tests aus. Die Integrationstests gegen eine
Datenbank laufen mit
`mvn test -Doratstz.jdbcUrl='jdbc:postgresql://localhost/test?user=test'`.

## Interne Darstellung

Varlena, 22 Bytes + Länge des Regionsnamens: `int64` Sekunden seit
1970-01-01 UTC, `int32` Nanosekunden, `int32` Offset (bei Regionen der Offset
beim Anlegen), Flags, Präzision und der Regionsname. Das Binärformat
(`send`/`recv`) ist dasselbe in Netzwerk-Byte-Reihenfolge, der Regionsname ist
nullterminiert.
