/*
 * tix/model.h -- entity types, operations and the op-pack codec (pure, no I/O).
 *
 * An op-pack is the NDJSON blob stored in each entity commit (architecture §3.3):
 *
 *   line 1  {"tix":1,"type":"ticket","author":"<identity hex>"|null,"lamport":N,"time":T}
 *   line 2+ one operation per line, e.g. {"op":"set-title","title":"..."}
 *
 * Encoding is canonical: decode re-encodes every known line and rejects any
 * byte difference (whitespace, key order, number form, escapes, unsorted sets).
 * So one logical pack has exactly one byte form. Unknown op names are kept
 * verbatim and ignored, for forward compatibility. Adding a field to an existing
 * op therefore needs a new op name or a format version bump.
 */
#ifndef TIX_MODEL_H
#define TIX_MODEL_H

#include "tix/base.h"
#include "tix/base_buf.h"
#include "tix/base_date.h"
#include "tix/base_oid.h"

/* ---- limits (validation of untrusted input) ---------------------------- */
#define TIX_PACK_MAX_BYTES (1u << 20)
#define TIX_PACK_MAX_OPS 1000
#define TIX_ENTITY_MAX_COMMITS 20000
#define TIX_TITLE_MAX 256
#define TIX_TEXT_MAX 65536   /* bodies, comments */
#define TIX_LABEL_MAX 64
#define TIX_LABELS_PER_OP 64
#define TIX_NAME_MAX 256     /* identity name, email */
#define TIX_FPR_MAX 128
#define TIX_PUBKEY_MAX 16384
#define TIX_LAMPORT_MAX (1ull << 53)
#define TIX_NONCE_HEX 32      /* create ops carry 128 random bits, see tix_op */
#define TIX_RANK_MAX 64       /* fractional index: [0-9a-z], never ending in '0' */
#define TIX_DURATION_MAX 100000 /* working days */
#define TIX_EFFORT_MAX 10000000 /* hours */
#define TIX_LAG_MAX 10000     /* +- working days */
#define TIX_UNITS_MAX 100000  /* percent of a resource's capacity */
#define TIX_EXCEPTION_MAX_DAYS 3660
#define TIX_COST_MAX (1ll << 50) /* minor currency units */
#define TIX_CURRENCY_MAX 8      /* letters */
#define TIX_EXTERNAL_MAX 512    /* "forgejo:host/owner/repo#12" */

/* ---- entity types -------------------------------------------------------- */
typedef enum tix_entity_type {
	TIX_ENTITY_IDENTITY = 1,
	TIX_ENTITY_TICKET = 2,
	TIX_ENTITY_RESOURCE = 3,
	TIX_ENTITY_CALENDAR = 4,
	TIX_ENTITY_BASELINE = 5, /* a plan snapshot (after v1, architecture §14.2) */
} tix_entity_type;

/* Every type, in sync order (identities first: other packs name their authors). */
extern const tix_entity_type tix_entity_types[5];
#define TIX_ENTITY_NTYPES 5

/* RETURNS "identity" / "ticket" / "resource" / "calendar" / "baseline". */
const char *tix_entity_type_name(tix_entity_type t);
/* RETURNS the ref namespace, e.g. "refs/tix/tickets/" (with trailing slash). */
const char *tix_entity_ref_prefix(tix_entity_type t);

/* Op id: the commit holding the op and its 1-based line in ops.ndjson (>= 2). */
typedef struct tix_opid {
	tix_oid commit;
	uint32_t line;
} tix_opid;

int tix_opid_cmp(const tix_opid *a, const tix_opid *b);
/* "<hex>:<line>"; REQUIRES out has TIX_OID_MAX_HEX + 12 bytes. RETURNS out. */
char *tix_opid_str(const tix_opid *id, char *out);
bool tix_opid_parse(tix_opid *out, const char *s);

typedef enum tix_ticket_status {
	TIX_TICKET_OPEN = 0,
	TIX_TICKET_IN_PROGRESS,
	TIX_TICKET_BLOCKED,
	TIX_TICKET_DONE,
	TIX_TICKET_CANCELLED,
} tix_ticket_status;

const char *tix_ticket_status_name(tix_ticket_status s);
bool tix_ticket_status_parse(const char *s, tix_ticket_status *out);

typedef enum tix_dep_kind { TIX_DEP_FS = 0, TIX_DEP_SS, TIX_DEP_FF, TIX_DEP_SF } tix_dep_kind;
const char *tix_dep_kind_name(tix_dep_kind k); /* "FS" ... */
bool tix_dep_kind_parse(const char *s, tix_dep_kind *out);

/* ---- scalar fields (LWW registers) -------------------------------------- */
/*
 * Planning, resource and calendar scalars share one op shape, driven by a table:
 * `{"op":"set-<name>","<key>":<value>}`, where the value kind, range, enum names
 * and nullability come from tix_field_info. One op name per field keeps the wire
 * self-describing and lets older readers skip them as unknown ops.
 */
typedef enum tix_field {
	/* ticket: WBS + scheduling (architecture §4.2) */
	TIX_F_PARENT, TIX_F_RANK, TIX_F_KIND, TIX_F_CALENDAR, TIX_F_MODE, TIX_F_START, TIX_F_FINISH,
	TIX_F_DURATION, TIX_F_EFFORT, TIX_F_DEADLINE, TIX_F_PROGRESS, TIX_F_ACTUAL_START, TIX_F_ACTUAL_FINISH,
	/* resource (§4.3) */
	TIX_F_RES_NAME, TIX_F_RES_KIND, TIX_F_CAPACITY, TIX_F_RES_CALENDAR, TIX_F_RES_IDENTITY, TIX_F_RATE,
	TIX_F_CURRENCY, TIX_F_ACTIVE,
	/* calendar (§4.4) */
	TIX_F_CAL_NAME, TIX_F_WEEKDAYS, TIX_F_HOURS,
	/* ticket: link to an issue in another tracker (after v1, §14.4) */
	TIX_F_EXTERNAL,
	TIX_F_COUNT
} tix_field;

typedef enum tix_value_kind {
	TIX_V_NULL = 0,
	TIX_V_INT,  /* i */
	TIX_V_BOOL, /* i: 0/1 */
	TIX_V_DATE, /* i: tix_date */
	TIX_V_ENUM, /* i: index into tix_field_info.enums */
	TIX_V_STR,  /* s */
	TIX_V_ID,   /* id: an entity id */
} tix_value_kind;

/* OWNS s. */
typedef struct tix_value {
	tix_value_kind kind;
	int64_t i;
	char *s;
	tix_oid id;
} tix_value;

typedef struct tix_field_info {
	tix_entity_type type;
	const char *op;    /* "set-start" */
	const char *key;   /* "start" */
	tix_value_kind kind;
	bool nullable;
	int64_t min, max;  /* INT: value range; STR: length range */
	const char *const *enums; /* ENUM: NULL-terminated names */
} tix_field_info;

const tix_field_info *tix_field_get(tix_field f);
/* RETURNS false if no field of type t has that key ("start", "capacity", ...). */
bool tix_field_by_key(tix_entity_type t, const char *key, tix_field *out);
/* RETURNS the enum index of name for field f, or -1. */
int tix_field_enum(tix_field f, const char *name);

void tix_value_free(tix_value *v);
tix_value tix_value_null(void);
tix_value tix_value_int(tix_value_kind kind, int64_t i); /* INT, BOOL, DATE or ENUM */
tix_value tix_value_str(const char *s);                 /* copies */
tix_value tix_value_id(const tix_oid *id);

/* Ticket kinds and schedule modes are TIX_V_ENUM indices of their fields. */
typedef enum tix_ticket_kind { TIX_KIND_TASK = 0, TIX_KIND_MILESTONE } tix_ticket_kind;
typedef enum tix_sched_mode { TIX_MODE_AUTO = 0, TIX_MODE_MANUAL } tix_sched_mode;
typedef enum tix_resource_kind { TIX_RES_PERSON = 0, TIX_RES_ROLE, TIX_RES_EQUIPMENT } tix_resource_kind;

/* Weekday mask: bit 0 = Monday ... bit 6 = Sunday. */
#define TIX_WEEKDAYS_MON_FRI 0x1f

/*
 * Fractional index strictly between a and b (NULL = open end), for sibling order
 * without rewriting siblings. Ranks are [0-9a-z]{1,64} and never end in '0'.
 * RETURNS TIX_ERR_INVALID if a >= b or a rank is malformed, TIX_ERR_CONFLICT if
 * the result would exceed TIX_RANK_MAX (rebalancing is a later concern).
 */
tix_status tix_rank_between(const char *a, const char *b, tix_buf *out, tix_error *err);
bool tix_rank_valid(const char *s);

/* ---- operations ---------------------------------------------------------- */
typedef enum tix_op_kind {
	TIX_OP_UNKNOWN = 0,
	/* ticket */
	TIX_OP_TICKET_CREATE,  /* "create": nonce, title, body, labels */
	TIX_OP_SET_TITLE,      /* "set-title": title */
	TIX_OP_SET_BODY,       /* "set-body": body */
	TIX_OP_SET_STATUS,     /* "set-status": status */
	TIX_OP_LABEL,          /* "label": add[], remove[] */
	TIX_OP_COMMENT,        /* "comment": body */
	TIX_OP_EDIT_COMMENT,   /* "edit-comment": target, body */
	/* identity */
	TIX_OP_IDENTITY_CREATE, /* "create": nonce, name, email */
	TIX_OP_SET_NAME,        /* "set-name": name */
	TIX_OP_SET_EMAIL,       /* "set-email": email */
	TIX_OP_KEY_ADD,         /* "key-add": kind, fingerprint, pubkey */
	TIX_OP_KEY_REMOVE,      /* "key-remove": fingerprint */
	/* ticket planning (M4) */
	TIX_OP_SET_FIELD,       /* "set-<field>": see tix_field */
	TIX_OP_DEP_ADD,         /* "dep-add": pred, kind, lag */
	TIX_OP_DEP_REMOVE,      /* "dep-remove": pred */
	TIX_OP_ASSIGN,          /* "assign": resource, units, effort|null */
	TIX_OP_UNASSIGN,        /* "unassign": resource */
	/* resource / calendar (M4) */
	TIX_OP_RESOURCE_CREATE, /* "create": nonce, name, kind */
	TIX_OP_CALENDAR_CREATE, /* "create": nonce, name */
	TIX_OP_EXCEPTION_ADD,   /* "exception-add": from, to, working, note */
	TIX_OP_EXCEPTION_REMOVE, /* "exception-remove": from, to */
	/* baseline (after v1) */
	TIX_OP_BASELINE_CREATE,   /* "create": nonce, name, taken, root|null */
	TIX_OP_BASELINE_SET_NAME, /* "set-name": name */
	TIX_OP_BASELINE_TASK,     /* "task": ticket, start, finish, duration, effort, cost, currency (null = unknown) */
} tix_op_kind;

/* "task" op of a baseline: one planned task as it was when the snapshot was taken. */
typedef struct tix_baseline_task_op {
	tix_oid ticket;
	tix_date start, finish;         /* TIX_DATE_NONE = unscheduled */
	int64_t duration, effort, cost; /* -1 = unknown */
	char *currency;                 /* NULL = none (no cost, or mixed currencies) */
} tix_baseline_task_op;

/*
 * One operation. OWNS every string/array member; free with tix_op_free.
 *
 * Create ops carry a `nonce` (TIX_NONCE_HEX lowercase hex chars of fresh randomness
 * from the caller). Entity ids are root-commit hashes, so without it two identical
 * creates (same content, author, time, lamport) would collapse into one entity.
 */
typedef struct tix_op {
	tix_op_kind kind;
	union {
		struct { char *nonce, *title, *body; tix_strs labels; } ticket_create;
		struct { char *nonce, *name, *email; } identity_create;
		struct { char *value; } text;   /* set-title/body/name/email, comment */
		struct { tix_ticket_status status; } set_status;
		struct { tix_strs add, remove; } label;
		struct { tix_opid target; char *body; } edit_comment;
		struct { char *kind, *fingerprint, *pubkey; } key_add;
		struct { char *fingerprint; } key_remove;
		struct { tix_field field; tix_value v; } set;
		struct { tix_oid pred; tix_dep_kind kind; int32_t lag; } dep_add;
		struct { tix_oid target; } unref; /* dep-remove (pred), unassign (resource) */
		struct { tix_oid resource; int32_t units; int64_t effort; /* -1 = null */ } assign;
		struct { char *nonce, *name; tix_resource_kind kind; } resource_create;
		struct { char *nonce, *name; } calendar_create;
		struct { tix_date from, to; bool working; char *note; } exception_add;
		struct { tix_date from, to; } exception_remove;
		struct { char *nonce, *name; tix_date taken; tix_oid root; /* null = whole plan */ } baseline_create;
		tix_baseline_task_op baseline_task;
		struct { char *name, *raw; } unknown; /* op name + original line */
	} u;
} tix_op;

void tix_op_free(tix_op *op);

/* RETURNS the wire name ("set-title", ...), or "unknown" (also for SET_FIELD,
 * whose name depends on the field: use tix_op_wire_name). */
const char *tix_op_name(tix_op_kind kind);
const char *tix_op_wire_name(const tix_op *op);
/* RETURNS true for any entity type's create op. */
bool tix_op_is_create(tix_op_kind kind);
/* RETURNS the create op kind of entity type t. */
tix_op_kind tix_create_kind(tix_entity_type t);

/* Constructors: copy their arguments. Validation happens in tix_pack_encode.
 * A NULL nonce is allowed here so core can fill in fresh randomness. */
tix_op tix_op_ticket_create(const char *nonce, const char *title, const char *body, const tix_strs *labels);
tix_op tix_op_identity_create(const char *nonce, const char *name, const char *email);
/* kind: SET_TITLE, SET_BODY, COMMENT, SET_NAME, SET_EMAIL or BASELINE_SET_NAME */
tix_op tix_op_text(tix_op_kind kind, const char *value);
tix_op tix_op_set_status(tix_ticket_status s);
tix_op tix_op_label(const tix_strs *add, const tix_strs *remove);
tix_op tix_op_edit_comment(const tix_opid *target, const char *body);
tix_op tix_op_key_add(const char *kind, const char *fingerprint, const char *pubkey);
tix_op tix_op_key_remove(const char *fingerprint);
/* TAKES v (the op owns it afterwards). */
tix_op tix_op_set(tix_field f, tix_value v);
tix_op tix_op_dep_add(const tix_oid *pred, tix_dep_kind kind, int32_t lag);
tix_op tix_op_dep_remove(const tix_oid *pred);
tix_op tix_op_assign(const tix_oid *resource, int32_t units, int64_t effort);
tix_op tix_op_unassign(const tix_oid *resource);
tix_op tix_op_resource_create(const char *nonce, const char *name, tix_resource_kind kind);
tix_op tix_op_calendar_create(const char *nonce, const char *name);
tix_op tix_op_exception_add(tix_date from, tix_date to, bool working, const char *note);
tix_op tix_op_exception_remove(tix_date from, tix_date to);
tix_op tix_op_baseline_create(const char *nonce, const char *name, tix_date taken, const tix_oid *root);
tix_op tix_op_baseline_task(const tix_oid *ticket, tix_date start, tix_date finish, int64_t duration, int64_t effort,
			    int64_t cost, const char *currency);

/* ---- op-packs ------------------------------------------------------------ */
typedef struct tix_pack {
	tix_entity_type type;
	tix_oid author;   /* identity id; null (algo NONE) only in identity packs */
	uint64_t lamport; /* 1 .. TIX_LAMPORT_MAX */
	int64_t time;     /* unix seconds, informational */
	tix_op *ops;
	size_t nops;
} tix_pack;

/* Frees ops (and the array) when the pack owns them, i.e. came from tix_pack_decode. */
void tix_pack_free(tix_pack *p);

/*
 * Appends the canonical NDJSON encoding of p to out.
 * RETURNS TIX_ERR_INVALID (out unchanged) if any field breaks the schema or limits,
 * so a writer can never produce a pack that readers would reject.
 * BORROWS p.
 */
tix_status tix_pack_encode(const tix_pack *p, tix_buf *out, tix_error *err);

/*
 * Parses and validates an untrusted pack. `algo` is the repository hash, which
 * every embedded id must use.
 * ENSURES on TIX_OK: *out is filled and owned by the caller (tix_pack_free).
 * On error *out is zeroed and err says which line failed.
 */
tix_status tix_pack_decode(const char *data, size_t len, tix_hash_algo algo, tix_pack *out,
			   tix_error *err);

#endif
