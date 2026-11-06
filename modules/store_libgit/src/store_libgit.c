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
#include "tree-walk.h"

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
	if (commit_tree_extended(msg, strlen(msg), &tree_oid, list, &commit, author.buf, committer.buf,
				 NULL, NULL)) {
		st = tix_error_set(err, TIX_ERR_IO, "cannot write commit");
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
	strvec_pushf(&cp.args, "+refs/tix/*:refs/remotes/%s/tix/*", remote);
	if (pipe_command(&cp, NULL, 0, &out, 0, &e, 0))
		st = child_error(err, TIX_ERR_IO, "fetch", &e);
	forget_caches();
	strbuf_release(&out);
	strbuf_release(&e);
	return st;
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
