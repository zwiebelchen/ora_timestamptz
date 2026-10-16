/*-------------------------------------------------------------------------
 *
 * ora_timestamptz.c
 *	  Oracle-compatible TIMESTAMP WITH TIME ZONE for PostgreSQL.
 *
 * Semantics follow Oracle Database:
 *
 *	- The value keeps its time zone: a fixed offset ("+02:00") or a region
 *	  ("Europe/Berlin").  Values without a zone get the session time zone.
 *	- Two values are equal if they denote the same instant in UTC, no matter
 *	  which zone they carry (btree/hash opclasses work on the instant).
 *	- Up to 9 fractional digits; typmod is the fractional seconds precision
 *	  (default 6), values are rounded, not truncated.
 *	- Local times in a DST gap are rejected (ORA-01878); ambiguous local
 *	  times (DST overlap) are interpreted as standard time unless a TZD or an
 *	  explicit offset disambiguates them, or ora_timestamptz.error_on_overlap_time
 *	  is on (ORA-01883).
 *	- All arithmetic happens in UTC; the zone of the operand is kept.
 *	- EXTRACT(YEAR|MONTH|DAY|HOUR|MINUTE|SECOND ...) returns UTC fields.
 *	- AT TIME ZONE / AT LOCAL convert to another zone, keeping the instant.
 *
 * ora_timestamptz/ora_timestamptz.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <limits.h>

#include "access/xact.h"
#include "common/hashfn.h"
#include "common/int.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "nodes/miscnodes.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datetime.h"
#include "utils/guc.h"
#include "utils/numeric.h"
#include "utils/sortsupport.h"
#include "utils/timestamp.h"

#include "ora_timestamptz.h"

PG_MODULE_MAGIC;

/* ---------- GUCs ---------- */

typedef enum
{
	OTZ_OUTPUT_ISO,
	OTZ_OUTPUT_NLS
} OtzOutputFormat;

static const struct config_enum_entry output_format_options[] = {
	{"iso", OTZ_OUTPUT_ISO, false},
	{"nls", OTZ_OUTPUT_NLS, false},
	{NULL, 0, false}
};

static int	otz_output_format = OTZ_OUTPUT_ISO;
char	   *otz_nls_format = NULL;
bool		otz_error_on_overlap = false;

void		_PG_init(void);

void
_PG_init(void)
{
	DefineCustomEnumVariable("ora_timestamptz.output_format",
							 "Text output format of ora_timestamptz values.",
							 "iso: lossless RFC 9557 style "
							 "'YYYY-MM-DD HH24:MI:SS.FF+HH:MI[Region]'. "
							 "nls: uses ora_timestamptz.nls_timestamp_tz_format "
							 "(may be lossy, do not use for dumps).",
							 &otz_output_format,
							 OTZ_OUTPUT_ISO,
							 output_format_options,
							 PGC_USERSET, 0,
							 NULL, NULL, NULL);

	DefineCustomStringVariable("ora_timestamptz.nls_timestamp_tz_format",
							   "Oracle NLS_TIMESTAMP_TZ_FORMAT equivalent.",
							   "Default format for to_char(ora_timestamptz), "
							   "to_timestamp_tz(text) and fallback input parsing.",
							   &otz_nls_format,
							   "DD-MON-RR HH.MI.SSXFF AM TZR",
							   PGC_USERSET, 0,
							   NULL, NULL, NULL);

	DefineCustomBoolVariable("ora_timestamptz.error_on_overlap_time",
							 "Oracle ERROR_ON_OVERLAP_TIME equivalent.",
							 "If on, ambiguous local times in a region (DST "
							 "overlap) raise an error instead of being "
							 "interpreted as standard time.",
							 &otz_error_on_overlap,
							 false,
							 PGC_USERSET, 0,
							 NULL, NULL, NULL);

	otz_ddl_init();

	MarkGUCPrefixReserved("ora_timestamptz");
}

/* ---------- small helpers ---------- */

#define EPOCH_DIFF_SECS \
	((int64) (POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY)

static const int32 pow10_tab[] = {
	1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000, 1000000000
};

static const int32 days_in_month_tab[2][12] = {
	{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31},
	{31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}
};

int64
otz_floor_div(int64 a, int64 b)
{
	int64		q = a / b;

	if ((a % b != 0) && ((a < 0) != (b < 0)))
		q--;
	return q;
}

static int
days_in_month(int year, int mon)
{
	return days_in_month_tab[isleap(year) ? 1 : 0][mon - 1];
}

/* local wall clock seconds (as if UTC) from broken-down date/time */
static int64
wall_from_fields(int y, int m, int d, int hh, int mi, int ss)
{
	return (int64) (date2j(y, m, d) - UNIX_EPOCH_JDATE) * SECS_PER_DAY +
		hh * SECS_PER_HOUR + mi * SECS_PER_MINUTE + ss;
}

static void
fields_from_wall(int64 wall, int *y, int *m, int *d, int *hh, int *mi, int *ss,
				 int *julian)
{
	int64		days = otz_floor_div(wall, SECS_PER_DAY);
	int64		rem = wall - days * SECS_PER_DAY;
	int			jd = (int) (days + UNIX_EPOCH_JDATE);

	j2date(jd, y, m, d);
	*hh = (int) (rem / SECS_PER_HOUR);
	rem -= (int64) (*hh) * SECS_PER_HOUR;
	*mi = (int) (rem / SECS_PER_MINUTE);
	*ss = (int) (rem - (int64) (*mi) * SECS_PER_MINUTE);
	if (julian)
		*julian = jd;
}

/* Oracle range: -4712-01-01 .. 9999-12-31 (astronomical -4711 .. 9999) */
#define OTZ_MIN_YEAR	(-4711)
#define OTZ_MAX_YEAR	9999

static void
check_wall_range(int64 wall)
{
	static int64 min_wall = 0,
				max_wall = 0;

	if (min_wall == 0)
	{
		min_wall = wall_from_fields(OTZ_MIN_YEAR, 1, 1, 0, 0, 0);
		max_wall = wall_from_fields(OTZ_MAX_YEAR, 12, 31, 23, 59, 59);
	}
	if (wall < min_wall || wall > max_wall)
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("ora_timestamptz out of range"),
				 errdetail("Oracle allows -4712 to 9999 (4713 BC to 9999 AD) (Oracle ORA-01841).")));
}

static void
round_to_prec(int64 *sec, int32 *nsec, int prec)
{
	int32		unit = pow10_tab[OTZ_MAX_PREC - prec];

	if (unit > 1)
	{
		int32		r = *nsec % unit;

		*nsec -= r;
		if (r * 2 >= unit)
			*nsec += unit;
		if (*nsec >= NSECS_PER_SEC)
		{
			*nsec -= NSECS_PER_SEC;
			(*sec)++;
		}
	}
}

/* offset (and DST info) in effect at a given instant */
static int32
zone_offset_at(const OtzZone *z, int64 sec, bool *isdst, char *abbr)
{
	pg_time_t	t = (pg_time_t) sec;
	struct pg_tm *tm;

	if (!z->is_region)
	{
		if (isdst)
			*isdst = false;
		if (abbr)
			abbr[0] = '\0';
		return z->offset;
	}
	tm = pg_localtime(&t, z->tz);
	if (tm == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("ora_timestamptz out of range for time zone \"%s\"",
						z->name)));
	if (isdst)
		*isdst = tm->tm_isdst > 0;
	if (abbr)
		strlcpy(abbr, tm->tm_zone ? tm->tm_zone : "", OTZ_ABBR_MAXLEN + 1);
	return (int32) tm->tm_gmtoff;
}

static void
format_offset(char *buf, int32 off, bool with_seconds_if_needed)
{
	int32		a = off < 0 ? -off : off;
	int			h = a / SECS_PER_HOUR,
				m = (a / SECS_PER_MINUTE) % 60,
				s = a % 60;

	if (s != 0 && with_seconds_if_needed)
		sprintf(buf, "%c%02d:%02d:%02d", off < 0 ? '-' : '+', h, m, s);
	else
		sprintf(buf, "%c%02d:%02d", off < 0 ? '-' : '+', h, m);
}

/* ---------- zones ---------- */

bool
otz_lookup_region(const char *name, OtzZone *z)
{
	pg_tz	   *tz;

	if (name[0] == '\0' || strlen(name) > OTZ_ZONE_MAXLEN)
		return false;
	tz = pg_tzset(name);
	if (tz == NULL || !pg_tz_acceptable(tz))
		return false;
	z->is_region = true;
	z->offset = 0;
	z->tz = tz;
	strlcpy(z->name, pg_get_timezone_name(tz), sizeof(z->name));
	return true;
}

/*
 * Parse "+HH", "+HH:MI", "+HHMI", "+H:MI" (and ":SS"), or "Z".
 * On success *p is advanced.  With strict_range, Oracle's range
 * -12:59 .. +14:59 is enforced for the hour part.
 */
bool
otz_parse_offset(const char **p, int32 *off, bool strict_range)
{
	const char *s = *p;
	int			sign,
				h = 0,
				m = 0,
				sec = 0,
				nd = 0;

	if (*s == 'Z' || *s == 'z')
	{
		*off = 0;
		*p = s + 1;
		return true;
	}
	if (*s != '+' && *s != '-')
		return false;
	sign = (*s == '-') ? -1 : 1;
	s++;
	while (isdigit((unsigned char) *s) && nd < 2)
		h = h * 10 + (*s++ - '0'), nd++;
	if (nd == 0)
		return false;
	if (*s == ':')
	{
		s++;
		if (!isdigit((unsigned char) s[0]) || !isdigit((unsigned char) s[1]))
			return false;
		m = (s[0] - '0') * 10 + (s[1] - '0');
		s += 2;
		if (*s == ':' && isdigit((unsigned char) s[1]) && isdigit((unsigned char) s[2]))
		{
			sec = (s[1] - '0') * 10 + (s[2] - '0');
			s += 3;
		}
	}
	else if (nd == 2 && isdigit((unsigned char) s[0]) && isdigit((unsigned char) s[1]))
	{
		m = (s[0] - '0') * 10 + (s[1] - '0');
		s += 2;
	}
	if (m > 59 || sec > 59)
		return false;
	if (strict_range && (sign > 0 ? h > 14 : h > 12))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("time zone offset out of range"),
				 errdetail("Oracle allows offsets from -12:59 to +14:59 (ORA-01874).")));
	if (h > 18)
		return false;
	*off = sign * (h * SECS_PER_HOUR + m * SECS_PER_MINUTE + sec);
	*p = s;
	return true;
}

void
otz_session_zone(OtzZone *z)
{
	const char *name = pg_get_timezone_name(session_timezone);
	long		gmtoff;

	if ((name[0] == '<' || name[0] == '+' || name[0] == '-' ||
		 isdigit((unsigned char) name[0])) &&
		pg_get_timezone_offset(session_timezone, &gmtoff))
	{
		z->is_region = false;
		z->offset = (int32) gmtoff;
		z->tz = NULL;
		z->name[0] = '\0';
		return;
	}
	z->is_region = true;
	z->offset = 0;
	z->tz = session_timezone;
	strlcpy(z->name, name, sizeof(z->name));
}

static void
db_zone(OtzZone *z);

/*
 * Resolve a zone given as text (AT TIME ZONE, FROM_TZ, TZ_OFFSET):
 * offset, region, or one of the Oracle keywords SESSIONTIMEZONE,
 * DBTIMEZONE, LOCAL.  Optionally "region TZD" (tzd may be NULL).
 */
static void
zone_from_text(const char *txt, OtzZone *z, char *tzd)
{
	char		buf[OTZ_ZONE_MAXLEN + OTZ_ABBR_MAXLEN + 8];
	char	   *s,
			   *e,
			   *sp;
	const char *q;
	int32		off;

	if (tzd)
		tzd[0] = '\0';
	if (strlen(txt) >= sizeof(buf))
		goto bad;
	strcpy(buf, txt);
	s = buf;
	while (isspace((unsigned char) *s))
		s++;
	e = s + strlen(s);
	while (e > s && isspace((unsigned char) e[-1]))
		*--e = '\0';

	if (pg_strcasecmp(s, "SESSIONTIMEZONE") == 0 || pg_strcasecmp(s, "LOCAL") == 0)
	{
		otz_session_zone(z);
		return;
	}
	if (pg_strcasecmp(s, "DBTIMEZONE") == 0)
	{
		db_zone(z);
		return;
	}

	q = s;
	if ((*s == '+' || *s == '-' || ((*s == 'Z' || *s == 'z') && s[1] == '\0')) &&
		otz_parse_offset(&q, &off, true) && *q == '\0')
	{
		z->is_region = false;
		z->offset = off;
		z->tz = NULL;
		z->name[0] = '\0';
		return;
	}

	/* "Region TZD"? */
	sp = strchr(s, ' ');
	if (sp && tzd)
	{
		char	   *a = sp;

		*a++ = '\0';
		while (isspace((unsigned char) *a))
			a++;
		strlcpy(tzd, a, OTZ_ABBR_MAXLEN + 1);
	}
	if (otz_lookup_region(s, z))
		return;
bad:
	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("time zone \"%s\" not recognized", txt),
			 errhint("Use an offset like '+02:00' or an IANA region like 'Europe/Berlin' (Oracle ORA-01882).")));
}

static void
db_zone(OtzZone *z)
{
	const char *s = GetConfigOptionResetString("TimeZone");
	pg_tz	   *tz = s ? pg_tzset(s) : NULL;
	const char *name;
	long		gmtoff;

	if (tz == NULL)
	{
		otz_session_zone(z);
		return;
	}
	name = pg_get_timezone_name(tz);
	if ((name[0] == '<' || name[0] == '+' || name[0] == '-' ||
		 isdigit((unsigned char) name[0])) &&
		pg_get_timezone_offset(tz, &gmtoff))
	{
		z->is_region = false;
		z->offset = (int32) gmtoff;
		z->tz = NULL;
		z->name[0] = '\0';
		return;
	}
	z->is_region = true;
	z->offset = 0;
	z->tz = tz;
	strlcpy(z->name, name, sizeof(z->name));
}

static void
zone_of_value(const OraTsTz *v, OtzZone *z)
{
	if (v->flags & OTZ_REGION)
	{
		if (!otz_lookup_region(v->region, z))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("time zone region \"%s\" stored in ora_timestamptz value is unknown to the time zone database",
							v->region)));
	}
	else
	{
		z->is_region = false;
		z->offset = v->offset;
		z->tz = NULL;
		z->name[0] = '\0';
	}
}

/*
 * Map a local wall-clock time in a region to UTC, with Oracle semantics.
 */
typedef struct
{
	int32		offset;
	bool		isdst;
	char		abbr[OTZ_ABBR_MAXLEN + 1];
} OtzCandidate;

static int64
resolve_region(int64 wall, const OtzZone *z, const char *tzd,
			   bool have_hint, int32 hint)
{
	int32		offs[16];
	int			noffs = 0;
	OtzCandidate valid[4];
	int			nvalid = 0;
	pg_time_t	t = (pg_time_t) (wall - 2 * SECS_PER_DAY);
	int			i,
				j;
	struct pg_tm *tm;

#define ADD_OFF(o) \
	do { \
		bool dup_ = false; \
		for (j = 0; j < noffs; j++) if (offs[j] == (int32) (o)) dup_ = true; \
		if (!dup_ && noffs < lengthof(offs)) offs[noffs++] = (int32) (o); \
	} while (0)

	for (i = 0; i < 8; i++)
	{
		long		bo,
					ao;
		int			bd,
					ad;
		pg_time_t	boundary;
		int			r;

		r = pg_next_dst_boundary(&t, &bo, &bd, &boundary, &ao, &ad, z->tz);
		if (r < 0)
			break;
		ADD_OFF(bo);
		if (r == 0 || boundary > (pg_time_t) (wall + 2 * SECS_PER_DAY))
			break;
		ADD_OFF(ao);
		t = boundary;
	}
	/* safety net: offset at the wall time interpreted as UTC */
	t = (pg_time_t) wall;
	tm = pg_localtime(&t, z->tz);
	if (tm)
		ADD_OFF(tm->tm_gmtoff);

	for (i = 0; i < noffs; i++)
	{
		pg_time_t	u = (pg_time_t) (wall - offs[i]);

		tm = pg_localtime(&u, z->tz);
		if (tm && tm->tm_gmtoff == offs[i] && nvalid < lengthof(valid))
		{
			valid[nvalid].offset = offs[i];
			valid[nvalid].isdst = tm->tm_isdst > 0;
			strlcpy(valid[nvalid].abbr, tm->tm_zone ? tm->tm_zone : "",
					OTZ_ABBR_MAXLEN + 1);
			nvalid++;
		}
	}
#undef ADD_OFF

	if (nvalid == 0)
	{
		int			y,
					m,
					d,
					hh,
					mi,
					ss;

		fields_from_wall(wall, &y, &m, &d, &hh, &mi, &ss, NULL);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
				 errmsg("local time %04d-%02d-%02d %02d:%02d:%02d does not exist in time zone \"%s\"",
						y, m, d, hh, mi, ss, z->name),
				 errdetail("The time falls into a daylight saving time gap (Oracle ORA-01878).")));
	}

	if (have_hint)
	{
		for (i = 0; i < nvalid; i++)
			if (valid[i].offset == hint)
				return wall - valid[i].offset;
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
				 errmsg("UTC offset does not match time zone region \"%s\" at the given local time",
						z->name)));
	}
	if (tzd && tzd[0])
	{
		for (i = 0; i < nvalid; i++)
			if (pg_strcasecmp(valid[i].abbr, tzd) == 0)
				return wall - valid[i].offset;
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
				 errmsg("time zone abbreviation \"%s\" does not match time zone region \"%s\" at the given local time",
						tzd, z->name),
				 errdetail("Oracle ORA-01857.")));
	}
	if (nvalid == 1)
		return wall - valid[0].offset;

	if (otz_error_on_overlap)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
				 errmsg("ambiguous local time in time zone \"%s\"", z->name),
				 errdetail("The time falls into a daylight saving time overlap (Oracle ORA-01883)."),
				 errhint("Add a time zone abbreviation (TZD) or a UTC offset to disambiguate.")));

	/* Oracle default: standard time; tie-break: the later instant */
	{
		int			best = -1;

		for (i = 0; i < nvalid; i++)
		{
			if (best < 0 ||
				(!valid[i].isdst && valid[best].isdst) ||
				(valid[i].isdst == valid[best].isdst &&
				 valid[i].offset < valid[best].offset))
				best = i;
		}
		return wall - valid[best].offset;
	}
}

/* ---------- value construction ---------- */

static OraTsTz *
make_value(int64 sec, int32 nsec, const OtzZone *z, int prec)
{
	size_t		len;
	OraTsTz    *r;
	int32		off;

	Assert(prec >= 0 && prec <= OTZ_MAX_PREC);
	round_to_prec(&sec, &nsec, prec);

	off = zone_offset_at(z, sec, NULL, NULL);
	check_wall_range(sec + off);

	len = OTZ_HDRSZ + (z->is_region ? strlen(z->name) + 1 : 0);
	r = (OraTsTz *) palloc0(len);
	SET_VARSIZE(r, len);
	r->sec = sec;
	r->nsec = nsec;
	r->prec = (uint8) prec;
	r->offset = off;
	if (z->is_region)
	{
		r->flags = OTZ_REGION;
		strcpy(r->region, z->name);
	}
	return r;
}

static int
typmod_prec(int32 typmod, int fallback)
{
	if (typmod < 0)
		return fallback;
	if (typmod > OTZ_MAX_PREC)
		return OTZ_MAX_PREC;
	return typmod;
}

OraTsTz *
otz_from_parsed(OtzParsed *ps, int32 typmod)
{
	int64		wall,
				utc;
	OtzZone		z;
	int			prec;

	if (ps->mon < 1 || ps->mon > 12)
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_FIELD_OVERFLOW),
				 errmsg("month must be between 1 and 12 (Oracle ORA-01843)")));
	if (ps->year < OTZ_MIN_YEAR || ps->year > OTZ_MAX_YEAR)
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_FIELD_OVERFLOW),
				 errmsg("year out of range"),
				 errdetail("Oracle allows -4712 to 9999 (4713 BC to 9999 AD); in ISO notation used by the text input this is -4711 to 9999, where 0000 is 1 BC (Oracle ORA-01841).")));
	if (ps->mday < 1 || ps->mday > days_in_month(ps->year, ps->mon))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_FIELD_OVERFLOW),
				 errmsg("day of month must be between 1 and last day of month (Oracle ORA-01847)")));
	if (ps->hour < 0 || ps->hour > 23 || ps->min < 0 || ps->min > 59 ||
		ps->sec < 0 || ps->sec > 59)
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_FIELD_OVERFLOW),
				 errmsg("time field out of range (Oracle ORA-01850/01851/01852)")));

	wall = wall_from_fields(ps->year, ps->mon, ps->mday, ps->hour, ps->min, ps->sec);

	if (ps->have_zone)
		z = ps->zone;
	else
		otz_session_zone(&z);

	if (z.is_region)
		utc = resolve_region(wall, &z, ps->tzd, ps->have_hint, ps->hint);
	else
		utc = wall - z.offset;

	prec = typmod_prec(typmod, Max(OTZ_DEFAULT_PREC, ps->ndigits));
	return make_value(utc, ps->nsec, &z, prec);
}

void
otz_get_fields(const OraTsTz *v, OtzFields *f)
{
	OtzZone		z;
	bool		isdst;

	zone_of_value(v, &z);
	f->offset = zone_offset_at(&z, v->sec, &isdst, f->abbr);
	fields_from_wall(v->sec + f->offset, &f->year, &f->mon, &f->mday,
					 &f->hour, &f->min, &f->sec, &f->julian);
	f->nsec = v->nsec;
	f->prec = v->prec;
	f->wday = j2day(f->julian);
	f->yday = f->julian - date2j(f->year, 1, 1) + 1;
	f->is_region = (v->flags & OTZ_REGION) != 0;
	f->region = f->is_region ? v->region : NULL;
}

void
otz_current_local(struct pg_tm *tm)
{
	fsec_t		fsec;
	int			tz;

	if (timestamp2tm(GetCurrentTransactionStartTimestamp(), &tz, tm, &fsec,
					 NULL, NULL) != 0)
		elog(ERROR, "could not determine current date");
}

/* ---------- ISO / RFC 9557 parser ---------- */

static int
read_digits(const char **p, int maxdigits, int *val)
{
	int			n = 0,
				v = 0;

	while (n < maxdigits && isdigit((unsigned char) **p))
	{
		v = v * 10 + (**p - '0');
		(*p)++;
		n++;
	}
	*val = v;
	return n;
}

/*
 * Accepted forms (case-insensitive, 'T' or blanks between date and time):
 *	 [+-]YYYY-MM-DD[ HH:MI[:SS[.F{1,9}]]][ ]zone
 *	 zone :=  (empty -> session time zone)
 *			| Z | +HH[:MI] | +HHMI				fixed offset
 *			| Region [TZD]						e.g. Europe/Berlin CEST
 *			| [Region]  | offset[Region]		RFC 9557 / java.time style
 * Returns false (with message) on syntax errors; semantic errors raise.
 */
static bool
parse_iso(const char *str, OtzParsed *ps, char *err, size_t errlen)
{
	const char *p = str;
	int			sign = 1,
				n;
	char		token[OTZ_ZONE_MAXLEN + 1];

	memset(ps, 0, sizeof(*ps));
	ps->mon = ps->mday = 1;

	while (isspace((unsigned char) *p))
		p++;
	if (*p == '-' || *p == '+')
		sign = (*p++ == '-') ? -1 : 1;
	if (read_digits(&p, 4, &ps->year) != 4 || *p++ != '-')
		goto syntax;
	ps->year *= sign;
	if (read_digits(&p, 2, &ps->mon) < 1 || *p++ != '-')
		goto syntax;
	if (read_digits(&p, 2, &ps->mday) < 1)
		goto syntax;

	/* time part */
	{
		const char *q = p;

		if (*q == 'T' || *q == 't')
			q++;
		else
			while (*q == ' ' || *q == '\t')
				q++;
		if (q != p && isdigit((unsigned char) *q))
		{
			p = q;
			if (read_digits(&p, 2, &ps->hour) < 1 || *p++ != ':')
				goto syntax;
			if (read_digits(&p, 2, &ps->min) != 2)
				goto syntax;
			if (*p == ':')
			{
				p++;
				if (read_digits(&p, 2, &ps->sec) != 2)
					goto syntax;
				if (*p == '.' || *p == ',')
				{
					int			frac;

					p++;
					n = read_digits(&p, 9, &frac);
					if (n == 0)
						goto syntax;
					if (isdigit((unsigned char) *p))
					{
						snprintf(err, errlen, "more than 9 fractional second digits");
						return false;
					}
					ps->nsec = frac * pow10_tab[9 - n];
					ps->ndigits = n;
				}
			}
		}
	}

	/* zone part */
	while (isspace((unsigned char) *p))
		p++;
	if (*p == '\0')
		return true;

	if (*p == '+' || *p == '-' ||
		((*p == 'Z' || *p == 'z') &&
		 (p[1] == '\0' || p[1] == '[' || isspace((unsigned char) p[1]))))
	{
		int32		off;

		if (!otz_parse_offset(&p, &off, true))
			goto syntax;
		ps->have_zone = true;
		ps->zone.is_region = false;
		ps->zone.offset = off;
		while (isspace((unsigned char) *p))
			p++;
		if (*p == '[')
		{
			ps->have_hint = true;
			ps->hint = off;
		}
		else if (isalpha((unsigned char) *p))
		{
			/* "offset Region" (Oracle style output 'TZH:TZM TZR') */
			ps->have_hint = true;
			ps->hint = off;
			goto region_token;
		}
	}

	if (*p == '[')
	{
		const char *e;

		p++;
		if (*p == '!')
			p++;
		e = strchr(p, ']');
		if (e == NULL || e == p || (size_t) (e - p) > OTZ_ZONE_MAXLEN)
			goto syntax;
		memcpy(token, p, e - p);
		token[e - p] = '\0';
		p = e + 1;
		if (strchr(token, '=') == NULL)
		{
			if (!otz_lookup_region(token, &ps->zone))
				goto badzone;
			ps->have_zone = true;
		}
		/* skip further RFC 9557 suffixes like [u-ca=iso8601] */
		while (*p == '[')
		{
			e = strchr(p, ']');
			if (e == NULL)
				goto syntax;
			p = e + 1;
		}
	}
	else if (isalpha((unsigned char) *p))
	{
		const char *s;

region_token:
		s = p;
		while (*p && !isspace((unsigned char) *p) && *p != '[')
			p++;
		if ((size_t) (p - s) > OTZ_ZONE_MAXLEN)
			goto syntax;
		memcpy(token, s, p - s);
		token[p - s] = '\0';
		if (!otz_lookup_region(token, &ps->zone))
			goto badzone;
		ps->have_zone = true;
		while (isspace((unsigned char) *p))
			p++;
		if (*p && *p != '[')
		{
			/* TZD */
			s = p;
			while (*p && !isspace((unsigned char) *p))
				p++;
			if ((size_t) (p - s) > OTZ_ABBR_MAXLEN)
				goto syntax;
			memcpy(ps->tzd, s, p - s);
			ps->tzd[p - s] = '\0';
		}
	}

	while (isspace((unsigned char) *p))
		p++;
	if (*p != '\0')
		goto syntax;
	return true;

syntax:
	snprintf(err, errlen, "invalid syntax");
	return false;

badzone:
	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("time zone \"%s\" not recognized", token),
			 errhint("Use an offset like '+02:00' or an IANA region like 'Europe/Berlin' (Oracle ORA-01882).")));
	return false;				/* keep compiler quiet */
}

/* ---------- I/O ---------- */

PG_FUNCTION_INFO_V1(ora_timestamptz_in);
Datum
ora_timestamptz_in(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);
	int32		typmod = PG_GETARG_INT32(2);
	Node	   *escontext = fcinfo->context;
	OtzParsed	ps;
	char		err[256],
				err2[256];

	if (parse_iso(str, &ps, err, sizeof(err)))
		PG_RETURN_ORATSTZ_P(otz_from_parsed(&ps, typmod));

	/* Oracle-like implicit conversion: try NLS_TIMESTAMP_TZ_FORMAT */
	if (otz_nls_format && otz_nls_format[0] &&
		otz_parse_format(str, otz_nls_format, &ps, err2, sizeof(err2)))
		PG_RETURN_ORATSTZ_P(otz_from_parsed(&ps, typmod));

	ereturn(escontext, (Datum) 0,
			(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
			 errmsg("invalid input syntax for type %s: \"%s\"",
					"ora_timestamptz", str),
			 errdetail("%s", err),
			 errhint("Expected 'YYYY-MM-DD HH24:MI:SS[.FF] zone' (zone: '+HH:MI', 'Region [TZD]', '+HH:MI[Region]') or the format in ora_timestamptz.nls_timestamp_tz_format.")));
}

static char *
otz_to_iso(const OraTsTz *v)
{
	OtzFields	f;
	StringInfoData buf;
	char		offbuf[16];

	otz_get_fields(v, &f);
	initStringInfo(&buf);
	if (f.year < 0)
		appendStringInfo(&buf, "-%04d", -f.year);
	else
		appendStringInfo(&buf, "%04d", f.year);
	appendStringInfo(&buf, "-%02d-%02d %02d:%02d:%02d",
					 f.mon, f.mday, f.hour, f.min, f.sec);
	if (f.prec > 0)
		appendStringInfo(&buf, ".%0*d", f.prec,
						 f.nsec / pow10_tab[OTZ_MAX_PREC - f.prec]);
	format_offset(offbuf, f.offset, true);
	appendStringInfoString(&buf, offbuf);
	if (f.is_region)
		appendStringInfo(&buf, "[%s]", f.region);
	return buf.data;
}

PG_FUNCTION_INFO_V1(ora_timestamptz_out);
Datum
ora_timestamptz_out(PG_FUNCTION_ARGS)
{
	OraTsTz    *v = PG_GETARG_ORATSTZ_P(0);

	if (otz_output_format == OTZ_OUTPUT_NLS && otz_nls_format && otz_nls_format[0])
		PG_RETURN_CSTRING(otz_to_char(v, otz_nls_format));
	PG_RETURN_CSTRING(otz_to_iso(v));
}

/*
 * Binary format: int8 sec (Unix epoch), int4 nsec, int4 offset, int1 flags,
 * int1 prec, then the region name as a NUL-terminated string if flags & 1.
 */
PG_FUNCTION_INFO_V1(ora_timestamptz_recv);
Datum
ora_timestamptz_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	int32		typmod = PG_GETARG_INT32(2);
	int64		sec;
	int32		nsec,
				off;
	int			flags,
				prec;
	OtzZone		z;

	sec = pq_getmsgint64(buf);
	nsec = pq_getmsgint(buf, 4);
	off = pq_getmsgint(buf, 4);
	flags = pq_getmsgbyte(buf);
	prec = pq_getmsgbyte(buf);
	if (nsec < 0 || nsec >= NSECS_PER_SEC || prec < 0 || prec > OTZ_MAX_PREC)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("invalid ora_timestamptz binary value")));
	if (flags & OTZ_REGION)
	{
		const char *name = pq_getmsgstring(buf);

		if (!otz_lookup_region(name, &z))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("time zone \"%s\" not recognized", name)));
	}
	else
	{
		z.is_region = false;
		z.offset = off;
		z.tz = NULL;
	}
	PG_RETURN_ORATSTZ_P(make_value(sec, nsec, &z, typmod_prec(typmod, prec)));
}

PG_FUNCTION_INFO_V1(ora_timestamptz_send);
Datum
ora_timestamptz_send(PG_FUNCTION_ARGS)
{
	OraTsTz    *v = PG_GETARG_ORATSTZ_P(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendint64(&buf, v->sec);
	pq_sendint32(&buf, v->nsec);
	pq_sendint32(&buf, v->offset);
	pq_sendbyte(&buf, v->flags);
	pq_sendbyte(&buf, v->prec);
	if (v->flags & OTZ_REGION)
		pq_sendstring(&buf, v->region);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

PG_FUNCTION_INFO_V1(ora_timestamptz_typmod_in);
Datum
ora_timestamptz_typmod_in(PG_FUNCTION_ARGS)
{
	ArrayType  *ta = PG_GETARG_ARRAYTYPE_P(0);
	int32	   *tl;
	int			n;

	tl = ArrayGetIntegerTypmods(ta, &n);
	if (n != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid type modifier"),
				 errdetail("Only the fractional seconds precision may be specified.")));
	if (tl[0] < 0 || tl[0] > OTZ_MAX_PREC)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("ora_timestamptz(%d) precision must be between 0 and %d",
						tl[0], OTZ_MAX_PREC)));
	PG_RETURN_INT32(tl[0]);
}

PG_FUNCTION_INFO_V1(ora_timestamptz_typmod_out);
Datum
ora_timestamptz_typmod_out(PG_FUNCTION_ARGS)
{
	int32		typmod = PG_GETARG_INT32(0);
	char	   *res = (char *) palloc(16);

	if (typmod >= 0)
		snprintf(res, 16, "(%d)", typmod);
	else
		res[0] = '\0';
	PG_RETURN_CSTRING(res);
}

/* length coercion: ora_timestamptz(ora_timestamptz, int4, bool) */
PG_FUNCTION_INFO_V1(ora_timestamptz_scale);
Datum
ora_timestamptz_scale(PG_FUNCTION_ARGS)
{
	OraTsTz    *v = PG_GETARG_ORATSTZ_P(0);
	int32		typmod = PG_GETARG_INT32(1);
	OtzZone		z;

	if (typmod < 0 || typmod == v->prec)
		PG_RETURN_ORATSTZ_P(v);
	zone_of_value(v, &z);
	PG_RETURN_ORATSTZ_P(make_value(v->sec, v->nsec, &z, typmod_prec(typmod, v->prec)));
}

/* ---------- comparison / hashing ---------- */

static inline int
otz_cmp_internal(const OraTsTz *a, const OraTsTz *b)
{
	if (a->sec != b->sec)
		return a->sec < b->sec ? -1 : 1;
	if (a->nsec != b->nsec)
		return a->nsec < b->nsec ? -1 : 1;
	return 0;
}

#define OTZ_CMP_FUNC(name, expr) \
PG_FUNCTION_INFO_V1(name); \
Datum \
name(PG_FUNCTION_ARGS) \
{ \
	OraTsTz *a = PG_GETARG_ORATSTZ_P(0); \
	OraTsTz *b = PG_GETARG_ORATSTZ_P(1); \
	int c = otz_cmp_internal(a, b); \
	PG_RETURN_BOOL(expr); \
}

OTZ_CMP_FUNC(ora_timestamptz_eq, c == 0)
OTZ_CMP_FUNC(ora_timestamptz_ne, c != 0)
OTZ_CMP_FUNC(ora_timestamptz_lt, c < 0)
OTZ_CMP_FUNC(ora_timestamptz_le, c <= 0)
OTZ_CMP_FUNC(ora_timestamptz_gt, c > 0)
OTZ_CMP_FUNC(ora_timestamptz_ge, c >= 0)

PG_FUNCTION_INFO_V1(ora_timestamptz_cmp);
Datum
ora_timestamptz_cmp(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(otz_cmp_internal(PG_GETARG_ORATSTZ_P(0),
									 PG_GETARG_ORATSTZ_P(1)));
}

static int
otz_fastcmp(Datum x, Datum y, SortSupport ssup)
{
	return otz_cmp_internal(DatumGetOraTsTzP(x), DatumGetOraTsTzP(y));
}

PG_FUNCTION_INFO_V1(ora_timestamptz_sortsupport);
Datum
ora_timestamptz_sortsupport(PG_FUNCTION_ARGS)
{
	SortSupport ssup = (SortSupport) PG_GETARG_POINTER(0);

	ssup->comparator = otz_fastcmp;
	PG_RETURN_VOID();
}

/* same instant AND same zone (Oracle has no operator for this) */
PG_FUNCTION_INFO_V1(ora_timestamptz_identical);
Datum
ora_timestamptz_identical(PG_FUNCTION_ARGS)
{
	OraTsTz    *a = PG_GETARG_ORATSTZ_P(0);
	OraTsTz    *b = PG_GETARG_ORATSTZ_P(1);

	if (otz_cmp_internal(a, b) != 0 || a->flags != b->flags)
		PG_RETURN_BOOL(false);
	if (a->flags & OTZ_REGION)
		PG_RETURN_BOOL(strcmp(a->region, b->region) == 0);
	PG_RETURN_BOOL(a->offset == b->offset);
}

PG_FUNCTION_INFO_V1(ora_timestamptz_larger);
Datum
ora_timestamptz_larger(PG_FUNCTION_ARGS)
{
	OraTsTz    *a = PG_GETARG_ORATSTZ_P(0);
	OraTsTz    *b = PG_GETARG_ORATSTZ_P(1);

	PG_RETURN_ORATSTZ_P(otz_cmp_internal(a, b) >= 0 ? a : b);
}

PG_FUNCTION_INFO_V1(ora_timestamptz_smaller);
Datum
ora_timestamptz_smaller(PG_FUNCTION_ARGS)
{
	OraTsTz    *a = PG_GETARG_ORATSTZ_P(0);
	OraTsTz    *b = PG_GETARG_ORATSTZ_P(1);

	PG_RETURN_ORATSTZ_P(otz_cmp_internal(a, b) <= 0 ? a : b);
}

static void
hash_key(const OraTsTz *v, char *buf)
{
	memcpy(buf, &v->sec, sizeof(int64));
	memcpy(buf + sizeof(int64), &v->nsec, sizeof(int32));
}

PG_FUNCTION_INFO_V1(ora_timestamptz_hash);
Datum
ora_timestamptz_hash(PG_FUNCTION_ARGS)
{
	char		buf[12];

	hash_key(PG_GETARG_ORATSTZ_P(0), buf);
	return hash_any((unsigned char *) buf, sizeof(buf));
}

PG_FUNCTION_INFO_V1(ora_timestamptz_hash_extended);
Datum
ora_timestamptz_hash_extended(PG_FUNCTION_ARGS)
{
	char		buf[12];

	hash_key(PG_GETARG_ORATSTZ_P(0), buf);
	return hash_any_extended((unsigned char *) buf, sizeof(buf),
							 PG_GETARG_INT64(1));
}

/* ---------- arithmetic (Oracle: always in UTC) ---------- */

static OraTsTz *
add_interval(const OraTsTz *v, const Interval *iv, int sign)
{
	int64		sec = v->sec,
				us;
	int32		nsec = v->nsec;
	int			prec = v->prec;
	OtzZone		z;

	if (INTERVAL_NOT_FINITE(iv))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("cannot add infinite interval to ora_timestamptz")));

	if (iv->month != 0)
	{
		int			y,
					m,
					d,
					hh,
					mi,
					ss;
		int64		months;

		fields_from_wall(sec, &y, &m, &d, &hh, &mi, &ss, NULL);
		months = (int64) y * 12 + (m - 1) + (int64) sign * iv->month;
		y = (int) otz_floor_div(months, 12);
		m = (int) (months - (int64) y * 12) + 1;
		if (y < OTZ_MIN_YEAR - 1 || y > OTZ_MAX_YEAR + 1)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("ora_timestamptz out of range")));
		if (d > days_in_month(y, m))
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_FIELD_OVERFLOW),
					 errmsg("date not valid for month specified"),
					 errdetail("Oracle ORA-01839: month arithmetic does not clamp to the last day of the month; it is evaluated in UTC.")));
		sec = wall_from_fields(y, m, d, hh, mi, ss);
	}

	sec += (int64) sign * iv->day * SECS_PER_DAY;
	us = (int64) sign * iv->time;
	sec += otz_floor_div(us, USECS_PER_SEC);
	nsec += (int32) ((us - otz_floor_div(us, USECS_PER_SEC) * USECS_PER_SEC) * 1000);
	if (nsec >= NSECS_PER_SEC)
	{
		nsec -= NSECS_PER_SEC;
		sec++;
	}
	if (iv->time % USECS_PER_SEC != 0)
		prec = Max(prec, OTZ_DEFAULT_PREC);

	zone_of_value(v, &z);
	return make_value(sec, nsec, &z, prec);
}

PG_FUNCTION_INFO_V1(ora_timestamptz_pl_interval);
Datum
ora_timestamptz_pl_interval(PG_FUNCTION_ARGS)
{
	PG_RETURN_ORATSTZ_P(add_interval(PG_GETARG_ORATSTZ_P(0),
									 PG_GETARG_INTERVAL_P(1), 1));
}

PG_FUNCTION_INFO_V1(ora_timestamptz_interval_pl);
Datum
ora_timestamptz_interval_pl(PG_FUNCTION_ARGS)
{
	PG_RETURN_ORATSTZ_P(add_interval(PG_GETARG_ORATSTZ_P(1),
									 PG_GETARG_INTERVAL_P(0), 1));
}

PG_FUNCTION_INFO_V1(ora_timestamptz_mi_interval);
Datum
ora_timestamptz_mi_interval(PG_FUNCTION_ARGS)
{
	PG_RETURN_ORATSTZ_P(add_interval(PG_GETARG_ORATSTZ_P(0),
									 PG_GETARG_INTERVAL_P(1), -1));
}

/* a - b -> INTERVAL DAY TO SECOND (days in the day field, no months) */
PG_FUNCTION_INFO_V1(ora_timestamptz_mi);
Datum
ora_timestamptz_mi(PG_FUNCTION_ARGS)
{
	OraTsTz    *a = PG_GETARG_ORATSTZ_P(0);
	OraTsTz    *b = PG_GETARG_ORATSTZ_P(1);
	Interval   *r = (Interval *) palloc(sizeof(Interval));
	int64		dsec,
				us,
				days;
	int32		dns;

	if (pg_sub_s64_overflow(a->sec, b->sec, &dsec))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("interval out of range")));
	dns = a->nsec - b->nsec;
	/* total in microseconds, rounded half away from zero */
	us = dsec * USECS_PER_SEC + (dns >= 0 ? (dns + 500) / 1000 : -((-dns + 500) / 1000));
	days = us / USECS_PER_DAY;	/* truncates toward zero: same sign parts */
	r->month = 0;
	r->day = (int32) days;
	r->time = us - days * USECS_PER_DAY;
	PG_RETURN_INTERVAL_P(r);
}

/* ---------- time zone conversion ---------- */

/* pg_catalog.timezone(text, ora_timestamptz): x AT TIME ZONE 'zone' */
PG_FUNCTION_INFO_V1(ora_timestamptz_at_zone);
Datum
ora_timestamptz_at_zone(PG_FUNCTION_ARGS)
{
	char	   *zname = text_to_cstring(PG_GETARG_TEXT_PP(0));
	OraTsTz    *v = PG_GETARG_ORATSTZ_P(1);
	OtzZone		z;

	zone_from_text(zname, &z, NULL);
	PG_RETURN_ORATSTZ_P(make_value(v->sec, v->nsec, &z, v->prec));
}

/* pg_catalog.timezone(interval, ora_timestamptz) */
PG_FUNCTION_INFO_V1(ora_timestamptz_at_interval);
Datum
ora_timestamptz_at_interval(PG_FUNCTION_ARGS)
{
	Interval   *iv = PG_GETARG_INTERVAL_P(0);
	OraTsTz    *v = PG_GETARG_ORATSTZ_P(1);
	OtzZone		z;

	if (INTERVAL_NOT_FINITE(iv) || iv->month != 0 || iv->day != 0 ||
		iv->time % USECS_PER_SEC != 0 ||
		iv->time > INT64CONST(15) * USECS_PER_HOUR ||
		iv->time < INT64CONST(-13) * USECS_PER_HOUR)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("interval time zone must be between -12:59 and +14:59 hours")));
	z.is_region = false;
	z.offset = (int32) (iv->time / USECS_PER_SEC);
	z.tz = NULL;
	z.name[0] = '\0';
	PG_RETURN_ORATSTZ_P(make_value(v->sec, v->nsec, &z, v->prec));
}

/* pg_catalog.timezone(ora_timestamptz): x AT LOCAL */
PG_FUNCTION_INFO_V1(ora_timestamptz_at_local);
Datum
ora_timestamptz_at_local(PG_FUNCTION_ARGS)
{
	OraTsTz    *v = PG_GETARG_ORATSTZ_P(0);
	OtzZone		z;

	otz_session_zone(&z);
	PG_RETURN_ORATSTZ_P(make_value(v->sec, v->nsec, &z, v->prec));
}

/* ---------- casts ---------- */

static void
pgts_to_secnsec(int64 ts, int64 *sec, int32 *nsec)
{
	int64		s = otz_floor_div(ts, USECS_PER_SEC);

	*nsec = (int32) ((ts - s * USECS_PER_SEC) * 1000);
	*sec = s + EPOCH_DIFF_SECS;
}

static int64
secnsec_to_pgts(int64 sec, int32 nsec)
{
	int64		us = (nsec + 500) / 1000;
	int64		r;

	r = (sec - EPOCH_DIFF_SECS) * USECS_PER_SEC + us;
	if (!IS_VALID_TIMESTAMP(r))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));
	return r;
}

PG_FUNCTION_INFO_V1(ora_timestamptz_to_timestamptz);
Datum
ora_timestamptz_to_timestamptz(PG_FUNCTION_ARGS)
{
	OraTsTz    *v = PG_GETARG_ORATSTZ_P(0);

	PG_RETURN_TIMESTAMPTZ(secnsec_to_pgts(v->sec, v->nsec));
}

/* Oracle CAST(tstz AS TIMESTAMP): local wall clock in the value's zone */
PG_FUNCTION_INFO_V1(ora_timestamptz_to_timestamp);
Datum
ora_timestamptz_to_timestamp(PG_FUNCTION_ARGS)
{
	OraTsTz    *v = PG_GETARG_ORATSTZ_P(0);
	OtzZone		z;

	zone_of_value(v, &z);
	PG_RETURN_TIMESTAMP(secnsec_to_pgts(v->sec + zone_offset_at(&z, v->sec, NULL, NULL),
										v->nsec));
}

PG_FUNCTION_INFO_V1(ora_timestamptz_to_date);
Datum
ora_timestamptz_to_date(PG_FUNCTION_ARGS)
{
	OraTsTz    *v = PG_GETARG_ORATSTZ_P(0);
	OtzZone		z;
	int64		wall;

	zone_of_value(v, &z);
	wall = v->sec + zone_offset_at(&z, v->sec, NULL, NULL);
	PG_RETURN_DATEADT((DateADT) (otz_floor_div(wall, SECS_PER_DAY) +
								 UNIX_EPOCH_JDATE - POSTGRES_EPOCH_JDATE));
}

PG_FUNCTION_INFO_V1(timestamptz_to_ora_timestamptz);
Datum
timestamptz_to_ora_timestamptz(PG_FUNCTION_ARGS)
{
	TimestampTz ts = PG_GETARG_TIMESTAMPTZ(0);
	int64		sec;
	int32		nsec;
	OtzZone		z;

	if (TIMESTAMP_NOT_FINITE(ts))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("infinite timestamps cannot be converted to ora_timestamptz")));
	pgts_to_secnsec(ts, &sec, &nsec);
	otz_session_zone(&z);
	PG_RETURN_ORATSTZ_P(make_value(sec, nsec, &z, OTZ_DEFAULT_PREC));
}

static OraTsTz *
local_to_value(int64 wall, int32 nsec, OtzZone *z, const char *tzd, int prec)
{
	int64		utc;

	if (z->is_region)
		utc = resolve_region(wall, z, tzd, false, 0);
	else
		utc = wall - z->offset;
	return make_value(utc, nsec, z, prec);
}

/* Oracle: TIMESTAMP -> TSTZ uses the session time zone */
PG_FUNCTION_INFO_V1(timestamp_to_ora_timestamptz);
Datum
timestamp_to_ora_timestamptz(PG_FUNCTION_ARGS)
{
	Timestamp	ts = PG_GETARG_TIMESTAMP(0);
	int64		wall;
	int32		nsec;
	OtzZone		z;

	if (TIMESTAMP_NOT_FINITE(ts))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("infinite timestamps cannot be converted to ora_timestamptz")));
	pgts_to_secnsec(ts, &wall, &nsec);
	otz_session_zone(&z);
	PG_RETURN_ORATSTZ_P(local_to_value(wall, nsec, &z, NULL, OTZ_DEFAULT_PREC));
}

PG_FUNCTION_INFO_V1(date_to_ora_timestamptz);
Datum
date_to_ora_timestamptz(PG_FUNCTION_ARGS)
{
	DateADT		d = PG_GETARG_DATEADT(0);
	OtzZone		z;

	if (DATE_NOT_FINITE(d))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("infinite dates cannot be converted to ora_timestamptz")));
	otz_session_zone(&z);
	PG_RETURN_ORATSTZ_P(local_to_value((int64) (d + POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY,
									   0, &z, NULL, 0));
}

/* ---------- Oracle functions ---------- */

/* FROM_TZ(timestamp, zone) */
PG_FUNCTION_INFO_V1(ora_from_tz);
Datum
ora_from_tz(PG_FUNCTION_ARGS)
{
	Timestamp	ts = PG_GETARG_TIMESTAMP(0);
	char	   *zname = text_to_cstring(PG_GETARG_TEXT_PP(1));
	char		tzd[OTZ_ABBR_MAXLEN + 1];
	int64		wall;
	int32		nsec;
	OtzZone		z;

	if (TIMESTAMP_NOT_FINITE(ts))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("infinite timestamps cannot be converted to ora_timestamptz")));
	zone_from_text(zname, &z, tzd);
	pgts_to_secnsec(ts, &wall, &nsec);
	PG_RETURN_ORATSTZ_P(local_to_value(wall, nsec, &z, tzd, OTZ_DEFAULT_PREC));
}

/* SYS_EXTRACT_UTC(tstz) -> timestamp */
PG_FUNCTION_INFO_V1(ora_sys_extract_utc);
Datum
ora_sys_extract_utc(PG_FUNCTION_ARGS)
{
	OraTsTz    *v = PG_GETARG_ORATSTZ_P(0);

	PG_RETURN_TIMESTAMP(secnsec_to_pgts(v->sec, v->nsec));
}

/* TZ_OFFSET(zone) -> '+HH:MI' (current offset) */
PG_FUNCTION_INFO_V1(ora_tz_offset);
Datum
ora_tz_offset(PG_FUNCTION_ARGS)
{
	char	   *zname = text_to_cstring(PG_GETARG_TEXT_PP(0));
	OtzZone		z;
	int64		sec;
	int32		nsec;
	char		buf[16];

	zone_from_text(zname, &z, NULL);
	pgts_to_secnsec(GetCurrentStatementStartTimestamp(), &sec, &nsec);
	format_offset(buf, zone_offset_at(&z, sec, NULL, NULL), false);
	PG_RETURN_TEXT_P(cstring_to_text(buf));
}

static text *
zone_display(const OtzZone *z)
{
	char		buf[16];

	if (z->is_region)
		return cstring_to_text(z->name);
	format_offset(buf, z->offset, false);
	return cstring_to_text(buf);
}

PG_FUNCTION_INFO_V1(ora_sessiontimezone);
Datum
ora_sessiontimezone(PG_FUNCTION_ARGS)
{
	OtzZone		z;

	otz_session_zone(&z);
	PG_RETURN_TEXT_P(zone_display(&z));
}

PG_FUNCTION_INFO_V1(ora_dbtimezone);
Datum
ora_dbtimezone(PG_FUNCTION_ARGS)
{
	OtzZone		z;

	db_zone(&z);
	PG_RETURN_TEXT_P(zone_display(&z));
}

static int
arg_prec(FunctionCallInfo fcinfo, int argno)
{
	int32		p;

	if (PG_NARGS() <= argno)
		return OTZ_DEFAULT_PREC;
	p = PG_GETARG_INT32(argno);
	if (p < 0 || p > OTZ_MAX_PREC)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("precision must be between 0 and %d", OTZ_MAX_PREC)));
	return p;
}

/* Oracle CURRENT_TIMESTAMP: statement time in the session time zone */
PG_FUNCTION_INFO_V1(ora_current_timestamp);
Datum
ora_current_timestamp(PG_FUNCTION_ARGS)
{
	int64		sec;
	int32		nsec;
	OtzZone		z;

	pgts_to_secnsec(GetCurrentStatementStartTimestamp(), &sec, &nsec);
	otz_session_zone(&z);
	PG_RETURN_ORATSTZ_P(make_value(sec, nsec, &z, arg_prec(fcinfo, 0)));
}

/*
 * Oracle SYSTIMESTAMP: statement time with the server's (OS) offset.
 * PostgreSQL's log_timezone is initialized from the server's system zone
 * by initdb, so its offset is used.
 */
PG_FUNCTION_INFO_V1(ora_systimestamp);
Datum
ora_systimestamp(PG_FUNCTION_ARGS)
{
	int64		sec;
	int32		nsec;
	OtzZone		z,
				sys;

	pgts_to_secnsec(GetCurrentStatementStartTimestamp(), &sec, &nsec);
	sys.is_region = true;
	sys.tz = log_timezone;
	strlcpy(sys.name, pg_get_timezone_name(log_timezone), sizeof(sys.name));
	z.is_region = false;
	z.offset = zone_offset_at(&sys, sec, NULL, NULL);
	z.tz = NULL;
	z.name[0] = '\0';
	PG_RETURN_ORATSTZ_P(make_value(sec, nsec, &z, arg_prec(fcinfo, 0)));
}

/* ---------- EXTRACT ---------- */

typedef enum
{
	F_YEAR, F_MONTH, F_DAY, F_HOUR, F_MINUTE, F_SECOND,
	F_TZ_HOUR, F_TZ_MINUTE, F_TZ_REGION, F_TZ_ABBR, F_TZ_OFFSET,
	F_EPOCH, F_TIMEZONE, F_UNKNOWN
} OtzField;

static OtzField
field_from_name(const char *name)
{
	static const struct
	{
		const char *n;
		OtzField	f;
	}			tab[] = {
		{"year", F_YEAR}, {"month", F_MONTH}, {"day", F_DAY},
		{"hour", F_HOUR}, {"minute", F_MINUTE}, {"second", F_SECOND},
		{"timezone_hour", F_TZ_HOUR}, {"timezone_minute", F_TZ_MINUTE},
		{"timezone_region", F_TZ_REGION}, {"timezone_abbr", F_TZ_ABBR},
		{"timezone_offset", F_TZ_OFFSET}, {"epoch", F_EPOCH},
		{"timezone", F_TIMEZONE},
	};

	for (int i = 0; i < lengthof(tab); i++)
		if (pg_strcasecmp(name, tab[i].n) == 0)
			return tab[i].f;
	return F_UNKNOWN;
}

/*
 * Returns the field as text.  Oracle semantics: date/time fields in UTC,
 * zone fields from the value's zone.
 */
static char *
extract_field(OtzField f, const OraTsTz *v, const char *fname)
{
	OtzZone		z;
	bool		isdst;
	char		abbr[OTZ_ABBR_MAXLEN + 1];
	int32		off;
	int			y,
				m,
				d,
				hh,
				mi,
				ss;
	char		buf[64];

	zone_of_value(v, &z);
	off = zone_offset_at(&z, v->sec, &isdst, abbr);
	fields_from_wall(v->sec, &y, &m, &d, &hh, &mi, &ss, NULL);

	switch (f)
	{
		case F_YEAR:
			/* Oracle numbering: no year 0, 1 BC = -1 */
			snprintf(buf, sizeof(buf), "%d", y <= 0 ? y - 1 : y);
			break;
		case F_MONTH:
			snprintf(buf, sizeof(buf), "%d", m);
			break;
		case F_DAY:
			snprintf(buf, sizeof(buf), "%d", d);
			break;
		case F_HOUR:
			snprintf(buf, sizeof(buf), "%d", hh);
			break;
		case F_MINUTE:
			snprintf(buf, sizeof(buf), "%d", mi);
			break;
		case F_SECOND:
			if (v->nsec == 0)
				snprintf(buf, sizeof(buf), "%d", ss);
			else
			{
				int			n = 9;
				int32		ns = v->nsec;

				while (ns % 10 == 0)
					ns /= 10, n--;
				snprintf(buf, sizeof(buf), "%d.%0*d", ss, n, ns);
			}
			break;
		case F_TZ_HOUR:
			snprintf(buf, sizeof(buf), "%d", off / SECS_PER_HOUR);
			break;
		case F_TZ_MINUTE:
			snprintf(buf, sizeof(buf), "%d", (off / SECS_PER_MINUTE) % 60);
			break;
		case F_TZ_REGION:
			return pstrdup(z.is_region ? z.name : "UNKNOWN");
		case F_TZ_ABBR:
			return pstrdup(z.is_region ? abbr : "UNK");
		case F_TZ_OFFSET:
			format_offset(buf, off, false);
			break;
		case F_EPOCH:
			if (v->nsec == 0)
				snprintf(buf, sizeof(buf), INT64_FORMAT, v->sec);
			else
				snprintf(buf, sizeof(buf), INT64_FORMAT ".%09d", v->sec, v->nsec);
			break;
		case F_TIMEZONE:
			snprintf(buf, sizeof(buf), "%d", off);
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("unit \"%s\" not supported for type ora_timestamptz", fname)));
	}
	return pstrdup(buf);
}

/* pg_catalog.extract(text, ora_timestamptz) -> numeric */
PG_FUNCTION_INFO_V1(ora_timestamptz_extract);
Datum
ora_timestamptz_extract(PG_FUNCTION_ARGS)
{
	char	   *fname = text_to_cstring(PG_GETARG_TEXT_PP(0));
	OraTsTz    *v = PG_GETARG_ORATSTZ_P(1);
	OtzField	f = field_from_name(fname);

	if (f == F_TZ_REGION || f == F_TZ_ABBR || f == F_TZ_OFFSET)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("EXTRACT(%s FROM ora_timestamptz) returns text in Oracle and cannot be expressed via EXTRACT in PostgreSQL",
						fname),
				 errhint("Use ora_extract('%s', value) instead.", fname)));
	return DirectFunctionCall3(numeric_in,
							   CStringGetDatum(extract_field(f, v, fname)),
							   ObjectIdGetDatum(InvalidOid),
							   Int32GetDatum(-1));
}

/* ora_extract(text, ora_timestamptz) -> text (all fields incl. region/abbr) */
PG_FUNCTION_INFO_V1(ora_timestamptz_extract_text);
Datum
ora_timestamptz_extract_text(PG_FUNCTION_ARGS)
{
	char	   *fname = text_to_cstring(PG_GETARG_TEXT_PP(0));
	OraTsTz    *v = PG_GETARG_ORATSTZ_P(1);

	PG_RETURN_TEXT_P(cstring_to_text(extract_field(field_from_name(fname), v, fname)));
}

/* ---------- formatting ---------- */

PG_FUNCTION_INFO_V1(ora_timestamptz_to_char);
Datum
ora_timestamptz_to_char(PG_FUNCTION_ARGS)
{
	OraTsTz    *v = PG_GETARG_ORATSTZ_P(0);
	char	   *fmt;

	if (PG_NARGS() > 1)
		fmt = text_to_cstring(PG_GETARG_TEXT_PP(1));
	else
		fmt = otz_nls_format;
	PG_RETURN_TEXT_P(cstring_to_text(otz_to_char(v, fmt)));
}

PG_FUNCTION_INFO_V1(ora_to_timestamp_tz);
Datum
ora_to_timestamp_tz(PG_FUNCTION_ARGS)
{
	char	   *str = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char	   *fmt;
	OtzParsed	ps;
	char		err[256];

	if (PG_NARGS() > 1)
		fmt = text_to_cstring(PG_GETARG_TEXT_PP(1));
	else
		fmt = otz_nls_format;
	if (!otz_parse_format(str, fmt, &ps, err, sizeof(err)))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
				 errmsg("\"%s\" does not match format \"%s\"", str, fmt),
				 errdetail("%s", err)));
	PG_RETURN_ORATSTZ_P(otz_from_parsed(&ps, -1));
}

PG_FUNCTION_INFO_V1(ora_timestamptz_iso);
Datum
ora_timestamptz_iso(PG_FUNCTION_ARGS)
{
	PG_RETURN_TEXT_P(cstring_to_text(otz_to_iso(PG_GETARG_ORATSTZ_P(0))));
}

/* ---------- time zone database changes (cf. Oracle DBMS_DST) ---------- */

/* true if the region's offset at this instant differs from creation time */
PG_FUNCTION_INFO_V1(ora_timestamptz_tzdata_changed);
Datum
ora_timestamptz_tzdata_changed(PG_FUNCTION_ARGS)
{
	OraTsTz    *v = PG_GETARG_ORATSTZ_P(0);
	OtzZone		z;

	if (!(v->flags & OTZ_REGION))
		PG_RETURN_BOOL(false);
	zone_of_value(v, &z);
	PG_RETURN_BOOL(zone_offset_at(&z, v->sec, NULL, NULL) != v->offset);
}

/*
 * Re-anchor a region value whose offset changed due to a tzdata update so
 * that the original local wall-clock time is preserved (Oracle
 * DBMS_DST.UPGRADE_* semantics).  Unchanged values are returned as is.
 */
PG_FUNCTION_INFO_V1(ora_timestamptz_tzdata_fix);
Datum
ora_timestamptz_tzdata_fix(PG_FUNCTION_ARGS)
{
	OraTsTz    *v = PG_GETARG_ORATSTZ_P(0);
	OtzZone		z;

	if (!(v->flags & OTZ_REGION))
		PG_RETURN_ORATSTZ_P(v);
	zone_of_value(v, &z);
	if (zone_offset_at(&z, v->sec, NULL, NULL) == v->offset)
		PG_RETURN_ORATSTZ_P(v);
	PG_RETURN_ORATSTZ_P(local_to_value(v->sec + v->offset, v->nsec, &z, NULL, v->prec));
}
