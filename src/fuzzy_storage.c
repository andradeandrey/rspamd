/*
 * Copyright (c) 2009, Rambler media
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
 * THIS SOFTWARE IS PROVIDED BY Rambler media ''AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL Rambler BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Rspamd fuzzy storage server
 */

#include "config.h"
#include "util.h"
#include "main.h"
#include "protocol.h"
#include "upstream.h"
#include "cfg_file.h"
#include "url.h"
#include "modules.h"
#include "message.h"
#include "fuzzy.h"
#include "bloom.h"
#include "fuzzy_storage.h"

/* This number is used as limit while comparing two fuzzy hashes, this value can vary from 0 to 100 */
#define LEV_LIMIT 99
/* This number is used as limit while we are making decision to write new hash file or not */
#define MOD_LIMIT 10000
/* This number is used as expire time in seconds for cache items  (2 days) */
#define DEFAULT_EXPIRE 172800L
/* Resync value in seconds */
#define SYNC_TIMEOUT 60
/* Number of hash buckets */
#define BUCKETS 1024

static GQueue *hashes[BUCKETS];
static bloom_filter_t *bf;

/* Number of cache modifications */
static uint32_t mods = 0;
/* For evtimer */
static struct timeval tmv;
static struct event tev;

struct rspamd_fuzzy_node {
	fuzzy_hash_t h;
	uint64_t time;
};

static void 
sig_handler (int signo)
{	
	switch (signo) {
		case SIGINT:
			/* Ignore SIGINT as we should got SIGTERM after it anyway */
			return;
		case SIGTERM:
#ifdef WITH_PROFILER
			exit (0);
#else
			_exit (1);
#endif
			break;
	}
}

static void
sync_cache (struct rspamd_worker *wrk)
{
	int fd, i;
	char *filename, *exp_str;
	GList *cur, *tmp;
	struct rspamd_fuzzy_node *node;
	uint64_t expire, now;
	
	/* Check for modifications */
	if (mods < MOD_LIMIT) {
		return;
	}
	
	msg_info ("sync_cache: syncing fuzzy hash storage");
	filename = g_hash_table_lookup (wrk->cf->params, "hashfile");
	if (filename == NULL) {
		return;
	}
	exp_str = g_hash_table_lookup (wrk->cf->params, "expire");
	if (exp_str != NULL) {
		expire = parse_seconds (exp_str) / 1000;
	}
	else {
		expire = DEFAULT_EXPIRE;
	}

	if ((fd = open (filename, O_WRONLY | O_TRUNC | O_CREAT, S_IWUSR | S_IRUSR | S_IRGRP | S_IROTH)) == -1) {
		msg_err ("sync_cache: cannot create hash file %s: %s", filename, strerror (errno));
		return;
	}
	
	now = (uint64_t)time (NULL);
	for (i = 0; i < BUCKETS; i ++) {
		cur = hashes[i]->head;
		while (cur) {
			node = cur->data;
			if (now - node->time > expire) {
				/* Remove expired item */
				tmp = cur;
				cur = g_list_next (cur);
				g_queue_delete_link (hashes[i], tmp);
				bloom_del (bf, node->h.hash_pipe);
				g_free (node);
				continue;
			}
			if (write (fd, node, sizeof (struct rspamd_fuzzy_node)) == -1) {
				msg_err ("sync_cache: cannot write file %s: %s", filename, strerror (errno));
			}
			cur = g_list_next (cur);
		}
	}

	close (fd);
}

static void 
sigterm_handler (int fd, short what, void *arg)
{
	struct rspamd_worker *worker = (struct rspamd_worker *)arg;
	static struct timeval tv = {
		.tv_sec = 0,
		.tv_usec = 0,
	};
	
	mods = MOD_LIMIT + 1;
	sync_cache (worker);
	(void)event_loopexit (&tv);
}

/*
 * Config reload is designed by sending sigusr to active workers and pending shutdown of them
 */
static void
sigusr_handler (int fd, short what, void *arg)
{
	struct rspamd_worker *worker = (struct rspamd_worker *)arg;
	/* Do not accept new connections, preparing to end worker's process */
	struct timeval tv;
	tv.tv_sec = SOFT_SHUTDOWN_TIME;
	tv.tv_usec = 0;
	event_del (&worker->sig_ev);
	event_del (&worker->bind_ev);
	do_reopen_log = 1;
	msg_info ("worker's shutdown is pending in %d sec", SOFT_SHUTDOWN_TIME);
	event_loopexit (&tv);
	return;
}

static gboolean
read_hashes_file (struct rspamd_worker *wrk)
{
	int r, fd, i;
	struct stat st;
	char *filename;
	struct rspamd_fuzzy_node *node;

	for (i = 0; i < BUCKETS; i ++) {
		hashes[i] = g_queue_new ();
	}

	filename = g_hash_table_lookup (wrk->cf->params, "hashfile");
	if (filename == NULL) {
		return FALSE;
	}

	if ((fd = open (filename, O_RDONLY)) == -1) {
		msg_err ("read_hashes_file: cannot open hash file %s: %s", filename, strerror (errno));
		return FALSE;
	}
	
	fstat (fd, &st);
	
	for (;;) {
		node = g_malloc (sizeof (struct rspamd_fuzzy_node));
		r = read (fd, node, sizeof (struct rspamd_fuzzy_node));
		if (r != sizeof (struct rspamd_fuzzy_node)) {
			break;	
		}
		g_queue_push_head (hashes[node->h.block_size % BUCKETS], node);
		bloom_add (bf, node->h.hash_pipe);
	}

	if (r > 0) {
		msg_warn ("read_hashes_file: ignore garbadge at the end of file, length of garbadge: %d", r);
	}
	else if (r == -1) {
		msg_err ("read_hashes_file: cannot open read file %s: %s", filename, strerror (errno));
		return FALSE;
	}

	return TRUE;
}

static void
free_session (struct fuzzy_session *session)
{
	/* Delete IO event */
	event_del (&session->ev);
	/* Close socket */
	close (session->fd);
	g_free (session);
}

static gboolean
process_check_command (struct fuzzy_cmd *cmd)
{
	GList *cur;
	struct rspamd_fuzzy_node *h;
	fuzzy_hash_t s;
	int prob = 0;
	
	if (!bloom_check (bf, cmd->hash)) {
		return FALSE;	
	}

	memcpy (s.hash_pipe, cmd->hash, sizeof (s.hash_pipe));
	s.block_size = cmd->blocksize;
	cur = hashes[cmd->blocksize % BUCKETS]->head;

	/* XXX: too slow way */
	while (cur) {
		h = cur->data;
		if ((prob = fuzzy_compare_hashes (&h->h, &s)) > LEV_LIMIT) {
			msg_info ("process_check_command: fuzzy hash was found, probability %d%%", prob);
			return TRUE;
		}
		cur = g_list_next (cur);
	}
	msg_debug ("process_check_command: fuzzy hash was NOT found, prob is %d%%", prob);

	return FALSE;
}

static gboolean
process_write_command (struct fuzzy_cmd *cmd)
{
	struct rspamd_fuzzy_node *h;

	if (bloom_check (bf, cmd->hash)) {
		return FALSE;	
	}

	h = g_malloc (sizeof (struct rspamd_fuzzy_node));
	memcpy (&h->h.hash_pipe, &cmd->hash, sizeof (cmd->hash));
	h->h.block_size = cmd->blocksize;
	h->time = (uint64_t)time (NULL);
	g_queue_push_head (hashes[cmd->blocksize % BUCKETS], h);
	bloom_add (bf, cmd->hash);
	mods ++;
	msg_info ("process_write_command: fuzzy hash was successfully added");
	
	return TRUE;
}

static gboolean
process_delete_command (struct fuzzy_cmd *cmd)
{
	GList *cur, *tmp;
	struct rspamd_fuzzy_node *h;
	fuzzy_hash_t s;
	gboolean res = FALSE;

	if (!bloom_check (bf, cmd->hash)) {
		return FALSE;	
	}
	
	memcpy (s.hash_pipe, cmd->hash, sizeof (s.hash_pipe));
	s.block_size = cmd->blocksize;
	cur = hashes[cmd->blocksize % BUCKETS]->head;

	/* XXX: too slow way */
	while (cur) {
		h = cur->data;
		if (fuzzy_compare_hashes (&h->h, &s) > LEV_LIMIT) {
			g_free (h);
			tmp = cur;
			cur = g_list_next (cur);
			g_queue_delete_link (hashes[cmd->blocksize % BUCKETS], tmp);
			bloom_del (bf, cmd->hash);
			msg_info ("process_delete_command: fuzzy hash was successfully deleted");
			res = TRUE;
			mods ++;
			continue;
		}
		cur = g_list_next (cur);
	}

	return res;
}

#define CMD_PROCESS(x)																			\
do {																							\
if (process_##x##_command (&session->cmd)) {													\
	if (write (session->fd, "OK" CRLF, sizeof ("OK" CRLF) - 1) == -1) {							\
		msg_err ("process_fuzzy_command: error while writing reply: %s", strerror (errno));		\
	}																							\
}																								\
else {																							\
	if (write (session->fd, "ERR" CRLF, sizeof ("ERR" CRLF) - 1) == -1) {						\
		msg_err ("process_fuzzy_command: error while writing reply: %s", strerror (errno));		\
	}																							\
}																								\
} while(0)

static void
process_fuzzy_command (struct fuzzy_session *session)
{
	switch (session->cmd.cmd) {
		case FUZZY_CHECK:
			CMD_PROCESS(check);
			break;
		case FUZZY_WRITE:
			CMD_PROCESS(write);
			break;	
		case FUZZY_DEL:
			CMD_PROCESS(delete);
			break;	
		default:
			if (write (session->fd, "ERR" CRLF, sizeof ("ERR" CRLF) - 1) == -1) {
				msg_err ("process_fuzzy_command: error while writing reply: %s", strerror (errno));
			}
			break;
	}
}

#undef CMD_PROCESS

/* Callback for network IO */
static void
fuzzy_io_callback (int fd, short what, void *arg)
{
	struct fuzzy_session *session = arg;
	ssize_t r;
	
	/* Got some data */
	if (what == EV_READ) {
		if ((r = read (fd, session->pos, (u_char *)&session->cmd + sizeof (struct fuzzy_cmd) - session->pos)) == -1) {
			msg_err ("fuzzy_io_callback: got error while reading from socket: %d, %s", errno, strerror (errno));
			free_session (session);
		}
		else if (session->pos + r == (u_char *)&session->cmd + sizeof (struct fuzzy_cmd)) {
			/* Assume that the whole command was read */
			process_fuzzy_command (session);
			free_session (session);
		}
		else {
			session->pos += r;
		}
	}
	else {
		free_session (session);
	}
}


/*
 * Accept new connection and construct task
 */
static void
accept_fuzzy_socket (int fd, short what, void *arg)
{
	struct rspamd_worker *worker = (struct rspamd_worker *)arg;
	struct sockaddr_storage ss;
    struct sockaddr_in *sin;
	struct fuzzy_session *session;
	socklen_t addrlen = sizeof(ss);
	int nfd;
	
	if ((nfd = accept_from_socket (fd, (struct sockaddr *)&ss, &addrlen)) == -1) {
		msg_warn ("accept_fuzzy_socket: accept failed: %s", strerror (errno));
		return;
	}
	/* Check for EAGAIN */
	if (nfd == 0) {
		msg_debug ("accept_fuzzy_socket: cannot accept socket as it was already accepted by other worker");
		return;
	}

    if (ss.ss_family == AF_UNIX) {
        msg_info ("accept_fuzzy_socket: accepted connection from unix socket");
    }
    else if (ss.ss_family == AF_INET) {
        sin = (struct sockaddr_in *) &ss;
        msg_info ("accept_fuzzy_socket: accepted connection from %s port %d", inet_ntoa (sin->sin_addr), ntohs (sin->sin_port));
    }
	
	session = g_malloc (sizeof (struct fuzzy_session));

	session->worker = worker;
	session->fd = nfd;
	session->tv.tv_sec = WORKER_IO_TIMEOUT;
	session->tv.tv_usec = 0;
	session->pos = (u_char *)&session->cmd;

	event_set (&session->ev, session->fd, EV_READ | EV_PERSIST, fuzzy_io_callback, session);
	event_add (&session->ev, &session->tv);

}

static void
sync_callback (int fd, short what, void *arg)
{
	struct rspamd_worker *worker = (struct rspamd_worker *)arg;
	/* Timer event */ 
	evtimer_set (&tev, sync_callback, worker);
	/* Plan event with jitter */
	tmv.tv_sec = SYNC_TIMEOUT + SYNC_TIMEOUT * g_random_double ();
	tmv.tv_usec = 0;
	evtimer_add (&tev, &tmv);

	sync_cache (worker);
}

/*
 * Start worker process
 */
void
start_fuzzy_storage (struct rspamd_worker *worker)
{
	struct sigaction signals;
	struct event sev;

	worker->srv->pid = getpid ();
	worker->srv->type = TYPE_FUZZY;

	event_init ();

	init_signals (&signals, sig_handler);
	sigprocmask (SIG_UNBLOCK, &signals.sa_mask, NULL);

	/* SIGUSR2 handler */
	signal_set (&worker->sig_ev, SIGUSR2, sigusr_handler, (void *) worker);
	signal_add (&worker->sig_ev, NULL);
	signal_set (&sev, SIGTERM, sigterm_handler, (void *) worker);
	signal_add (&sev, NULL);

	/* Send SIGUSR2 to parent */
	kill (getppid (), SIGUSR2);

	/* Init bloom filter */
	bf = bloom_create (20000000L, DEFAULT_BLOOM_HASHES);
	/* Try to read hashes from file */
	if (!read_hashes_file (worker)) {
		msg_err ("read_hashes_file: cannot read hashes file, it can be created after save procedure");
	}
	/* Timer event */ 
	evtimer_set (&tev, sync_callback, worker);
	/* Plan event with jitter */
	tmv.tv_sec = SYNC_TIMEOUT + SYNC_TIMEOUT * g_random_double ();
	tmv.tv_usec = 0;
	evtimer_add (&tev, &tmv);

	/* Accept event */
	event_set(&worker->bind_ev, worker->cf->listen_sock, EV_READ | EV_PERSIST, accept_fuzzy_socket, (void *)worker);
	event_add(&worker->bind_ev, NULL);

	gperf_profiler_init (worker->srv->cfg, "fuzzy");

	event_loop (0);
	exit (EXIT_SUCCESS);
}

