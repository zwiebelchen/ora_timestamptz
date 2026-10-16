package io.github.zwiebelchen.oratstz;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.junit.jupiter.api.Assumptions.assumeTrue;

import java.sql.Connection;
import java.sql.DriverManager;
import java.sql.PreparedStatement;
import java.sql.ResultSet;
import java.sql.SQLException;
import java.sql.Statement;
import java.time.Instant;
import java.time.ZoneId;
import java.time.ZonedDateTime;

import org.joda.time.DateTime;
import org.joda.time.DateTimeZone;
import org.junit.jupiter.api.Test;

/**
 * Integration test against a PostgreSQL 17 server with the extension.
 * Runs only if -Doratstz.jdbcUrl=jdbc:postgresql://host/db?user=... is given.
 */
class OraTimestampTzJdbcIT {

    private static Connection connect() throws SQLException {
        String url = System.getProperty("oratstz.jdbcUrl");
        assumeTrue(url != null && !url.isEmpty() && !url.startsWith("${"), "oratstz.jdbcUrl not set");
        Connection c = DriverManager.getConnection(url);
        try (Statement st = c.createStatement()) {
            st.execute("CREATE EXTENSION IF NOT EXISTS ora_timestamptz");
            st.execute("DROP TABLE IF EXISTS oratstz_it");
            st.execute("CREATE TABLE oratstz_it (id int PRIMARY KEY, ts ora_timestamptz(6))");
        }
        return c;
    }

    @Test
    void jodaRoundTripKeepsZone() throws Exception {
        try (Connection c = connect()) {
            PGOraTimestampTz.register(c);
            DateTime[] values = {
                new DateTime(2026, 10, 16, 14, 30, 0, 123, DateTimeZone.forID("Europe/Berlin")),
                new DateTime(2026, 10, 16, 14, 30, 0, 0, DateTimeZone.forOffsetHoursMinutes(-3, -30)),
                new DateTime(2026, 1, 1, 0, 0, DateTimeZone.UTC),
                // DST overlap: the earlier (CEST) 02:30 must survive
                new DateTime(2026, 10, 25, 2, 30, DateTimeZone.forID("Europe/Berlin")).withEarlierOffsetAtOverlap(),
                new DateTime(2026, 10, 25, 2, 30, DateTimeZone.forID("Europe/Berlin")).withLaterOffsetAtOverlap(),
                new DateTime(2026, 7, 1, 9, 15, DateTimeZone.forID("America/New_York")),
            };
            try (PreparedStatement ps = c.prepareStatement("INSERT INTO oratstz_it VALUES (?, ?)")) {
                for (int i = 0; i < values.length; i++) {
                    ps.setInt(1, i);
                    OraTimestampTzJdbc.setDateTime(ps, 2, values[i]);
                    ps.executeUpdate();
                }
                ps.setInt(1, 99);
                OraTimestampTzJdbc.setDateTime(ps, 2, null);
                ps.executeUpdate();
            }
            try (Statement st = c.createStatement();
                 ResultSet rs = st.executeQuery("SELECT id, ts FROM oratstz_it ORDER BY id")) {
                for (DateTime expected : values) {
                    assertTrue(rs.next());
                    Object raw = rs.getObject("ts");
                    assertTrue(raw instanceof PGOraTimestampTz, "registered type expected");
                    DateTime got = OraTimestampTzJdbc.getDateTime(rs, "ts");
                    assertEquals(expected, got);
                    assertEquals(expected.getZone(), got.getZone());
                }
                assertTrue(rs.next());
                assertNull(OraTimestampTzJdbc.getDateTime(rs, "ts"));
            }
        }
    }

    @Test
    void worksWithoutRegistrationAndComparesByInstant() throws Exception {
        try (Connection c = connect()) {
            ZonedDateTime z = ZonedDateTime.of(2026, 10, 16, 14, 30, 0, 123456000, ZoneId.of("Asia/Tokyo"));
            try (PreparedStatement ps = c.prepareStatement("INSERT INTO oratstz_it VALUES (1, ?)")) {
                OraTimestampTzJdbc.setZonedDateTime(ps, 1, z);
                ps.executeUpdate();
            }
            // same instant, different zone -> found (Oracle semantics)
            try (PreparedStatement ps = c.prepareStatement("SELECT ts, ts::text FROM oratstz_it WHERE ts = ?")) {
                OraTimestampTzJdbc.set(ps, 1, OraTimestampTz.ofRegion(z.toInstant(), "Europe/Berlin"));
                try (ResultSet rs = ps.executeQuery()) {
                    assertTrue(rs.next());
                    OraTimestampTz v = OraTimestampTzJdbc.get(rs, 1);
                    assertEquals("Asia/Tokyo", v.getRegion());
                    assertEquals(z, v.toZonedDateTime());
                    assertEquals(v, OraTimestampTz.parse(rs.getString(2)));
                }
            }
            // timestamptz parameters are compared by instant, too
            try (PreparedStatement ps = c.prepareStatement("SELECT count(*) FROM oratstz_it WHERE ts = ?")) {
                ps.setObject(1, z.toOffsetDateTime());
                try (ResultSet rs = ps.executeQuery()) {
                    assertTrue(rs.next());
                    assertEquals(1, rs.getInt(1));
                }
            }
        }
    }

    @Test
    void serverSideFunctions() throws Exception {
        try (Connection c = connect(); Statement st = c.createStatement();
             ResultSet rs = st.executeQuery(
                 "SELECT x AT TIME ZONE 'America/New_York', sys_extract_utc(x), ora_extract('timezone_region', x) "
                 + "FROM (SELECT '2026-10-16 14:30 Europe/Berlin'::ora_timestamptz AS x) s")) {
            assertTrue(rs.next());
            OraTimestampTz ny = OraTimestampTzJdbc.get(rs, 1);
            assertEquals("America/New_York", ny.getRegion());
            assertEquals(Instant.parse("2026-10-16T12:30:00Z"), ny.toInstant());
            assertEquals("Europe/Berlin", rs.getString(3));
        }
    }
}
