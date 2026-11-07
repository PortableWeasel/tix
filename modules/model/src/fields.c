/* Scalar field table, values, and fractional ranks (see tix/model.h). */
#include "tix/model.h"

#include <stdlib.h>
#include <string.h>

static const char *const kind_names[] = {"task", "milestone", NULL};
static const char *const mode_names[] = {"auto", "manual", NULL};
static const char *const res_kind_names[] = {"person", "role", "equipment", NULL};
static const char *const id_kind_names[] = {"human", "agent", NULL};

#define T TIX_ENTITY_TICKET
#define R TIX_ENTITY_RESOURCE
#define C TIX_ENTITY_CALENDAR
#define I TIX_ENTITY_IDENTITY

/* Indexed by tix_field. Changing a row is a format change. */
static const tix_field_info fields[TIX_F_COUNT] = {
	[TIX_F_PARENT] = {T, "set-parent", "parent", TIX_V_ID, true, 0, 0, NULL},
	[TIX_F_RANK] = {T, "set-rank", "rank", TIX_V_STR, true, 1, TIX_RANK_MAX, NULL},
	[TIX_F_KIND] = {T, "set-kind", "kind", TIX_V_ENUM, false, 0, 0, kind_names},
	[TIX_F_CALENDAR] = {T, "set-calendar", "calendar", TIX_V_ID, true, 0, 0, NULL},
	[TIX_F_MODE] = {T, "set-mode", "mode", TIX_V_ENUM, false, 0, 0, mode_names},
	[TIX_F_START] = {T, "set-start", "start", TIX_V_DATE, true, 0, 0, NULL},
	[TIX_F_FINISH] = {T, "set-finish", "finish", TIX_V_DATE, true, 0, 0, NULL},
	[TIX_F_DURATION] = {T, "set-duration", "duration", TIX_V_INT, true, 0, TIX_DURATION_MAX, NULL},
	[TIX_F_EFFORT] = {T, "set-effort", "effort", TIX_V_INT, true, 0, TIX_EFFORT_MAX, NULL},
	[TIX_F_DEADLINE] = {T, "set-deadline", "deadline", TIX_V_DATE, true, 0, 0, NULL},
	[TIX_F_PROGRESS] = {T, "set-progress", "progress", TIX_V_INT, false, 0, 100, NULL},
	[TIX_F_ACTUAL_START] = {T, "set-actual-start", "actual_start", TIX_V_DATE, true, 0, 0, NULL},
	[TIX_F_ACTUAL_FINISH] = {T, "set-actual-finish", "actual_finish", TIX_V_DATE, true, 0, 0, NULL},

	[TIX_F_RES_NAME] = {R, "set-name", "name", TIX_V_STR, false, 1, TIX_NAME_MAX, NULL},
	[TIX_F_RES_KIND] = {R, "set-kind", "kind", TIX_V_ENUM, false, 0, 0, res_kind_names},
	[TIX_F_CAPACITY] = {R, "set-capacity", "capacity", TIX_V_INT, false, 1, TIX_UNITS_MAX, NULL},
	[TIX_F_RES_CALENDAR] = {R, "set-calendar", "calendar", TIX_V_ID, true, 0, 0, NULL},
	[TIX_F_RES_IDENTITY] = {R, "set-identity", "identity", TIX_V_ID, true, 0, 0, NULL},
	[TIX_F_RATE] = {R, "set-rate", "rate", TIX_V_INT, true, 0, 1000000000, NULL},
	[TIX_F_CURRENCY] = {R, "set-currency", "currency", TIX_V_STR, true, 3, 3, NULL},
	[TIX_F_ACTIVE] = {R, "set-active", "active", TIX_V_BOOL, false, 0, 1, NULL},

	[TIX_F_CAL_NAME] = {C, "set-name", "name", TIX_V_STR, false, 1, TIX_NAME_MAX, NULL},
	[TIX_F_WEEKDAYS] = {C, "set-weekdays", "weekdays", TIX_V_INT, false, 0, 127, NULL},
	[TIX_F_HOURS] = {C, "set-hours", "hours", TIX_V_INT, false, 1, 24, NULL},

	[TIX_F_EXTERNAL] = {T, "set-external", "external", TIX_V_STR, true, 1, TIX_EXTERNAL_MAX, NULL},
	[TIX_F_ID_KIND] = {I, "set-kind", "kind", TIX_V_ENUM, false, 0, 0, id_kind_names},
};

#undef T
#undef R
#undef C

const tix_field_info *tix_field_get(tix_field f)
{
	TIX_REQUIRE((unsigned)f < TIX_F_COUNT);
	return &fields[f];
}

bool tix_field_by_key(tix_entity_type t, const char *key, tix_field *out)
{
	for (unsigned i = 0; i < TIX_F_COUNT; i++)
		if (fields[i].type == t && !strcmp(fields[i].key, key)) {
			*out = (tix_field)i;
			return true;
		}
	return false;
}

int tix_field_enum(tix_field f, const char *name)
{
	const char *const *e = tix_field_get(f)->enums;
	for (int i = 0; e && e[i]; i++)
		if (!strcmp(e[i], name))
			return i;
	return -1;
}

void tix_value_free(tix_value *v)
{
	free(v->s);
	memset(v, 0, sizeof(*v));
}

tix_value tix_value_null(void)
{
	tix_value v;
	memset(&v, 0, sizeof(v));
	return v;
}

tix_value tix_value_int(tix_value_kind kind, int64_t i)
{
	TIX_REQUIRE(kind == TIX_V_INT || kind == TIX_V_BOOL || kind == TIX_V_DATE || kind == TIX_V_ENUM);
	tix_value v = tix_value_null();
	v.kind = kind;
	v.i = i;
	return v;
}

tix_value tix_value_str(const char *s)
{
	tix_value v = tix_value_null();
	v.kind = TIX_V_STR;
	v.s = tix_xstrdup(s);
	return v;
}

tix_value tix_value_id(const tix_oid *id)
{
	tix_value v = tix_value_null();
	v.kind = TIX_V_ID;
	v.id = *id;
	return v;
}

/* ---- fractional ranks (base 36, digits 0-9a-z) ------------------------------ */

static int digit(char c)
{
	return c >= '0' && c <= '9' ? c - '0' : c - 'a' + 10;
}

static char glyph(int d)
{
	return (char)(d < 10 ? '0' + d : 'a' + d - 10);
}

bool tix_rank_valid(const char *s)
{
	size_t n = strlen(s);
	if (n < 1 || n > TIX_RANK_MAX || s[n - 1] == '0')
		return false;
	for (; *s; s++)
		if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'z')))
			return false;
	return true;
}

/* Appends a key strictly between a and b; a is "" for -inf, b NULL for +inf.
 * REQUIRES a < b, neither ends in '0'. */
static void midpoint(const char *a, const char *b, tix_buf *out)
{
	if (b) {
		size_t n = 0, an = strlen(a); /* common prefix, treating a as padded with '0' */
		while (b[n] && (n < an ? a[n] : '0') == b[n])
			n++;
		if (n) {
			tix_buf_add(out, b, n);
			midpoint(n < an ? a + n : "", b + n, out);
			return;
		}
	}
	int da = a[0] ? digit(a[0]) : 0, db = b ? digit(b[0]) : 36;
	if (db - da > 1) {
		tix_buf_addc(out, glyph((da + db) / 2));
	} else if (b && b[1]) {
		tix_buf_addc(out, b[0]); /* shorter than b, still above a */
	} else {
		tix_buf_addc(out, glyph(da));
		midpoint(a[0] ? a + 1 : "", NULL, out);
	}
}

tix_status tix_rank_between(const char *a, const char *b, tix_buf *out, tix_error *err)
{
	if ((a && !tix_rank_valid(a)) || (b && !tix_rank_valid(b)))
		return tix_error_set(err, TIX_ERR_INVALID, "malformed rank");
	if (a && b && strcmp(a, b) >= 0)
		return tix_error_set(err, TIX_ERR_INVALID, "rank '%s' is not below '%s'", a, b);
	tix_buf tmp = TIX_BUF_INIT;
	midpoint(a ? a : "", b, &tmp);
	if (tmp.len > TIX_RANK_MAX) {
		tix_buf_free(&tmp);
		return tix_error_set(err, TIX_ERR_CONFLICT, "no room between ranks (more than %d characters)",
				     TIX_RANK_MAX);
	}
	TIX_REQUIRE(tix_rank_valid(tmp.data) && (!a || strcmp(a, tmp.data) < 0) &&
		    (!b || strcmp(tmp.data, b) < 0));
	tix_buf_add(out, tmp.data, tmp.len);
	tix_buf_free(&tmp);
	return TIX_OK;
}
