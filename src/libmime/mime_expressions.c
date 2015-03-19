/*
 * Copyright (c) 2009-2012, Vsevolod Stakhov
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY AUTHOR ''AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "util.h"
#include "cfg_file.h"
#include "main.h"
#include "message.h"
#include "fuzzy.h"
#include "mime_expressions.h"
#include "html.h"
#include "lua/lua_common.h"
#include "diff.h"

gboolean rspamd_compare_encoding (struct rspamd_task *task,
	GList * args,
	void *unused);
gboolean rspamd_header_exists (struct rspamd_task *task,
	GList * args,
	void *unused);
gboolean rspamd_parts_distance (struct rspamd_task *task,
	GList * args,
	void *unused);
gboolean rspamd_recipients_distance (struct rspamd_task *task,
	GList * args,
	void *unused);
gboolean rspamd_has_only_html_part (struct rspamd_task *task,
	GList * args,
	void *unused);
gboolean rspamd_is_recipients_sorted (struct rspamd_task *task,
	GList * args,
	void *unused);
gboolean rspamd_compare_transfer_encoding (struct rspamd_task *task,
	GList * args,
	void *unused);
gboolean rspamd_is_html_balanced (struct rspamd_task *task,
	GList * args,
	void *unused);
gboolean rspamd_has_html_tag (struct rspamd_task *task,
	GList * args,
	void *unused);
gboolean rspamd_has_fake_html (struct rspamd_task *task,
	GList * args,
	void *unused);

static rspamd_expression_atom_t * rspamd_mime_expr_parse (const gchar *line, gsize len,
		rspamd_mempool_t *pool, gpointer ud, GError **err);
static gint rspamd_mime_expr_process (gpointer input, rspamd_expression_atom_t *atom);
static gint rspamd_mime_expr_priority (rspamd_expression_atom_t *atom);
static void rspamd_mime_expr_destroy (rspamd_expression_atom_t *atom);

/**
 * Regexp type: /H - header, /M - mime, /U - url /X - raw header
 */
enum rspamd_regexp_type {
	REGEXP_NONE = 0,
	REGEXP_HEADER,
	REGEXP_MIME,
	REGEXP_MESSAGE,
	REGEXP_URL,
	REGEXP_RAW_HEADER
};

/**
 * Regexp structure
 */
struct rspamd_regexp_atom {
	enum rspamd_regexp_type type;                   /**< regexp type										*/
	gchar *regexp_text;                             /**< regexp text representation							*/
	rspamd_regexp_t *regexp;                        /**< regexp structure									*/
	gchar *header;                                  /**< header name for header regexps						*/
	gboolean is_test;                               /**< true if this expression must be tested				*/
	gboolean is_strong;                             /**< true if headers search must be case sensitive		*/
};

/**
 * Rspamd expression function
 */
struct rspamd_function_atom {
	gchar *name;	/**< name of function								*/
	GArray *args;	/**< its args										*/
};

struct rspamd_mime_atom {
	gchar *str;
	union {
		struct rspamd_regexp_atom *re;
		struct rspamd_function_atom *func;
	} d;
	gboolean is_function;
};

/*
 * List of internal functions of rspamd
 * Sorted by name to use bsearch
 */
static struct _fl {
	const gchar *name;
	rspamd_internal_func_t func;
	void *user_data;
} rspamd_functions_list[] = {
	{"compare_encoding", rspamd_compare_encoding, NULL},
	{"compare_parts_distance", rspamd_parts_distance, NULL},
	{"compare_recipients_distance", rspamd_recipients_distance, NULL},
	{"compare_transfer_encoding", rspamd_compare_transfer_encoding, NULL},
	{"has_fake_html", rspamd_has_fake_html, NULL},
	{"has_html_tag", rspamd_has_html_tag, NULL},
	{"has_only_html_part", rspamd_has_only_html_part, NULL},
	{"header_exists", rspamd_header_exists, NULL},
	{"is_html_balanced", rspamd_is_html_balanced, NULL},
	{"is_recipients_sorted", rspamd_is_recipients_sorted, NULL}
};

const struct rspamd_atom_subr mime_expr_subr = {
	.parse = rspamd_mime_expr_parse,
	.process = rspamd_mime_expr_process,
	.priority = rspamd_mime_expr_priority,
	.destroy = rspamd_mime_expr_destroy
};

static struct _fl *list_ptr = &rspamd_functions_list[0];
static guint32 functions_number = sizeof (rspamd_functions_list) /
	sizeof (struct _fl);
static gboolean list_allocated = FALSE;
static guint max_re_data = 0;

/* Bsearch routine */
static gint
fl_cmp (const void *s1, const void *s2)
{
	struct _fl *fl1 = (struct _fl *)s1;
	struct _fl *fl2 = (struct _fl *)s2;
	return strcmp (fl1->name, fl2->name);
}

static GQuark
rspamd_mime_expr_quark (void)
{
	return g_quark_from_static_string ("mime-expressions");
}

/*
 * Rspamd regexp utility functions
 */
static struct rspamd_regexp_atom *
rspamd_mime_expr_parse_regexp_atom (rspamd_mempool_t * pool, const gchar *line)
{
	const gchar *begin, *end, *p, *src, *start;
	gchar *dbegin, *dend;
	struct rspamd_regexp_atom *result;
	rspamd_regexp_t *re;
	GError *err = NULL;
	GString *re_flags;

	if (line == NULL) {
		msg_err ("cannot parse NULL line");
		return NULL;
	}

	if ((re = rspamd_regexp_cache_query (NULL, line, NULL)) != NULL) {
		return ((struct rspamd_regexp_atom *)rspamd_regexp_get_ud (re));
	}

	src = line;
	result = rspamd_mempool_alloc0 (pool, sizeof (struct rspamd_regexp_atom));
	/* Skip whitespaces */
	while (g_ascii_isspace (*line)) {
		line++;
	}
	if (*line == '\0') {
		msg_warn ("got empty regexp");
		return NULL;
	}
	start = line;
	/* First try to find header name */
	begin = strchr (line, '/');
	if (begin != NULL) {
		p = begin;
		end = NULL;
		while (p != line) {
			if (*p == '=') {
				end = p;
				break;
			}
			p--;
		}
		if (end) {
			result->header = rspamd_mempool_alloc (pool, end - line + 1);
			rspamd_strlcpy (result->header, line, end - line + 1);
			result->type = REGEXP_HEADER;
			line = end;
		}
	}
	else {
		result->header = rspamd_mempool_strdup (pool, line);
		result->type = REGEXP_HEADER;
		line = start;
	}
	/* Find begin of regexp */
	while (*line && *line != '/') {
		line++;
	}
	if (*line != '\0') {
		begin = line + 1;
	}
	else if (result->header == NULL) {
		/* Assume that line without // is just a header name */
		result->header = rspamd_mempool_strdup (pool, line);
		result->type = REGEXP_HEADER;
		return result;
	}
	else {
		/* We got header name earlier but have not found // expression, so it is invalid regexp */
		msg_warn (
			"got no header name (eg. header=) but without corresponding regexp, %s",
			src);
		return NULL;
	}
	/* Find end */
	end = begin;
	while (*end && (*end != '/' || *(end - 1) == '\\')) {
		end++;
	}
	if (end == begin || *end != '/') {
		msg_warn ("no trailing / in regexp %s", src);
		return NULL;
	}
	/* Parse flags */
	p = end + 1;
	re_flags = g_string_sized_new (32);
	while (p != NULL) {
		switch (*p) {
		case 'i':
		case 'm':
		case 's':
		case 'x':
		case 'u':
		case 'O':
		case 'r':
			g_string_append_c (re_flags, *p);
			p++;
			break;
		case 'o':
			p++;
			break;
		/* Type flags */
		case 'H':
			if (result->type == REGEXP_NONE) {
				result->type = REGEXP_HEADER;
			}
			p++;
			break;
		case 'M':
			if (result->type == REGEXP_NONE) {
				result->type = REGEXP_MESSAGE;
			}
			p++;
			break;
		case 'P':
			if (result->type == REGEXP_NONE) {
				result->type = REGEXP_MIME;
			}
			p++;
			break;
		case 'U':
			if (result->type == REGEXP_NONE) {
				result->type = REGEXP_URL;
			}
			p++;
			break;
		case 'X':
			if (result->type == REGEXP_NONE || result->type == REGEXP_HEADER) {
				result->type = REGEXP_RAW_HEADER;
			}
			p++;
			break;
		case 'T':
			result->is_test = TRUE;
			p++;
			break;
		case 'S':
			result->is_strong = TRUE;
			p++;
			break;
		/* Stop flags parsing */
		default:
			p = NULL;
			break;
		}
	}

	result->regexp_text = rspamd_mempool_strdup (pool, start);
	dbegin = result->regexp_text + (begin - start);
	dend = result->regexp_text + (end - start);
	*dend = '\0';

	result->regexp = rspamd_regexp_new (dbegin, re_flags->str,
			&err);

	g_string_free (re_flags, TRUE);

	if (result->regexp == NULL || err != NULL) {
		msg_warn ("could not read regexp: %s while reading regexp %s",
				err ? err->message : "unknown error",
						src);
		return NULL;
	}

	rspamd_mempool_add_destructor (pool,
		(rspamd_mempool_destruct_t) rspamd_regexp_unref,
		(void *)result->regexp);

	rspamd_regexp_set_ud (result->regexp, result);

	rspamd_regexp_cache_insert (NULL, line, NULL, result->regexp);

	*dend = '/';

	return result;
}

struct rspamd_function_atom *
rspamd_mime_expr_parse_function_atom (const gchar *input)
{
	const gchar *obrace, *ebrace, *p, *c;
	gchar t, *databuf;
	struct rspamd_function_atom *res;
	struct expression_argument arg;
	GError *err = NULL;
	enum {
		start_read_argument = 0,
		in_string,
		in_regexp,
		got_backslash,
		got_comma
	} state, prev_state = 0;

	obrace = strchr (input, '(');
	ebrace = strrchr (input, ')');

	g_assert (obrace != NULL && ebrace != NULL);

	res = g_slice_alloc0 (sizeof (*res));
	res->name = g_malloc (obrace - input + 1);
	rspamd_strlcpy (res->name, input, obrace - input + 1);
	res->args = g_array_new (FALSE, FALSE, sizeof (struct expression_argument));

	p = obrace + 1;
	c = p;
	state = start_read_argument;

	/* Read arguments */
	while (p <= ebrace) {
		t = *p;
		switch (state) {
		case start_read_argument:
			if (t == '/') {
				state = in_regexp;
				c = p;
			}
			else if (!g_ascii_isspace (t)) {
				state = in_string;
				c = p;
			}
			p ++;
			break;
		case in_regexp:
			if (t == '\\') {
				state = got_backslash;
				prev_state = in_regexp;
			}
			else if (t == ',' || p == ebrace) {
				databuf = g_malloc (p - c + 1);
				rspamd_strlcpy (databuf, c, p - c + 1);
				arg.type = EXPRESSION_ARGUMENT_REGEXP;
				arg.data = rspamd_regexp_cache_create (NULL, databuf, NULL, &err);

				if (arg.data == NULL) {
					/* Fallback to string */
					msg_warn ("cannot parse slashed argument %s as regexp: %s",
							databuf, err->message);
					g_error_free (err);
					arg.type = EXPRESSION_ARGUMENT_NORMAL;
					arg.data = databuf;
				}
				else {
					g_free (databuf);
				}

				g_array_append_val (res->args, arg);
			}
			p ++;
			break;
		case in_string:
			if (t == '\\') {
				state = got_backslash;
				prev_state = in_string;
			}
			else if (t == ',' || p == ebrace) {
				databuf = g_malloc (p - c + 1);
				rspamd_strlcpy (databuf, c, p - c + 1);
				arg.type = EXPRESSION_ARGUMENT_NORMAL;
				arg.data = databuf;
				g_array_append_val (res->args, arg);
			}
			p ++;
			break;
		case got_backslash:
			state = prev_state;
			p ++;
			break;
		case got_comma:
			state = start_read_argument;
			break;
		}
	}

	return res;
}

static rspamd_expression_atom_t *
rspamd_mime_expr_parse (const gchar *line, gsize len,
		rspamd_mempool_t *pool, gpointer ud, GError **err)
{
	rspamd_expression_atom_t *a = NULL;
	struct rspamd_mime_atom *mime_atom = NULL;
	const gchar *p, *end;
	gchar t;
	gboolean is_function = FALSE;
	enum {
		in_header = 0,
		got_slash,
		in_regexp,
		got_backslash,
		got_second_slash,
		in_flags,
		got_obrace,
		in_function,
		got_ebrace,
		end_atom,
		bad_atom
	} state = 0, prev_state = 0;

	p = line;
	end = p + len;

	while (p < end) {
		t = *p;

		switch (state) {
		case in_header:
			if (t == '/') {
				/* Regexp */
				state = got_slash;
			}
			else if (t == '(') {
				/* Function */
				state = got_obrace;
			}
			else if (g_ascii_isspace (t)) {
				state = bad_atom;
			}
			p ++;
			break;
		case got_slash:
			state = in_regexp;
			break;
		case in_regexp:
			if (t == '\\') {
				state = got_backslash;
				prev_state = in_regexp;
			}
			else if (t == '/') {
				state = got_second_slash;
			}
			p ++;
			break;
		case got_second_slash:
			state = in_flags;
			break;
		case in_flags:
			if (!g_ascii_isalpha (t)) {
				state = end_atom;
			}
			else {
				p ++;
			}
			break;
		case got_backslash:
			state = prev_state;
			p ++;
			break;
		case got_obrace:
			state = in_function;
			is_function = TRUE;
			break;
		case in_function:
			if (t == '\\') {
				state = got_backslash;
				prev_state = in_function;
			}
			else if (t == ')') {
				state = got_ebrace;
			}
			p ++;
			break;
		case got_ebrace:
			state = end_atom;
			break;
		case bad_atom:
			g_set_error (err, rspamd_mime_expr_quark(), 100, "cannot parse"
					" mime atom '%*.s' when reading symbol '%c'", len, line, t);
			return NULL;
		case end_atom:
			goto set;
		}
	}
set:

	if (p - line == 0 || (state != got_ebrace || state != got_second_slash ||
			state != in_flags)) {
		g_set_error (err, rspamd_mime_expr_quark(), 200, "inclomplete or empty"
				" mime atom");
		return NULL;
	}

	mime_atom = g_slice_alloc (sizeof (*mime_atom));
	mime_atom->is_function = is_function;
	mime_atom->str = g_malloc (p - line + 1);
	rspamd_strlcpy (mime_atom->str, line, p - line + 1);

	if (!is_function) {
		mime_atom->d.re = rspamd_mime_expr_parse_regexp_atom (pool,
				mime_atom->str);
		if (mime_atom->d.re == NULL) {
			g_set_error (err, rspamd_mime_expr_quark(), 200, "cannot parse regexp '%s'",
					mime_atom->str);
			goto err;
		}
	}
	else {
		mime_atom->d.func = rspamd_mime_expr_parse_function_atom (mime_atom->str);
		if (mime_atom->d.func == NULL) {
			g_set_error (err, rspamd_mime_expr_quark(), 200, "cannot parse function '%s'",
					mime_atom->str);
			goto err;
		}
	}

	a = rspamd_mempool_alloc (pool, sizeof (*a));
	a->len = p - line;
	a->priority = 0;
	a->data = mime_atom;

err:
	if (mime_atom != NULL) {
		g_free (mime_atom->str);
		g_slice_free1 (sizeof (*mime_atom), mime_atom);
	}
}

struct url_regexp_param {
	struct rspamd_task *task;
	rspamd_regexp_t *regexp;
	struct rspamd_regexp_atom *re;
	gboolean found;
};

static gboolean
tree_url_callback (gpointer key, gpointer value, void *data)
{
	struct url_regexp_param *param = data;
	struct rspamd_url *url = value;

	if (rspamd_regexp_search (param->regexp, struri (url), 0, NULL, NULL, FALSE)
			== TRUE) {
		if (G_UNLIKELY (param->re->is_test)) {
			msg_info ("process test regexp %s for url %s returned TRUE",
				struri (url));
		}
		task_cache_add (param->task, param->re, 1);
		param->found = TRUE;
		return TRUE;
	}
	else if (G_UNLIKELY (param->re->is_test)) {
		msg_info ("process test regexp %s for url %s returned FALSE",
			struri (url));
	}

	return FALSE;
}

static gint
rspamd_mime_regexp_element_process (struct rspamd_task *task,
		struct rspamd_regexp_atom *re, const guchar data, gsize len)
{
	gint r;
	if ((r = task_cache_check (task, re)) != -1) {
		debug_task ("regexp /%s/ is found in cache, result: %d",
				re->regexp_text,
				r);
		return r == 1;
	}
}

static gint
rspamd_mime_expr_process_regexp (struct rspamd_regexp_atom *re,
		struct rspamd_task *task)
{
	guint8 *ct;
	gsize clen;
	gint r, passed = 0;
	gboolean matched = FALSE, raw = FALSE;
	const gchar *in, *start, *end;

	GList *cur, *headerlist;
	rspamd_regexp_t *regexp;
	struct url_regexp_param callback_param = {
		.task = task,
		.re = re,
		.found = FALSE
	};
	struct mime_text_part *part;
	struct raw_header *rh;

	if (re == NULL) {
		msg_info ("invalid regexp passed");
		return 0;
	}

	callback_param.regexp = re->regexp;


	switch (re->type) {
	case REGEXP_NONE:
		msg_warn ("bad error detected: %s has invalid regexp type",
			re->regexp_text);
		break;
	case REGEXP_HEADER:
	case REGEXP_RAW_HEADER:
		/* Check header's name */
		if (re->header == NULL) {
			msg_info ("header regexp without header name: '%s'",
				re->regexp_text);
			task_cache_add (task, re, 0);
			return 0;
		}
		debug_task ("checking %s header regexp: %s = %s",
			re->type == REGEXP_RAW_HEADER ? "raw" : "decoded",
			re->header,
			re->regexp_text);

		/* Get list of specified headers */
		headerlist = message_get_header (task,
				re->header,
				re->is_strong);
		if (headerlist == NULL) {
			/* Header is not found */
			if (G_UNLIKELY (re->is_test)) {
				msg_info (
					"process test regexp %s for header %s returned FALSE: no header found",
					re->regexp_text,
					re->header);
			}
			task_cache_add (task, re, 0);
			return 0;
		}
		else {
			/* Check whether we have regexp for it */
			if (re->regexp == NULL) {
				debug_task ("regexp contains only header and it is found %s",
					re->header);
				task_cache_add (task, re, 1);
				return 1;
			}
			/* Iterate through headers */
			cur = headerlist;
			while (cur) {
				rh = cur->data;
				debug_task ("found header \"%s\" with value \"%s\"",
					re->header, rh->decoded);
				regexp = re->regexp;

				if (re->type == REGEXP_RAW_HEADER) {
					in = rh->value;
					raw = TRUE;
				}
				else {
					in = rh->decoded;
					/* Validate input */
					if (!in || !g_utf8_validate (in, -1, NULL)) {
						cur = g_list_next (cur);
						continue;
					}
				}

				/* Match re */
				if (in &&
					rspamd_regexp_search (regexp, in, 0, NULL, NULL, raw)) {
					if (G_UNLIKELY (re->is_test)) {
						msg_info (
							"process test regexp %s for header %s with value '%s' returned TRUE",
							re->regexp_text,
							re->header,
							in);
					}
					task_cache_add (task, re, 1);
					return 1;
				}
				else if (G_UNLIKELY (re->is_test)) {
					msg_info (
						"process test regexp %s for header %s with value '%s' returned FALSE",
						re->regexp_text,
						re->header,
						in);
				}
				cur = g_list_next (cur);
			}
			task_cache_add (task, re, 0);
			return 0;
		}
		break;
	case REGEXP_MIME:
		debug_task ("checking mime regexp: %s", re->regexp_text);
		/* Iterate throught text parts */
		cur = g_list_first (task->text_parts);
		while (cur) {
			part = (struct mime_text_part *)cur->data;
			/* Skip empty parts */
			if (part->is_empty) {
				cur = g_list_next (cur);
				continue;
			}
			/* Skip too large parts */
			if (max_re_data != 0 && part->content->len > max_re_data) {
				msg_info ("<%s> skip part of size %Hud",
					task->message_id,
					part->content->len);
				cur = g_list_next (cur);
				continue;
			}

			regexp = re->regexp;

			/* Check raw flags */
			if (part->is_raw) {
				raw = TRUE;
			}
			/* Select data for regexp */
			if (raw) {
				ct = part->orig->data;
				clen = part->orig->len;
			}
			else {
				ct = part->content->data;
				clen = part->content->len;
			}
			/* If we have limit, apply regexp so much times as we can */
			if (rspamd_regexp_search (regexp, ct, clen, NULL, NULL, raw)) {
				if (G_UNLIKELY (re->is_test)) {
					msg_info (
							"process test regexp %s for mime part of length %d returned TRUE",
							re->regexp_text,
							(gint)clen);
				}
				task_cache_add (task, re, 1);
				return 1;
			}
			if (!matched && G_UNLIKELY (re->is_test)) {
				msg_info (
					"process test regexp %s for mime part of length %d returned FALSE",
					re->regexp_text,
					(gint)clen);
			}
			cur = g_list_next (cur);
		}
		task_cache_add (task, re, 0);
		break;
	case REGEXP_MESSAGE:
		debug_task ("checking message regexp: %s", re->regexp_text);
		raw = TRUE;
		regexp = re->regexp;
		ct = (guint8 *)task->msg.start;
		clen = task->msg.len;

		if (max_re_data != 0 && clen > max_re_data) {
			msg_info ("<%s> skip message of size %Hz", task->message_id, clen);
			return 0;
		}
		if (rspamd_regexp_search (regexp, ct, clen, NULL, NULL, raw)) {
			if (G_UNLIKELY (re->is_test)) {
				msg_info (
						"process test regexp %s for message part of length %d returned TRUE",
						re->regexp_text,
						(gint)clen);
			}
			task_cache_add (task, re, 1);
			return 1;
		}
		if (!matched && G_UNLIKELY (re->is_test)) {
			msg_info (
				"process test regexp %s for message part of length %d returned FALSE",
				re->regexp_text,
				(gint)clen);
		}
		task_cache_add (task, re, 0);
		break;
	case REGEXP_URL:
		debug_task ("checking url regexp: %s", re->regexp_text);
		regexp = re->regexp;
		callback_param.task = task;
		callback_param.regexp = regexp;
		callback_param.re = re;
		callback_param.found = FALSE;
		if (task->urls) {
			g_tree_foreach (task->urls, tree_url_callback, &callback_param);
		}
		if (task->emails && callback_param.found == FALSE) {
			g_tree_foreach (task->emails, tree_url_callback, &callback_param);
		}
		if (callback_param.found == FALSE) {
			task_cache_add (task, re, 0);
		}
		break;
	default:
		msg_warn ("bad error detected: %p is not a valid regexp object", re);
		break;
	}

	/* Not reached */
	return 0;
}


static gint
rspamd_mime_expr_process (gpointer input, rspamd_expression_atom_t *atom)
{

}

static gint
rspamd_mime_expr_priority (rspamd_expression_atom_t *atom)
{
	/* TODO: implement priorities for mime expressions */
	return 0;
}

static void
rspamd_mime_expr_destroy (rspamd_expression_atom_t *atom)
{
	struct rspamd_mime_atom *mime_atom = atom->data;
	guint i;
	struct expression_argument *arg;

	if (mime_atom) {
		if (mime_atom->is_function) {
			/* Need to cleanup arguments */
			for (i = 0; i < mime_atom->d.func->args->len; i ++) {
				arg = &g_array_index (mime_atom->d.func->args,
						struct expression_argument, i);

				if (arg->type == EXPRESSION_ARGUMENT_NORMAL) {
					g_free (arg->data);
				}
			}
			g_array_free (mime_atom->d.func->args, TRUE);
		}
		/* XXX: regexp shouldn't be special */
		g_slice_free1 (sizeof (*mime_atom), mime_atom);
	}
}

gboolean
call_expression_function (struct rspamd_function_atom * func,
	struct rspamd_task * task,
	lua_State *L)
{
	struct _fl *selected, key;

	key.name = func->name;

	selected = bsearch (&key,
			list_ptr,
			functions_number,
			sizeof (struct _fl),
			fl_cmp);
	if (selected == NULL) {
		/* Try to check lua function */
		return FALSE;
	}

	return selected->func (task, func->args, selected->user_data);
}

void
register_expression_function (const gchar *name,
	rspamd_internal_func_t func,
	void *user_data)
{
	static struct _fl *new;

	functions_number++;

	new = g_new (struct _fl, functions_number);
	memcpy (new, list_ptr, (functions_number - 1) * sizeof (struct _fl));
	if (list_allocated) {
		g_free (list_ptr);
	}

	list_allocated = TRUE;
	new[functions_number - 1].name = name;
	new[functions_number - 1].func = func;
	new[functions_number - 1].user_data = user_data;
	qsort (new, functions_number, sizeof (struct _fl), fl_cmp);
	list_ptr = new;
}

gboolean
rspamd_compare_encoding (struct rspamd_task *task, GList * args, void *unused)
{
	struct expression_argument *arg;

	if (args == NULL || task == NULL) {
		return FALSE;
	}

	arg = get_function_arg (args->data, task, TRUE);
	if (arg->type == EXPRESSION_ARGUMENT_BOOL) {
		msg_warn ("invalid argument to function is passed");
		return FALSE;
	}

	/* XXX: really write this function */
	return TRUE;
}

gboolean
rspamd_header_exists (struct rspamd_task * task, GList * args, void *unused)
{
	struct expression_argument *arg;
	GList *headerlist;

	if (args == NULL || task == NULL) {
		return FALSE;
	}

	arg = get_function_arg (args->data, task, TRUE);
	if (!arg || arg->type == EXPRESSION_ARGUMENT_BOOL) {
		msg_warn ("invalid argument to function is passed");
		return FALSE;
	}

	debug_task ("try to get header %s", (gchar *)arg->data);
	headerlist = message_get_header (task,
			(gchar *)arg->data,
			FALSE);
	if (headerlist) {
		return TRUE;
	}
	return FALSE;
}

/*
 * This function is designed to find difference between text/html and text/plain parts
 * It takes one argument: difference threshold, if we have two text parts, compare
 * its hashes and check for threshold, if value is greater than threshold, return TRUE
 * and return FALSE otherwise.
 */
gboolean
rspamd_parts_distance (struct rspamd_task * task, GList * args, void *unused)
{
	gint threshold, threshold2 = -1, diff;
	struct mime_text_part *p1, *p2;
	GList *cur;
	struct expression_argument *arg;
	GMimeObject *parent;
	const GMimeContentType *ct;
	gint *pdiff;

	if (args == NULL) {
		debug_task ("no threshold is specified, assume it 100");
		threshold = 100;
	}
	else {
		errno = 0;
		arg = get_function_arg (args->data, task, TRUE);
		threshold = strtoul ((gchar *)arg->data, NULL, 10);
		if (errno != 0) {
			msg_info ("bad numeric value for threshold \"%s\", assume it 100",
				(gchar *)args->data);
			threshold = 100;
		}
		if (args->next) {
			arg = get_function_arg (args->next->data, task, TRUE);
			errno = 0;
			threshold2 = strtoul ((gchar *)arg->data, NULL, 10);
			if (errno != 0) {
				msg_info ("bad numeric value for threshold \"%s\", ignore it",
					(gchar *)arg->data);
				threshold2 = -1;
			}
		}
	}

	if ((pdiff =
		rspamd_mempool_get_variable (task->task_pool,
		"parts_distance")) != NULL) {
		diff = *pdiff;
		if (diff != -1) {
			if (threshold2 > 0) {
				if (diff >=
					MIN (threshold,
					threshold2) && diff < MAX (threshold, threshold2)) {
					return TRUE;
				}
			}
			else {
				if (diff <= threshold) {
					return TRUE;
				}
			}
			return FALSE;
		}
		else {
			return FALSE;
		}
	}

	if (g_list_length (task->text_parts) == 2) {
		cur = g_list_first (task->text_parts);
		p1 = cur->data;
		cur = g_list_next (cur);
		pdiff = rspamd_mempool_alloc (task->task_pool, sizeof (gint));
		*pdiff = -1;

		if (cur == NULL) {
			msg_info ("bad parts list");
			return FALSE;
		}
		p2 = cur->data;
		/* First of all check parent object */
		if (p1->parent && p1->parent == p2->parent) {
			parent = p1->parent;
			ct = g_mime_object_get_content_type (parent);
#ifndef GMIME24
			if (ct == NULL ||
				!g_mime_content_type_is_type (ct, "multipart", "alternative")) {
#else
			if (ct == NULL ||
				!g_mime_content_type_is_type ((GMimeContentType *)ct,
				"multipart", "alternative")) {
#endif
				debug_task (
					"two parts are not belong to multipart/alternative container, skip check");
				rspamd_mempool_set_variable (task->task_pool,
					"parts_distance",
					pdiff,
					NULL);
				return FALSE;
			}
		}
		else {
			debug_task (
				"message contains two parts but they are in different multi-parts");
			rspamd_mempool_set_variable (task->task_pool,
				"parts_distance",
				pdiff,
				NULL);
			return FALSE;
		}
		if (!p1->is_empty && !p2->is_empty) {
			if (p1->diff_str != NULL && p2->diff_str != NULL) {
				diff = rspamd_diff_distance_normalized (p1->diff_str,
						p2->diff_str);
			}
			else {
				diff = rspamd_fuzzy_compare_parts (p1, p2);
			}
			debug_task (
				"got likeliness between parts of %d%%, threshold is %d%%",
				diff,
				threshold);
			*pdiff = diff;
			rspamd_mempool_set_variable (task->task_pool,
				"parts_distance",
				pdiff,
				NULL);
			if (threshold2 > 0) {
				if (diff >=
					MIN (threshold,
					threshold2) && diff < MAX (threshold, threshold2)) {
					return TRUE;
				}
			}
			else {
				if (diff <= threshold) {
					return TRUE;
				}
			}
		}
		else if ((p1->is_empty &&
			!p2->is_empty) || (!p1->is_empty && p2->is_empty)) {
			/* Empty and non empty parts are different */
			*pdiff = 0;
			rspamd_mempool_set_variable (task->task_pool,
				"parts_distance",
				pdiff,
				NULL);
			return TRUE;
		}
	}
	else {
		debug_task (
			"message has too many text parts, so do not try to compare them with each other");
		rspamd_mempool_set_variable (task->task_pool,
			"parts_distance",
			pdiff,
			NULL);
		return FALSE;
	}

	rspamd_mempool_set_variable (task->task_pool, "parts_distance", pdiff,
		NULL);
	return FALSE;
}

struct addr_list {
	const gchar *name;
	const gchar *addr;
};

#define COMPARE_RCPT_LEN 3
#define MIN_RCPT_TO_COMPARE 7

gboolean
rspamd_recipients_distance (struct rspamd_task *task, GList * args,
	void *unused)
{
	struct expression_argument *arg;
	InternetAddressList *cur;
	double threshold;
	struct addr_list *ar;
	gchar *c;
	gint num, i, j, hits = 0, total = 0;

	if (args == NULL) {
		msg_warn ("no parameters to function");
		return FALSE;
	}

	arg = get_function_arg (args->data, task, TRUE);
	errno = 0;
	threshold = strtod ((gchar *)arg->data, NULL);
	if (errno != 0) {
		msg_warn ("invalid numeric value '%s': %s",
			(gchar *)arg->data,
			strerror (errno));
		return FALSE;
	}

	if (!task->rcpt_mime) {
		return FALSE;
	}
	num = internet_address_list_length (task->rcpt_mime);
	if (num < MIN_RCPT_TO_COMPARE) {
		return FALSE;
	}
	ar =
		rspamd_mempool_alloc0 (task->task_pool, num *
			sizeof (struct addr_list));

	/* Fill array */
	cur = task->rcpt_mime;
#ifdef GMIME24
	for (i = 0; i < num; i++) {
		InternetAddress *iaelt =
			internet_address_list_get_address(cur, i);
		InternetAddressMailbox *iamb =
			INTERNET_ADDRESS_IS_MAILBOX(iaelt) ?
			INTERNET_ADDRESS_MAILBOX (iaelt) : NULL;
		if (iamb) {
			ar[i].name = internet_address_mailbox_get_addr (iamb);
			if (ar[i].name != NULL && (c = strchr (ar[i].name, '@')) != NULL) {
				ar[i].addr = c + 1;
			}
		}
	}
#else
	InternetAddress *addr;
	i = 0;
	while (cur) {
		addr = internet_address_list_get_address (cur);
		if (addr && internet_address_get_type (addr) == INTERNET_ADDRESS_NAME) {
			ar[i].name = rspamd_mempool_strdup (task->task_pool,
					internet_address_get_addr (addr));
			if (ar[i].name != NULL && (c = strchr (ar[i].name, '@')) != NULL) {
				*c = '\0';
				ar[i].addr = c + 1;
			}
			cur = internet_address_list_next (cur);
			i++;
		}
		else {
			cur = internet_address_list_next (cur);
		}
	}
#endif

	/* Cycle all elements in array */
	for (i = 0; i < num; i++) {
		for (j = i + 1; j < num; j++) {
			if (ar[i].name && ar[j].name &&
				g_ascii_strncasecmp (ar[i].name, ar[j].name,
				COMPARE_RCPT_LEN) == 0) {
				/* Common name part */
				hits++;
			}
			else if (ar[i].addr && ar[j].addr &&
				g_ascii_strcasecmp (ar[i].addr, ar[j].addr) == 0) {
				/* Common address part, but different name */
				hits++;
			}
			total++;
		}
	}

	if ((double)(hits * num / 2.) / (double)total >= threshold) {
		return TRUE;
	}

	return FALSE;
}

gboolean
rspamd_has_only_html_part (struct rspamd_task * task, GList * args,
	void *unused)
{
	struct mime_text_part *p;
	GList *cur;
	gboolean res = FALSE;

	cur = g_list_first (task->text_parts);
	while (cur) {
		p = cur->data;
		if (p->is_html) {
			res = TRUE;
		}
		else {
			res = FALSE;
			break;
		}
		cur = g_list_next (cur);
	}

	return res;
}

static gboolean
is_recipient_list_sorted (const InternetAddressList * ia)
{
	const InternetAddressList *cur;
	InternetAddress *addr;
	gboolean res = TRUE;
	struct addr_list current = { NULL, NULL }, previous = {
		NULL, NULL
	};
#ifdef GMIME24
	gint num, i;
#endif

	/* Do not check to short address lists */
	if (internet_address_list_length ((InternetAddressList *)ia) <
		MIN_RCPT_TO_COMPARE) {
		return FALSE;
	}
#ifdef GMIME24
	num = internet_address_list_length ((InternetAddressList *)ia);
	cur = ia;
	for (i = 0; i < num; i++) {
		addr =
			internet_address_list_get_address ((InternetAddressList *)cur, i);
		current.addr = (gchar *)internet_address_get_name (addr);
		if (previous.addr != NULL) {
			if (current.addr &&
				g_ascii_strcasecmp (current.addr, previous.addr) < 0) {
				res = FALSE;
				break;
			}
		}
		previous.addr = current.addr;
	}
#else
	cur = ia;
	while (cur) {
		addr = internet_address_list_get_address (cur);
		if (internet_address_get_type (addr) == INTERNET_ADDRESS_NAME) {
			current.addr = internet_address_get_addr (addr);
			if (previous.addr != NULL) {
				if (current.addr &&
					g_ascii_strcasecmp (current.addr, previous.addr) < 0) {
					res = FALSE;
					break;
				}
			}
			previous.addr = current.addr;
		}
		cur = internet_address_list_next (cur);
	}
#endif

	return res;
}

gboolean
rspamd_is_recipients_sorted (struct rspamd_task * task,
	GList * args,
	void *unused)
{
	/* Check all types of addresses */
	if (is_recipient_list_sorted (g_mime_message_get_recipients (task->message,
		GMIME_RECIPIENT_TYPE_TO)) == TRUE) {
		return TRUE;
	}
	if (is_recipient_list_sorted (g_mime_message_get_recipients (task->message,
		GMIME_RECIPIENT_TYPE_BCC)) == TRUE) {
		return TRUE;
	}
	if (is_recipient_list_sorted (g_mime_message_get_recipients (task->message,
		GMIME_RECIPIENT_TYPE_CC)) == TRUE) {
		return TRUE;
	}

	return FALSE;
}

gboolean
rspamd_compare_transfer_encoding (struct rspamd_task * task,
	GList * args,
	void *unused)
{
	GMimeObject *part;
#ifndef GMIME24
	GMimePartEncodingType enc_req, part_enc;
#else
	GMimeContentEncoding enc_req, part_enc;
#endif
	struct expression_argument *arg;

	if (args == NULL) {
		msg_warn ("no parameters to function");
		return FALSE;
	}

	arg = get_function_arg (args->data, task, TRUE);
#ifndef GMIME24
	enc_req = g_mime_part_encoding_from_string (arg->data);
	if (enc_req == GMIME_PART_ENCODING_DEFAULT) {
#else
	enc_req = g_mime_content_encoding_from_string (arg->data);
	if (enc_req == GMIME_CONTENT_ENCODING_DEFAULT) {
#endif
		msg_warn ("bad encoding type: %s", (gchar *)arg->data);
		return FALSE;
	}

	part = g_mime_message_get_mime_part (task->message);
	if (part) {
		if (GMIME_IS_PART (part)) {
#ifndef GMIME24
			part_enc = g_mime_part_get_encoding (GMIME_PART (part));
			if (part_enc == GMIME_PART_ENCODING_DEFAULT) {
				/* Assume 7bit as default transfer encoding */
				part_enc = GMIME_PART_ENCODING_7BIT;
			}
#else
			part_enc = g_mime_part_get_content_encoding (GMIME_PART (part));
			if (part_enc == GMIME_CONTENT_ENCODING_DEFAULT) {
				/* Assume 7bit as default transfer encoding */
				part_enc = GMIME_CONTENT_ENCODING_7BIT;
			}
#endif


			debug_task ("got encoding in part: %d and compare with %d",
				(gint)part_enc,
				(gint)enc_req);
#ifndef GMIME24
			g_object_unref (part);
#endif

			return part_enc == enc_req;
		}
#ifndef GMIME24
		g_object_unref (part);
#endif
	}

	return FALSE;
}

gboolean
rspamd_is_html_balanced (struct rspamd_task * task, GList * args, void *unused)
{
	struct mime_text_part *p;
	GList *cur;
	gboolean res = TRUE;

	cur = g_list_first (task->text_parts);
	while (cur) {
		p = cur->data;
		if (!p->is_empty && p->is_html) {
			if (p->is_balanced) {
				res = TRUE;
			}
			else {
				res = FALSE;
				break;
			}
		}
		cur = g_list_next (cur);
	}

	return res;

}

struct html_callback_data {
	struct html_tag *tag;
	gboolean *res;
};

static gboolean
search_html_node_callback (GNode * node, gpointer data)
{
	struct html_callback_data *cd = data;
	struct html_node *nd;

	nd = node->data;
	if (nd) {
		if (nd->tag == cd->tag) {
			*cd->res = TRUE;
			return TRUE;
		}
	}

	return FALSE;
}

gboolean
rspamd_has_html_tag (struct rspamd_task * task, GList * args, void *unused)
{
	struct mime_text_part *p;
	GList *cur;
	struct expression_argument *arg;
	struct html_tag *tag;
	gboolean res = FALSE;
	struct html_callback_data cd;

	if (args == NULL) {
		msg_warn ("no parameters to function");
		return FALSE;
	}

	arg = get_function_arg (args->data, task, TRUE);
	tag = get_tag_by_name (arg->data);
	if (tag == NULL) {
		msg_warn ("unknown tag type passed as argument: %s",
			(gchar *)arg->data);
		return FALSE;
	}

	cur = g_list_first (task->text_parts);
	cd.res = &res;
	cd.tag = tag;

	while (cur && res == FALSE) {
		p = cur->data;
		if (!p->is_empty && p->is_html && p->html_nodes) {
			g_node_traverse (p->html_nodes,
				G_PRE_ORDER,
				G_TRAVERSE_ALL,
				-1,
				search_html_node_callback,
				&cd);
		}
		cur = g_list_next (cur);
	}

	return res;

}

gboolean
rspamd_has_fake_html (struct rspamd_task * task, GList * args, void *unused)
{
	struct mime_text_part *p;
	GList *cur;
	gboolean res = FALSE;

	cur = g_list_first (task->text_parts);

	while (cur && res == FALSE) {
		p = cur->data;
		if (!p->is_empty && p->is_html && p->html_nodes == NULL) {
			res = TRUE;
		}
		cur = g_list_next (cur);
	}

	return res;

}

guint
rspamd_mime_expression_set_re_limit (guint limit)
{
	guint ret = max_re_data;

	max_re_data = limit;
	return ret;
}