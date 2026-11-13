#ifndef SYSREF_CORE_H
#define SYSREF_CORE_H

#include <stddef.h>
#include <stdint.h>

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

typedef struct {
	gbuf text;         /* final output: header + fragment */
	size_t full_bytes; /* rendered page bytes (internal only) */
	int status;        /* 0 ok, 1 page-not-found, 2 no-match */
	char pagename[128];
	char pagesec[16];
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
 * page is 1-based; callers validate (page < 1 is treated as 1). */
int sysref_run(const char *query, const char *section, const char *source, int page, result *res);
int sysref_api(const char *query, int semantics, int page, result *res);
void result_free(result *res);

#endif
