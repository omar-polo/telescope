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

#ifndef TELESCOPE_H
#define TELESCOPE_H

#include "cmd.h"
#include "compat.h"
#include "phos/phos.h"

#include <limits.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

#define GEMINI_URL_LEN 1024

struct imsgev {
	struct imsgbuf	 ibuf;
	void		(*handler)(int, short, void *);
	struct event	 ev;
	short		 events;
};

enum imsg_type {
	/* ui <-> client/fs */
	IMSG_GET,		/* data is URL, peerid the tab id */
	IMSG_GET_FILE,		/* data is path, without \r\n or such */
	IMSG_GET_RAW,		/* get but with an explicit req str */
	IMSG_ERR,
	IMSG_CHECK_CERT,
	IMSG_CERT_STATUS,
	IMSG_GOT_CODE,
	IMSG_GOT_META,
	IMSG_PROCEED,
	IMSG_STOP,
	IMSG_BUF,
	IMSG_EOF,
	IMSG_QUIT,

	/* ui <-> fs */
	IMSG_BOOKMARK_PAGE,
	IMSG_BOOKMARK_OK,
	IMSG_SAVE_CERT,
	IMSG_SAVE_CERT_OK,
	IMSG_UPDATE_CERT,
	IMSG_UPDATE_CERT_OK,

	IMSG_FILE_OPEN,
	IMSG_FILE_OPENED,

	IMSG_SESSION_START,
	IMSG_SESSION_TAB,
	IMSG_SESSION_TAB_TITLE,
	IMSG_SESSION_END,
};

enum line_type {
	/* text/gemini */
	LINE_TEXT,
	LINE_LINK,
	LINE_TITLE_1,
	LINE_TITLE_2,
	LINE_TITLE_3,
	LINE_ITEM,
	LINE_QUOTE,
	LINE_PRE_START,
	LINE_PRE_CONTENT,
	LINE_PRE_END,

	/* text/x-patch */
	LINE_PATCH,
	LINE_PATCH_HDR,
	LINE_PATCH_HUNK_HDR,
	LINE_PATCH_ADD,
	LINE_PATCH_DEL,

	/* minibuffer */
	LINE_COMPL,
	LINE_COMPL_CURRENT,

	/* help */
	LINE_HELP,
};

/* for lines: mark as hidden */
#define L_HIDDEN	1

/* for vlines: mark as continuation */
#define L_CONTINUATION	2

struct line {
	enum line_type		 type;
	char			*line;
	char			*alt;
	void			*data;
	int			 flags;
	TAILQ_ENTRY(line)	 lines;
};

struct vline {
	struct line		*parent;
	char			*line;
	int			 flags;
	TAILQ_ENTRY(vline)	 vlines;
};

struct parser;
struct page;

typedef int	(*parsechunkfn)(struct parser*, const char*, size_t);
typedef int	(*parserfreefn)(struct parser*);

typedef void (imsg_handlerfn)(struct imsg*, size_t);

struct parser {
	const char	*name;
	char		 title[128+1];
	char		*buf;
	size_t		 len;
	size_t		 cap;

#define PARSER_IN_BODY	1
#define PARSER_IN_PRE	2
#define PARSER_IN_PATCH_HDR 4
	int		 flags;
	parsechunkfn	 parse;
	parserfreefn	 free;

	TAILQ_HEAD(, line)	 head;
};

/*
 * differnt types of trust for a certificate.  Following
 * gemini://thfr.info/gemini/modified-trust-verify.gmi
 */
enum trust_state {
	TS_UNKNOWN,
	TS_UNTRUSTED,
	TS_TEMP_TRUSTED,
	TS_TRUSTED,
	TS_VERIFIED,
};

struct tofu_entry {
	char	domain[GEMINI_URL_LEN];

	/*
	 * enough space for ``PROTO:HASH''.  probably isn't a good
	 * idea tho.
	 */
	char	hash[128+1];
	int	verified;
};

struct histhead {
	TAILQ_HEAD(mhisthead, hist)	head;
	size_t				len;
};
struct hist {
	char			h[1025];
	TAILQ_ENTRY(hist)	entries;
};

struct buffer {
	struct parser		 page;

	size_t			 last_line_off;
	int			 force_redraw;

	int			 curs_x;
	int			 curs_y;
	size_t			 line_off;
	size_t			 line_max;
	struct vline		*top_line;
	struct vline		*current_line;
	size_t			 cpoff;
	TAILQ_HEAD(vhead, vline) head;
};

#define TAB_CURRENT	0x1	/* only for save_session */
#define TAB_URGENT	0x2
#define TAB_LAZY	0x4	/* to lazy load tabs */

#define NEW_TAB_URL	"about:new"

extern TAILQ_HEAD(tabshead, tab) tabshead;
struct tab {
	TAILQ_ENTRY(tab)	 tabs;
	uint32_t		 id;
	uint32_t		 flags;

	char			*cert;
	enum trust_state	 trust;
	struct proxy		*proxy;
	struct phos_uri		 uri;
	struct histhead		 hist;
	struct hist		*hist_cur;
	size_t			 hist_off;

	int			 code;
	char			 meta[GEMINI_URL_LEN];
	int			 redirect_count;

	struct buffer		 buffer;

	short			 loading_anim;
	short			 loading_anim_step;
	struct event		 loadingev;

	int			 fd;
	size_t			 bytes;
	char			*path;
};

extern TAILQ_HEAD(proxylist, proxy) proxies;
struct proxy {
	char	*match_proto;

	char	*host;
	char	*port;
	int	 proto;

	TAILQ_ENTRY(proxy) proxies;
};

enum {
	PROTO_FINGER,
	PROTO_GEMINI,
	PROTO_GOPHER,
	/* ... */
};

struct get_req {
	int		proto;
	char		host[254];
	char		port[16];
	char		req[1027];
};

struct kmap {
	TAILQ_HEAD(map, keymap)	m;
	void			(*unhandled_input)(void);
};
extern struct kmap global_map, minibuffer_map;

typedef void(interactivefn)(struct buffer *);

struct keymap {
	int			 meta;
	int			 key;
	struct kmap		 map;
	interactivefn		 *fn;

	TAILQ_ENTRY(keymap)	 keymaps;
};

struct cmd {
	const char	*cmd;
	void		(*fn)(struct buffer *);
	const char	*descr;
};
extern struct cmd cmds[];

/* defaults.c */
void		 config_init(void);
int		 config_setprfx(const char *, const char *, const char *);
int		 config_setvari(const char *, int);
int		 config_setvars(const char *, char *);
int		 config_setcolor(int, const char *, int, int, int);
int		 config_setattr(const char *, int, int, int);
void		 config_apply_style(void);

/* fs.c */
extern char	session_file[PATH_MAX];

int		 fs_init(void);
int		 fs_main(void);
int		 last_time_crashed(void);
int		 lock_session(void);
int		 load_certs(struct ohash*);

/* help.c */
void		 recompute_help(void);

/* hist.c */
void		 hist_clear_forward(struct histhead*, struct hist*);
void		 hist_push(struct histhead*, struct hist*);

/* keymap.c */
int		 kbd(const char*);
const char	*unkbd(int);
int		 kmap_define_key(struct kmap*, const char*, void(*)(struct buffer*));

/* mime.c */
int		 setup_parser_for(struct tab*);

/* net.c */
int		 net_main(void);

/* parse.y */
void		 parseconfig(const char *, int);

/* sandbox.c */
void		 sandbox_net_process(void);
void		 sandbox_ui_process(void);
void		 sandbox_fs_process(void);

/* telescope.c */
extern int operating;

void		 gopher_send_search_req(struct tab *, const char *);
void		 load_url(struct tab *, const char *, const char *, int);
void		 load_url_in_tab(struct tab *, const char *, const char *, int);
int		 load_previous_page(struct tab*);
int		 load_next_page(struct tab*);
void		 add_to_bookmarks(const char*);
void		 humanify_url(const char *, char *, size_t);
int		 ui_send_net(int, uint32_t, const void *, uint16_t);
int		 ui_send_fs(int, uint32_t, const void *, uint16_t);

/* tofu.c */
void			 tofu_init(struct ohash*, unsigned int, ptrdiff_t);
struct tofu_entry	*tofu_lookup(struct ohash*, const char*, const char*);
void			 tofu_add(struct ohash*, struct tofu_entry*);
void			 tofu_update(struct ohash*, struct tofu_entry*);
void			 tofu_temp_trust(struct ohash *, const char *, const char *, const char *);

/* util.c */
int		 mark_nonblock(int);
int		 has_prefix(const char*, const char*);
int		 has_suffix(const char *, const char *);
int		 unicode_isspace(uint32_t);
int		 unicode_isgraph(uint32_t);
int		 dispatch_imsg(struct imsgev *, short, imsg_handlerfn **, size_t);
int		 imsg_compose_event(struct imsgev *, uint16_t, uint32_t, pid_t, int, const void *, uint16_t);

/* wrap.c */
void		 erase_buffer(struct buffer *);
void		 empty_linelist(struct buffer*);
void		 empty_vlist(struct buffer*);
int		 wrap_one(struct buffer *, const char *, struct line *, size_t);
int		 wrap_text(struct buffer*, const char*, struct line*, size_t);
int		 hardwrap_text(struct buffer*, struct line*, size_t);
int		 wrap_page(struct buffer *, int width);

#endif /* TELESCOPE_H */
