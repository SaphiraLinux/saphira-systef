#ifndef AKAMAN_CORE_H
#define AKAMAN_CORE_H

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
#define AKAMAN_MAX_QUERY_BYTES 8192u
#define AKAMAN_MAX_COMMAND_OUTPUT (4u * 1024u * 1024u)

/* source: NULL or "man" = native man pages (default); "doc" = /usr/share/doc */
int akaman_run(const char *query, const char *section, const char *source, result *res);
void result_free(result *res);

#endif
