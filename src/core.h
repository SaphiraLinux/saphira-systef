#ifndef SYSREF_CORE_H
#define SYSREF_CORE_H

#include <stddef.h>
#include <stdint.h>
#include <limits.h>

typedef struct {
	char *p;
	size_t len;
	size_t cap;
	int failed;
} gbuf;

void gbuf_init(gbuf *b);
void gbuf_free(gbuf *b);
void gbuf_addn(gbuf *b, const char *s, size_t n);
void gbuf_adds(gbuf *b, const char *s);
void gbuf_addch(gbuf *b, char c);

/* Output formats. Text is the historical default and stays byte-identical;
 * markdown and json render the same collected evidence through the same
 * fields below. No JSONC or JSONP exists. */
#define SYSREF_FMT_TEXT 0
#define SYSREF_FMT_MARKDOWN 1
#define SYSREF_FMT_JSON 2

/* One ranked documentation excerpt. text holds the raw slice (at most
 * 1500 bytes); over marks a slice that exceeded the excerpt budget and is
 * reported, not quoted. */
typedef struct {
	char path[1024];
	int ln;
	int owner;
	int code;
	int freq;
	size_t len;
	int over;
	int html;
	char text[1500];
} sysref_excerpt;

/* One recorded C declaration: the verbatim source span (or the over-budget
 * marker), with its provenance. Bounded: the first declarations win. */
typedef struct {
	char path[1024];
	int ln;
	char text[1500];
	int over;
} sysref_decl;

#define SYSREF_MAX_DECLS 8
#define SYSREF_RANK_MAX 256

typedef struct {
	gbuf text;         /* final output, rendered per format */
	size_t full_bytes; /* rendered page bytes (internal only) */
	int status;        /* 0 ok, 1 page-not-found, 2 no-match */
	char pagename[128];
	char pagesec[16];
	int format;        /* SYSREF_FMT_*; set from format before the run */
	char verb[8];      /* man|doc|api|headers: the evidence shape below */
	const char *query; /* borrowed: caller keeps it alive past render */
	int page, total_pages;
	int per_page;
	/* man/doc: full pre-paging body. Markdown and JSON slice the same
	 * bytes the text path pages, so the three formats cannot disagree. */
	gbuf fullbody;
	/* man: alternative sections, when the default choice hid some. */
	char man_altsec[64];
	/* api/headers sidecar: the same scalars the text path prints. */
	char symbol[128];
	sysref_decl decls[SYSREF_MAX_DECLS];
	int ndecls;
	int kind, k_static, k_inline, k_extern;
	char guard[300];
	char header[PATH_MAX];
	int header_line;
	char pkg[256];
	int pkg_known;
	char declparams[512];
	char docform[512];
	int matches;
	sysref_excerpt *docs; /* ranked excerpts, owned; NULL when unscanned */
	int ndocs;
	int docs_total;
	int docs_skipped;
} result;

#define EST_TOKENS(n) (((size_t)(n) + 3) / 4)
#define SYSREF_MAX_QUERY_BYTES 8192u
#define SYSREF_MAX_COMMAND_OUTPUT (4u * 1024u * 1024u)

/* Paged response budget: lines per page for man and doc bodies. Small
 * bodies stay byte-identical to their unpaged form; only the navigation
 * block is added. Page boundaries snap to blank lines where one is near,
 * so short passages are never split mid-paragraph when avoidable. */
#define SYSREF_PAGE_LINES 30
/* Sanity cap on a single built body before paging; practically unreachable
 * for fragments (man renders whole pages, but only the matched fragment
 * is ever paged). Hit it and the reply says so instead of silently
 * dropping content. */
#define SYSREF_BODY_MAX_BYTES (64u * 1024u)

/* source: NULL or "man" = native man pages (default); "doc" = /usr/share/doc.
 * page is 1-based; callers validate (page < 1 is treated as 1).
 * per_page sizes the api documentation-excerpt window (1-10, default 3);
 * the man/doc paths page whole lines and ignore it.
 * format selects the rendering (text/markdown/json); text is the default
 * and stays byte-identical to the historical output. */
int sysref_run(const char *query, const char *section, const char *source, int page, int per_page, int format, result *res);
int sysref_api(const char *query, int semantics, int page, int per_page, int format, result *res);
/* Re-render res->text from the collected evidence in res->format.
 * No-op for text. Call after the run, before reading res->text. */
void sysref_render(result *res);
/* Render res into o in the given format without touching res->text, so one
 * search can serve both the human text and the structured object. */
void sysref_render_fmt(result *res, int format, gbuf *o);
/* api/headers renderer, implemented in api.c; shared envelope lives here. */
void sysref_render_api(result *res, gbuf *o);
void sysref_json_head(gbuf *o, result *res);
void render_nav_md(gbuf *o, int page, int total);
void result_free(result *res);

#endif
