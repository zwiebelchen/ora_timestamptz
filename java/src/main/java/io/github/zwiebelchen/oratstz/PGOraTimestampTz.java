package io.github.zwiebelchen.oratstz;

import java.sql.Connection;
import java.sql.SQLException;

import org.postgresql.PGConnection;
import org.postgresql.util.PGobject;
import org.postgresql.util.PSQLException;
import org.postgresql.util.PSQLState;

/**
 * pgJDBC adapter for {@code ora_timestamptz}.
 *
 * <p>Writing: {@code ps.setObject(i, new PGOraTimestampTz(value))} sends the
 * value typed as {@code ora_timestamptz} (no implicit cast from timestamptz,
 * so the zone is kept).</p>
 *
 * <p>Reading: after {@link #register(Connection)} pgJDBC returns instances of
 * this class from {@code ResultSet.getObject()}. Registration is optional:
 * {@link OraTimestampTzJdbc} also handles plain {@link PGobject}s and strings,
 * which is handy with connection pools.</p>
 */
public final class PGOraTimestampTz extends PGobject {

    private static final long serialVersionUID = 1L;

    private transient OraTimestampTz parsed;

    /** Required by pgJDBC (instantiated reflectively). */
    public PGOraTimestampTz() {
        setType(OraTimestampTz.PG_TYPE);
    }

    /** @param v the value, or {@code null} for SQL NULL */
    public PGOraTimestampTz(OraTimestampTz v) {
        this();
        this.parsed = v;
        this.value = (v == null) ? null : v.toSqlLiteral();
    }

    /** Registers this class for {@code ora_timestamptz} on the given connection (unwraps pools). */
    public static void register(Connection connection) throws SQLException {
        connection.unwrap(PGConnection.class).addDataType(OraTimestampTz.PG_TYPE, PGOraTimestampTz.class);
    }

    @Override
    public void setValue(String value) throws SQLException {
        OraTimestampTz p = null;
        if (value != null) {
            try {
                p = OraTimestampTz.parse(value);
            } catch (IllegalArgumentException e) {
                throw new PSQLException(e.getMessage(), PSQLState.DATA_TYPE_MISMATCH, e);
            }
        }
        super.setValue(value);
        this.parsed = p;
    }

    /** The parsed value, or {@code null} for SQL NULL. */
    public OraTimestampTz get() {
        if (parsed == null && value != null) {
            parsed = OraTimestampTz.parse(value);
        }
        return parsed;
    }
}
