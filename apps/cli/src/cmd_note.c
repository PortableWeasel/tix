/* tix note: typed comments (architecture §15.4) */
#include "cli.h"
#include "tix/base_json.h"

#include <cjson/cJSON.h>
#include <stdlib.h>
#include <string.h>

void cli_print_note(const tix_ticket *t, const tix_comment *cm)
{
	cJSON *o = cm->data ? cJSON_Parse(cm->data) : NULL;
	const cJSON *f = cJSON_GetObjectItemCaseSensitive(o, "fields"), *it;
	cJSON_ArrayForEach(it, f)
		printf("    %s: %s\n", it->string, cJSON_IsString(it) ? it->valuestring : "");
	cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(o, "items"))
	{
		const cJSON *id = cJSON_GetObjectItemCaseSensitive(it, "id"), *tx = cJSON_GetObjectItemCaseSensitive(it, "text");
		if (!cJSON_IsString(id) || !cJSON_IsString(tx))
			continue;
		char *st = tix_note_item_state(t, cm, id->valuestring);
		printf("    [%s] %s: %s\n", st ? st : "?", id->valuestring, tx->valuestring);
		free(st);
	}
	cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(o, "questions"))
	{
		const cJSON *id = cJSON_GetObjectItemCaseSensitive(it, "id"), *tx = cJSON_GetObjectItemCaseSensitive(it, "text");
		if (cJSON_IsString(id) && cJSON_IsString(tx))
			printf("    ? %s: %s\n", id->valuestring, tx->valuestring);
	}
	cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(o, "updates"))
	{
		const cJSON *ref = cJSON_GetObjectItemCaseSensitive(it, "ref"), *st = cJSON_GetObjectItemCaseSensitive(it, "state");
		if (cJSON_IsString(ref) && cJSON_IsString(st))
			printf("    -> %s: %s\n", ref->valuestring, st->valuestring);
	}
	cJSON_Delete(o);
}

/* "A=B" or "A=B=C": splits at the first (and second, when c is wanted) '='. */
static bool split_eq(char *s, char **a, char **b, char **c)
{
	char *e1 = strchr(s, '=');
	if (!e1 || e1 == s)
		return false;
	*e1 = '\0';
	*a = s;
	*b = e1 + 1;
	if (c) {
		char *e2 = strrchr(*b, '=');
		*c = NULL;
		if (e2) {
			*e2 = '\0';
			*c = e2 + 1;
		}
	}
	return true;
}

typedef struct kv3 {
	char *a, *b, *c;
} kv3;

static void put_list(tix_jw *w, const char *key, const kv3 *v, size_t n, const char *k1, const char *k2, const char *k3)
{
	if (!n)
		return;
	tix_jw_key(w, key);
	tix_jw_arr_begin(w);
	for (size_t i = 0; i < n; i++) {
		tix_jw_obj_begin(w);
		tix_jw_key(w, k1);
		tix_jw_str(w, v[i].a);
		tix_jw_key(w, k2);
		tix_jw_str(w, v[i].b);
		if (k3) {
			tix_jw_key(w, k3);
			tix_jw_str(w, v[i].c ? v[i].c : "open");
		}
		tix_jw_obj_end(w);
	}
	tix_jw_arr_end(w);
}

int cmd_note(cli *c, int argc, char **argv)
{
	const char *ticket = NULL, *kind = NULL, *msg = NULL, *reply = NULL, *data = NULL, *data_file = NULL;
	TIX_VEC(kv3) fields = {0}, items = {0}, questions = {0}, updates = {0};
	int rc = EXIT_OK;
	for (int i = 1; i < argc && rc == EXIT_OK; i++) {
		const char *a = argv[i];
		bool has = i + 1 < argc;
		kv3 x = {0};
		if (!strcmp(a, "--kind") && has)
			kind = argv[++i];
		else if (!strcmp(a, "-m") && has)
			msg = argv[++i];
		else if (!strcmp(a, "--reply-to") && has)
			reply = argv[++i];
		else if (!strcmp(a, "--data") && has)
			data = argv[++i];
		else if (!strcmp(a, "--data-file") && has)
			data_file = argv[++i];
		else if (!strcmp(a, "--field") && has) {
			if (!split_eq(argv[++i], &x.a, &x.b, NULL))
				rc = cli_usage("--field takes KEY=VALUE");
			else
				TIX_VEC_PUSH(&fields, x);
		} else if (!strcmp(a, "--item") && has) {
			if (!split_eq(argv[++i], &x.a, &x.b, &x.c))
				rc = cli_usage("--item takes ID=TEXT or ID=TEXT=STATE");
			else
				TIX_VEC_PUSH(&items, x);
		} else if (!strcmp(a, "--question") && has) {
			if (!split_eq(argv[++i], &x.a, &x.b, NULL))
				rc = cli_usage("--question takes ID=TEXT");
			else
				TIX_VEC_PUSH(&questions, x);
		} else if (!strcmp(a, "--update") && has) {
			if (!split_eq(argv[++i], &x.a, &x.b, NULL))
				rc = cli_usage("--update takes COMMENT-ID#ITEM=STATE");
			else
				TIX_VEC_PUSH(&updates, x);
		} else if (!ticket && a[0] != '-')
			ticket = a;
		else
			rc = cli_usage("note: unexpected '%s'", a);
	}
	bool shortcuts = fields.n || items.n || questions.n || updates.n;
	if (rc == EXIT_OK && (!ticket || !kind))
		rc = cli_usage("usage: tix note ID --kind K [-m TEXT] [--data JSON | --data-file F | --field/--item/...]");
	if (rc == EXIT_OK && (!!data + !!data_file + shortcuts) > 1)
		rc = cli_usage("note: use one of --data, --data-file, or the --field/--item/--question/--update shortcuts");
	tix_entity *e = NULL;
	tix_buf json = TIX_BUF_INIT;
	tix_opid rt;
	memset(&rt, 0, sizeof(rt));
	if (rc == EXIT_OK) {
		tix_oid id;
		rc = cli_resolve(c, TIX_ENTITY_TICKET, ticket, &id);
		if (!rc) {
			tix_status st = tix_entity_load(c->repo, TIX_ENTITY_TICKET, &id, &e, &c->err);
			rc = st ? cli_fail(c, st) : EXIT_OK;
		}
	}
	if (rc == EXIT_OK && reply)
		rc = cli_resolve_comment(e, reply, &rt);
	/* update refs name comments by their short form: expand to full op ids */
	for (size_t i = 0; rc == EXIT_OK && i < updates.n; i++) {
		char *hash = strchr(updates.v[i].a, '#');
		tix_opid full;
		if (!hash)
			rc = cli_usage("--update takes COMMENT-ID#ITEM=STATE");
		else {
			*hash = '\0';
			if (!(rc = cli_resolve_comment(e, updates.v[i].a, &full))) {
				char opid[TIX_OID_MAX_HEX + 12];
				size_t n = strlen(tix_opid_str(&full, opid)) + strlen(hash + 1) + 2;
				char *ref = malloc(n);
				snprintf(ref, n, "%s#%s", opid, hash + 1);
				updates.v[i].a = ref; /* freed below */
				updates.v[i].c = (char *)1; /* marks an allocated ref */
			}
		}
	}
	if (rc == EXIT_OK) {
		if (data) {
			tix_buf_adds(&json, data);
		} else if (data_file) {
			FILE *f = strcmp(data_file, "-") ? fopen(data_file, "r") : stdin;
			char chunk[4096];
			size_t n;
			if (!f) {
				fprintf(stderr, "tix: can't read %s\n", data_file);
				rc = EXIT_ERR;
			} else {
				while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0 && json.len <= TIX_TEXT_MAX)
					tix_buf_add(&json, chunk, n);
				if (f != stdin)
					fclose(f);
				while (json.len && (json.data[json.len - 1] == '\n' || json.data[json.len - 1] == ' '))
					json.data[--json.len] = '\0';
			}
		} else {
			tix_jw w;
			tix_jw_init(&w, &json);
			tix_jw_obj_begin(&w);
			if (fields.n) {
				tix_jw_key(&w, "fields");
				tix_jw_obj_begin(&w);
				for (size_t i = 0; i < fields.n; i++) {
					tix_jw_key(&w, fields.v[i].a);
					tix_jw_str(&w, fields.v[i].b);
				}
				tix_jw_obj_end(&w);
			}
			put_list(&w, "items", items.v, items.n, "id", "text", "state");
			put_list(&w, "questions", questions.v, questions.n, "id", "text", NULL);
			if (updates.n) {
				tix_jw_key(&w, "updates");
				tix_jw_arr_begin(&w);
				for (size_t i = 0; i < updates.n; i++) {
					tix_jw_obj_begin(&w);
					tix_jw_key(&w, "ref");
					tix_jw_str(&w, updates.v[i].a);
					tix_jw_key(&w, "state");
					tix_jw_str(&w, updates.v[i].b);
					tix_jw_obj_end(&w);
				}
				tix_jw_arr_end(&w);
			}
			tix_jw_obj_end(&w);
		}
	}
	if (rc == EXIT_OK) {
		tix_status st = tix_note_data_check(tix_buf_cstr(&json), &c->err);
		if (st)
			rc = cli_usage("note: %s", c->err.msg);
	}
	if (rc == EXIT_OK) {
		tix_op op = tix_op_typed_comment(kind, msg ? msg : "", tix_buf_cstr(&json), reply ? &rt : NULL);
		tix_status st = tix_entity_apply(c->repo, TIX_ENTITY_TICKET, &e->id, &op, 1, &c->err);
		tix_op_free(&op);
		rc = st ? cli_fail(c, st) : EXIT_OK;
	}
	for (size_t i = 0; i < updates.n; i++)
		if (updates.v[i].c == (char *)1)
			free(updates.v[i].a);
	free(fields.v);
	free(items.v);
	free(questions.v);
	free(updates.v);
	tix_buf_free(&json);
	tix_entity_free(e);
	return rc;
}
