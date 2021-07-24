/*
 * Copyright (c) 2021 Omar Polo <op@omarpolo.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "compat.h"

#include <sys/types.h>
#include <sys/socket.h>

#include <netinet/in.h>

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tls.h>
#include <unistd.h>

#if HAVE_ASR_RUN
# include <asr.h>
#endif

#include "telescope.h"

static struct imsgev		*iev_ui;
static struct tls_config	*tlsconf;

struct req;

static void		 die(void) __attribute__((__noreturn__));

static void		 try_to_connect(int, short, void*);

#if HAVE_ASR_RUN
static void		 query_done(struct asr_result*, void*);
static void		 async_conn_towards(struct req*);
#else
static void 		 blocking_conn_towards(struct req*);
#endif

static void		 close_with_err(struct req*, const char*);
static void		 close_with_errf(struct req*, const char*, ...) __attribute__((format(printf, 2, 3)));
static struct req	*req_by_id(uint32_t);
static struct req	*req_by_id_try(uint32_t);

static void		 setup_tls(struct req*);
static void		 do_handshake(int, short, void*);
static void		 write_request(int, short, void*);
static void		 read_reply(int, short, void*);
static void		 parse_reply(struct req*);
static void		 copy_body(int, short, void*);

static void		 handle_get_raw(struct imsg *, size_t);
static void		 handle_cert_status(struct imsg*, size_t);
static void		 handle_proceed(struct imsg*, size_t);
static void		 handle_stop(struct imsg*, size_t);
static void		 handle_quit(struct imsg*, size_t);
static void		 handle_dispatch_imsg(int, short, void*);

static int		 net_send_ui(int, uint32_t, const void *, uint16_t);

/* TODO: making this customizable */
struct timeval timeout_for_handshake = { 5, 0 };

static imsg_handlerfn *handlers[] = {
	[IMSG_GET_RAW]		= handle_get_raw,
	[IMSG_CERT_STATUS]	= handle_cert_status,
	[IMSG_PROCEED]		= handle_proceed,
	[IMSG_STOP]		= handle_stop,
	[IMSG_QUIT]		= handle_quit,
};

typedef void (*statefn)(int, short, void*);

TAILQ_HEAD(, req) reqhead;
/* a pending request */
struct req {
	struct event		 ev;
	struct phos_uri		 url;
	uint32_t		 id;
	int			 fd;
	struct tls		*ctx;
	char			 buf[1024];
	size_t			 off;

	struct addrinfo		*servinfo, *p;
#if HAVE_ASR_RUN
	struct addrinfo		 hints;
	struct event_asr	*asrev;
#endif

	TAILQ_ENTRY(req)	 reqs;
};

static inline void
yield_r(struct req *req, statefn fn, struct timeval *tv)
{
	event_once(req->fd, EV_READ, fn, req, tv);
}

static inline void
yield_w(struct req *req, statefn fn, struct timeval *tv)
{
	event_once(req->fd, EV_WRITE, fn, req, tv);
}

static inline void
advance_buf(struct req *req, size_t len)
{
	assert(len <= req->off);

	req->off -= len;
	memmove(req->buf, req->buf + len, req->off);
}

static void __attribute__((__noreturn__))
die(void)
{
	abort(); 		/* TODO */
}

static void
try_to_connect(int fd, short ev, void *d)
{
	struct req	*req = d;
	int		 error = 0;
	socklen_t	 len = sizeof(error);

again:
	if (req->p == NULL)
		goto err;

	if (req->fd != -1) {
		if (getsockopt(req->fd, SOL_SOCKET, SO_ERROR, &error, &len) == -1)
			goto err;
		if (error != 0) {
                        errno = error;
                        goto err;
		}
		goto done;
	}

	req->fd = socket(req->p->ai_family, req->p->ai_socktype, req->p->ai_protocol);
	if (req->fd == -1) {
		req->p = req->p->ai_next;
		goto again;
	} else {
		mark_nonblock(req->fd);
		if (connect(req->fd, req->p->ai_addr, req->p->ai_addrlen) == 0)
			goto done;
		yield_w(req, try_to_connect, NULL);
	}
	return;

err:
	freeaddrinfo(req->servinfo);
	close_with_errf(req, "failed to connect to %s",
	    req->url.host);
	return;

done:
	freeaddrinfo(req->servinfo);
	setup_tls(req);
}

#if HAVE_ASR_RUN
static void
query_done(struct asr_result *res, void *d)
{
	struct req	*req = d;

	req->asrev = NULL;
	if (res->ar_gai_errno != 0) {
		close_with_errf(req, "failed to resolve %s: %s",
		    req->url.host, gai_strerror(res->ar_gai_errno));
		return;
	}

	req->fd = -1;
	req->servinfo = res->ar_addrinfo;
	req->p = res->ar_addrinfo;
	try_to_connect(0, 0, req);
}

static void
async_conn_towards(struct req *req)
{
	struct asr_query	*q;
	const char		*proto = "1965";

	if (*req->url.port != '\0')
		proto = req->url.port;

	req->hints.ai_family = AF_UNSPEC;
	req->hints.ai_socktype = SOCK_STREAM;
	q = getaddrinfo_async(req->url.host, proto, &req->hints, NULL);
	req->asrev = event_asr_run(q, query_done, req);
}
#else
static void
blocking_conn_towards(struct req *req)
{
	struct addrinfo	 hints;
	struct phos_uri	*url = &req->url;
	int		 status;
	const char	*proto = "1965";

	if (*url->port != '\0')
		proto = url->port;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	if ((status = getaddrinfo(url->host, proto, &hints, &req->servinfo))) {
		close_with_errf(req, "failed to resolve %s: %s",
		    url->host, gai_strerror(status));
		return;
	}

	req->fd = -1;
	req->p = req->servinfo;
	try_to_connect(0, 0, req);
}
#endif

static struct req *
req_by_id(uint32_t id)
{
	struct req *r;

	if ((r = req_by_id_try(id)) == NULL)
		die();
	return r;
}

static struct req *
req_by_id_try(uint32_t id)
{
	struct req *r;

	TAILQ_FOREACH(r, &reqhead, reqs) {
		if (r->id == id)
			return r;
	}

	return NULL;
}

static void
close_conn(int fd, short ev, void *d)
{
	struct req	*req = d;

#if HAVE_ASR_RUN
	if (req->asrev != NULL)
		event_asr_abort(req->asrev);
#endif

	if (req->ctx != NULL) {
		switch (tls_close(req->ctx)) {
		case TLS_WANT_POLLIN:
			yield_r(req, close_conn, NULL);
			return;
		case TLS_WANT_POLLOUT:
			yield_w(req, close_conn, NULL);
			return;
		}

		tls_free(req->ctx);
	}

	TAILQ_REMOVE(&reqhead, req, reqs);
	if (req->fd != -1)
		close(req->fd);
	free(req);
}

static void
close_with_err(struct req *req, const char *err)
{
	net_send_ui(IMSG_ERR, req->id, err, strlen(err)+1);
	close_conn(0, 0, req);
}

static void
close_with_errf(struct req *req, const char *fmt, ...)
{
	va_list	 ap;
	char	*s;

	va_start(ap, fmt);
	if (vasprintf(&s, fmt, ap) == -1)
		abort();
	va_end(ap);

	close_with_err(req, s);
	free(s);
}

static void
setup_tls(struct req *req)
{
	if ((req->ctx = tls_client()) == NULL) {
		close_with_errf(req, "tls_client: %s", strerror(errno));
		return;
	}
	if (tls_configure(req->ctx, tlsconf) == -1) {
		close_with_errf(req, "tls_configure: %s", tls_error(req->ctx));
		return;
	}
	if (tls_connect_socket(req->ctx, req->fd, req->url.host) == -1) {
		close_with_errf(req, "tls_connect_socket: %s", tls_error(req->ctx));
		return;
	}
	yield_w(req, do_handshake, &timeout_for_handshake);
}

static void
do_handshake(int fd, short ev, void *d)
{
	struct req	*req = d;
	const char	*hash;

	if (ev == EV_TIMEOUT) {
		close_with_err(req, "Timeout loading page");
		return;
	}

	switch (tls_handshake(req->ctx)) {
	case TLS_WANT_POLLIN:
		yield_r(req, do_handshake, NULL);
		return;
	case TLS_WANT_POLLOUT:
		yield_w(req, do_handshake, NULL);
		return;
	}

	hash = tls_peer_cert_hash(req->ctx);
	if (hash == NULL) {
		close_with_errf(req, "handshake failed: %s", tls_error(req->ctx));
		return;
	}
	net_send_ui(IMSG_CHECK_CERT, req->id, hash, strlen(hash)+1);
}

static void
write_request(int fd, short ev, void *d)
{
	struct req	*req = d;
	ssize_t		 r;
	size_t		 len;

	len = strlen(req->buf);

	switch (r = tls_write(req->ctx, req->buf, len)) {
	case -1:
		close_with_errf(req, "tls_write: %s", tls_error(req->ctx));
		break;
	case TLS_WANT_POLLIN:
		yield_r(req, write_request, NULL);
		break;
	case TLS_WANT_POLLOUT:
		yield_w(req, write_request, NULL);
		break;
	default:
		/* assume r == len */
		(void)r;
		yield_r(req, read_reply, NULL);
		break;
	}
}

static void
read_reply(int fd, short ev, void *d)
{
	struct req	*req = d;
	size_t		 len;
	ssize_t		 r;
	char		*buf;

	buf = req->buf + req->off;
	len = sizeof(req->buf) - req->off;

	switch (r = tls_read(req->ctx, buf, len)) {
	case -1:
		close_with_errf(req, "tls_read: %s", tls_error(req->ctx));
		break;
	case TLS_WANT_POLLIN:
		yield_r(req, read_reply, NULL);
		break;
	case TLS_WANT_POLLOUT:
		yield_w(req, read_reply, NULL);
		break;
	default:
		req->off += r;

		if (memmem(req->buf, req->off, "\r\n", 2) != NULL)
			parse_reply(req);
		else if (req->off == sizeof(req->buf))
			close_with_err(req, "invalid response");
		else
			yield_r(req, read_reply, NULL);
		break;
	}
}

static void
parse_reply(struct req *req)
{
	int	 code;
	char	*e;
	size_t	 len;

	if (req->off < 4)
		goto err;

	if (!isdigit(req->buf[0]) || !isdigit(req->buf[1]))
		goto err;

	code = (req->buf[0] - '0')*10 + (req->buf[1] - '0');

	if (!isspace(req->buf[2]))
		goto err;

	advance_buf(req, 3);
	if ((e = memmem(req->buf, req->off, "\r\n", 2)) == NULL)
		goto err;

	*e = '\0';
	e++;
	len = e - req->buf;
	net_send_ui(IMSG_GOT_CODE, req->id, &code, sizeof(code));
	net_send_ui(IMSG_GOT_META, req->id, req->buf, len);

	if (20 <= code && code < 30)
		advance_buf(req, len+1); /* skip \n too */
	else
		close_conn(0, 0, req);

	return;

err:
	close_with_err(req, "malformed request");
}

static void
copy_body(int fd, short ev, void *d)
{
	struct req	*req = d;
	ssize_t		 r;

	for (;;) {
		if (req->off != 0) {
			net_send_ui(IMSG_BUF, req->id,
			    req->buf, req->off);
			req->off = 0;
		}

		switch (r = tls_read(req->ctx, req->buf, sizeof(req->buf))) {
		case TLS_WANT_POLLIN:
			yield_r(req, copy_body, NULL);
			return;
		case TLS_WANT_POLLOUT:
			yield_w(req, copy_body, NULL);
			return;
		case -1:
			/*
			 * XXX: should notify the user that the
			 * download was aborted.
			 */
			/* fallthrough */
		case 0:
			net_send_ui(IMSG_EOF, req->id, NULL, 0);
			close_conn(0, 0, req);
			return;
		default:
			req->off = r;
		}
	}
}

static void
handle_get_raw(struct imsg *imsg, size_t datalen)
{
	struct req	*req;
	struct get_req	*r;

	r = imsg->data;

	if (datalen != sizeof(*r))
		die();

	if ((req = calloc(1, sizeof(*req))) == NULL)
		die();

	req->id = imsg->hdr.peerid;
	TAILQ_INSERT_HEAD(&reqhead, req, reqs);

	strlcpy(req->url.host, r->host, sizeof(req->url.host));
	strlcpy(req->url.port, r->port, sizeof(req->url.port));
	strlcpy(req->buf, r->req, sizeof(req->buf));

#if HAVE_ASR_RUN
        async_conn_towards(req);
#else
	blocking_conn_towards(req);
#endif
}

static void
handle_cert_status(struct imsg *imsg, size_t datalen)
{
	struct req	*req;
	int		 is_ok;

	req = req_by_id(imsg->hdr.peerid);

	if (datalen < sizeof(is_ok))
		die();
	memcpy(&is_ok, imsg->data, sizeof(is_ok));

	if (is_ok)
		yield_w(req, write_request, NULL);
	else
		close_conn(0, 0, req);
}

static void
handle_proceed(struct imsg *imsg, size_t datalen)
{
	yield_r(req_by_id(imsg->hdr.peerid),
	    copy_body, NULL);
}

static void
handle_stop(struct imsg *imsg, size_t datalen)
{
	struct req	*req;

	if ((req = req_by_id_try(imsg->hdr.peerid)) == NULL)
		return;
	close_conn(0, 0, req);
}

static void
handle_quit(struct imsg *imsg, size_t datalen)
{
	event_loopbreak();
}

static void
handle_dispatch_imsg(int fd, short ev, void *d)
{
	struct imsgev	*iev = d;

	if (dispatch_imsg(iev, ev, handlers, sizeof(handlers)) == -1)
		err(1, "connection closed");
}

static int
net_send_ui(int type, uint32_t peerid, const void *data,
    uint16_t datalen)
{
	return imsg_compose_event(iev_ui, type, peerid, 0, -1,
	    data, datalen);
}

int
net_main(void)
{
	setproctitle("net");

	TAILQ_INIT(&reqhead);

	if ((tlsconf = tls_config_new()) == NULL)
		die();
	tls_config_insecure_noverifycert(tlsconf);
	tls_config_insecure_noverifyname(tlsconf);

	event_init();

	/* Setup pipe and event handler to the main process */
	if ((iev_ui = malloc(sizeof(*iev_ui))) == NULL)
		die();
	imsg_init(&iev_ui->ibuf, 3);
	iev_ui->handler = handle_dispatch_imsg;
	iev_ui->events = EV_READ;
	event_set(&iev_ui->ev, iev_ui->ibuf.fd, iev_ui->events,
	    iev_ui->handler, iev_ui);
	event_add(&iev_ui->ev, NULL);

	sandbox_net_process();

	event_dispatch();
	return 0;
}