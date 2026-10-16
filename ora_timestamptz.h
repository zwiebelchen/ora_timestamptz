/*-------------------------------------------------------------------------
 *
 * ora_timestamptz.h
 *	  Oracle-compatible TIMESTAMP WITH TIME ZONE for PostgreSQL.
 *
 * Unlike PostgreSQL's timestamptz, which normalizes to UTC and discards the
 * zone, this type keeps the time zone that came with the value: either a
 * fixed UTC offset (Oracle "+02:00") or a time zone region (Oracle TZR,
 * e.g. "Europe/Berlin").  Like Oracle, the instant itself is stored in UTC,
 * and two values are equal when they denote the same instant, regardless
 * of the zone they carry.
 *
 * ora_timestamptz/ora_timestamptz.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef ORA_TIMESTAMPTZ_H
#define ORA_TIMESTAMPTZ_H

#include "fmgr.h"
#include "pgtime.h"
#include "datatype/timestamp.h"

/*
 * On-disk representation (varlena, double-aligned).
 *
 * sec/nsec: the instant, as seconds since 1970-01-01 00:00:00 UTC plus
 *		nanoseconds (0..999999999).  Oracle supports 9 fractional digits, so
 *		we do too.
 * offset:	for fixed-offset values the offset (seconds east of UTC).  For
 *		region values the offset that was in effect according to the time
 *		zone database when the value was created; it is only used to detect
 *		and repair values affected by later tzdata changes (cf. Oracle's
 *		DBMS_DST), never for display.
 * flags:	OTZ_REGION if the value carries a region name.
 * prec:	fractional seconds precision 0..9 (Oracle default 6).
 * region:	NUL-terminated canonical IANA zone name, present only for region
 *		values.
 */
typedef struct OraTsTz
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	int32		nsec;
	int64		sec;
	int32		offset;
	uint8		flags;
	uint8		prec;
	char		region[FLEXIBLE_ARRAY_MEMBER];
} OraTsTz;

#define OTZ_REGION		0x01
#define OTZ_HDRSZ		offsetof(OraTsTz, region)
#define OTZ_MAX_PREC	9
#define OTZ_DEFAULT_PREC 6
#define OTZ_ZONE_MAXLEN	TZ_STRLEN_MAX
#define OTZ_ABBR_MAXLEN	32

#define NSECS_PER_SEC	INT64CONST(1000000000)

#define DatumGetOraTsTzP(X)		((OraTsTz *) PG_DETOAST_DATUM(X))
#define PG_GETARG_ORATSTZ_P(n)	DatumGetOraTsTzP(PG_GETARG_DATUM(n))
#define PG_RETURN_ORATSTZ_P(x)	PG_RETURN_POINTER(x)

/* A resolved time zone: fixed offset or region. */
typedef struct OtzZone
{
	bool		is_region;
	int32		offset;			/* fixed offset, seconds east of UTC */
	pg_tz	   *tz;				/* region only */
	char		name[OTZ_ZONE_MAXLEN + 1];	/* canonical region name */
} OtzZone;

/* Broken-down local time of a value, as needed for formatting. */
typedef struct OtzFields
{
	int			year;			/* astronomical year: 0 = 1 BC */
	int			mon;			/* 1..12 */
	int			mday;			/* 1..31 */
	int			hour;
	int			min;
	int			sec;
	int32		nsec;
	int			prec;
	int			wday;			/* 0 = Sunday */
	int			yday;			/* 1..366 */
	int			julian;			/* Julian day number */
	int32		offset;			/* offset in effect, seconds east */
	bool		is_region;
	const char *region;
	char		abbr[OTZ_ABBR_MAXLEN + 1];
} OtzFields;

/* Parsed but not yet resolved datetime (used by the parsers). */
typedef struct OtzParsed
{
	int			year;			/* astronomical */
	int			mon;
	int			mday;
	int			hour;
	int			min;
	int			sec;
	int32		nsec;
	int			ndigits;		/* number of fractional digits given */
	bool		have_zone;
	OtzZone		zone;
	bool		have_hint;		/* offset given together with a region */
	int32		hint;
	char		tzd[OTZ_ABBR_MAXLEN + 1];	/* TZD disambiguator, or "" */
} OtzParsed;

/* GUCs */
extern char *otz_nls_format;
extern bool otz_error_on_overlap;

/* ora_timestamptz.c */
extern int64 otz_floor_div(int64 a, int64 b);
extern bool otz_lookup_region(const char *name, OtzZone *z);
extern bool otz_parse_offset(const char **p, int32 *off, bool strict_range);
extern void otz_session_zone(OtzZone *z);
extern OraTsTz *otz_from_parsed(OtzParsed *ps, int32 typmod);
extern void otz_get_fields(const OraTsTz *v, OtzFields *f);
extern void otz_current_local(struct pg_tm *tm);

/* ora_tstz_format.c */
extern char *otz_to_char(const OraTsTz *v, const char *fmt);
extern bool otz_parse_format(const char *str, const char *fmt, OtzParsed *ps,
							 char *err, size_t errlen);

#endif							/* ORA_TIMESTAMPTZ_H */
