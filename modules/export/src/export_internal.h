/* Shared between the export module's renderers; not a public header. */
#ifndef TIX_EXPORT_INTERNAL_H
#define TIX_EXPORT_INTERNAL_H

#include "tix/export.h"

/* MS Project XML (mspdi.c). */
void tix_export_mspdi(const tix_schedule *s, const char *title, const tix_plan_variance *v, tix_buf *out);

#endif
