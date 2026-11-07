/*
 * tix/export.h -- render a computed schedule (architecture §2). Pure: writes
 * into a tix_buf, never to files or terminals.
 *
 *   mermaid  a Mermaid `gantt` block: one section per top-level task, critical
 *            tasks tagged `crit`, milestones as milestones; unscheduled tasks
 *            are left out. Names are sanitised for Mermaid's syntax.
 *   csv      RFC 4180, one row per task in WBS order, with a header row. Text
 *            cells that a spreadsheet would run as a formula are prefixed "'".
 *   json     one canonical JSON object: tasks, resource load, diagnostics.
 *   mspdi    Microsoft Project XML: calendars, tasks (WBS outline, dependencies with
 *            lags, constraints, deadlines, progress), resources and assignments.
 */
#ifndef TIX_EXPORT_H
#define TIX_EXPORT_H

#include "tix/base_buf.h"
#include "tix/plan.h"

typedef enum tix_export_format { TIX_EXPORT_MERMAID, TIX_EXPORT_CSV, TIX_EXPORT_JSON, TIX_EXPORT_MSPDI } tix_export_format;

bool tix_export_format_parse(const char *s, tix_export_format *out);

/* Appends the rendering to out. title: Mermaid chart / MSPDI project title (NULL = "tix plan"). */
void tix_export(const tix_schedule *s, tix_export_format f, const char *title, tix_buf *out);

#endif
