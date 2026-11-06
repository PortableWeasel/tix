/*
 * M0 spike: prove tix can link git.git's libgit.a (v2.47.3) and do everything
 * the store adapter will need, in-process:
 *
 *   m0 write <ref> [<expected-old-oid>|-]   blob+tree+commit, CAS-update <ref>
 *   m0 show  <ref>                          print ops.ndjson at <ref>, and parent count
 *   m0 fetch <remote>                       git fetch +refs/tix/(all):refs/remotes/<remote>/tix/(all)
 *   m0 push  <remote>                       git push refs/tix/(all):refs/tix/(all)
 *
 * Network operations run a child `git` process (architecture §6).
 * This file is throwaway: it becomes modules/store_libgit in M2.
 */
#define USE_THE_REPOSITORY_VARIABLE
#include "git-compat-util.h"
#include "commit.h"
#include "config.h"
#include "hex.h"
#include "object-store-ll.h"
#include "refs.h"
#include "repository.h"
#include "run-command.h"
#include "setup.h"
#include "strbuf.h"
#include "strvec.h"
#include "tree-walk.h"

/* git-compat-util.h wraps exit() through this; normally provided by common-main.c. */
int common_exit(const char *file, int line, int code)
{
	(void)file;
	(void)line;
	return code & 0xff;
}

static int resolve(const char *ref, struct object_id *out)
{
	return refs_read_ref_full(get_main_ref_store(the_repository), ref,
				  RESOLVE_REF_READING, out, NULL);
}

static int cmd_write(const char *ref, const char *expect)
{
	struct strbuf ops = STRBUF_INIT, tree = STRBUF_INIT, err = STRBUF_INIT;
	struct object_id blob, tree_oid, commit, cur, old;
	struct commit_list *parents = NULL;
	struct ref_transaction *tx;
	int have_cur = !resolve(ref, &cur);

	/* expected old value: "-" = must not exist; omitted = current value */
	if (expect && !strcmp(expect, "-"))
		oidcpy(&old, null_oid());
	else if (expect) {
		if (get_oid_hex(expect, &old))
			return error("bad oid '%s'", expect);
	} else
		oidcpy(&old, have_cur ? &cur : null_oid());

	strbuf_addf(&ops, "{\"tix\":1,\"type\":\"ticket\",\"lamport\":%d}\n{\"op\":\"comment\",\"body\":\"m0\"}\n",
		    have_cur ? 2 : 1);
	if (write_object_file(ops.buf, ops.len, OBJ_BLOB, &blob))
		return error("write blob");

	strbuf_addstr(&tree, "100644 ops.ndjson");
	strbuf_addch(&tree, '\0');
	strbuf_add(&tree, blob.hash, the_hash_algo->rawsz);
	if (write_object_file(tree.buf, tree.len, OBJ_TREE, &tree_oid))
		return error("write tree");

	if (have_cur)
		commit_list_insert(lookup_commit_reference(the_repository, &cur), &parents);
	if (commit_tree("tix m0: spike op-pack\n", 22, &tree_oid, parents, &commit, NULL, NULL))
		return error("commit_tree");

	tx = ref_store_transaction_begin(get_main_ref_store(the_repository), &err);
	if (!tx ||
	    ref_transaction_update(tx, ref, &commit, &old, NULL, NULL, 0, "tix m0", &err) ||
	    ref_transaction_commit(tx, &err)) {
		fprintf(stderr, "CAS failed: %s\n", err.buf);
		ref_transaction_free(tx);
		return 2;
	}
	ref_transaction_free(tx);
	printf("%s\n", oid_to_hex(&commit));
	strbuf_release(&ops);
	strbuf_release(&tree);
	strbuf_release(&err);
	free_commit_list(parents);
	return 0;
}

static int cmd_show(const char *ref)
{
	struct object_id oid, blob;
	struct commit *c;
	unsigned short mode;
	enum object_type type;
	unsigned long size;
	char *buf;

	if (resolve(ref, &oid))
		return error("no such ref %s", ref);
	c = lookup_commit_reference(the_repository, &oid);
	if (!c || repo_parse_commit(the_repository, c))
		return error("not a commit");
	if (get_tree_entry(the_repository, get_commit_tree_oid(c), "ops.ndjson", &blob, &mode))
		return error("no ops.ndjson");
	buf = repo_read_object_file(the_repository, &blob, &type, &size);
	if (!buf || type != OBJ_BLOB)
		return error("bad blob");
	printf("parents=%d\n", commit_list_count(c->parents));
	fwrite(buf, 1, size, stdout);
	free(buf);
	return 0;
}

static int cmd_net(const char *verb, const char *remote)
{
	struct child_process cp = CHILD_PROCESS_INIT;
	struct strbuf spec = STRBUF_INIT;

	cp.git_cmd = 1; /* runs the system `git`, same version as libgit.a */
	if (!strcmp(verb, "fetch"))
		strbuf_addf(&spec, "+refs/tix/*:refs/remotes/%s/tix/*", remote);
	else
		strbuf_addstr(&spec, "refs/tix/*:refs/tix/*");
	strvec_pushl(&cp.args, verb, "--quiet", remote, spec.buf, NULL);
	strbuf_release(&spec);
	return run_command(&cp) ? 3 : 0;
}

int main(int argc, const char **argv)
{
	sanitize_stdfds();
	initialize_repository(the_repository);
	setup_git_directory();
	git_config(git_default_config, NULL);

	if (argc >= 3 && !strcmp(argv[1], "write"))
		return cmd_write(argv[2], argc > 3 ? argv[3] : NULL);
	if (argc == 3 && !strcmp(argv[1], "show"))
		return cmd_show(argv[2]);
	if (argc == 3 && (!strcmp(argv[1], "fetch") || !strcmp(argv[1], "push")))
		return cmd_net(argv[1], argv[2]);
	fprintf(stderr, "usage: m0 write <ref> [old|-] | show <ref> | fetch|push <remote>\n");
	return 1;
}
