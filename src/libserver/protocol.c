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
#include "main.h"
#include "util.h"
#include "cfg_file.h"
#include "cfg_rcl.h"
#include "message.h"
#include "utlist.h"

/* Max line size */
#define OUTBUFSIZ BUFSIZ
/*
 * Just check if the passed message is spam or not and reply as
 * described below
 */
#define MSG_CMD_CHECK "check"
/*
 * Check if message is spam or not, and return score plus list
 * of symbols hit
 */
#define MSG_CMD_SYMBOLS "symbols"
/*
 * Check if message is spam or not, and return score plus report
 */
#define MSG_CMD_REPORT "report"
/*
 * Check if message is spam or not, and return score plus report
 * if the message is spam
 */
#define MSG_CMD_REPORT_IFSPAM "report_ifspam"
/*
 * Ignore this message -- client opened connection then changed
 */
#define MSG_CMD_SKIP "skip"
/*
 * Return a confirmation that spamd is alive
 */
#define MSG_CMD_PING "ping"
/*
 * Process this message as described above and return modified message
 */
#define MSG_CMD_PROCESS "process"

/*
 * Learn specified statfile using message
 */
#define MSG_CMD_LEARN "learn"

/*
 * spamassassin greeting:
 */
#define SPAMC_GREETING "SPAMC"
/*
 * rspamd greeting:
 */
#define RSPAMC_GREETING "RSPAMC"
/*
 * Headers
 */
#define CONTENT_LENGTH_HEADER "Content-length"
#define HELO_HEADER "Helo"
#define FROM_HEADER "From"
#define IP_ADDR_HEADER "IP"
#define NRCPT_HEADER "Recipient-Number"
#define RCPT_HEADER "Rcpt"
#define SUBJECT_HEADER "Subject"
#define STATFILE_HEADER "Statfile"
#define QUEUE_ID_HEADER "Queue-ID"
#define ERROR_HEADER "Error"
#define USER_HEADER "User"
#define URLS_HEADER "URL-Format"
#define PASS_HEADER "Pass"
#define JSON_HEADER "Json"
#define HOSTNAME_HEADER "Hostname"
#define DELIVER_TO_HEADER "Deliver-To"
#define NO_LOG_HEADER "Log"
#define MLEN_HEADER "Message-Length"

static GList *custom_commands = NULL;

 static gchar *
 rspamd_protocol_header_dup (struct rspamd_task *task, GString *h)
 {
 	gchar *res;

 	g_assert (h != NULL);
 	g_assert (h->len > 0);

 	res = rspamd_mempool_alloc (task->task_pool, h->len + 1);
 	rspamd_strlcpy (res, h->str, h->len + 1);

 	return res;
 }

/*
 * Remove <> from the fixed string and copy it to the pool
 */
static gchar *
rspamd_protocol_escape_braces (struct rspamd_task *task, GString *in)
{
	gint len = 0;
	gchar *orig, *p, *res;

	g_assert (in != NULL);
	g_assert (in->len > 0);

	orig = in->str;

	while ((g_ascii_isspace (*orig) || *orig ==
		'<') && orig - in->str < (gint)in->len) {
		orig++;
	}

	res = rspamd_mempool_alloc (task->task_pool,
			in->len - (orig - in->str));

	p = in->str + in->len - 1;
	len = in->len - (orig - in->str);

	while ((!g_ascii_isspace (*p) && *p !=
		'>') && p > orig) {
		p--;
		len--;
	}

	rspamd_strlcpy (res, orig, len + 1);

	return res;
}

static gboolean
rspamd_protocol_handle_url (struct rspamd_task *task,
	struct rspamd_http_message *msg)
{
	GList *cur;
	struct custom_command *cmd;
	const gchar *p;

	if (msg->url == NULL || msg->url->len == 0) {
		task->last_error = "command is absent";
		task->error_code = 400;
		return FALSE;
	}

	if (msg->url->str[0] == '/') {
		p = &msg->url->str[1];
	}
	else {
		p = msg->url->str;
	}

	switch (*p) {
	case 'c':
	case 'C':
		/* check */
		if (g_ascii_strcasecmp (p + 1, MSG_CMD_CHECK + 1) == 0) {
			task->cmd = CMD_CHECK;
		}
		else {
			goto err;
		}
		break;
	case 's':
	case 'S':
		/* symbols, skip */
		if (g_ascii_strcasecmp (p + 1, MSG_CMD_SYMBOLS + 1) == 0) {
			task->cmd = CMD_SYMBOLS;
		}
		else if (g_ascii_strcasecmp (p + 1, MSG_CMD_SKIP + 1) == 0) {
			task->cmd = CMD_SKIP;
		}
		else {
			goto err;
		}
		break;
	case 'p':
	case 'P':
		/* ping, process */
		if (g_ascii_strcasecmp (p + 1, MSG_CMD_PING + 1) == 0) {
			task->cmd = CMD_PING;
		}
		else if (g_ascii_strcasecmp (p + 1, MSG_CMD_PROCESS + 1) == 0) {
			task->cmd = CMD_PROCESS;
		}
		else {
			goto err;
		}
		break;
	case 'r':
	case 'R':
		/* report, report_ifspam */
		if (g_ascii_strcasecmp (p + 1, MSG_CMD_REPORT + 1) == 0) {
			task->cmd = CMD_REPORT;
		}
		else if (g_ascii_strcasecmp (p + 1, MSG_CMD_REPORT_IFSPAM + 1) == 0) {
			task->cmd = CMD_REPORT_IFSPAM;
		}
		else {
			goto err;
		}
		break;
	default:
		cur = custom_commands;
		while (cur) {
			cmd = cur->data;
			if (g_ascii_strcasecmp (p, cmd->name) == 0) {
				task->cmd = CMD_OTHER;
				task->custom_cmd = cmd;
				break;
			}
			cur = g_list_next (cur);
		}

		if (cur == NULL) {
			goto err;
		}
		break;
	}

	return TRUE;

err:
	debug_task ("bad command: %s", p);
	task->last_error = "invalid command";
	task->error_code = 400;
	return FALSE;
}

gboolean
rspamd_protocol_handle_headers (struct rspamd_task *task,
	struct rspamd_http_message *msg)
{
	gchar *headern, *tmp;
	gboolean res = TRUE, validh, fl, has_ip = FALSE;
	gsize hlen;
	struct rspamd_http_header *h;

	LL_FOREACH (msg->headers, h)
	{
		headern = h->name->str;
		hlen = h->name->len;
		validh = TRUE;

		switch (headern[0]) {
		case 'd':
		case 'D':
			if (g_ascii_strncasecmp (headern, DELIVER_TO_HEADER, hlen) == 0) {
				task->deliver_to = rspamd_protocol_escape_braces (task, h->value);
				debug_task ("read deliver-to header, value: %s",
					task->deliver_to);
			}
			else {
				debug_task ("wrong header: %v", h->name);
				validh = FALSE;
			}
			break;
		case 'h':
		case 'H':
			if (g_ascii_strncasecmp (headern, HELO_HEADER, hlen) == 0) {
				task->helo = rspamd_protocol_header_dup (task, h->value);
				debug_task ("read helo header, value: %s", task->helo);
			}
			else if (g_ascii_strncasecmp (headern, HOSTNAME_HEADER, hlen) == 0) {
				task->hostname = rspamd_protocol_header_dup (task, h->value);
				debug_task ("read hostname header, value: %s", task->hostname);
			}
			else {
				debug_task ("wrong header: %v", h->value);
				validh = FALSE;
			}
			break;
		case 'f':
		case 'F':
			if (g_ascii_strncasecmp (headern, FROM_HEADER, hlen) == 0) {
				if (!rspamd_task_add_sender (task,
						rspamd_protocol_header_dup (task, h->value))) {
					msg_err ("bad from header: '%v'", h->value);
					validh = FALSE;
				}
			}
			else {
				debug_task ("wrong header: %v", h->value);
				validh = FALSE;
			}
			break;
		case 'j':
		case 'J':
			if (g_ascii_strncasecmp (headern, JSON_HEADER, hlen) == 0) {
				fl = rspamd_config_parse_flag (h->value->str, h->value->len);
				if (fl) {
					task->flags |= RSPAMD_TASK_FLAG_JSON;
				}
				else {
					task->flags &= ~RSPAMD_TASK_FLAG_JSON;
				}
			}
			else {
				debug_task ("wrong header: %v", h->value);
				validh = FALSE;
			}
			break;
		case 'q':
		case 'Q':
			if (g_ascii_strncasecmp (headern, QUEUE_ID_HEADER, hlen) == 0) {
				task->queue_id = rspamd_protocol_header_dup (task, h->value);
				debug_task ("read queue_id header, value: %s", task->queue_id);
			}
			else {
				debug_task ("wrong header: %v", h->name);
				validh = FALSE;
			}
			break;
		case 'r':
		case 'R':
			if (g_ascii_strncasecmp (headern, RCPT_HEADER, hlen) == 0) {
				if (!rspamd_task_add_recipient (task,
						rspamd_protocol_header_dup (task, h->value))) {
					msg_err ("bad from header: '%v'", h->value);
					validh = FALSE;
				}
				debug_task ("read rcpt header, value: %v", h->value);
			}
			else {
				debug_task ("wrong header: %v", h->name);
				validh = FALSE;
			}
			break;
		case 'i':
		case 'I':
			if (g_ascii_strncasecmp (headern, IP_ADDR_HEADER, hlen) == 0) {
				tmp = rspamd_protocol_header_dup (task, h->value);

				if (!rspamd_parse_inet_address (&task->from_addr, tmp)) {
					msg_err ("bad ip header: '%s'", tmp);
					return FALSE;
				}
				debug_task ("read IP header, value: %s", tmp);
				has_ip = TRUE;
			}
			else {
				debug_task ("wrong header: %v", h->name);
				validh = FALSE;
			}
			break;
		case 'p':
		case 'P':
			if (g_ascii_strncasecmp (headern, PASS_HEADER, hlen) == 0) {
				if (h->value->len == sizeof ("all") - 1 &&
					g_ascii_strncasecmp (h->value->str, "all", h->value->len) == 0) {
					task->flags |= RSPAMD_TASK_FLAG_PASS_ALL;
					debug_task ("pass all filters");
				}
			}
			else {
				validh = FALSE;
			}
			break;
		case 's':
		case 'S':
			if (g_ascii_strncasecmp (headern, SUBJECT_HEADER, hlen) == 0) {
				task->subject = rspamd_protocol_header_dup (task, h->value);
			}
			else {
				validh = FALSE;
			}
			break;
		case 'u':
		case 'U':
			if (g_ascii_strncasecmp (headern, USER_HEADER, hlen) == 0) {
				task->user = rspamd_protocol_header_dup (task, h->value);
			}
			if (g_ascii_strncasecmp (headern, URLS_HEADER, hlen) == 0) {
				if (h->value->len == sizeof ("extended") - 1 &&
						g_ascii_strncasecmp (h->value->str, "extended",
								h->value->len) == 0) {
					task->flags |= RSPAMD_TASK_FLAG_EXT_URLS;
					debug_task ("extended urls information");
				}
			}
			else {
				validh = FALSE;
			}
			break;
		case 'l':
		case 'L':
			if (g_ascii_strncasecmp (headern, NO_LOG_HEADER, hlen) == 0) {
				if (g_ascii_strncasecmp (h->value->str, "no", h->value->len) == 0) {
					task->flags |= RSPAMD_TASK_FLAG_NO_LOG;
				}
			}
			else {
				validh = FALSE;
			}
			break;
		case 'm':
		case 'M':
			if (g_ascii_strncasecmp (headern, MLEN_HEADER, hlen) == 0) {
				task->message_len = strtoul (h->value->str, NULL, 10);

				if (task->message_len == 0) {
					msg_err ("Invalid message length header: %v", h->value);
					validh = FALSE;
				}
				else {
					task->flags |= RSPAMD_TASK_FLAG_HAS_CONTROL;
				}
			}
			else {
				validh = FALSE;
			}
			break;
		default:
			debug_task ("unknown header: %v", h->name);
			validh = FALSE;
			break;
		}

		if (!validh) {
			res = FALSE;
		}
	}

	if (!res && task->cfg->strict_protocol_headers) {
		msg_err (
			"deny processing of a request with incorrect or unknown headers");
		task->last_error = "invalid header";
		task->error_code = 400;
		return FALSE;
	}

	if (task->hostname == NULL || task->hostname[0] == '\0') {
		/* We assume that hostname is either "unknown" or existing */
		task->hostname = rspamd_mempool_strdup (task->task_pool, "unknown");
	}

	if (!has_ip) {
		task->flags |= RSPAMD_TASK_FLAG_NO_IP;
	}

	return TRUE;
}

#define BOOL_TO_FLAG(val, flags, flag) do {									\
	if ((val)) (flags) |= (flag);											\
	else (flags) &= ~(flag);												\
} while(0)

gboolean
rspamd_protocol_parse_task_flags (rspamd_mempool_t *pool,
	const ucl_object_t *obj,
	gpointer ud,
	struct rspamd_rcl_section *section,
	GError **err)
{
	struct rspamd_rcl_struct_parser *pd = ud;
	gint *target;
	const gchar *key;
	gboolean value;

	target = (gint *)(((gchar *)pd->user_struct) + pd->offset);
	key = ucl_object_key (obj);
	value = ucl_object_toboolean (obj);

	if (key != NULL) {
		if (g_ascii_strcasecmp (key, "pass_all") == 0) {
			BOOL_TO_FLAG (value, *target, RSPAMD_TASK_FLAG_PASS_ALL);
		}
		else if (g_ascii_strcasecmp (key, "no_log") == 0) {
			BOOL_TO_FLAG (value, *target, RSPAMD_TASK_FLAG_NO_LOG);
		}
	}

	return TRUE;
}

static struct rspamd_rcl_section *control_parser = NULL;

static void
rspamd_protocol_control_parser_init (void)
{
	struct rspamd_rcl_section *sub;

	if (control_parser == NULL) {
		sub = rspamd_rcl_add_section (&control_parser,
				"*",
				NULL,
				UCL_OBJECT,
				FALSE,
				TRUE);
		/* Default handlers */
		rspamd_rcl_add_default_handler (sub,
				"ip",
				rspamd_rcl_parse_struct_addr,
				G_STRUCT_OFFSET (struct rspamd_task, from_addr),
				0);
		rspamd_rcl_add_default_handler (sub,
				"from",
				rspamd_rcl_parse_struct_mime_addr,
				G_STRUCT_OFFSET (struct rspamd_task, from_envelope),
				0);
		rspamd_rcl_add_default_handler (sub,
				"rcpt",
				rspamd_rcl_parse_struct_mime_addr,
				G_STRUCT_OFFSET (struct rspamd_task, rcpt_envelope),
				0);
		rspamd_rcl_add_default_handler (sub,
				"helo",
				rspamd_rcl_parse_struct_string,
				G_STRUCT_OFFSET (struct rspamd_task, helo),
				0);
		rspamd_rcl_add_default_handler (sub,
				"user",
				rspamd_rcl_parse_struct_string,
				G_STRUCT_OFFSET (struct rspamd_task, user),
				0);
		rspamd_rcl_add_default_handler (sub,
				"pass_all",
				rspamd_protocol_parse_task_flags,
				G_STRUCT_OFFSET (struct rspamd_task, flags),
				0);
		rspamd_rcl_add_default_handler (sub,
				"json",
				rspamd_protocol_parse_task_flags,
				G_STRUCT_OFFSET (struct rspamd_task, flags),
				0);
	}
}

gboolean
rspamd_protocol_handle_control (struct rspamd_task *task,
		const ucl_object_t *control)
{
	GError *err = NULL;

	rspamd_protocol_control_parser_init ();

	if (!rspamd_rcl_parse (control_parser, task, task->task_pool,
			control, &err)) {
		msg_warn ("cannot parse control block: %e", err);
		g_error_free (err);

		return FALSE;
	}

	return TRUE;
}

gboolean
rspamd_protocol_handle_request (struct rspamd_task *task,
	struct rspamd_http_message *msg)
{
	gboolean ret = TRUE;

	if (msg->method == HTTP_SYMBOLS) {
		task->cmd = CMD_SYMBOLS;
		task->flags &= ~RSPAMD_TASK_FLAG_JSON;
	}
	else if (msg->method == HTTP_CHECK) {
		task->cmd = CMD_CHECK;
		task->flags &= ~RSPAMD_TASK_FLAG_JSON;
	}
	else {
		task->flags |= RSPAMD_TASK_FLAG_JSON;
		ret = rspamd_protocol_handle_url (task, msg);
	}

	if (msg->flags & RSPAMD_HTTP_FLAG_SPAMC) {
		task->flags &= ~RSPAMD_TASK_FLAG_JSON;
		task->flags |= RSPAMD_TASK_FLAG_SPAMC;
	}

	return ret;
}

static void
write_hashes_to_log (struct rspamd_task *task, GString *logbuf)
{
	GList *cur;
	struct mime_text_part *text_part;

	cur = task->text_parts;

	while (cur) {
		text_part = cur->data;
		if (text_part->fuzzy) {
			if (cur->next != NULL) {
				rspamd_printf_gstring (logbuf,
					" part: %Xd,",
					text_part->fuzzy->h);
			}
			else {
				rspamd_printf_gstring (logbuf, " part: %Xd",
					text_part->fuzzy->h);
			}
		}
		cur = g_list_next (cur);
	}
}


/* Structure for writing tree data */
struct tree_cb_data {
	ucl_object_t *top;
	struct rspamd_task *task;
};

/*
 * Callback for writing urls
 */
static void
urls_protocol_cb (gpointer key, gpointer value, gpointer ud)
{
	struct tree_cb_data *cb = ud;
	struct rspamd_url *url = value;
	ucl_object_t *obj, *elt;

	if (!(cb->task->flags & RSPAMD_TASK_FLAG_EXT_URLS)) {
		obj = ucl_object_fromlstring (url->host, url->hostlen);
	}
	else {
		obj = ucl_object_typed_new (UCL_OBJECT);

		elt = ucl_object_fromstring (url->string);
		ucl_object_insert_key (obj, elt, "url", 0, false);

		if (url->hostlen > 0) {
			elt = ucl_object_fromlstring (url->host, url->hostlen);
			ucl_object_insert_key (obj, elt, "host", 0, false);
		}

		if (url->surbllen > 0) {
			elt = ucl_object_fromlstring (url->surbl, url->surbllen);
			ucl_object_insert_key (obj, elt, "surbl", 0, false);
		}

		elt = ucl_object_frombool (url->is_phished);
		ucl_object_insert_key (obj, elt, "phished", 0, false);
	}
	ucl_array_append (cb->top, obj);

	if (cb->task->cfg->log_urls) {
		msg_info ("<%s> URL: %s - %s: %s",
			cb->task->message_id,
			cb->task->user ?
			cb->task->user : "unknown",
			rspamd_inet_address_to_string (cb->task->from_addr),
			struri (url));
	}
}

static ucl_object_t *
rspamd_urls_tree_ucl (GHashTable *input, struct rspamd_task *task)
{
	struct tree_cb_data cb;
	ucl_object_t *obj;

	obj = ucl_object_typed_new (UCL_ARRAY);
	cb.top = obj;
	cb.task = task;

	g_hash_table_foreach (input, urls_protocol_cb, &cb);

	return obj;
}

static void
emails_protocol_cb (gpointer key, gpointer value, gpointer ud)
{
	struct tree_cb_data *cb = ud;
	struct rspamd_url *url = value;
	ucl_object_t *obj;

	obj = ucl_object_fromlstring (url->user, url->userlen + url->hostlen + 1);
	ucl_array_append (cb->top, obj);
}

static ucl_object_t *
rspamd_emails_tree_ucl (GHashTable *input, struct rspamd_task *task)
{
	struct tree_cb_data cb;
	ucl_object_t *obj;

	obj = ucl_object_typed_new (UCL_ARRAY);
	cb.top = obj;
	cb.task = task;

	g_hash_table_foreach (input, emails_protocol_cb, &cb);

	return obj;
}


/* Write new subject */
static const gchar *
make_rewritten_subject (struct metric *metric, struct rspamd_task *task)
{
	static gchar subj_buf[1024];
	gchar *p = subj_buf, *end, *res;
	const gchar *s, *c;

	end = p + sizeof(subj_buf);
	c = metric->subject;
	if (c == NULL) {
		c = SPAM_SUBJECT;
	}

	s = g_mime_message_get_subject (task->message);

	while (p < end) {
		if (*c == '\0') {
			*p = '\0';
			break;
		}
		else if (*c == '%' && *(c + 1) == 's') {
			p += rspamd_strlcpy (p, (s != NULL) ? s : "", end - p);
			c += 2;
		}
		else {
			*p = *c++;
		}
		p++;
	}
	res = g_mime_utils_header_encode_text (subj_buf);

	rspamd_mempool_add_destructor (task->task_pool,
		(rspamd_mempool_destruct_t)g_free,
		res);

	return res;
}

static ucl_object_t *
rspamd_str_list_ucl (GList *str_list)
{
	ucl_object_t *top = NULL, *obj;
	GList *cur;

	top = ucl_object_typed_new (UCL_ARRAY);
	cur = str_list;
	while (cur) {
		obj = ucl_object_fromstring (cur->data);
		ucl_array_append (top, obj);
		cur = g_list_next (cur);
	}

	return top;
}

static ucl_object_t *
rspamd_metric_symbol_ucl (struct rspamd_task *task, struct metric *m,
	struct symbol *sym, GString *logbuf)
{
	ucl_object_t *obj = NULL;
	const gchar *description = NULL;

	if (logbuf != NULL) {
		rspamd_printf_gstring (logbuf, "%s,", sym->name);
	}

	description = g_hash_table_lookup (m->descriptions, sym->name);

	obj = ucl_object_typed_new (UCL_OBJECT);
	ucl_object_insert_key (obj, ucl_object_fromstring (
			sym->name),	 "name",  0, false);
	ucl_object_insert_key (obj, ucl_object_fromdouble (
			sym->score), "score", 0, false);
	if (description) {
		ucl_object_insert_key (obj, ucl_object_fromstring (
				description), "description", 0, false);
	}
	if (sym->options != NULL) {
		ucl_object_insert_key (obj, rspamd_str_list_ucl (
				sym->options), "options", 0, false);
	}

	return obj;
}

static ucl_object_t *
rspamd_metric_result_ucl (struct rspamd_task *task,
	struct metric_result *mres,
	GString *logbuf)
{
	GHashTableIter hiter;
	struct symbol *sym;
	struct metric *m;
	gboolean is_spam;
	enum rspamd_metric_action action = METRIC_ACTION_NOACTION;
	ucl_object_t *obj = NULL, *sobj;;
	gpointer h, v;
	double required_score;
	const gchar *subject;
	gchar action_char;

	m = mres->metric;

	/* XXX: handle settings */
	if (mres->action == METRIC_ACTION_MAX) {
		mres->action = rspamd_check_action_metric (task, mres->score,
				&required_score, m);
	}
	else {
		required_score = mres->metric->actions[mres->action].score;
	}
	action = mres->action;
	is_spam = (action == METRIC_ACTION_REJECT);

	if (RSPAMD_TASK_IS_SKIPPED (task)) {
		action_char = 'S';
	}
	else if (is_spam) {
		action_char = 'T';
	}
	else {
		action_char = 'F';
	}

	if (logbuf != NULL) {
		rspamd_printf_gstring (logbuf, "(%s: %c (%s): [%.2f/%.2f] [",
				m->name, action_char,
				rspamd_action_to_str (action),
				mres->score, required_score);
	}

	obj = ucl_object_typed_new (UCL_OBJECT);
	ucl_object_insert_key (obj,	  ucl_object_frombool (is_spam),
		"is_spam", 0, false);
	ucl_object_insert_key (obj,	  ucl_object_frombool (RSPAMD_TASK_IS_SKIPPED (task)),
		"is_skipped", 0, false);
	ucl_object_insert_key (obj, ucl_object_fromdouble (mres->score),
		"score", 0, false);
	ucl_object_insert_key (obj, ucl_object_fromdouble (required_score),
		"required_score", 0, false);
	ucl_object_insert_key (obj,
		ucl_object_fromstring (rspamd_action_to_str (action)),
		"action", 0, false);

	if (action == METRIC_ACTION_REWRITE_SUBJECT) {
		subject = make_rewritten_subject (m, task);
		ucl_object_insert_key (obj, ucl_object_fromstring (subject),
			"subject", 0, false);
	}
	/* Now handle symbols */
	g_hash_table_iter_init (&hiter, mres->symbols);
	while (g_hash_table_iter_next (&hiter, &h, &v)) {
		sym = (struct symbol *)v;
		sobj = rspamd_metric_symbol_ucl (task, m, sym, logbuf);
		ucl_object_insert_key (obj, sobj, h, 0, false);
	}

	if (logbuf != NULL) {
		/* Cut the trailing comma if needed */
		if (logbuf->str[logbuf->len - 1] == ',') {
			logbuf->len--;
		}

		rspamd_printf_gstring (logbuf, "]), len: %z, time: %s, dns req: %d,",
				task->msg.len,
				calculate_check_time (task->time_real,
						task->time_virtual,
						task->cfg->clock_res,
						&task->scan_milliseconds),
				task->dns_requests);
	}

	return obj;
}

static void
rspamd_ucl_torspamc_output (struct rspamd_task *task,
	ucl_object_t *top,
	GString *out)
{
	const ucl_object_t *metric, *score,
	*required_score, *is_spam, *elt, *cur;
	ucl_object_iter_t iter = NULL;

	metric = ucl_object_find_key (top, DEFAULT_METRIC);
	if (metric != NULL) {
		score = ucl_object_find_key (metric, "score");
		required_score = ucl_object_find_key (metric, "required_score");
		is_spam = ucl_object_find_key (metric, "is_spam");
		g_string_append_printf (out,
			"Metric: default; %s; %.2f / %.2f / 0.0\r\n",
			ucl_object_toboolean (is_spam) ? "True" : "False",
			ucl_object_todouble (score),
			ucl_object_todouble (required_score));
		elt = ucl_object_find_key (metric, "action");
		if (elt != NULL) {
			g_string_append_printf (out, "Action: %s\r\n",
				ucl_object_tostring (elt));
		}

		iter = NULL;
		while ((elt = ucl_iterate_object (metric, &iter, true)) != NULL) {
			if (elt->type == UCL_OBJECT) {
				const ucl_object_t *sym_score;
				sym_score = ucl_object_find_key (elt, "score");
				g_string_append_printf (out, "Symbol: %s(%.2f)\r\n",
					ucl_object_key (elt),
					ucl_object_todouble (sym_score));
			}
		}

		elt = ucl_object_find_key (metric, "subject");
		if (elt != NULL) {
			g_string_append_printf (out, "Subject: %s\r\n",
				ucl_object_tostring (elt));
		}
	}

	elt = ucl_object_find_key (top, "messages");
	if (elt != NULL) {
		iter = NULL;
		while ((cur = ucl_iterate_object (elt, &iter, true)) != NULL) {
			if (cur->type == UCL_STRING) {
				g_string_append_printf (out, "Message: %s\r\n",
						ucl_object_tostring (cur));
			}
		}
	}

	g_string_append_printf (out, "Message-ID: %s\r\n", task->message_id);
}

static void
rspamd_ucl_tospamc_output (struct rspamd_task *task,
	ucl_object_t *top,
	GString *out)
{
	const ucl_object_t *metric, *score,
		*required_score, *is_spam, *elt;
	ucl_object_iter_t iter = NULL;

	metric = ucl_object_find_key (top, DEFAULT_METRIC);
	if (metric != NULL) {
		score = ucl_object_find_key (metric, "score");
		required_score = ucl_object_find_key (metric, "required_score");
		is_spam = ucl_object_find_key (metric, "is_spam");
		g_string_append_printf (out,
			"Spam: %s ; %.2f / %.2f\r\n\r\n",
			ucl_object_toboolean (is_spam) ? "True" : "False",
			ucl_object_todouble (score),
			ucl_object_todouble (required_score));

		while ((elt = ucl_iterate_object (metric, &iter, true)) != NULL) {
			if (elt->type == UCL_OBJECT) {
				g_string_append_printf (out, "%s,",
					ucl_object_key (elt));
			}
		}
		/* Ugly hack, but the whole spamc is ugly */
		if (out->str[out->len - 1] == ',') {
			g_string_truncate (out, out->len - 1);
			g_string_append (out, CRLF);
		}
	}
}

ucl_object_t *
rspamd_protocol_write_ucl (struct rspamd_task *task, GString *logbuf)
{
	struct metric_result *metric_res;
	ucl_object_t *top = NULL, *obj;
	GHashTableIter hiter;
	gpointer h, v;

	if (logbuf != NULL) {
		rspamd_printf_gstring (logbuf,
				"id: <%s>, qid: <%s>, ",
				task->message_id,
				task->queue_id);

		if (task->user) {
			rspamd_printf_gstring (logbuf, "user: %s, ", task->user);
		}
	}

	g_hash_table_iter_init (&hiter, task->results);
	top = ucl_object_typed_new (UCL_OBJECT);
	/* Convert results to an ucl object */
	while (g_hash_table_iter_next (&hiter, &h, &v)) {
		metric_res = (struct metric_result *)v;
		obj = rspamd_metric_result_ucl (task, metric_res, logbuf);
		ucl_object_insert_key (top, obj, h, 0, false);
	}

	if (task->messages != NULL) {
		ucl_object_insert_key (top, rspamd_str_list_ucl (
				task->messages), "messages", 0, false);
	}
	if (g_hash_table_size (task->urls) > 0) {
		ucl_object_insert_key (top, rspamd_urls_tree_ucl (task->urls,
				task), "urls", 0, false);
	}
	if (g_hash_table_size (task->emails) > 0) {
		ucl_object_insert_key (top, rspamd_emails_tree_ucl (task->emails, task),
				"emails", 0, false);
	}

	ucl_object_insert_key (top, ucl_object_fromstring (task->message_id),
			"message-id", 0, false);

	if (logbuf != NULL) {
		write_hashes_to_log (task, logbuf);
	}

	return top;
}

void
rspamd_protocol_http_reply (struct rspamd_http_message *msg,
	struct rspamd_task *task)
{
	GString *logbuf;
	struct metric_result *metric_res;
	GHashTableIter hiter;
	gpointer h, v;
	ucl_object_t *top = NULL;
	gdouble required_score;
	gint action;

	/* Output the first line - check status */
	logbuf = g_string_sized_new (BUFSIZ);

	/* Write custom headers */
	g_hash_table_iter_init (&hiter, task->reply_headers);
	while (g_hash_table_iter_next (&hiter, &h, &v)) {
		GString *hn = (GString *)h, *hv = (GString *)v;

		rspamd_http_message_add_header (msg, hn->str, hv->str);
	}

	top = rspamd_protocol_write_ucl (task, logbuf);

	if (!(task->flags & RSPAMD_TASK_FLAG_NO_LOG)) {
		rspamd_roll_history_update (task->worker->srv->history, task);
	}

	if (!(task->flags & RSPAMD_TASK_FLAG_NO_LOG)) {
		msg_info ("%v", logbuf);
	}
	g_string_free (logbuf, TRUE);

	msg->body = g_string_sized_new (BUFSIZ);

	if (msg->method < HTTP_SYMBOLS && !RSPAMD_TASK_IS_SPAMC (task)) {
		rspamd_ucl_emit_gstring (top, UCL_EMIT_JSON_COMPACT, msg->body);
	}
	else {
		if (RSPAMD_TASK_IS_SPAMC (task)) {
			rspamd_ucl_tospamc_output (task, top, msg->body);
		}
		else {
			rspamd_ucl_torspamc_output (task, top, msg->body);
		}
	}
	ucl_object_unref (top);

	/* Update stat for default metric */
	metric_res = g_hash_table_lookup (task->results, DEFAULT_METRIC);
	if (metric_res != NULL) {
		action = rspamd_check_action_metric (task, metric_res->score, &required_score,
				metric_res->metric);
		if (action <= METRIC_ACTION_NOACTION) {
			task->worker->srv->stat->actions_stat[action]++;
		}
	}

	/* Increase counters */
	task->worker->srv->stat->messages_scanned++;
}

void
rspamd_protocol_write_reply (struct rspamd_task *task)
{
	struct rspamd_http_message *msg;
	const gchar *ctype = "application/json";

	msg = rspamd_http_new_message (HTTP_RESPONSE);

	if (rspamd_http_connection_is_encrypted (task->http_conn)) {
		msg_info ("<%s> writing encrypted reply", task->message_id);
	}

	if (!RSPAMD_TASK_IS_JSON (task)) {
		/* Turn compatibility on */
		msg->method = HTTP_SYMBOLS;
	}
	if (RSPAMD_TASK_IS_SPAMC (task)) {
		msg->flags |= RSPAMD_HTTP_FLAG_SPAMC;
	}

	msg->date = time (NULL);

	task->state = WRITING_REPLY;

	debug_task ("writing reply to client");
	if (task->error_code != 0) {
		ucl_object_t *top = NULL;

		top = ucl_object_typed_new (UCL_OBJECT);
		msg->code = 500 + task->error_code % 100;
		msg->status = g_string_new (task->last_error);
		ucl_object_insert_key (top, ucl_object_fromstring (task->last_error),
			"error", 0, false);
		msg->body = g_string_sized_new (256);
		rspamd_ucl_emit_gstring (top, UCL_EMIT_JSON_COMPACT, msg->body);
		ucl_object_unref (top);
	}
	else {
		switch (task->cmd) {
		case CMD_REPORT_IFSPAM:
		case CMD_REPORT:
		case CMD_CHECK:
		case CMD_SYMBOLS:
		case CMD_PROCESS:
		case CMD_SKIP:
			rspamd_protocol_http_reply (msg, task);
			break;
		case CMD_PING:
			msg->body = g_string_new ("pong" CRLF);
			ctype = "text/plain";
			break;
		case CMD_OTHER:
			msg_err ("BROKEN");
			break;
		}
	}

	rspamd_http_connection_reset (task->http_conn);
	rspamd_http_connection_write_message (task->http_conn, msg, NULL,
		ctype, task, task->sock, &task->tv, task->ev_base);
}

void
register_protocol_command (const gchar *name, protocol_reply_func func)
{
	struct custom_command *cmd;

	cmd = g_malloc (sizeof (struct custom_command));
	cmd->name = name;
	cmd->func = func;

	custom_commands = g_list_prepend (custom_commands, cmd);
}
