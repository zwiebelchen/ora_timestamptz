/*-------------------------------------------------------------------------
 *
 * ora_tstz_format.c
 *	  Oracle datetime format models (TO_CHAR / TO_TIMESTAMP_TZ) for
 *	  ora_timestamptz.
 *
 * Supported elements (NLS_DATE_LANGUAGE = AMERICAN):
 *	 SYYYY YYYY YYY YY Y RRRR RR  MM MON MONTH  DD DDD D DY DAY  J
 *	 HH HH12 HH24 MI SS SSSSS  FF FF1..FF9 X  AM PM A.M. P.M.
 *	 TZH TZM TZR TZD  FM  "quoted text"  punctuation
 * Capitalization of MON/MONTH/DY/DAY/AM/PM follows the element spelling
 * like in Oracle ('MON' -> OCT, 'Mon' -> Oct, 'mon' -> oct); 'TZR' and
 * 'TZD' print the region/abbreviation in upper case like Oracle does.
 *
 * ora_timestamptz/ora_tstz_format.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "lib/stringinfo.h"
#include "utils/datetime.h"

#include "ora_timestamptz.h"

typedef enum
{
	K_SYYYY, K_SSSSS, K_MONTH, K_AM_DOT, K_PM_DOT, K_HH24, K_HH12, K_YYYY,
	K_RRRR, K_FFN, K_DAY, K_DDD, K_MON, K_TZH, K_TZM, K_TZR, K_TZD, K_YYY,
	K_YY, K_RR, K_MM, K_DD, K_DY, K_HH, K_MI, K_SS, K_FF, K_AM, K_PM, K_FM,
	K_Y, K_D, K_J, K_X
} KeyId;

typedef struct
{
	const char *name;
	int			len;
	KeyId		id;
} KeyWord;

/* longest first */
static const KeyWord keywords[] = {
	{"SYYYY", 5, K_SYYYY}, {"SSSSS", 5, K_SSSSS}, {"MONTH", 5, K_MONTH},
	{"A.M.", 4, K_AM_DOT}, {"P.M.", 4, K_PM_DOT}, {"HH24", 4, K_HH24},
	{"HH12", 4, K_HH12}, {"YYYY", 4, K_YYYY}, {"RRRR", 4, K_RRRR},
	{"FF1", 3, K_FFN}, {"FF2", 3, K_FFN}, {"FF3", 3, K_FFN},
	{"FF4", 3, K_FFN}, {"FF5", 3, K_FFN}, {"FF6", 3, K_FFN},
	{"FF7", 3, K_FFN}, {"FF8", 3, K_FFN}, {"FF9", 3, K_FFN},
	{"DAY", 3, K_DAY}, {"DDD", 3, K_DDD}, {"MON", 3, K_MON},
	{"TZH", 3, K_TZH}, {"TZM", 3, K_TZM}, {"TZR", 3, K_TZR},
	{"TZD", 3, K_TZD}, {"YYY", 3, K_YYY},
	{"YY", 2, K_YY}, {"RR", 2, K_RR}, {"MM", 2, K_MM}, {"DD", 2, K_DD},
	{"DY", 2, K_DY}, {"HH", 2, K_HH}, {"MI", 2, K_MI}, {"SS", 2, K_SS},
	{"FF", 2, K_FF}, {"AM", 2, K_AM}, {"PM", 2, K_PM}, {"FM", 2, K_FM},
	{"Y", 1, K_Y}, {"D", 1, K_D}, {"J", 1, K_J}, {"X", 1, K_X},
};

static const char *const month_names[] = {
	"January", "February", "March", "April", "May", "June", "July",
	"August", "September", "October", "November", "December"
};
static const char *const day_names[] = {
	"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday",
	"Saturday"
};

static const KeyWord *
match_keyword(const char *p)
{
	for (int i = 0; i < lengthof(keywords); i++)
		if (pg_strncasecmp(p, keywords[i].name, keywords[i].len) == 0)
			return &keywords[i];
	return NULL;
}

typedef enum
{
	CASE_UPPER, CASE_CAP, CASE_LOWER
} CaseStyle;

static CaseStyle
case_style(const char *p, int len)
{
	if (islower((unsigned char) p[0]))
		return CASE_LOWER;
	if (len > 1 && isalpha((unsigned char) p[1]) && islower((unsigned char) p[1]))
		return CASE_CAP;
	return CASE_UPPER;
}

static void
append_cased(StringInfo buf, const char *s, int maxlen, CaseStyle cs, int pad)
{
	int			n = 0;

	for (const char *c = s; *c && n < maxlen; c++, n++)
	{
		char		ch = *c;

		if (cs == CASE_UPPER || (cs == CASE_CAP && n == 0))
			ch = pg_toupper((unsigned char) ch);
		else
			ch = pg_tolower((unsigned char) ch);
		appendStringInfoChar(buf, ch);
	}
	while (n++ < pad)
		appendStringInfoChar(buf, ' ');
}

static void
append_num(StringInfo buf, int64 val, int width, bool fm)
{
	if (fm)
		appendStringInfo(buf, INT64_FORMAT, val);
	else
		appendStringInfo(buf, "%0*lld", width, (long long) val);
}

static void
bad_format(const char *fmt)
{
	ereport(ERROR,
			(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
			 errmsg("date format not recognized: \"%s\"", fmt),
			 errdetail("Oracle ORA-01821.")));
}

char *
otz_to_char(const OraTsTz *v, const char *fmt)
{
	OtzFields	f;
	StringInfoData buf;
	const char *p = fmt;
	bool		fm = false;
	int			oy;

	otz_get_fields(v, &f);
	oy = f.year <= 0 ? f.year - 1 : f.year;	/* Oracle year numbering */
	initStringInfo(&buf);

	while (*p)
	{
		const KeyWord *k;
		CaseStyle	cs;

		if (*p == '"')
		{
			p++;
			while (*p && *p != '"')
				appendStringInfoChar(&buf, *p++);
			if (*p)
				p++;
			continue;
		}
		k = match_keyword(p);
		if (k == NULL)
		{
			if (isalpha((unsigned char) *p))
				bad_format(fmt);
			appendStringInfoChar(&buf, *p++);
			continue;
		}
		cs = case_style(p, k->len);

		switch (k->id)
		{
			case K_SYYYY:
				if (oy < 0)
					appendStringInfoChar(&buf, '-');
				else if (!fm)
					appendStringInfoChar(&buf, ' ');
				append_num(&buf, abs(oy), 4, fm);
				break;
			case K_YYYY:
			case K_RRRR:
				append_num(&buf, abs(oy), 4, fm);
				break;
			case K_YYY:
				append_num(&buf, abs(oy) % 1000, 3, fm);
				break;
			case K_YY:
			case K_RR:
				append_num(&buf, abs(oy) % 100, 2, fm);
				break;
			case K_Y:
				append_num(&buf, abs(oy) % 10, 1, fm);
				break;
			case K_MM:
				append_num(&buf, f.mon, 2, fm);
				break;
			case K_MONTH:
				append_cased(&buf, month_names[f.mon - 1], 99, cs, fm ? 0 : 9);
				break;
			case K_MON:
				append_cased(&buf, month_names[f.mon - 1], 3, cs, 0);
				break;
			case K_DD:
				append_num(&buf, f.mday, 2, fm);
				break;
			case K_DDD:
				append_num(&buf, f.yday, 3, fm);
				break;
			case K_D:
				append_num(&buf, f.wday + 1, 1, fm);
				break;
			case K_DAY:
				append_cased(&buf, day_names[f.wday], 99, cs, fm ? 0 : 9);
				break;
			case K_DY:
				append_cased(&buf, day_names[f.wday], 3, cs, 0);
				break;
			case K_J:
				append_num(&buf, f.julian, 7, fm);
				break;
			case K_HH:
			case K_HH12:
				append_num(&buf, (f.hour % 12 == 0) ? 12 : f.hour % 12, 2, fm);
				break;
			case K_HH24:
				append_num(&buf, f.hour, 2, fm);
				break;
			case K_MI:
				append_num(&buf, f.min, 2, fm);
				break;
			case K_SS:
				append_num(&buf, f.sec, 2, fm);
				break;
			case K_SSSSS:
				append_num(&buf, f.hour * 3600 + f.min * 60 + f.sec, 5, fm);
				break;
			case K_FF:
			case K_FFN:
				{
					int			n = (k->id == K_FF) ? f.prec : (p[2] - '0');
					char		digits[16];

					snprintf(digits, sizeof(digits), "%09d", f.nsec);
					appendBinaryStringInfo(&buf, digits, n);	/* truncates */
				}
				break;
			case K_X:
				appendStringInfoChar(&buf, '.');
				break;
			case K_AM:
			case K_PM:
				append_cased(&buf, f.hour < 12 ? "am" : "pm", 2, cs, 0);
				break;
			case K_AM_DOT:
			case K_PM_DOT:
				append_cased(&buf, f.hour < 12 ? "a.m." : "p.m.", 4, cs, 0);
				break;
			case K_TZH:
				appendStringInfo(&buf, "%c%02d", f.offset < 0 ? '-' : '+',
								 abs(f.offset) / 3600);
				break;
			case K_TZM:
				appendStringInfo(&buf, "%02d", (abs(f.offset) / 60) % 60);
				break;
			case K_TZR:
				if (f.is_region && cs == CASE_UPPER)
					append_cased(&buf, f.region, 999, CASE_UPPER, 0);
				else if (f.is_region)
					appendStringInfoString(&buf, f.region);
				else
					appendStringInfo(&buf, "%c%02d:%02d", f.offset < 0 ? '-' : '+',
									 abs(f.offset) / 3600, (abs(f.offset) / 60) % 60);
				break;
			case K_TZD:
				if (f.is_region)
				{
					if (cs == CASE_UPPER)
						append_cased(&buf, f.abbr, 999, CASE_UPPER, 0);
					else
						appendStringInfoString(&buf, f.abbr);
				}
				break;
			case K_FM:
				fm = !fm;
				break;
		}
		p += k->len;
	}
	return buf.data;
}

/* ---------------- parsing ---------------- */

typedef struct
{
	const char *s;				/* current input position */
	char	   *err;
	size_t		errlen;
} PState;

static bool
perr(PState *st, const char *msg, const char *what)
{
	snprintf(st->err, st->errlen, msg, what);
	return false;
}

static void
skip_spaces(PState *st)
{
	while (isspace((unsigned char) *st->s))
		st->s++;
}

static int
read_num(PState *st, int maxdigits, int *val)
{
	int			n = 0,
				v = 0;

	skip_spaces(st);
	while (n < maxdigits && isdigit((unsigned char) *st->s))
	{
		v = v * 10 + (*st->s - '0');
		st->s++;
		n++;
	}
	*val = v;
	return n;
}

static int
match_name(PState *st, const char *const *names, int count, bool allow_abbrev)
{
	skip_spaces(st);
	for (int i = 0; i < count; i++)
	{
		int			len = strlen(names[i]);

		if (pg_strncasecmp(st->s, names[i], len) == 0)
		{
			st->s += len;
			return i;
		}
	}
	if (allow_abbrev)
		for (int i = 0; i < count; i++)
			if (pg_strncasecmp(st->s, names[i], 3) == 0)
			{
				st->s += 3;
				return i;
			}
	return -1;
}

static int
rr_year(int yy, int cur)
{
	int			c2 = cur % 100,
				base = cur - c2;

	if (yy < 50)
		return c2 < 50 ? base + yy : base + 100 + yy;
	return c2 < 50 ? base - 100 + yy : base + yy;
}

bool
otz_parse_format(const char *str, const char *fmt, OtzParsed *ps,
				 char *err, size_t errlen)
{
	PState		st;
	const char *p = fmt;
	struct pg_tm now;
	int			year = 0,
				mon = 0,
				mday = 0,
				yday = 0,
				julian = 0,
				hour = 0,
				v,
				n;
	bool		have_year = false,
				h12 = false,
				pm = false,
				have_ampm = false;
	bool		have_tzh = false,
				tzh_neg = false;
	int			tzh = 0,
				tzm = 0;
	bool		have_tzr_offset = false;
	int32		tzr_offset = 0;

	st.s = str;
	st.err = err;
	st.errlen = errlen;
	memset(ps, 0, sizeof(*ps));
	otz_current_local(&now);

	while (*p)
	{
		const KeyWord *k;

		if (*p == '"')
		{
			const char *e = strchr(p + 1, '"');
			int			len = e ? (int) (e - p - 1) : (int) strlen(p + 1);

			if (pg_strncasecmp(st.s, p + 1, len) != 0)
				return perr(&st, "literal \"%s\" not found in input", p + 1);
			st.s += len;
			p += len + 1 + (e ? 1 : 0);
			continue;
		}
		if (isspace((unsigned char) *p))
		{
			skip_spaces(&st);
			p++;
			continue;
		}
		k = match_keyword(p);
		if (k == NULL)
		{
			if (isalpha((unsigned char) *p))
				return perr(&st, "date format not recognized at \"%s\"", p);
			/* punctuation: lenient like Oracle */
			if (*st.s && !isalnum((unsigned char) *st.s) && !isspace((unsigned char) *st.s))
				st.s++;
			p++;
			continue;
		}

		switch (k->id)
		{
			case K_SYYYY:
			case K_YYYY:
			case K_RRRR:
				{
					int			sign = 1;

					skip_spaces(&st);
					if (*st.s == '-' || *st.s == '+')
					{
						if (k->id != K_SYYYY)
							return perr(&st, "signed year requires SYYYY%s", "");
						sign = (*st.s++ == '-') ? -1 : 1;
					}
					n = read_num(&st, 4, &v);
					if (n == 0)
						return perr(&st, "year expected at \"%s\"", st.s);
					if (k->id == K_RRRR && n <= 2)
						v = rr_year(v, now.tm_year);
					v *= sign;
					if (v == 0)
						return perr(&st, "year 0 is not allowed%s", "");
					year = v < 0 ? v + 1 : v;	/* to astronomical */
					have_year = true;
				}
				break;
			case K_YYY:
			case K_YY:
			case K_Y:
				{
					int			digits = (k->id == K_YYY) ? 3 : (k->id == K_YY) ? 2 : 1;
					int			mod = (digits == 3) ? 1000 : (digits == 2) ? 100 : 10;

					n = read_num(&st, digits, &v);
					if (n == 0)
						return perr(&st, "year expected at \"%s\"", st.s);
					year = now.tm_year - now.tm_year % mod + v;
					have_year = true;
				}
				break;
			case K_RR:
				n = read_num(&st, 4, &v);
				if (n == 0)
					return perr(&st, "year expected at \"%s\"", st.s);
				year = (n <= 2) ? rr_year(v, now.tm_year) : v;
				have_year = true;
				break;
			case K_MM:
				if (read_num(&st, 2, &mon) == 0)
					return perr(&st, "month expected at \"%s\"", st.s);
				break;
			case K_MON:
			case K_MONTH:
				v = match_name(&st, month_names, 12, true);
				if (v < 0)
					return perr(&st, "month name expected at \"%s\"", st.s);
				mon = v + 1;
				break;
			case K_DD:
				if (read_num(&st, 2, &mday) == 0)
					return perr(&st, "day expected at \"%s\"", st.s);
				break;
			case K_DDD:
				if (read_num(&st, 3, &yday) == 0)
					return perr(&st, "day of year expected at \"%s\"", st.s);
				break;
			case K_D:
				if (read_num(&st, 1, &v) == 0)
					return perr(&st, "day of week expected at \"%s\"", st.s);
				break;
			case K_DAY:
			case K_DY:
				if (match_name(&st, day_names, 7, true) < 0)
					return perr(&st, "day name expected at \"%s\"", st.s);
				break;
			case K_J:
				if (read_num(&st, 7, &julian) == 0)
					return perr(&st, "julian day expected at \"%s\"", st.s);
				break;
			case K_HH:
			case K_HH12:
				if (read_num(&st, 2, &hour) == 0 || hour < 1 || hour > 12)
					return perr(&st, "hour must be between 1 and 12 at \"%s\"", st.s);
				h12 = true;
				break;
			case K_HH24:
				if (read_num(&st, 2, &hour) == 0)
					return perr(&st, "hour expected at \"%s\"", st.s);
				break;
			case K_MI:
				if (read_num(&st, 2, &ps->min) == 0)
					return perr(&st, "minutes expected at \"%s\"", st.s);
				break;
			case K_SS:
				if (read_num(&st, 2, &ps->sec) == 0)
					return perr(&st, "seconds expected at \"%s\"", st.s);
				break;
			case K_SSSSS:
				if (read_num(&st, 5, &v) == 0 || v > 86399)
					return perr(&st, "seconds past midnight expected at \"%s\"", st.s);
				hour = v / 3600;
				ps->min = (v / 60) % 60;
				ps->sec = v % 60;
				break;
			case K_FF:
			case K_FFN:
				{
					static const int32 scale[] = {1, 10, 100, 1000, 10000,
					100000, 1000000, 10000000, 100000000};
					int			maxd = (k->id == K_FF) ? 9 : (p[2] - '0');

					n = read_num(&st, maxd, &v);
					if (n == 0)
						return perr(&st, "fractional seconds expected at \"%s\"", st.s);
					ps->nsec = v * scale[9 - n];
					ps->ndigits = n;
				}
				break;
			case K_X:
				if (*st.s == '.' || *st.s == ',')
					st.s++;
				else
					return perr(&st, "decimal character expected at \"%s\"", st.s);
				break;
			case K_AM:
			case K_PM:
			case K_AM_DOT:
			case K_PM_DOT:
				skip_spaces(&st);
				if (pg_strncasecmp(st.s, "A.M.", 4) == 0 || pg_strncasecmp(st.s, "P.M.", 4) == 0)
				{
					pm = pg_toupper((unsigned char) st.s[0]) == 'P';
					st.s += 4;
				}
				else if (pg_strncasecmp(st.s, "AM", 2) == 0 || pg_strncasecmp(st.s, "PM", 2) == 0)
				{
					pm = pg_toupper((unsigned char) st.s[0]) == 'P';
					st.s += 2;
				}
				else
					return perr(&st, "AM/PM expected at \"%s\"", st.s);
				have_ampm = true;
				break;
			case K_TZH:
				skip_spaces(&st);
				tzh_neg = false;
				if (*st.s == '-' || *st.s == '+')
					tzh_neg = (*st.s++ == '-');
				if (read_num(&st, 2, &tzh) == 0)
					return perr(&st, "time zone hour expected at \"%s\"", st.s);
				have_tzh = true;
				break;
			case K_TZM:
				if (read_num(&st, 2, &tzm) == 0 || tzm > 59)
					return perr(&st, "time zone minute expected at \"%s\"", st.s);
				break;
			case K_TZR:
				skip_spaces(&st);
				if (*st.s == '+' || *st.s == '-')
				{
					if (!otz_parse_offset(&st.s, &tzr_offset, true))
						return perr(&st, "time zone offset expected at \"%s\"", st.s);
					have_tzr_offset = true;
				}
				else
				{
					char		tok[OTZ_ZONE_MAXLEN + 1];
					const char *s0 = st.s;

					while (*st.s && (isalnum((unsigned char) *st.s) || *st.s == '/' ||
									 *st.s == '_' || *st.s == '-' || *st.s == '+'))
						st.s++;
					if (st.s == s0 || (size_t) (st.s - s0) > OTZ_ZONE_MAXLEN)
						return perr(&st, "time zone region expected at \"%s\"", s0);
					memcpy(tok, s0, st.s - s0);
					tok[st.s - s0] = '\0';
					if (!otz_lookup_region(tok, &ps->zone))
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
								 errmsg("time zone region \"%s\" not recognized", tok),
								 errdetail("Oracle ORA-01882.")));
					ps->have_zone = true;
				}
				break;
			case K_TZD:
				{
					const char *s0;

					skip_spaces(&st);
					s0 = st.s;
					while (*st.s && (isalnum((unsigned char) *st.s) || *st.s == '+' || *st.s == '-'))
						st.s++;
					if (st.s == s0 || (size_t) (st.s - s0) > OTZ_ABBR_MAXLEN)
						return perr(&st, "time zone abbreviation expected at \"%s\"", s0);
					memcpy(ps->tzd, s0, st.s - s0);
					ps->tzd[st.s - s0] = '\0';
				}
				break;
			case K_FM:
				break;
		}
		p += k->len;
	}
	skip_spaces(&st);
	if (*st.s != '\0')
		return perr(&st, "input value has extra characters at the end: \"%s\" (Oracle ORA-01830)", st.s);

	/* assemble date */
	if (julian > 0)
	{
		j2date(julian, &ps->year, &ps->mon, &ps->mday);
	}
	else
	{
		ps->year = have_year ? year : now.tm_year;
		if (yday > 0)
		{
			int			jd = date2j(ps->year, 1, 1) + yday - 1;
			int			yy;

			j2date(jd, &yy, &ps->mon, &ps->mday);
			if (yy != ps->year)
				return perr(&st, "day of year out of range%s", "");
		}
		else
		{
			ps->mon = mon ? mon : now.tm_mon;
			ps->mday = mday ? mday : 1;
		}
	}

	if (h12)
	{
		if (hour == 12)
			hour = 0;
		if (pm)
			hour += 12;
	}
	else if (have_ampm && pm && hour < 12)
		hour += 12;
	ps->hour = hour;

	if (have_tzr_offset)
	{
		ps->have_zone = true;
		ps->zone.is_region = false;
		ps->zone.offset = tzr_offset;
	}
	else if (have_tzh)
	{
		int32		off = tzh * 3600 + tzm * 60;

		if (tzh > (tzh_neg ? 12 : 14))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("time zone hour must be between -12 and 14 (Oracle ORA-01874)")));
		if (tzh_neg)
			off = -off;
		if (ps->have_zone && ps->zone.is_region)
		{
			ps->have_hint = true;
			ps->hint = off;
		}
		else
		{
			ps->have_zone = true;
			ps->zone.is_region = false;
			ps->zone.offset = off;
		}
	}
	return true;
}
