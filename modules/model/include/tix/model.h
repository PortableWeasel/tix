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

/* ---- entity types -------------------------------------------------------- */
typedef enum tix_entity_type {
	TIX_ENTITY_IDENTITY = 1,
	TIX_ENTITY_TICKET = 2,
} tix_entity_type;

/* RETURNS "identity" / "ticket". */
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
} tix_op_kind;

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
		struct { char *name, *raw; } unknown; /* op name + original line */
	} u;
} tix_op;

void tix_op_free(tix_op *op);

/* Constructors: copy their arguments. Validation happens in tix_pack_encode. */
tix_op tix_op_ticket_create(const char *nonce, const char *title, const char *body, const tix_strs *labels);
tix_op tix_op_identity_create(const char *nonce, const char *name, const char *email);
/* kind: SET_TITLE, SET_BODY, COMMENT, SET_NAME or SET_EMAIL */
tix_op tix_op_text(tix_op_kind kind, const char *value);
tix_op tix_op_set_status(tix_ticket_status s);
tix_op tix_op_label(const tix_strs *add, const tix_strs *remove);
tix_op tix_op_edit_comment(const tix_opid *target, const char *body);
tix_op tix_op_key_add(const char *kind, const char *fingerprint, const char *pubkey);
tix_op tix_op_key_remove(const char *fingerprint);

/* ---- op-packs ------------------------------------------------------------ */
typedef struct tix_pack {
	tix_entity_type type;
	tix_oid author;   /* identity id; null (algo NONE) only for an identity's own root pack */
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
