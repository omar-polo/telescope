/*
 * Copyright (c) 2021, 2024 Omar Polo <op@omarpolo.com>
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

#include <sys/mman.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>

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

#include <openssl/err.h>

#if HAVE_ASR_RUN
# include <asr.h>
#endif

#include "telescope.h"
#include "utils.h"

static struct imsgev		*iev_ui;

/* a pending request */
struct req {
	uint32_t		 id;
	int			 proto;
	int			 fd;
	struct tls		*ctx;
	char			*host;
	char			*port;
	char			*req;
	size_t			 len;
	void			*ccert;
	size_t			 ccert_len;
	int			 ccert_fd;
	int			 done_header;
	struct bufferevent	*bev;

	struct addrinfo		*servinfo, *p;
#if HAVE_ASR_RUN
	struct addrinfo		 hints;
	struct event_asr	*asrev;
#endif

	TAILQ_ENTRY(req)	 reqs;
};

static struct req	*req_by_id(uint32_t);

static void	 die(void) __attribute__((__noreturn__));

static void	 try_to_connect(int, short, void*);

#if HAVE_ASR_RUN
static void	 query_done(struct asr_result*, void*);
#endif
static void	 conn_towards(struct req*);

static void	 close_with_err(struct req*, const char*);
static void	 close_with_errf(struct req*, const char*, ...)
    __attribute__((format(printf, 2, 3)));

static void	 net_tls_handshake(int, short, void *);
static void	 net_tls_readcb(int, short, void *);
static void	 net_tls_writecb(int, short, void *);

static int	 gemini_parse_reply(struct req *, const char *, size_t);

static void	 net_ready(struct req *req);
static void	 net_read(struct bufferevent *, void *);
static void	 net_write(struct bufferevent *, void *);
static void	 net_error(struct bufferevent *, short, void *);

static void	 handle_dispatch_imsg(int, short, void*);

static int	 net_send_ui(int, uint32_t, const void *, uint16_t);

/* TODO: making this customizable */
struct timeval timeout_for_handshake = { 5, 0 };

typedef void (*statefn)(int, short, void*);

TAILQ_HEAD(, req) reqhead;

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

static struct req *
req_by_id(uint32_t id)
{
	struct req *r;

	TAILQ_FOREACH(r, &reqhead, reqs) {
		if (r->id == id)
			return r;
	}

	return NULL;
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
		if (getsockopt(req->fd, SOL_SOCKET, SO_ERROR, &error,
		    &len) == -1)
			goto err;
		if (error != 0) {
			errno = error;
			goto err;
		}
		goto done;
	}

	req->fd = socket(req->p->ai_family, req->p->ai_socktype,
	    req->p->ai_protocol);
	if (req->fd == -1) {
		req->p = req->p->ai_next;
		goto again;
	}

	if (!mark_nonblock_cloexec(req->fd))
		goto err;
	if (connect(req->fd, req->p->ai_addr, req->p->ai_addrlen) == 0)
		goto done;
	yield_w(req, try_to_connect, NULL);
	return;

err:
	freeaddrinfo(req->servinfo);
	close_with_errf(req, "failed to connect to %s", req->host);
	return;

done:
	freeaddrinfo(req->servinfo);

	switch (req->proto) {
	case PROTO_FINGER:
	case PROTO_GOPHER:
		/* finger and gopher don't have a header nor TLS */
		req->done_header = 1;
		net_ready(req);
		break;

	case PROTO_GEMINI: {
		struct tls_config *conf;

		if ((conf = tls_config_new()) == NULL)
			die();

		tls_config_insecure_noverifycert(conf);
		tls_config_insecure_noverifyname(conf);

		if (req->ccert && tls_config_set_keypair_mem(conf,
		    req->ccert, req->ccert_len, req->ccert, req->ccert_len)
		    == -1) {
			close_with_errf(req, "failed to load keypair: %s",
			    tls_config_error(conf));
			tls_config_free(conf);
			return;
		}

		/* prepare tls */
		if ((req->ctx = tls_client()) == NULL) {
			close_with_errf(req, "tls_client: %s",
			    strerror(errno));
			tls_config_free(conf);
			return;
		}

		if (tls_configure(req->ctx, conf) == -1) {
			close_with_errf(req, "tls_configure: %s",
			    tls_error(req->ctx));
			tls_config_free(conf);
			return;
		}
		tls_config_free(conf);

		if (tls_connect_socket(req->ctx, req->fd, req->host)
		    == -1) {
			close_with_errf(req, "tls_connect_socket: %s",
			    tls_error(req->ctx));
			return;
		}
		yield_w(req, net_tls_handshake, &timeout_for_handshake);
		break;
	}

	default:
		die();
	}
}

#if HAVE_ASR_RUN
static void
query_done(struct asr_result *res, void *d)
{
	struct req	*req = d;

	req->asrev = NULL;
	if (res->ar_gai_errno != 0) {
		close_with_errf(req, "failed to resolve %s: %s",
		    req->host, gai_strerror(res->ar_gai_errno));
		return;
	}

	req->fd = -1;
	req->servinfo = res->ar_addrinfo;
	req->p = res->ar_addrinfo;
	try_to_connect(0, 0, req);
}

static void
conn_towards(struct req *req)
{
	struct asr_query	*q;

	req->hints.ai_family = AF_UNSPEC;
	req->hints.ai_socktype = SOCK_STREAM;
	q = getaddrinfo_async(req->host, req->port, &req->hints,
	    NULL);
	req->asrev = event_asr_run(q, query_done, req);
}
#else
static void
conn_towards(struct req *req)
{
	struct addrinfo	 hints;
	int		 status;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	if ((status = getaddrinfo(req->host, req->port, &hints,
	    &req->servinfo))) {
		close_with_errf(req, "failed to resolve %s: %s",
		    req->host, gai_strerror(status));
		return;
	}

	req->fd = -1;
	req->p = req->servinfo;
	try_to_connect(0, 0, req);
}
#endif

static void
ssl_error(const char *where)
{
	unsigned long	 code;
	char		 errbuf[256];

	fprintf(stderr, "failure(s) in %s:\n", where);
	while ((code = ERR_get_error()) != 0) {
		ERR_error_string_n(code, errbuf, sizeof(errbuf));
		fprintf(stderr, "- %s\n", errbuf);
	}
}

static void
close_conn(int fd, short ev, void *d)
{
	struct req	*req = d;

#if HAVE_ASR_RUN
	if (req->asrev != NULL)
		event_asr_abort(req->asrev);
#endif

	if (req->bev != NULL) {
		bufferevent_free(req->bev);
		req->bev = NULL;
	}

	if (req->ctx != NULL) {
		switch (tls_close(req->ctx)) {
		case TLS_WANT_POLLIN:
			yield_r(req, close_conn, NULL);
			return;
		case TLS_WANT_POLLOUT:
			yield_w(req, close_conn, NULL);
			return;
		case -1:
			ssl_error("tls_close");
		}

		tls_free(req->ctx);
		req->ctx = NULL;
	}

	if (req->ccert != NULL) {
		munmap(req->ccert, req->ccert_len);
		close(req->ccert_fd);
	}

	free(req->host);
	free(req->port);
	free(req->req);

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
net_tls_handshake(int fd, short event, void *d)
{
	struct req	*req = d;
	const char	*hash;

	if (event == EV_TIMEOUT) {
		close_with_err(req, "Timeout loading page");
		return;
	}

	switch (tls_handshake(req->ctx)) {
	case TLS_WANT_POLLIN:
		yield_r(req, net_tls_handshake, NULL);
		return;
	case TLS_WANT_POLLOUT:
		yield_w(req, net_tls_handshake, NULL);
		return;
	case -1:
		ssl_error("tls_handshake");
	}

	hash = tls_peer_cert_hash(req->ctx);
	if (hash == NULL) {
		ssl_error("tls_peer_cert_hash");
		close_with_errf(req, "handshake failed: %s",
		    tls_error(req->ctx));
		return;
	}
	net_send_ui(IMSG_CHECK_CERT, req->id, hash, strlen(hash)+1);
}

static void
net_tls_readcb(int fd, short event, void *d)
{
	struct bufferevent	*bufev = d;
	struct req		*req = bufev->cbarg;
	char			 buf[IBUF_READ_SIZE];
	int			 what = EVBUFFER_READ;
	int			 howmuch = IBUF_READ_SIZE;
	int			 res;
	ssize_t			 ret;
	size_t			 len;

	if (event == EV_TIMEOUT) {
		what |= EVBUFFER_TIMEOUT;
		goto err;
	}

	if (bufev->wm_read.high != 0)
		howmuch = MIN(sizeof(buf), bufev->wm_read.high);

	switch (ret = tls_read(req->ctx, buf, howmuch)) {
	case TLS_WANT_POLLIN:
	case TLS_WANT_POLLOUT:
		goto retry;
	case -1:
		ssl_error("tls_read");
		what |= EVBUFFER_ERROR;
		goto err;
	}
	len = ret;

	if (len == 0) {
		what |= EVBUFFER_EOF;
		goto err;
	}

	res = evbuffer_add(bufev->input, buf, len);
	if (res == -1) {
		what |= EVBUFFER_ERROR;
		goto err;
	}

	event_add(&bufev->ev_read, NULL);

	len = EVBUFFER_LENGTH(bufev->input);
	if (bufev->wm_read.low != 0 && len < bufev->wm_read.low)
		return;

	if (bufev->readcb != NULL)
		(*bufev->readcb)(bufev, bufev->cbarg);
	return;

retry:
	event_add(&bufev->ev_read, NULL);
	return;

err:
	(*bufev->errorcb)(bufev, what, bufev->cbarg);
}

static void
net_tls_writecb(int fd, short event, void *d)
{
	struct bufferevent	*bufev = d;
	struct req		*req = bufev->cbarg;
	ssize_t			 ret;
	size_t			 len;
	short			 what = EVBUFFER_WRITE;

	if (event & EV_TIMEOUT) {
		what |= EVBUFFER_TIMEOUT;
		goto err;
	}

	if (EVBUFFER_LENGTH(bufev->output) != 0) {
		ret = tls_write(req->ctx, EVBUFFER_DATA(bufev->output),
		    EVBUFFER_LENGTH(bufev->output));
		switch (ret) {
		case TLS_WANT_POLLIN:
		case TLS_WANT_POLLOUT:
			goto retry;
		case -1:
			ssl_error("tls_write");
			what |= EVBUFFER_ERROR;
			goto err;
		}
		len = ret;

		evbuffer_drain(bufev->output, len);
	}

	if (EVBUFFER_LENGTH(bufev->output) != 0)
		event_add(&bufev->ev_write, NULL);

	if (bufev->writecb != NULL &&
	    EVBUFFER_LENGTH(bufev->output) <= bufev->wm_write.low)
		(*bufev->writecb)(bufev, bufev->cbarg);
	return;

retry:
	event_add(&bufev->ev_write, NULL);
	return;

err:
	(*bufev->errorcb)(bufev, what, bufev->cbarg);
}

static int
gemini_parse_reply(struct req *req, const char *header, size_t len)
{
	struct ibuf	*ibuf;
	int		 code;
	const char	*t;

	if (len < 4)
		return 0;

	if (!isdigit(header[0]) || !isdigit(header[1]))
		return 0;

	code = (header[0] - '0')*10 + (header[1] - '0');
	if (header[2] != ' ')
		return 0;

	t = header + 3;
	len = strlen(t) + 1;

	if ((ibuf = imsg_create(&iev_ui->ibuf, IMSG_REPLY, req->id, 0,
	    sizeof(code) + len)) == NULL)
		die();
	if (imsg_add(ibuf, &code, sizeof(code)) == -1 ||
	    imsg_add(ibuf, t, len) == -1)
		die();
	imsg_close(&iev_ui->ibuf, ibuf);
	imsg_event_add(iev_ui);

	bufferevent_disable(req->bev, EV_READ|EV_WRITE);

	return code;
}

/* called when we're ready to read/write */
static void
net_ready(struct req *req)
{
	req->bev = bufferevent_new(req->fd, net_read, net_write, net_error,
	    req);
	if (req->bev == NULL)
		die();

#if HAVE_EVENT2
	evbuffer_unfreeze(req->bev->input, 0);
	evbuffer_unfreeze(req->bev->output, 1);
#endif

	/* setup tls i/o layer */
	if (req->ctx != NULL) {
		event_set(&req->bev->ev_read, req->fd, EV_READ,
		    net_tls_readcb, req->bev);
		event_set(&req->bev->ev_write, req->fd, EV_WRITE,
		    net_tls_writecb, req->bev);
	}

	/* TODO: adjust watermarks */
	bufferevent_setwatermark(req->bev, EV_WRITE, 1, 0);
	bufferevent_setwatermark(req->bev, EV_READ,  1, 0);

	bufferevent_enable(req->bev, EV_READ|EV_WRITE);

	bufferevent_write(req->bev, req->req, req->len);
}

/* called after a read has been done */
static void
net_read(struct bufferevent *bev, void *d)
{
	static char	 buf[4096];
	struct req	*req = d;
	struct evbuffer	*src = EVBUFFER_INPUT(bev);
	size_t		 len;
	int		 r;
	char		*header;

	if (!req->done_header) {
		header = evbuffer_readln(src, &len, EVBUFFER_EOL_CRLF_STRICT);
		if (header == NULL && EVBUFFER_LENGTH(src) >= 1024) {
			(*bev->errorcb)(bev, EVBUFFER_READ, bev->cbarg);
			return;
		}
		if (header == NULL)
			return;
		r = gemini_parse_reply(req, header, len);
		free(header);
		req->done_header = 1;
		if (r == 0) {
			(*bev->errorcb)(bev, EVBUFFER_READ, bev->cbarg);
			return;
		}
		if (r < 20 || r >= 30) {
			close_conn(0, 0, req);
			return;
		}
	}

	/*
	 * Split data into chunks before sending.  imsg can't handle
	 * message that are "too big".
	 */
	for (;;) {
		if ((len = bufferevent_read(bev, buf, sizeof(buf))) == 0)
			break;
		net_send_ui(IMSG_BUF, req->id, buf, len);
	}
}

/* called after a write has been done */
static void
net_write(struct bufferevent *bev, void *d)
{
	struct evbuffer	*dst = EVBUFFER_OUTPUT(bev);

	if (EVBUFFER_LENGTH(dst) == 0)
		(*bev->errorcb)(bev, EVBUFFER_WRITE, bev->cbarg);
}

static void
net_error(struct bufferevent *bev, short error, void *d)
{
	struct req	*req = d;
	struct evbuffer	*src;

	if (error & EVBUFFER_TIMEOUT) {
		close_with_err(req, "Timeout loading page");
		return;
	}

	if (error & EVBUFFER_ERROR) {
		close_with_errf(req, "%s error (0x%x)",
		    (error & EVBUFFER_READ) ? "read" : "write", error);
		return;
	}

	if (error & EVBUFFER_EOF) {
		/* EOF and no header */
		if (!req->done_header) {
			close_with_err(req, "protocol error");
			return;
		}

		src = EVBUFFER_INPUT(req->bev);
		if (EVBUFFER_LENGTH(src) != 0)
			net_send_ui(IMSG_BUF, req->id, EVBUFFER_DATA(src),
			    EVBUFFER_LENGTH(src));
		net_send_ui(IMSG_EOF, req->id, NULL, 0);
		close_conn(0, 0, req);
		return;
	}

	if (error & EVBUFFER_WRITE) {
		/* finished sending request */
		bufferevent_disable(bev, EV_WRITE);
		return;
	}

	if (error & EVBUFFER_READ) {
		close_with_err(req, "protocol error");
		return;
	}

	close_with_errf(req, "unknown event error %x", error);
}

static int
load_cert(struct imsg *imsg, struct req *req)
{
	struct stat	 sb;
	int		 fd;

	if ((fd = imsg_get_fd(imsg)) == -1)
		return (0);

	if (fstat(fd, &sb) == -1)
		return (-1);

#if 0
	if (sb.st_size >= (off_t)SIZE_MAX) {
		close(fd);
		return (-1);
	}
#endif

	req->ccert = mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (req->ccert == MAP_FAILED) {
		req->ccert = NULL;
		close(fd);
		return (-1);
	}

	req->ccert_len = sb.st_size;
	req->ccert_fd = fd;

	return (0);
}

static void
handle_dispatch_imsg(int fd, short event, void *d)
{
	struct imsgev	*iev = d;
	struct imsgbuf	*ibuf = &iev->ibuf;
	struct imsg	 imsg;
	struct req	*req;
	struct get_req	 r;
	ssize_t		 n;
	int		 certok;

	if (event & EV_READ) {
		if ((n = imsg_read(ibuf)) == -1 && errno != EAGAIN)
			err(1, "imsg_read");
		if (n == 0)
			err(1, "connection closed");
	}
	if (event & EV_WRITE) {
		if ((n = msgbuf_write(&ibuf->w)) == -1 && errno != EAGAIN)
			err(1, "msgbuf_write");
		if (n == 0)
			err(1, "connection closed");
	}

	for (;;) {
		if ((n = imsg_get(ibuf, &imsg)) == -1)
			err(1, "imsg_get");
		if (n == 0)
			break;
		switch (imsg_get_type(&imsg)) {
		case IMSG_GET:
			if (imsg_get_data(&imsg, &r, sizeof(r)) == -1 ||
			    r.host[sizeof(r.host) - 1] != '\0' ||
			    r.port[sizeof(r.port) - 1] != '\0' ||
			    r.req[sizeof(r.req) - 1] != '\0')
				die();
			if (r.proto != PROTO_FINGER &&
			    r.proto != PROTO_GEMINI &&
			    r.proto != PROTO_GOPHER)
				die();

			if ((req = calloc(1, sizeof(*req))) == NULL)
				die();

			req->ccert_fd = -1;
			req->id = imsg_get_id(&imsg);
			TAILQ_INSERT_HEAD(&reqhead, req, reqs);

			if ((req->host = strdup(r.host)) == NULL)
				die();
			if ((req->port = strdup(r.port)) == NULL)
				die();
			if ((req->req = strdup(r.req)) == NULL)
				die();
			if (load_cert(&imsg, req) == -1)
				die();

			req->len = strlen(req->req);

			req->proto = r.proto;
			conn_towards(req);
			break;

		case IMSG_CERT_STATUS:
			if ((req = req_by_id(imsg_get_id(&imsg))) == NULL)
				break;

			if (imsg_get_data(&imsg, &certok, sizeof(certok)) ==
			    -1)
				die();
			if (certok)
				net_ready(req);
			else
				close_conn(0, 0, req);
			break;

		case IMSG_PROCEED:
			if ((req = req_by_id(imsg_get_id(&imsg))) == NULL)
				break;
			bufferevent_enable(req->bev, EV_READ);
			break;

		case IMSG_STOP:
			if ((req = req_by_id(imsg_get_id(&imsg))) == NULL)
				break;
			close_conn(0, 0, req);
			break;

		case IMSG_QUIT:
			event_loopbreak();
			imsg_free(&imsg);
			return;

		default:
			errx(1, "got unknown imsg %d", imsg_get_type(&imsg));
		}

		imsg_free(&imsg);
	}

	imsg_event_add(iev);
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

	msgbuf_clear(&iev_ui->ibuf.w);
	close(iev_ui->ibuf.fd);
	free(iev_ui);

	return 0;
}
