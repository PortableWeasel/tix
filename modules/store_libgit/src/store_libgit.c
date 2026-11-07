/*
 * The only translation unit allowed to include git.git headers (AGENTS.md).
 * Converts between tix types and git types at this boundary.
 */
#define USE_THE_REPOSITORY_VARIABLE
#include "git-compat-util.h"
#include "abspath.h"
#include "commit.h"
#include "config.h"
#include "environment.h"
#include "hash.h"
#include "hex.h"
#include "ident.h"
#include "lockfile.h"
#include "object-store-ll.h"
#include "packfile.h"
#include "refs.h"
#include "remote.h"
#include "repository.h"
#include "run-command.h"
#include "setup.h"
#include "strbuf.h"
#include "strvec.h"
#include "dir.h"
#include "gpg-interface.h"
#include "tree-walk.h"
#include "version.h"

#include "tix/store_libgit.h"

/* git-compat-util.h wraps exit() through this; normally provided by common-main.c. */
int common_exit(const char *file, int line, int code)
{
	(void)file;
	(void)line;
	return code & 0xff;
}

typedef struct git_store {
	tix_store base; /* must be first */
	bool sign;      /* git config tix.sign: sign every commit with user.signingkey (gpg.format) */
} git_store;

static const tix_store_vt git_vt;
static int opened;

static void to_git(const tix_oid *in, struct object_id *out)
{
	TIX_REQUIRE((int)in->algo == hash_algo_by_ptr(the_hash_algo));
	oidread(out, in->bytes, the_hash_algo);
}

static void from_git(const struct object_id *in, tix_oid *out)
{
	memset(out, 0, sizeof(*out));
	out->algo = (uint8_t)hash_algo_by_ptr(the_hash_algo);
	memcpy(out->bytes, in->hash, the_hash_algo->rawsz);
}

static tix_status git_commit_write(tix_store *s, const tix_oid *parents, size_t np, const char *pack,
				   size_t len, const char *msg, tix_oid *out, tix_error *err)
{
	struct strbuf tree = STRBUF_INIT, author = STRBUF_INIT, committer = STRBUF_INIT;
	struct object_id blob, tree_oid, commit;
	struct commit_list *list = NULL, **tail = &list;
	tix_status st = TIX_OK;

	for (size_t i = 0; i < np && !st; i++) {
		struct object_id p;
		struct commit *c;
		to_git(&parents[i], &p);
		if (oid_object_info(the_repository, &p, NULL) != OBJ_COMMIT ||
		    !(c = lookup_commit(the_repository, &p)))
			st = tix_error_set(err, TIX_ERR_NOT_FOUND, "parent %s is not a commit", oid_to_hex(&p));
		else
			tail = &commit_list_insert(c, tail)->next;
	}
	if (st)
		goto out;
	if (write_object_file(pack, len, OBJ_BLOB, &blob)) {
		st = tix_error_set(err, TIX_ERR_IO, "cannot write blob");
		goto out;
	}
	strbuf_addstr(&tree, "100644 ops.ndjson");
	strbuf_addch(&tree, '\0');
	strbuf_add(&tree, blob.hash, the_hash_algo->rawsz);
	if (write_object_file(tree.buf, tree.len, OBJ_TREE, &tree_oid)) {
		st = tix_error_set(err, TIX_ERR_IO, "cannot write tree");
		goto out;
	}
	/* Non-strict idents: fall back to git's defaults rather than die(). */
	strbuf_addstr(&author, git_author_info(0));
	strbuf_addstr(&committer, git_committer_info(0));
	char *key = ((git_store *)s)->sign ? get_signing_key() : NULL;
	if (((git_store *)s)->sign && (!key || !*key)) {
		free(key);
		st = tix_error_set(err, TIX_ERR_INVALID, "tix.sign is set but there is no user.signingkey");
		goto out;
	}
	int failed = commit_tree_extended(msg, strlen(msg), &tree_oid, list, &commit, author.buf, committer.buf,
					  key, NULL);
	free(key);
	if (failed) {
		st = tix_error_set(err, TIX_ERR_IO, ((git_store *)s)->sign
							    ? "cannot sign the commit (see user.signingkey and gpg.format)"
							    : "cannot write commit");
		goto out;
	}
	from_git(&commit, out);
out:
	free_commit_list(list);
	strbuf_release(&tree);
	strbuf_release(&author);
	strbuf_release(&committer);
	return st;
}

/* Exactly one entry: regular file "ops.ndjson" that is a blob. */
static tix_status find_pack_blob(const struct object_id *tree_oid, struct object_id *blob, tix_error *err)
{
	enum object_type type;
	unsigned long size;
	struct tree_desc desc;
	struct name_entry entry;
	int n = 0, found = 0;
	void *buf = repo_read_object_file(the_repository, tree_oid, &type, &size);

	if (!buf || type != OBJ_TREE) {
		free(buf);
		return tix_error_set(err, TIX_ERR_INVALID, "commit tree unreadable");
	}
	if (init_tree_desc_gently(&desc, tree_oid, buf, size, 0)) {
		free(buf);
		return tix_error_set(err, TIX_ERR_INVALID, "corrupt tree");
	}
	while (tree_entry_gently(&desc, &entry)) {
		n++;
		if (entry.pathlen == 10 && !memcmp(entry.path, "ops.ndjson", 10) && S_ISREG(entry.mode)) {
			oidcpy(blob, &entry.oid);
			found = 1;
		}
	}
	free(buf);
	if (n != 1 || !found)
		return tix_error_set(err, TIX_ERR_INVALID, "not an op-pack commit (tree must hold only ops.ndjson)");
	return TIX_OK;
}

static tix_status git_commit_read(tix_store *s, const tix_oid *oid, tix_commit *out, tix_error *err)
{
	struct object_id goid, blob;
	struct commit *c;
	enum object_type type;
	unsigned long size;
	tix_status st;

	memset(out, 0, sizeof(*out));
	to_git(oid, &goid);
	int t = oid_object_info(the_repository, &goid, NULL);
	if (t < 0)
		return tix_error_set(err, TIX_ERR_NOT_FOUND, "no object %s", oid_to_hex(&goid));
	if (t != OBJ_COMMIT || !(c = lookup_commit(the_repository, &goid)) ||
	    repo_parse_commit_gently(the_repository, c, 1))
		return tix_error_set(err, TIX_ERR_INVALID, "%s is not a commit", oid_to_hex(&goid));
	if ((st = find_pack_blob(get_commit_tree_oid(c), &blob, err)))
		return st;
	if (oid_object_info(the_repository, &blob, &size) != OBJ_BLOB || size > TIX_STORE_MAX_BLOB)
		return tix_error_set(err, TIX_ERR_INVALID, "ops.ndjson missing or larger than %u bytes",
				     TIX_STORE_MAX_BLOB);
	char *data = repo_read_object_file(the_repository, &blob, &type, &size);
	if (!data || type != OBJ_BLOB) {
		free(data);
		return tix_error_set(err, TIX_ERR_IO, "cannot read ops.ndjson");
	}

	out->oid = *oid;
	tix_buf_add(&out->pack, data, size);
	free(data);
	out->nparents = commit_list_count(c->parents);
	if (out->nparents)
		out->parents = tix_xcalloc(out->nparents, sizeof(tix_oid));
	size_t i = 0;
	for (struct commit_list *p = c->parents; p; p = p->next)
		from_git(&p->item->object.oid, &out->parents[i++]);

	unsigned long blen;
	const char *cbuf = repo_get_commit_buffer(the_repository, c, &blen);
	const char *body = cbuf ? strstr(cbuf, "\n\n") : NULL;
	out->message = tix_xstrdup(body ? body + 2 : "");
	if (cbuf)
		repo_unuse_commit_buffer(the_repository, c, cbuf);
	return TIX_OK;
}

static tix_status git_ref_read(tix_store *s, const char *name, tix_oid *out, tix_error *err)
{
	struct object_id oid;
	if (refs_read_ref_full(get_main_ref_store(the_repository), name, RESOLVE_REF_READING, &oid, NULL))
		return tix_error_set(err, TIX_ERR_NOT_FOUND, "no ref %s", name);
	from_git(&oid, out);
	return TIX_OK;
}

typedef struct list_ctx {
	TIX_VEC(tix_ref) refs;
} list_ctx;

static int collect_ref(const char *refname, const char *referent, const struct object_id *oid, int flags,
		       void *cb)
{
	list_ctx *lc = cb;
	(void)referent;
	if (flags & REF_ISSYMREF)
		return 0; /* tix never writes symrefs */
	tix_ref r = {tix_xstrdup(refname), {0}};
	from_git(oid, &r.oid);
	TIX_VEC_PUSH(&lc->refs, r);
	return 0;
}

static int cmp_ref(const void *a, const void *b)
{
	return strcmp(((const tix_ref *)a)->name, ((const tix_ref *)b)->name);
}

static tix_status git_ref_list(tix_store *s, const char *prefix, tix_refs *out, tix_error *err)
{
	list_ctx lc = {{0}};
	refs_for_each_fullref_in(get_main_ref_store(the_repository), prefix, NULL, collect_ref, &lc);
	if (lc.refs.n)
		qsort(lc.refs.v, lc.refs.n, sizeof(tix_ref), cmp_ref);
	out->v = lc.refs.v;
	out->n = lc.refs.n;
	return TIX_OK;
}

static tix_status git_ref_cas(tix_store *s, const char *name, const tix_oid *old_oid, const tix_oid *new_oid,
			      tix_error *err)
{
	struct object_id gnew, gold, cur;
	struct strbuf e = STRBUF_INIT;
	struct ref_store *refs = get_main_ref_store(the_repository);
	int exists = !refs_read_ref_full(refs, name, RESOLVE_REF_READING, &cur, NULL);
	tix_status st = TIX_OK;

	to_git(new_oid, &gnew);
	if (oid_object_info(the_repository, &gnew, NULL) != OBJ_COMMIT)
		return tix_error_set(err, TIX_ERR_NOT_FOUND, "%s: new value is not a commit", name);
	if (!old_oid && exists)
		return tix_error_set(err, TIX_ERR_EXISTS, "%s already exists", name);
	if (old_oid) {
		to_git(old_oid, &gold);
		if (!exists || !oideq(&cur, &gold))
			return tix_error_set(err, TIX_ERR_CONFLICT, "%s changed underneath", name);
	} else {
		oidcpy(&gold, null_oid());
	}
	/* The transaction re-checks old value under the ref lock. */
	struct ref_transaction *tx = ref_store_transaction_begin(refs, &e);
	if (!tx || ref_transaction_update(tx, name, &gnew, &gold, NULL, NULL, 0, "tix", &e) ||
	    ref_transaction_commit(tx, &e))
		st = tix_error_set(err, TIX_ERR_CONFLICT, "%s: %s", name, e.buf);
	ref_transaction_free(tx);
	strbuf_release(&e);
	return st;
}

static void local_path(struct strbuf *p, const char *name)
{
	strbuf_addf(p, "%s/tix", repo_get_git_dir(the_repository));
	if (name)
		strbuf_addf(p, "/%s", name);
}

static tix_status git_local_get(tix_store *s, const char *name, tix_buf *out, tix_error *err)
{
	struct strbuf path = STRBUF_INIT, data = STRBUF_INIT;
	tix_status st = TIX_OK;
	local_path(&path, name);
	if (strbuf_read_file(&data, path.buf, 0) < 0)
		st = errno == ENOENT ? tix_error_set(err, TIX_ERR_NOT_FOUND, "local state '%s' unset", name)
				     : tix_error_set(err, TIX_ERR_IO, "%s: %s", path.buf, strerror(errno));
	else {
		tix_buf_reset(out);
		tix_buf_add(out, data.buf, data.len);
	}
	strbuf_release(&path);
	strbuf_release(&data);
	return st;
}

static tix_status git_local_set(tix_store *s, const char *name, const char *data, size_t len, tix_error *err)
{
	struct strbuf dir = STRBUF_INIT, path = STRBUF_INIT;
	struct lock_file lk = LOCK_INIT;
	tix_status st = TIX_OK;
	local_path(&dir, NULL);
	local_path(&path, name);
	if (mkdir(dir.buf, 0777) && errno != EEXIST)
		st = tix_error_set(err, TIX_ERR_IO, "%s: %s", dir.buf, strerror(errno));
	else if (hold_lock_file_for_update(&lk, path.buf, 0) < 0)
		st = tix_error_set(err, TIX_ERR_IO, "cannot lock %s", path.buf);
	else if (write_in_full(get_lock_file_fd(&lk), data, len) < 0 || commit_lock_file(&lk)) {
		rollback_lock_file(&lk);
		st = tix_error_set(err, TIX_ERR_IO, "cannot write %s", path.buf);
	}
	strbuf_release(&dir);
	strbuf_release(&path);
	return st;
}

/* ---- network: always a child `git` (AGENTS.md) ---------------------------- */

static tix_status check_remote(const char *remote, tix_error *err)
{
	struct remote *rm = remote_get(remote);
	if (!rm || !remote_is_configured(rm, 1))
		return tix_error_set(err, TIX_ERR_NOT_FOUND, "no remote named '%s'", remote);
	return TIX_OK;
}

/* Copies the most useful line of a child's stderr into err, made printable:
 * the first "fatal:"/"error:" line, else the last non-empty one. Remote
 * servers control part of this text, so control characters are replaced. */
static tix_status child_error(tix_error *err, tix_status code, const char *what, const struct strbuf *e)
{
	const char *best = NULL, *last = NULL;
	size_t best_len = 0, last_len = 0;
	for (const char *p = e->buf; p < e->buf + e->len;) {
		const char *nl = memchr(p, '\n', e->buf + e->len - p);
		size_t n = nl ? (size_t)(nl - p) : strlen(p);
		if (n && !best && (starts_with(p, "fatal:") || starts_with(p, "error:"))) {
			best = p;
			best_len = n;
		}
		if (n) {
			last = p;
			last_len = n;
		}
		p += n + 1;
	}
	if (!best) {
		best = last;
		best_len = last_len;
	}
	struct strbuf clean = STRBUF_INIT;
	for (size_t i = 0; best && i < best_len && i < 200; i++)
		strbuf_addch(&clean, (unsigned char)best[i] < 0x20 || best[i] == 0x7f ? '?' : best[i]);
	tix_status st = tix_error_set(err, code, "git %s failed%s%s", what, clean.len ? ": " : "",
				      clean.len ? clean.buf : "");
	strbuf_release(&clean);
	return st;
}

/* A child changed refs and objects behind libgit's back: drop its caches. */
static void forget_caches(void)
{
	struct ref_store *refs = the_repository->refs_private;
	if (refs) {
		ref_store_release(refs);
		free(refs);
		the_repository->refs_private = NULL;
	}
	reprepare_packed_git(the_repository);
}

static void drop_legacy_mirror(const char *remote);

static tix_status git_remote_fetch(tix_store *s, const char *remote, tix_error *err)
{
	struct child_process cp = CHILD_PROCESS_INIT;
	struct strbuf out = STRBUF_INIT, e = STRBUF_INIT;
	tix_status st = check_remote(remote, err);
	if (st)
		return st;
	cp.git_cmd = 1; /* the system `git`, same version as libgit.a */
	cp.no_stdin = 1;
	/* "--" ends options; the remote name was validated anyway. */
	strvec_pushl(&cp.args, "fetch", "--quiet", "--no-tags", "--no-write-fetch-head",
		     "--no-recurse-submodules", "--prune", "--", remote, NULL);
	char mirror[128];
	tix_remote_mirror_prefix(remote, mirror, sizeof(mirror));
	strvec_pushf(&cp.args, "+refs/tix/*:%s*", mirror);
	if (pipe_command(&cp, NULL, 0, &out, 0, &e, 0))
		st = child_error(err, TIX_ERR_IO, "fetch", &e);
	forget_caches();
	if (!st)
		drop_legacy_mirror(remote);
	strbuf_release(&out);
	strbuf_release(&e);
	return st;
}

/* "<kind>/<hex id>" for a kind tix writes: what a pre-0.6 mirror ref looks like. */
static bool legacy_mirror_name(const char *rest)
{
	static const char *const kinds[] = {"identities/", "tickets/", "resources/", "calendars/"};
	for (size_t i = 0; i < sizeof(kinds) / sizeof(*kinds); i++) {
		size_t k = strlen(kinds[i]);
		if (strncmp(rest, kinds[i], k))
			continue;
		size_t n = strspn(rest + k, "0123456789abcdef");
		return !rest[k + n] && n == (size_t)the_hash_algo->hexsz;
	}
	return false;
}

/*
 * Until v0.6 the mirror lived in refs/remotes/<remote>/tix/. After a successful fetch
 * into the new place, delete what is left there so a remote branch named "tix" can be
 * fetched again. Only refs shaped like tix mirrors go; anything else under that prefix
 * is a real remote-tracking branch and stays. Best effort: failures leave stale refs
 * that nothing reads.
 */
static void drop_legacy_mirror(const char *remote)
{
	struct strbuf prefix = STRBUF_INIT, e = STRBUF_INIT;
	list_ctx lc = {{0}};
	struct ref_store *refs = get_main_ref_store(the_repository);
	strbuf_addf(&prefix, "refs/remotes/%s/tix/", remote);
	refs_for_each_fullref_in(refs, prefix.buf, NULL, collect_ref, &lc);
	struct ref_transaction *tx = NULL;
	size_t n = 0;
	for (size_t i = 0; i < lc.refs.n; i++) {
		if (!legacy_mirror_name(lc.refs.v[i].name + prefix.len))
			continue;
		struct object_id old;
		to_git(&lc.refs.v[i].oid, &old);
		if (!tx)
			tx = ref_store_transaction_begin(refs, &e);
		if (tx && !ref_transaction_delete(tx, lc.refs.v[i].name, &old, NULL, 0, "tix: old mirror", &e))
			n++;
	}
	if (tx && n)
		ref_transaction_commit(tx, &e);
	ref_transaction_free(tx);
	if (n)
		forget_caches();
	for (size_t i = 0; i < lc.refs.n; i++)
		free(lc.refs.v[i].name);
	free(lc.refs.v);
	strbuf_release(&prefix);
	strbuf_release(&e);
}

static tix_status git_remote_push(tix_store *s, const char *remote, const char *const *refs, size_t n,
				  tix_error *err)
{
	struct child_process cp = CHILD_PROCESS_INIT;
	struct strbuf out = STRBUF_INIT, e = STRBUF_INIT;
	tix_status st = check_remote(remote, err);
	if (st)
		return st;
	cp.git_cmd = 1;
	cp.no_stdin = 1;
	strvec_pushl(&cp.args, "push", "--porcelain", "--no-verify", "--no-recurse-submodules", "--", remote,
		     NULL);
	for (size_t i = 0; i < n; i++)
		strvec_pushf(&cp.args, "%s:%s", refs[i], refs[i]); /* no '+': never forced */
	int rc = pipe_command(&cp, NULL, 0, &out, 0, &e, 0);
	/* Porcelain: "<flag>\t<src>:<dst>\t<summary>"; '!' = rejected. A plain
	 * "[rejected]" means not a fast-forward; "[remote rejected]" is a hook or
	 * policy refusal, which pulling won't fix. */
	size_t nonff = 0, refused = 0;
	for (const char *p = out.buf; p < out.buf + out.len;) {
		const char *nl = memchr(p, '\n', out.buf + out.len - p);
		size_t len = nl ? (size_t)(nl - p) : strlen(p);
		if (len > 2 && p[0] == '!' && p[1] == '\t') {
			const char *tab = memchr(p + 2, '\t', len - 2);
			if (tab && starts_with(tab + 1, "[rejected]"))
				nonff++;
			else
				refused++;
		}
		p += len + 1;
	}
	if (refused)
		st = child_error(err, TIX_ERR_IO, "push", &e);
	else if (nonff)
		st = tix_error_set(err, TIX_ERR_CONFLICT, "%zu ref(s) rejected by %s (not a fast-forward)", nonff,
				   remote);
	else if (rc)
		st = child_error(err, TIX_ERR_IO, "push", &e);
	forget_caches();
	strbuf_release(&out);
	strbuf_release(&e);
	return st;
}

static void git_refresh(tix_store *s)
{
	(void)s;
	forget_caches();
}

/* ---- signatures (architecture §14.3) ------------------------------------------------ */

/* argv only, never a shell; stdin from `in`. RETURNS the exit status (non-zero also when it
 * couldn't run); stdout goes to out (may be NULL). */
static int run_tool(const char *const *argv, const char *in, size_t in_len, struct strbuf *out)
{
	struct child_process cp = CHILD_PROCESS_INIT;
	struct strbuf o = STRBUF_INIT, e = STRBUF_INIT;
	for (size_t i = 0; argv[i]; i++)
		strvec_push(&cp.args, argv[i]);
	if (!in)
		cp.no_stdin = 1;
	int rc = pipe_command(&cp, in, in_len, &o, 0, &e, 0);
	if (out)
		strbuf_addbuf(out, &o);
	strbuf_release(&o);
	strbuf_release(&e);
	return rc;
}

static bool write_tmp(const char *dir, const char *name, const char *data, size_t len, struct strbuf *path)
{
	strbuf_reset(path);
	strbuf_addf(path, "%s/%s", dir, name);
	int fd = open(path->buf, O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (fd < 0)
		return false;
	bool ok = write_in_full(fd, data, len) >= 0;
	return !close(fd) && ok;
}

/* Upper-case hex digits only: "ab12 CD34" -> "AB12CD34". REQUIRES in is not out's buffer. */
static void norm_fpr(const char *in, struct strbuf *out)
{
	strbuf_reset(out);
	for (; *in; in++)
		if (isxdigit((unsigned char)*in))
			strbuf_addch(out, (char)toupper((unsigned char)*in));
}

static bool verify_ssh(const char *dir, const struct strbuf *payload, const char *sigpath, const tix_sig_key *k)
{
	struct strbuf signers = STRBUF_INIT, path = STRBUF_INIT;
	strbuf_addf(&signers, "tix %s\n", k->pubkey);
	bool good = !strchr(k->pubkey, '\n') && write_tmp(dir, "allowed", signers.buf, signers.len, &path);
	if (good) {
		const char *argv[] = {"ssh-keygen", "-Y", "verify", "-f", path.buf, "-I", "tix", "-n", "git", "-s",
				      sigpath, NULL};
		good = run_tool(argv, payload->buf, payload->len, NULL) == 0;
	}
	if (path.len)
		unlink(path.buf);
	strbuf_release(&signers);
	strbuf_release(&path);
	return good;
}

/* RETURNS the index of the gpg key whose fingerprint gpg reports as VALIDSIG, or SIZE_MAX. */
static size_t verify_gpg(const char *dir, const struct strbuf *payload, const char *sigpath, const tix_sig_key *keys,
			 size_t n)
{
	struct strbuf home = STRBUF_INIT, status = STRBUF_INIT, want = STRBUF_INIT, got = STRBUF_INIT;
	size_t which = SIZE_MAX;
	strbuf_addf(&home, "%s/gnupg", dir);
	if (mkdir(home.buf, 0700))
		goto out;
	for (size_t i = 0; i < n; i++)
		if (!strcmp(keys[i].kind, "gpg")) {
			const char *argv[] = {"gpg", "--homedir", home.buf, "--batch", "--quiet", "--no-autostart", "--import", NULL};
			run_tool(argv, keys[i].pubkey, strlen(keys[i].pubkey), NULL);
		}
	const char *argv[] = {"gpg", "--homedir", home.buf, "--batch", "--no-autostart", "--status-fd", "1", "--verify", sigpath, "-", NULL};
	run_tool(argv, payload->buf, payload->len, &status);
	const char *v = strstr(status.buf, "[GNUPG:] VALIDSIG ");
	if (!v)
		goto out;
	v += strlen("[GNUPG:] VALIDSIG ");
	char *raw = xstrndup(v, strcspn(v, " \n"));
	norm_fpr(raw, &got); /* not in place: norm_fpr resets its output first */
	free(raw);
	for (size_t i = 0; i < n && which == SIZE_MAX; i++) {
		if (strcmp(keys[i].kind, "gpg"))
			continue;
		norm_fpr(keys[i].fingerprint, &want);
		/* the full fingerprint, or its long key id (last 16 digits) */
		if (want.len >= 16 && got.len >= want.len && !strcmp(got.buf + got.len - want.len, want.buf))
			which = i;
	}
out:
	strbuf_release(&home);
	strbuf_release(&status);
	strbuf_release(&want);
	strbuf_release(&got);
	return which;
}

static tix_status git_commit_verify(tix_store *s, const tix_oid *oid, const tix_sig_key *keys, size_t n,
				    tix_sig_status *out, size_t *which, tix_error *err)
{
	struct object_id g;
	to_git(oid, &g);
	struct commit *c = lookup_commit(the_repository, &g);
	if (oid_object_info(the_repository, &g, NULL) != OBJ_COMMIT || !c || repo_parse_commit(the_repository, c))
		return tix_error_set(err, TIX_ERR_NOT_FOUND, "no commit %s", oid_to_hex(&g));
	struct strbuf payload = STRBUF_INIT, sig = STRBUF_INIT, dir = STRBUF_INIT, path = STRBUF_INIT;
	tix_status st = TIX_OK;
	if (parse_signed_commit(c, &payload, &sig, the_hash_algo) <= 0)
		goto out; /* unsigned: NONE */
	*out = TIX_SIG_BAD;
	bool ssh = starts_with(sig.buf, "-----BEGIN SSH SIGNATURE-----");
	bool pgp = starts_with(sig.buf, "-----BEGIN PGP SIGNATURE-----");
	const char *tmp = getenv("TMPDIR");
	strbuf_addf(&dir, "%s/tix-verify-XXXXXX", tmp && *tmp ? tmp : "/tmp");
	if (!mkdtemp(dir.buf)) {
		st = tix_error_set(err, TIX_ERR_IO, "cannot make a temporary directory: %s", strerror(errno));
		strbuf_reset(&dir);
		goto out;
	}
	if (!write_tmp(dir.buf, "sig", sig.buf, sig.len, &path)) {
		st = tix_error_set(err, TIX_ERR_IO, "cannot write %s", path.buf);
		goto out;
	}
	if (ssh) {
		for (size_t i = 0; i < n && *out != TIX_SIG_GOOD; i++)
			if (!strcmp(keys[i].kind, "ssh") && verify_ssh(dir.buf, &payload, path.buf, &keys[i])) {
				*out = TIX_SIG_GOOD;
				*which = i;
			}
	} else if (pgp) {
		size_t k = verify_gpg(dir.buf, &payload, path.buf, keys, n);
		if (k != SIZE_MAX) {
			*out = TIX_SIG_GOOD;
			*which = k;
		}
	}
out:
	if (dir.len)
		remove_dir_recursively(&dir, 0);
	strbuf_release(&payload);
	strbuf_release(&sig);
	strbuf_release(&dir);
	strbuf_release(&path);
	return st;
}

static void git_destroy(tix_store *s)
{
	free(s);
}

static const tix_store_vt git_vt = {
	.commit_write = git_commit_write,
	.commit_read = git_commit_read,
	.ref_read = git_ref_read,
	.ref_list = git_ref_list,
	.ref_cas = git_ref_cas,
	.local_get = git_local_get,
	.local_set = git_local_set,
	.remote_fetch = git_remote_fetch,
	.remote_push = git_remote_push,
	.refresh = git_refresh,
	.commit_verify = git_commit_verify,
	.destroy = git_destroy,
};

tix_status tix_store_libgit_open(tix_store **out, tix_error *err)
{
	int nongit = 0;
	TIX_REQUIRE(out && !opened);
	opened = 1;
	*out = NULL;
	sanitize_stdfds();
	initialize_repository(the_repository);
	setup_git_directory_gently(&nongit);
	if (nongit)
		return tix_error_set(err, TIX_ERR_NOT_FOUND, "not a git repository");
	git_config(git_default_config, NULL);

	int algo = hash_algo_by_ptr(the_hash_algo);
	if (algo != GIT_HASH_SHA1 && algo != GIT_HASH_SHA256)
		return tix_error_set(err, TIX_ERR_UNSUPPORTED, "unsupported hash algorithm");
	git_store *g = tix_xcalloc(1, sizeof(*g));
	g->base.vt = &git_vt;
	g->base.algo = algo == GIT_HASH_SHA1 ? TIX_HASH_SHA1 : TIX_HASH_SHA256;
	int sign = 0;
	g->sign = !repo_config_get_bool(the_repository, "tix.sign", &sign) && sign;
	*out = &g->base;
	return TIX_OK;
}

static void check_store(tix_store *s)
{
	TIX_REQUIRE(s && s->vt == &git_vt);
}

tix_status tix_store_libgit_config_get(tix_store *s, const char *key, tix_buf *out, tix_error *err)
{
	char *v = NULL;
	check_store(s);
	if (repo_config_get_string(the_repository, key, &v) || !v)
		return tix_error_set(err, TIX_ERR_NOT_FOUND, "config %s unset", key);
	tix_buf_reset(out);
	tix_buf_adds(out, v);
	free(v);
	return TIX_OK;
}

tix_status tix_store_libgit_config_set(tix_store *s, const char *key, const char *value, tix_error *err)
{
	check_store(s);
	if (repo_config_set_gently(the_repository, key, value))
		return tix_error_set(err, TIX_ERR_IO, "cannot set config %s", key);
	return TIX_OK;
}

tix_status tix_store_libgit_local_path(tix_store *s, const char *name, tix_buf *out, tix_error *err)
{
	struct strbuf dir = STRBUF_INIT, path = STRBUF_INIT;
	char base[40];
	size_t n = strlen(name);
	check_store(s);
	TIX_REQUIRE(n < sizeof(base));
	memcpy(base, name, n + 1);
	if (n > 3 && !strcmp(base + n - 3, ".db"))
		base[n - 3] = '\0';
	TIX_REQUIRE(tix_local_name_valid(base));
	local_path(&dir, NULL);
	strbuf_addf(&path, "%s/%s", absolute_path(dir.buf), name);
	tix_status st = TIX_OK;
	if (mkdir(dir.buf, 0777) && errno != EEXIST)
		st = tix_error_set(err, TIX_ERR_IO, "%s: %s", dir.buf, strerror(errno));
	else {
		tix_buf_reset(out);
		tix_buf_adds(out, path.buf);
	}
	strbuf_release(&dir);
	strbuf_release(&path);
	return st;
}

const char *tix_store_libgit_git_version(void)
{
	return git_version_string;
}

/* The public half of user.signingkey under gpg.format ssh: "key::<pubkey>" or a key file
 * (a .pub file, or a private key whose .pub sits next to it). */
static bool ssh_pubkey(const char *key, struct strbuf *pub)
{
	strbuf_reset(pub);
	if (skip_prefix(key, "key::", &key)) {
		strbuf_addstr(pub, key);
	} else {
		struct strbuf path = STRBUF_INIT;
		strbuf_addstr(&path, key);
		if (strbuf_read_file(pub, path.buf, 0) < 0 || !starts_with(pub->buf, "ssh-")) {
			strbuf_reset(pub);
			strbuf_addstr(&path, ".pub");
			if (strbuf_read_file(pub, path.buf, 0) < 0)
				strbuf_reset(pub);
		}
		strbuf_release(&path);
	}
	strbuf_trim(pub);
	/* keep "type base64": a trailing comment is not part of the key */
	const char *sp = strchr(pub->buf, ' ');
	if (sp && (sp = strchr(sp + 1, ' ')))
		strbuf_setlen(pub, (size_t)(sp - pub->buf));
	return starts_with(pub->buf, "ssh-") || starts_with(pub->buf, "ecdsa-") || starts_with(pub->buf, "sk-");
}

tix_status tix_store_libgit_signing_key(tix_store *s, tix_buf *kind, tix_buf *fingerprint, tix_buf *pubkey,
				       tix_error *err)
{
	check_store(s);
	char *format = NULL, *key = get_signing_key();
	struct strbuf pub = STRBUF_INIT, out = STRBUF_INIT, dir = STRBUF_INIT, path = STRBUF_INIT;
	tix_status st = TIX_OK;
	repo_config_get_string(the_repository, "gpg.format", &format);
	if (!key || !*key) {
		st = tix_error_set(err, TIX_ERR_NOT_FOUND, "no signing key: set git config user.signingkey");
		goto out;
	}
	tix_buf_reset(kind);
	tix_buf_reset(fingerprint);
	tix_buf_reset(pubkey);
	if (format && !strcmp(format, "ssh")) {
		if (!ssh_pubkey(key, &pub)) {
			st = tix_error_set(err, TIX_ERR_NOT_FOUND, "cannot read the public key of user.signingkey");
			goto out;
		}
		const char *tmp = getenv("TMPDIR");
		strbuf_addf(&dir, "%s/tix-key-XXXXXX", tmp && *tmp ? tmp : "/tmp");
		if (!mkdtemp(dir.buf)) {
			strbuf_reset(&dir);
			st = tix_error_set(err, TIX_ERR_IO, "cannot make a temporary directory");
			goto out;
		}
		strbuf_addch(&pub, '\n');
		if (!write_tmp(dir.buf, "key.pub", pub.buf, pub.len, &path)) {
			st = tix_error_set(err, TIX_ERR_IO, "cannot write %s", path.buf);
			goto out;
		}
		const char *argv[] = {"ssh-keygen", "-l", "-E", "sha256", "-f", path.buf, NULL};
		if (run_tool(argv, NULL, 0, &out)) {
			st = tix_error_set(err, TIX_ERR_IO, "ssh-keygen could not read the key");
			goto out;
		}
		strbuf_trim(&pub);
		const char *f = strchr(out.buf, ' '); /* "256 SHA256:... comment (ED25519)" */
		if (!f || !starts_with(f + 1, "SHA256:")) {
			st = tix_error_set(err, TIX_ERR_IO, "unexpected ssh-keygen output");
			goto out;
		}
		tix_buf_adds(kind, "ssh");
		tix_buf_add(fingerprint, f + 1, strcspn(f + 1, " \n"));
		tix_buf_adds(pubkey, pub.buf);
	} else if (!format || !strcmp(format, "openpgp")) {
		const char *argv[] = {"gpg", "--batch", "--with-colons", "--fingerprint", key, NULL};
		if (run_tool(argv, NULL, 0, &out)) {
			st = tix_error_set(err, TIX_ERR_NOT_FOUND, "gpg doesn't know the key %s", key);
			goto out;
		}
		const char *f = strstr(out.buf, "\nfpr:");
		if (!f) {
			st = tix_error_set(err, TIX_ERR_IO, "no fingerprint in gpg's output");
			goto out;
		}
		f += 1;
		for (int i = 0; i < 9 && f; i++) /* fpr:::::::::<fingerprint>: */
			f = strchr(f, ':') ? strchr(f, ':') + 1 : NULL;
		size_t n = f ? strcspn(f, ":\n") : 0;
		if (!n) {
			st = tix_error_set(err, TIX_ERR_IO, "no fingerprint in gpg's output");
			goto out;
		}
		tix_buf_add(fingerprint, f, n);
		const char *exp[] = {"gpg", "--batch", "--armor", "--export", fingerprint->data, NULL};
		strbuf_reset(&pub);
		if (run_tool(exp, NULL, 0, &pub) || !starts_with(pub.buf, "-----BEGIN PGP PUBLIC KEY BLOCK-----")) {
			st = tix_error_set(err, TIX_ERR_IO, "gpg could not export the key");
			goto out;
		}
		tix_buf_adds(kind, "gpg");
		tix_buf_adds(pubkey, pub.buf);
	} else {
		st = tix_error_set(err, TIX_ERR_UNSUPPORTED, "gpg.format %s is not supported (ssh or openpgp)", format);
	}
out:
	if (dir.len)
		remove_dir_recursively(&dir, 0);
	free(format);
	free(key);
	strbuf_release(&pub);
	strbuf_release(&out);
	strbuf_release(&dir);
	strbuf_release(&path);
	return st;
}

tix_status tix_store_libgit_credential(tix_store *s, const char *protocol, const char *host, const char *path,
				      tix_buf *password, tix_error *err)
{
	check_store(s);
	struct child_process cp = CHILD_PROCESS_INIT;
	struct strbuf in = STRBUF_INIT, out = STRBUF_INIT, e = STRBUF_INIT;
	tix_status st = TIX_OK;
	for (const char *v[] = {protocol, host, path}, **p = v; p < v + 3; p++)
		if (*p && strpbrk(*p, "\n\r=") ) /* the protocol is line-based: no injection */
			return tix_error_set(err, TIX_ERR_INVALID, "credential request has a line break or '='");
	cp.git_cmd = 1;
	strvec_pushl(&cp.args, "credential", "fill", NULL);
	strvec_push(&cp.env, "GIT_TERMINAL_PROMPT=0"); /* never prompt from inside tix */
	strbuf_addf(&in, "protocol=%s\nhost=%s\n", protocol, host);
	if (path && *path)
		strbuf_addf(&in, "path=%s\n", path);
	strbuf_addch(&in, '\n');
	if (pipe_command(&cp, in.buf, in.len, &out, 0, &e, 0)) {
		st = tix_error_set(err, TIX_ERR_NOT_FOUND, "no stored credential for %s://%s", protocol, host);
		goto done;
	}
	const char *pw = NULL;
	for (const char *l = out.buf; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL)
		if (starts_with(l, "password="))
			pw = l + strlen("password=");
	if (!pw) {
		st = tix_error_set(err, TIX_ERR_NOT_FOUND, "no stored credential for %s://%s", protocol, host);
		goto done;
	}
	tix_buf_reset(password);
	tix_buf_add(password, pw, strcspn(pw, "\n"));
done:
	memset(out.buf, 0, out.len); /* don't leave secrets in freed memory */
	strbuf_release(&in);
	strbuf_release(&out);
	strbuf_release(&e);
	return st;
}

