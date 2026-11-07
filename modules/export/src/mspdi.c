/*
 * MS Project XML (MSPDI) export: calendars, tasks with WBS outline and dependencies,
 * resources and assignments, as Microsoft Project, ProjectLibre, GanttProject and MPXJ
 * read it. Element order follows the MSPDI schema's sequences (taken from MPXJ's
 * generated schema classes); MSPDI readers are order-sensitive.
 *
 * Mapping (architecture §14.1):
 *   - durations are working days x 8 h (MinutesPerDay 480), lags in tenths of minutes;
 *   - calendar UID 1 is tix's built-in Mon-Fri 8 h; tix calendars follow in id order.
 *     Working hours are 08:00-12:00 then 13:00 onwards. tix exceptions may overlap (later
 *     wins); MSPDI's may not, so they are flattened into runs of covered days that share
 *     the resulting working/off status and the winning note;
 *   - task UID 0 is the project summary, then one task per tix task in WBS order;
 *   - auto tasks with a start date get "start no earlier than", manual tasks Manual=1 with
 *     fixed start and finish; deadlines, progress and actual dates carry over.
 */
#include "export_internal.h"

#include <stdio.h>
#include <string.h>

#define MIN_PER_DAY 480
#define LAG_UNITS_PER_DAY (MIN_PER_DAY * 10) /* LinkLag/TotalSlack: tenths of minutes */

/* Text content: XML-escaped; control characters XML 1.0 forbids become spaces. */
static void x_text(tix_buf *out, const char *s)
{
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;
		switch (c) {
		case '&': tix_buf_adds(out, "&amp;"); break;
		case '<': tix_buf_adds(out, "&lt;"); break;
		case '>': tix_buf_adds(out, "&gt;"); break;
		case '"': tix_buf_adds(out, "&quot;"); break;
		case '\'': tix_buf_adds(out, "&apos;"); break;
		default: tix_buf_addc(out, c < 0x20 && c != '\t' && c != '\n' ? ' ' : (char)c);
		}
	}
}

static void el_s(tix_buf *out, const char *name, const char *v)
{
	tix_buf_addf(out, "<%s>", name);
	x_text(out, v);
	tix_buf_addf(out, "</%s>", name);
}

static void el_i(tix_buf *out, const char *name, long long v)
{
	tix_buf_addf(out, "<%s>%lld</%s>", name, v, name);
}

/* Hundredths as a decimal: 150 -> "1.50". */
static void el_cents(tix_buf *out, const char *name, long long v)
{
	tix_buf_addf(out, "<%s>%s%lld.%02lld</%s>", name, v < 0 ? "-" : "", (v < 0 ? -v : v) / 100,
		     (v < 0 ? -v : v) % 100, name);
}

static void el_dt(tix_buf *out, const char *name, tix_date d, const char *time)
{
	char ds[TIX_DATE_STR];
	tix_buf_addf(out, "<%s>%sT%s</%s>", name, tix_date_str(d, ds), time, name);
}

static void el_hours(tix_buf *out, const char *name, long long hours)
{
	tix_buf_addf(out, "<%s>PT%lldH0M0S</%s>", name, hours, name);
}

/* Hours per day -> the end of the working day ("17:00:00" for 8). */
static void day_end(int hours, char *out, size_t n)
{
	hours = hours < 0 ? 0 : hours > 24 ? 24 : hours;
	if (hours <= 4)
		snprintf(out, n, "%02d:00:00", 8 + hours);
	else if (hours <= 15)
		snprintf(out, n, "%02d:00:00", 9 + hours);
	else
		snprintf(out, n, "%02d:00:00", hours >= 24 ? 23 : hours);
}

static void working_times(tix_buf *out, int hours)
{
	tix_buf_adds(out, "<WorkingTimes>");
	if (hours <= 4) {
		tix_buf_addf(out, "<WorkingTime><FromTime>08:00:00</FromTime><ToTime>%02d:00:00</ToTime></WorkingTime>",
			     8 + hours);
	} else if (hours <= 15) {
		tix_buf_adds(out, "<WorkingTime><FromTime>08:00:00</FromTime><ToTime>12:00:00</ToTime></WorkingTime>");
		tix_buf_addf(out, "<WorkingTime><FromTime>13:00:00</FromTime><ToTime>%02d:00:00</ToTime></WorkingTime>",
			     9 + hours);
	} else if (hours < 24) {
		tix_buf_addf(out, "<WorkingTime><FromTime>00:00:00</FromTime><ToTime>%02d:00:00</ToTime></WorkingTime>",
			     hours);
	} else {
		tix_buf_adds(out, "<WorkingTime><FromTime>00:00:00</FromTime><ToTime>23:59:00</ToTime></WorkingTime>");
	}
	tix_buf_adds(out, "</WorkingTimes>");
}

static int cal_hours(const tix_entity *cal)
{
	return cal ? cal->u.calendar.hours : 8;
}

static size_t index_of(const tix_entity *const *v, size_t n, const tix_oid *id)
{
	for (size_t i = 0; i < n; i++)
		if (tix_oid_eq(&v[i]->id, id))
			return i;
	return SIZE_MAX;
}

/* UID of a calendar entity (NULL = built-in = 1). */
static long cal_uid(const tix_schedule *s, const tix_entity *cal)
{
	if (!cal)
		return 1;
	size_t i = index_of(s->calendars, s->ncalendars, &cal->id);
	return i == SIZE_MAX ? 1 : (long)i + 2;
}

/* The latest exception note covering d, for a flattened run's name. */
static const char *note_at(const tix_calendar *c, tix_date d)
{
	const char *note = NULL;
	for (size_t i = 0; i < c->nexceptions; i++)
		if (c->exceptions[i].from <= d && d <= c->exceptions[i].to && c->exceptions[i].note &&
		    *c->exceptions[i].note)
			note = c->exceptions[i].note;
	return note;
}

static bool covered(const tix_calendar *c, tix_date d)
{
	for (size_t i = 0; i < c->nexceptions; i++)
		if (c->exceptions[i].from <= d && d <= c->exceptions[i].to)
			return true;
	return false;
}

#define MAX_EXCEPTION_SPAN 36600 /* days scanned per calendar (100 years) */

static void exceptions(tix_buf *out, const tix_entity *cal)
{
	const tix_calendar *c = &cal->u.calendar;
	if (!c->nexceptions)
		return;
	tix_date lo = c->exceptions[0].from, hi = c->exceptions[0].to;
	for (size_t i = 1; i < c->nexceptions; i++) {
		if (c->exceptions[i].from < lo)
			lo = c->exceptions[i].from;
		if (c->exceptions[i].to > hi)
			hi = c->exceptions[i].to;
	}
	if ((int64_t)hi - lo > MAX_EXCEPTION_SPAN)
		hi = (tix_date)(lo + MAX_EXCEPTION_SPAN);
	tix_buf_adds(out, "<Exceptions>");
	for (tix_date d = lo; d <= hi;) {
		if (!covered(c, d)) {
			d++;
			continue;
		}
		bool working = tix_plan_is_working(cal, d);
		const char *note = note_at(c, d);
		tix_date end = d;
		while (end < hi && covered(c, end + 1) && tix_plan_is_working(cal, end + 1) == working &&
		       note_at(c, end + 1) == note)
			end++;
		tix_buf_adds(out, "<Exception><EnteredByOccurrences>0</EnteredByOccurrences><TimePeriod>");
		el_dt(out, "FromDate", d, "00:00:00");
		el_dt(out, "ToDate", end, "23:59:00");
		tix_buf_adds(out, "</TimePeriod><Occurrences>1</Occurrences>");
		el_s(out, "Name", note ? note : working ? "working" : "off");
		tix_buf_adds(out, "<Type>1</Type>");
		el_i(out, "DayWorking", working);
		if (working)
			working_times(out, c->hours);
		tix_buf_adds(out, "</Exception>");
		d = end + 1;
	}
	tix_buf_adds(out, "</Exceptions>");
}

static void calendar(tix_buf *out, long uid, const char *name, unsigned weekdays, int hours, const tix_entity *cal)
{
	tix_buf_adds(out, "<Calendar>");
	el_i(out, "UID", uid);
	el_s(out, "Name", name);
	tix_buf_adds(out, "<IsBaseCalendar>1</IsBaseCalendar><IsBaselineCalendar>0</IsBaselineCalendar>"
			  "<BaseCalendarUID>-1</BaseCalendarUID><WeekDays>");
	for (int day = 1; day <= 7; day++) { /* MSPDI: 1 = Sunday ... 7 = Saturday */
		int bit = day == 1 ? 6 : day - 2; /* tix: bit 0 = Monday */
		bool on = weekdays & (1u << bit);
		tix_buf_adds(out, "<WeekDay>");
		el_i(out, "DayType", day);
		el_i(out, "DayWorking", on);
		if (on)
			working_times(out, hours);
		tix_buf_adds(out, "</WeekDay>");
	}
	tix_buf_adds(out, "</WeekDays>");
	if (cal)
		exceptions(out, cal);
	tix_buf_adds(out, "</Calendar>");
}

static long long days_to_hours(int64_t days)
{
	return days < 0 ? 0 : (long long)days * (MIN_PER_DAY / 60);
}

static void task(tix_buf *out, const tix_schedule *s, size_t i)
{
	const tix_plan_task *t = &s->tasks[i];
	const tix_ticket *tk = &t->ticket->u.ticket;
	const tix_ticket_plan *p = &tk->plan;
	char end[32];
	day_end(cal_hours(t->calendar), end, sizeof(end));
	bool manual = p->mode == TIX_MODE_MANUAL;
	tix_buf_adds(out, "<Task>");
	el_i(out, "UID", (long long)i + 1);
	el_i(out, "ID", (long long)i + 1);
	el_s(out, "Name", tk->title);
	el_i(out, "Active", tk->status != TIX_TICKET_CANCELLED);
	el_i(out, "Manual", manual && !t->summary);
	el_i(out, "Type", 0);
	el_i(out, "IsNull", 0);
	el_s(out, "WBS", t->wbs);
	el_s(out, "OutlineNumber", t->wbs);
	el_i(out, "OutlineLevel", (long long)t->depth + 1);
	el_i(out, "Priority", 500);
	const char *begin = t->milestone ? end : "08:00:00"; /* a milestone is a moment: day's end */
	if (t->scheduled) {
		el_dt(out, "Start", t->start, begin);
		el_dt(out, "Finish", t->finish, end);
	}
	el_hours(out, "Duration", t->milestone ? 0 : days_to_hours(t->duration));
	el_i(out, "DurationFormat", 7);
	if (t->effort >= 0)
		el_hours(out, "Work", t->effort);
	el_i(out, "EffortDriven", 0);
	el_i(out, "Estimated", !t->scheduled);
	el_i(out, "Milestone", t->milestone);
	el_i(out, "Summary", t->summary);
	el_i(out, "Critical", t->critical);
	if (t->scheduled) {
		el_dt(out, "EarlyStart", t->start, begin);
		el_dt(out, "EarlyFinish", t->finish, end);
		if (t->late_start != TIX_DATE_NONE && t->late_finish != TIX_DATE_NONE) {
			el_dt(out, "LateStart", t->late_start, begin);
			el_dt(out, "LateFinish", t->late_finish, end);
		}
		el_i(out, "TotalSlack", (long long)t->total_float * LAG_UNITS_PER_DAY);
	}
	el_i(out, "PercentComplete", t->progress);
	if (t->actual_start != TIX_DATE_NONE)
		el_dt(out, "ActualStart", t->actual_start, "08:00:00");
	if (t->actual_finish != TIX_DATE_NONE)
		el_dt(out, "ActualFinish", t->actual_finish, end);
	bool snet = !manual && !t->summary && p->start != TIX_DATE_NONE;
	el_i(out, "ConstraintType", snet ? 4 : 0); /* 4: start no earlier than; 0: as soon as possible */
	if (t->calendar)
		el_i(out, "CalendarUID", cal_uid(s, t->calendar));
	if (snet)
		el_dt(out, "ConstraintDate", p->start, "08:00:00");
	if (p->deadline != TIX_DATE_NONE)
		el_dt(out, "Deadline", p->deadline, end);
	if (tk->body && *tk->body)
		el_s(out, "Notes", tk->body);
	for (size_t k = 0; k < p->ndeps; k++) {
		size_t j = tix_schedule_find(s, &p->deps[k].pred);
		if (j == SIZE_MAX)
			continue; /* outside this plan */
		static const int types[] = {[TIX_DEP_FF] = 0, [TIX_DEP_FS] = 1, [TIX_DEP_SF] = 2, [TIX_DEP_SS] = 3};
		tix_buf_adds(out, "<PredecessorLink>");
		el_i(out, "PredecessorUID", (long long)j + 1);
		el_i(out, "Type", types[p->deps[k].kind]);
		el_i(out, "CrossProject", 0);
		el_i(out, "LinkLag", (long long)p->deps[k].lag * LAG_UNITS_PER_DAY);
		el_i(out, "LagFormat", 7);
		tix_buf_adds(out, "</PredecessorLink>");
	}
	if (manual && !t->summary && t->scheduled) {
		el_dt(out, "ManualStart", t->start, "08:00:00");
		el_dt(out, "ManualFinish", t->finish, end);
		el_hours(out, "ManualDuration", days_to_hours(t->duration));
	}
	tix_buf_adds(out, "</Task>");
}

/* The one currency every rated resource uses, or NULL. */
static const char *common_currency(const tix_schedule *s)
{
	const char *cur = NULL;
	for (size_t i = 0; i < s->nresources; i++) {
		const tix_resource *r = &s->resources[i]->u.resource;
		if (r->rate < 0 || !r->currency)
			continue;
		if (cur && strcmp(cur, r->currency))
			return NULL;
		cur = r->currency;
	}
	return cur;
}

void tix_export_mspdi(const tix_schedule *s, const char *title, tix_buf *out)
{
	const char *name = title ? title : "tix plan";
	tix_buf_adds(out, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
			  "<Project xmlns=\"http://schemas.microsoft.com/project\">");
	el_i(out, "SaveVersion", 14);
	el_s(out, "Name", name);
	el_s(out, "Title", name);
	el_i(out, "ScheduleFromStart", 1);
	if (s->start != TIX_DATE_NONE) {
		el_dt(out, "StartDate", s->start, "08:00:00");
		el_dt(out, "FinishDate", s->finish, "17:00:00");
	}
	const char *currency = common_currency(s);
	if (currency)
		el_s(out, "CurrencyCode", currency);
	tix_buf_adds(out, "<CalendarUID>1</CalendarUID><DefaultStartTime>08:00:00</DefaultStartTime>"
			  "<DefaultFinishTime>17:00:00</DefaultFinishTime><MinutesPerDay>480</MinutesPerDay>"
			  "<MinutesPerWeek>2400</MinutesPerWeek><DaysPerMonth>20</DaysPerMonth>"
			  "<DurationFormat>7</DurationFormat><WorkFormat>2</WorkFormat><HonorConstraints>1</HonorConstraints>"
			  "<WeekStartDay>1</WeekStartDay>\n");

	tix_buf_adds(out, "<Calendars>");
	calendar(out, 1, "Standard", 0x1f, 8, NULL);
	for (size_t i = 0; i < s->ncalendars; i++) {
		const tix_calendar *c = &s->calendars[i]->u.calendar;
		calendar(out, (long)i + 2, c->name, c->weekdays, c->hours, s->calendars[i]);
	}
	tix_buf_adds(out, "</Calendars>\n<Tasks>");

	/* UID 0: the project summary task */
	tix_buf_adds(out, "<Task><UID>0</UID><ID>0</ID>");
	el_s(out, "Name", name);
	tix_buf_adds(out, "<Type>1</Type><IsNull>0</IsNull><WBS>0</WBS><OutlineNumber>0</OutlineNumber>"
			  "<OutlineLevel>0</OutlineLevel>");
	if (s->start != TIX_DATE_NONE) {
		el_dt(out, "Start", s->start, "08:00:00");
		el_dt(out, "Finish", s->finish, "17:00:00");
	}
	tix_buf_adds(out, "<Summary>1</Summary></Task>\n");
	for (size_t i = 0; i < s->ntasks; i++) {
		task(out, s, i);
		tix_buf_addc(out, '\n');
	}
	tix_buf_adds(out, "</Tasks>\n<Resources>");
	for (size_t i = 0; i < s->nresources; i++) {
		const tix_resource *r = &s->resources[i]->u.resource;
		tix_buf_adds(out, "<Resource>");
		el_i(out, "UID", (long long)i + 1);
		el_i(out, "ID", (long long)i + 1);
		el_s(out, "Name", r->name);
		el_i(out, "Type", 1); /* work: people, roles and equipment are all booked by time */
		el_i(out, "IsNull", 0);
		el_cents(out, "MaxUnits", r->capacity);
		if (r->rate >= 0) {
			el_cents(out, "StandardRate", r->rate);
			el_i(out, "StandardRateFormat", 2); /* per hour */
		}
		if (!tix_oid_is_null(&r->calendar)) {
			size_t k = index_of(s->calendars, s->ncalendars, &r->calendar);
			if (k != SIZE_MAX)
				el_i(out, "CalendarUID", (long long)k + 2);
		}
		el_i(out, "IsInactive", !r->active);
		tix_buf_adds(out, "</Resource>\n");
	}
	tix_buf_adds(out, "</Resources>\n<Assignments>");
	long uid = 1;
	for (size_t i = 0; i < s->ntasks; i++) {
		const tix_plan_task *t = &s->tasks[i];
		const tix_ticket_plan *p = &t->ticket->u.ticket.plan;
		if (t->summary)
			continue;
		for (size_t k = 0; k < p->nassignments; k++) {
			size_t r = index_of(s->resources, s->nresources, &p->assignments[k].resource);
			if (r == SIZE_MAX)
				continue;
			tix_buf_adds(out, "<Assignment>");
			el_i(out, "UID", uid++);
			el_i(out, "TaskUID", (long long)i + 1);
			el_i(out, "ResourceUID", (long long)r + 1);
			if (t->scheduled) {
				char end[32];
				day_end(cal_hours(t->calendar), end, sizeof(end));
				el_dt(out, "Finish", t->finish, end);
				el_dt(out, "Start", t->start, "08:00:00");
			}
			el_cents(out, "Units", p->assignments[k].units);
			if (p->assignments[k].effort >= 0)
				el_hours(out, "Work", p->assignments[k].effort);
			tix_buf_adds(out, "</Assignment>\n");
		}
	}
	tix_buf_adds(out, "</Assignments>\n</Project>\n");
}
