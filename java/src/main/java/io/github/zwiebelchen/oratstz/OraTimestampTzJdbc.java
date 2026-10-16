package io.github.zwiebelchen.oratstz;

import java.sql.PreparedStatement;
import java.sql.ResultSet;
import java.sql.SQLException;
import java.time.ZonedDateTime;

import org.joda.time.DateTime;
import org.postgresql.util.PGobject;

/**
 * Convenience helpers for JDBC code working with Joda-Time.
 *
 * <pre>{@code
 * // write: keeps Europe/Berlin (or +02:00) in the database
 * OraTimestampTzJdbc.setDateTime(ps, 1, new DateTime(DateTimeZone.forID("Europe/Berlin")));
 *
 * // read: DateTime in the stored zone
 * DateTime dt = OraTimestampTzJdbc.getDateTime(rs, "created_at");
 * }</pre>
 *
 * <p>Do <b>not</b> bind {@code ora_timestamptz} parameters with
 * {@code setTimestamp(..)} or {@code setObject(.., OffsetDateTime)}: pgJDBC
 * sends those as {@code timestamp[tz]}, the server converts them with the
 * session time zone (= JVM default zone of pgJDBC) and the original zone is
 * lost.</p>
 */
public final class OraTimestampTzJdbc {

    private OraTimestampTzJdbc() {
    }

    public static void set(PreparedStatement ps, int index, OraTimestampTz value) throws SQLException {
        ps.setObject(index, new PGOraTimestampTz(value));
    }

    public static void setDateTime(PreparedStatement ps, int index, DateTime value) throws SQLException {
        set(ps, index, value == null ? null : OraTimestampTz.of(value));
    }

    public static void setZonedDateTime(PreparedStatement ps, int index, ZonedDateTime value) throws SQLException {
        set(ps, index, value == null ? null : OraTimestampTz.of(value));
    }

    public static OraTimestampTz get(ResultSet rs, int column) throws SQLException {
        return convert(rs.getObject(column));
    }

    public static OraTimestampTz get(ResultSet rs, String column) throws SQLException {
        return convert(rs.getObject(column));
    }

    public static DateTime getDateTime(ResultSet rs, int column) throws SQLException {
        OraTimestampTz v = get(rs, column);
        return v == null ? null : v.toDateTime();
    }

    public static DateTime getDateTime(ResultSet rs, String column) throws SQLException {
        OraTimestampTz v = get(rs, column);
        return v == null ? null : v.toDateTime();
    }

    public static ZonedDateTime getZonedDateTime(ResultSet rs, String column) throws SQLException {
        OraTimestampTz v = get(rs, column);
        return v == null ? null : v.toZonedDateTime();
    }

    /** Converts what pgJDBC returns from getObject() for an ora_timestamptz column. */
    public static OraTimestampTz convert(Object o) throws SQLException {
        if (o == null) {
            return null;
        }
        if (o instanceof PGOraTimestampTz) {
            return ((PGOraTimestampTz) o).get();
        }
        if (o instanceof PGobject) {
            String s = ((PGobject) o).getValue();
            return s == null ? null : parse(s);
        }
        if (o instanceof String) {
            return parse((String) o);
        }
        throw new SQLException("Unexpected JDBC value for ora_timestamptz: " + o.getClass().getName());
    }

    private static OraTimestampTz parse(String s) throws SQLException {
        try {
            return OraTimestampTz.parse(s);
        } catch (IllegalArgumentException e) {
            throw new SQLException(e.getMessage(), "22007", e);
        }
    }
}
