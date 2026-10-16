package io.github.zwiebelchen.oratstz;

import java.io.Serializable;
import java.time.DateTimeException;
import java.time.Instant;
import java.time.LocalDateTime;
import java.time.OffsetDateTime;
import java.time.ZoneId;
import java.time.ZoneOffset;
import java.time.ZonedDateTime;
import java.util.Objects;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import org.joda.time.DateTime;
import org.joda.time.DateTimeZone;

/**
 * Immutable Java counterpart of the PostgreSQL type {@code ora_timestamptz}
 * (Oracle {@code TIMESTAMP WITH TIME ZONE}).
 *
 * <p>A value consists of an instant (epoch second + nanoseconds) and the time
 * zone it was recorded in: either a fixed UTC offset ({@code +02:00}) or a
 * region ({@code Europe/Berlin}). Nothing is lost on the way between Java
 * and the database.</p>
 *
 * <h2>Equality</h2>
 * <ul>
 *   <li>{@link #equals(Object)} / {@link #hashCode()}: same instant <b>and</b>
 *       same zone (value-object semantics, safe for maps/sets).</li>
 *   <li>{@link #isSameInstant(OraTimestampTz)} and {@link #compareTo}: Oracle /
 *       SQL semantics, only the instant counts. Note that {@code compareTo} is
 *       therefore not consistent with {@code equals}.</li>
 * </ul>
 *
 * <h2>Text format</h2>
 * The server's default output ({@code ora_timestamptz.output_format = 'iso'})
 * is RFC 9557 style: {@code 2026-10-16 14:30:00.000000+02:00[Europe/Berlin]}
 * or {@code 2026-10-16 14:30:00.123+05:30}. {@link #parse(String)} reads it,
 * {@link #toSqlLiteral()} writes the same format, which the server accepts as
 * input. The offset in front of the region makes ambiguous local times (DST
 * overlap) unambiguous.
 */
public final class OraTimestampTz implements Comparable<OraTimestampTz>, Serializable {

    private static final long serialVersionUID = 1L;

    /** Server type name. */
    public static final String PG_TYPE = "ora_timestamptz";

    private static final Pattern ISO = Pattern.compile(
            "^\\s*([+-]?\\d{4,})-(\\d{1,2})-(\\d{1,2})[ Tt](\\d{1,2}):(\\d{2})(?::(\\d{2})(?:[.,](\\d{1,9}))?)?"
                    + "\\s*([+-]\\d{2}(?::?\\d{2}(?::\\d{2})?)?|[Zz])"
                    + "(?:\\s*\\[!?([^\\]=]+)\\])?(?:\\[[^\\]]*=[^\\]]*\\])*\\s*$");

    private static final Pattern OFFSET_ID = Pattern.compile("^[+-]\\d{2}(:\\d{2}){0,2}$");

    private final long epochSecond;
    private final int nano;
    private final int offsetSeconds;
    private final String region;

    private OraTimestampTz(long epochSecond, int nano, int offsetSeconds, String region) {
        if (nano < 0 || nano > 999_999_999) {
            throw new IllegalArgumentException("nano out of range: " + nano);
        }
        this.epochSecond = epochSecond;
        this.nano = nano;
        this.offsetSeconds = offsetSeconds;
        this.region = region;
    }

    // ------------------------------------------------------------------ factories

    /**
     * Parses the server's text representation (output format {@code iso}).
     *
     * @throws IllegalArgumentException if the text cannot be parsed, e.g. because
     *         the session uses {@code ora_timestamptz.output_format = 'nls'}
     */
    public static OraTimestampTz parse(String text) {
        Objects.requireNonNull(text, "text");
        Matcher m = ISO.matcher(text);
        if (!m.matches()) {
            throw new IllegalArgumentException("Cannot parse ora_timestamptz value '" + text
                    + "'. Use ora_timestamptz.output_format = 'iso' (default) or select "
                    + "ora_timestamptz_iso(column).");
        }
        try {
            int nanos = 0;
            String frac = m.group(7);
            if (frac != null) {
                nanos = Integer.parseInt((frac + "000000000").substring(0, 9));
            }
            LocalDateTime local = LocalDateTime.of(
                    Integer.parseInt(m.group(1)),
                    Integer.parseInt(m.group(2)),
                    Integer.parseInt(m.group(3)),
                    Integer.parseInt(m.group(4)),
                    Integer.parseInt(m.group(5)),
                    m.group(6) == null ? 0 : Integer.parseInt(m.group(6)),
                    nanos);
            String off = m.group(8);
            ZoneOffset offset = ("Z".equalsIgnoreCase(off)) ? ZoneOffset.UTC : ZoneOffset.of(normalizeOffset(off));
            Instant instant = local.toInstant(offset);
            String region = m.group(9);
            return new OraTimestampTz(instant.getEpochSecond(), instant.getNano(),
                    offset.getTotalSeconds(), region == null ? null : region.trim());
        } catch (DateTimeException | NumberFormatException e) {
            throw new IllegalArgumentException("Cannot parse ora_timestamptz value '" + text + "'", e);
        }
    }

    /**
     * From a Joda {@link DateTime}. A fixed-offset zone (ID like {@code +02:00})
     * becomes an offset value, any other zone ID (including {@code UTC}) a region
     * value. Joda has millisecond precision.
     */
    public static OraTimestampTz of(DateTime dateTime) {
        Objects.requireNonNull(dateTime, "dateTime");
        DateTimeZone zone = dateTime.getZone();
        long millis = dateTime.getMillis();
        long sec = Math.floorDiv(millis, 1000L);
        int nanos = (int) Math.floorMod(millis, 1000L) * 1_000_000;
        int off = zone.getOffset(millis) / 1000;
        String id = zone.getID();
        return new OraTimestampTz(sec, nanos, off, OFFSET_ID.matcher(id).matches() ? null : id);
    }

    /** From a {@link ZonedDateTime} (region if the zone is a region, else offset). */
    public static OraTimestampTz of(ZonedDateTime zdt) {
        Objects.requireNonNull(zdt, "zdt");
        ZoneId zone = zdt.getZone();
        String region = (zone instanceof ZoneOffset) ? null : zone.getId();
        return new OraTimestampTz(zdt.toEpochSecond(), zdt.getNano(),
                zdt.getOffset().getTotalSeconds(), region);
    }

    /** From an {@link OffsetDateTime} (fixed offset value). */
    public static OraTimestampTz of(OffsetDateTime odt) {
        Objects.requireNonNull(odt, "odt");
        return new OraTimestampTz(odt.toEpochSecond(), odt.getNano(),
                odt.getOffset().getTotalSeconds(), null);
    }

    /** Instant in a region, offset computed with the JVM's time zone data. */
    public static OraTimestampTz ofRegion(Instant instant, String region) {
        return of(instant.atZone(ZoneId.of(region)));
    }

    /** Instant with a fixed offset. */
    public static OraTimestampTz ofOffset(Instant instant, ZoneOffset offset) {
        return of(instant.atOffset(offset));
    }

    // ------------------------------------------------------------------ accessors

    public long getEpochSecond() {
        return epochSecond;
    }

    public int getNano() {
        return nano;
    }

    /** Offset in effect at this instant, in seconds east of UTC. */
    public int getOffsetSeconds() {
        return offsetSeconds;
    }

    /** Region name (Oracle TZR) or {@code null} for fixed-offset values. */
    public String getRegion() {
        return region;
    }

    public boolean isRegion() {
        return region != null;
    }

    public Instant toInstant() {
        return Instant.ofEpochSecond(epochSecond, nano);
    }

    public ZoneOffset getOffset() {
        return ZoneOffset.ofTotalSeconds(offsetSeconds);
    }

    // ------------------------------------------------------------------ conversions

    /**
     * To a Joda {@link DateTime} in the stored zone. Sub-millisecond digits are
     * truncated. If Joda does not know the region, the stored offset is used.
     */
    public DateTime toDateTime() {
        return new DateTime(Math.addExact(Math.multiplyExact(epochSecond, 1000L), nano / 1_000_000),
                toDateTimeZone());
    }

    /** The stored zone as Joda zone (falls back to the offset if the region is unknown to Joda). */
    public DateTimeZone toDateTimeZone() {
        if (region != null) {
            try {
                return DateTimeZone.forID(region);
            } catch (IllegalArgumentException unknownToJoda) {
                // fall through
            }
        }
        return DateTimeZone.forOffsetMillis(offsetSeconds * 1000);
    }

    /** Full nanosecond precision. Falls back to the offset if java.time does not know the region. */
    public ZonedDateTime toZonedDateTime() {
        ZoneId zone;
        if (region != null) {
            try {
                zone = ZoneId.of(region);
            } catch (DateTimeException unknown) {
                zone = getOffset();
            }
        } else {
            zone = getOffset();
        }
        return toInstant().atZone(zone);
    }

    public OffsetDateTime toOffsetDateTime() {
        return toInstant().atOffset(getOffset());
    }

    /**
     * Server input literal: local time, offset and optional region, e.g.
     * {@code 2026-10-25 02:30:00.000000000+02:00[Europe/Berlin]}.
     * <p>The server checks the offset against its own time zone data; if JVM and
     * server tzdata disagree for that instant, the insert fails instead of
     * silently storing a different instant.</p>
     */
    public String toSqlLiteral() {
        LocalDateTime local = LocalDateTime.ofEpochSecond(epochSecond, nano, getOffset());
        StringBuilder sb = new StringBuilder(48);
        int y = local.getYear();
        if (y < 0) {
            sb.append('-');
        }
        pad(sb, Math.abs(y), 4).append('-');
        pad(sb, local.getMonthValue(), 2).append('-');
        pad(sb, local.getDayOfMonth(), 2).append(' ');
        pad(sb, local.getHour(), 2).append(':');
        pad(sb, local.getMinute(), 2).append(':');
        pad(sb, local.getSecond(), 2).append('.');
        pad(sb, local.getNano(), 9);
        int a = Math.abs(offsetSeconds);
        sb.append(offsetSeconds < 0 ? '-' : '+');
        pad(sb, a / 3600, 2).append(':');
        pad(sb, (a / 60) % 60, 2);
        if (a % 60 != 0) {
            sb.append(':');
            pad(sb, a % 60, 2);
        }
        if (region != null) {
            sb.append('[').append(region).append(']');
        }
        return sb.toString();
    }

    // ------------------------------------------------------------------ comparison

    /** Oracle semantics: true if both denote the same instant, whatever the zone. */
    public boolean isSameInstant(OraTimestampTz other) {
        return epochSecond == other.epochSecond && nano == other.nano;
    }

    /** Orders by instant only (like the database). Not consistent with {@link #equals}. */
    @Override
    public int compareTo(OraTimestampTz o) {
        int c = Long.compare(epochSecond, o.epochSecond);
        return c != 0 ? c : Integer.compare(nano, o.nano);
    }

    /** Same instant and same zone (cf. SQL function ora_timestamptz_identical). */
    @Override
    public boolean equals(Object obj) {
        if (this == obj) {
            return true;
        }
        if (!(obj instanceof OraTimestampTz)) {
            return false;
        }
        OraTimestampTz o = (OraTimestampTz) obj;
        if (!isSameInstant(o) || !Objects.equals(region, o.region)) {
            return false;
        }
        return region != null || offsetSeconds == o.offsetSeconds;
    }

    @Override
    public int hashCode() {
        return Objects.hash(epochSecond, nano, region, region == null ? offsetSeconds : 0);
    }

    @Override
    public String toString() {
        return toSqlLiteral();
    }

    // ------------------------------------------------------------------ helpers

    private static StringBuilder pad(StringBuilder sb, int v, int width) {
        String s = Integer.toString(v);
        for (int i = s.length(); i < width; i++) {
            sb.append('0');
        }
        return sb.append(s);
    }

    private static String normalizeOffset(String off) {
        // accepts +HH, +HHMM, +HH:MM, +HH:MM:SS
        if (off.length() == 3) {
            return off + ":00";
        }
        if (off.length() == 5 && off.indexOf(':') < 0) {
            return off.substring(0, 3) + ":" + off.substring(3);
        }
        return off;
    }
}
