package io.github.zwiebelchen.oratstz;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Instant;
import java.time.ZoneId;
import java.time.ZonedDateTime;

import org.joda.time.DateTime;
import org.joda.time.DateTimeZone;
import org.junit.jupiter.api.Test;

class OraTimestampTzTest {

    @Test
    void parsesRegionValue() {
        OraTimestampTz v = OraTimestampTz.parse("2026-10-16 14:30:00.123456+02:00[Europe/Berlin]");
        assertEquals("Europe/Berlin", v.getRegion());
        assertEquals(7200, v.getOffsetSeconds());
        assertEquals(Instant.parse("2026-10-16T12:30:00.123456Z"), v.toInstant());
        DateTime dt = v.toDateTime();
        assertEquals(DateTimeZone.forID("Europe/Berlin"), dt.getZone());
        assertEquals(14, dt.getHourOfDay());
        assertEquals(123, dt.getMillisOfSecond());
    }

    @Test
    void parsesOffsetValueAndNanos() {
        OraTimestampTz v = OraTimestampTz.parse("2026-10-16 14:30:00.123456789-03:30");
        assertNull(v.getRegion());
        assertEquals(-12600, v.getOffsetSeconds());
        assertEquals(123456789, v.getNano());
        assertEquals("-03:30", v.toDateTime().getZone().getID());
        assertEquals(123456789, v.toZonedDateTime().getNano());
    }

    @Test
    void parsesPrecisionZeroAndBcYears() {
        OraTimestampTz v = OraTimestampTz.parse("2026-10-17 00:00:00+00:00[UTC]");
        assertEquals("UTC", v.getRegion());
        OraTimestampTz bc = OraTimestampTz.parse("-4711-01-01 00:00:00.000000+00:00[UTC]");
        assertEquals(-4711, bc.toZonedDateTime().getYear());
        assertEquals("-4711-01-01 00:00:00.000000000+00:00[UTC]", bc.toSqlLiteral());
    }

    @Test
    void historicOffsetWithSeconds() {
        OraTimestampTz v = OraTimestampTz.parse("1890-01-01 00:00:00.000000+00:53:28[Europe/Berlin]");
        assertEquals(53 * 60 + 28, v.getOffsetSeconds());
        assertTrue(v.toSqlLiteral().contains("+00:53:28[Europe/Berlin]"));
    }

    @Test
    void ambiguousTimeKeepsOffset() {
        OraTimestampTz dst = OraTimestampTz.parse("2026-10-25 02:30:00.000000+02:00[Europe/Berlin]");
        OraTimestampTz std = OraTimestampTz.parse("2026-10-25 02:30:00.000000+01:00[Europe/Berlin]");
        assertEquals(3600, std.getEpochSecond() - dst.getEpochSecond());
        assertEquals(7200, dst.toDateTime().getZone().getOffset(dst.toDateTime().getMillis()) / 1000);
        assertEquals("2026-10-25 02:30:00.000000000+02:00[Europe/Berlin]", dst.toSqlLiteral());
    }

    @Test
    void fromJodaRegionAndOffset() {
        DateTime berlin = new DateTime(2026, 10, 16, 14, 30, 0, 250, DateTimeZone.forID("Europe/Berlin"));
        OraTimestampTz r = OraTimestampTz.of(berlin);
        assertEquals("Europe/Berlin", r.getRegion());
        assertEquals("2026-10-16 14:30:00.250000000+02:00[Europe/Berlin]", r.toSqlLiteral());
        assertEquals(berlin, r.toDateTime());

        DateTime fixed = new DateTime(2026, 10, 16, 14, 30, DateTimeZone.forOffsetHoursMinutes(5, 30));
        OraTimestampTz o = OraTimestampTz.of(fixed);
        assertNull(o.getRegion());
        assertEquals("2026-10-16 14:30:00.000000000+05:30", o.toSqlLiteral());

        OraTimestampTz utc = OraTimestampTz.of(new DateTime(0L, DateTimeZone.UTC));
        assertEquals("UTC", utc.getRegion());
    }

    @Test
    void fromJavaTime() {
        ZonedDateTime z = ZonedDateTime.of(2026, 3, 1, 8, 0, 0, 1, ZoneId.of("America/New_York"));
        OraTimestampTz v = OraTimestampTz.of(z);
        assertEquals("America/New_York", v.getRegion());
        assertEquals(z, v.toZonedDateTime());
        assertNull(OraTimestampTz.of(z.toOffsetDateTime()).getRegion());
    }

    @Test
    void equalitySemantics() {
        OraTimestampTz a = OraTimestampTz.parse("1999-01-15 08:00:00-08:00");
        OraTimestampTz b = OraTimestampTz.parse("1999-01-15 11:00:00-05:00");
        assertTrue(a.isSameInstant(b));
        assertEquals(0, a.compareTo(b));
        assertFalse(a.equals(b));
        assertEquals(a, OraTimestampTz.parse("1999-01-15 08:00:00.000-08:00"));
        assertEquals(a.hashCode(), OraTimestampTz.parse("1999-01-15 08:00:00.000-08:00").hashCode());
    }

    @Test
    void roundTripLiteral() {
        String[] inputs = {
            "2026-10-16 14:30:00.000000+02:00[Europe/Berlin]",
            "2026-10-16 14:30:00.123+05:30",
            "2026-10-16T14:30:00Z",
            "2026-10-16 14:30:00.1-0330",
        };
        for (String in : inputs) {
            OraTimestampTz v = OraTimestampTz.parse(in);
            assertEquals(v, OraTimestampTz.parse(v.toSqlLiteral()), in);
        }
    }

    @Test
    void unknownRegionFallsBackToOffset() {
        OraTimestampTz v = OraTimestampTz.parse("2026-10-16 14:30:00+03:00[Mars/Olympus]");
        assertEquals("+03:00", v.toDateTime().getZone().getID());
        assertEquals(ZoneId.of("+03:00"), v.toZonedDateTime().getZone());
    }

    @Test
    void rejectsNlsFormat() {
        IllegalArgumentException e = assertThrows(IllegalArgumentException.class,
                () -> OraTimestampTz.parse("16-OCT-26 02.30.00.000000 PM EUROPE/BERLIN"));
        assertTrue(e.getMessage().contains("output_format"));
    }
}
