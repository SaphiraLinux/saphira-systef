#include "core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <limits.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/wait.h>
#include <sys/stat.h>

/* ------------------------------------------------------------------ */
/* growable buffer                                                     */
/* ------------------------------------------------------------------ */
void gbuf_init(gbuf *b) { b->p = NULL; b->len = 0; b->cap = 0; b->failed = 0; }
void gbuf_free(gbuf *b) { free(b->p); b->p = NULL; b->len = 0; b->cap = 0; b->failed = 0; }
static int gbuf_reserve(gbuf *b, size_t extra) {
	if (b->failed) return 0;
	if (b->len == SIZE_MAX || extra > SIZE_MAX - b->len - 1) { b->failed = 1; return 0; }
	size_t need = b->len + extra + 1;
	if (need <= b->cap) return 1;
	size_t nc = b->cap ? b->cap : 256;
	while (nc < need) {
		if (nc > SIZE_MAX / 2) { nc = need; break; }
		nc *= 2;
	}
	char *np = realloc(b->p, nc);
	if (!np) { b->failed = 1; return 0; }
	b->p = np;
	b->cap = nc;
	return 1;
}
void gbuf_addn(gbuf *b, const char *s, size_t n) {
	if (!b || b->failed || n == 0) return;
	if (!s || !gbuf_reserve(b, n)) return;
	memcpy(b->p + b->len, s, n);
	b->len += n;
	b->p[b->len] = 0;
}
void gbuf_adds(gbuf *b, const char *s) { gbuf_addn(b, s, strlen(s)); }
void gbuf_addch(gbuf *b, char c) { gbuf_addn(b, &c, 1); }

/* ------------------------------------------------------------------ */
/* run a program via argv (never through a shell)                      */
/* ------------------------------------------------------------------ */
static int run_cmd(char *const argv[], char **out, size_t *outlen, int *status) {
	*out = NULL;
	if (outlen) *outlen = 0;
	*status = -1;
	int to[2];
	if (pipe(to)) return -1;
	for (int i = 0; i < 2; i++) {
		int flags = fcntl(to[i], F_GETFD);
		if (flags >= 0) (void)fcntl(to[i], F_SETFD, flags | FD_CLOEXEC);
	}
	pid_t pid = fork();
	if (pid < 0) { close(to[0]); close(to[1]); return -1; }
	if (pid == 0) {
		dup2(to[1], 1);
		close(to[0]);
		close(to[1]);
		int fd = open("/dev/null", O_WRONLY);
		if (fd >= 0) { dup2(fd, 2); close(fd); }
		setenv("LC_ALL", "C", 1);
		setenv("LANG", "C", 1);
		setenv("MANWIDTH", "80", 1);
		setenv("MANPAGER", "cat", 1);
		setenv("PAGER", "cat", 1);
		execvp(argv[0], argv);
		_exit(127);
	}
	close(to[1]);
	gbuf o;
	gbuf_init(&o);
	char buf[8192];
	ssize_t n;
	for (;;) {
		n = read(to[0], buf, sizeof buf);
		if (n > 0) {
			if (!o.failed) {
				if (o.len > AKAMAN_MAX_COMMAND_OUTPUT ||
				    (size_t)n > AKAMAN_MAX_COMMAND_OUTPUT - o.len)
					o.failed = 1;
				else
					gbuf_addn(&o, buf, (size_t)n);
			}
			continue;
		}
		if (n < 0 && errno == EINTR) continue;
		break;
	}
	close(to[0]);
	int st = 0;
	pid_t wr;
	do { wr = waitpid(pid, &st, 0); } while (wr < 0 && errno == EINTR);
	if (wr < 0 || o.failed) { free(o.p); return -1; }
	*out = o.p;
	if (outlen) *outlen = o.len;
	*status = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
	return 0;
}

/* ------------------------------------------------------------------ */
/* tokenisation                                                         */
/* ------------------------------------------------------------------ */
typedef struct {
	char **v;
	int n;
} toks;

static void toks_free(toks *t) {
	if (!t->v) return;
	for (int i = 0; i < t->n; i++) free(t->v[i]);
	free(t->v);
	t->v = NULL;
	t->n = 0;
}
static int split_tokens(const char *q, toks *t) {
	t->v = NULL;
	t->n = 0;
	int cap = 0;
	const char *p = q;
	while (*p) {
		while (*p && isspace((unsigned char)*p)) p++;
		if (!*p) break;
		const char *s = p;
		while (*p && !isspace((unsigned char)*p)) p++;
		if (t->n == cap) {
			cap = cap ? cap * 2 : 8;
			t->v = realloc(t->v, (size_t)cap * sizeof(char *));
		}
		size_t len = (size_t)(p - s);
		char *c = malloc(len + 1);
		memcpy(c, s, len);
		c[len] = 0;
		t->v[t->n++] = c;
	}
	return t->n;
}
static int pagechar(unsigned char c) {
	return isalnum(c) || c == '-' || c == '_' || c == '.' || c == '/' || c == '+';
}

/* Query paths are names within an installed documentation tree, never paths
 * supplied to the host filesystem.  Reject absolute names and parent
 * components before any native resolver or filesystem call sees them. */
static int safe_relative_path(const char *s, int allow_slash) {
	if (!s || !s[0] || s[0] == '/') return 0;
	const char *p = s;
	while (*p) {
		const char *start = p;
		while (*p && *p != '/') p++;
		size_t n = (size_t)(p - start);
		if (n == 2 && start[0] == '.' && start[1] == '.') return 0;
		if (!allow_slash && *p == '/') return 0;
		while (*p == '/') p++;
	}
	return 1;
}

static int path_under(const char *path, const char *root) {
	size_t n = strlen(root);
	return strncmp(path, root, n) == 0 && (path[n] == 0 || path[n] == '/');
}

static int canonical_under(const char *path, const char *root, char *out, size_t outsz) {
	char rp[PATH_MAX];
	char rr[PATH_MAX];
	if (!realpath(path, rp) || !realpath(root, rr)) return 0;
	if (!path_under(rp, rr) || strlen(rp) + 1 > outsz) return 0;
	strcpy(out, rp);
	return 1;
}

static int man_root_allowed(const char *path) {
	static const char *fixed[] = {
		"/usr/share/man", "/usr/local/share/man", "/usr/local/man",
		"/usr/lib/man", "/opt/local/share/man", NULL};
	for (int i = 0; fixed[i]; i++) {
		char root[PATH_MAX];
		if (realpath(fixed[i], root) && path_under(path, root)) return 1;
	}
	const char *mp = getenv("MANPATH");
	if (mp && *mp) {
		const char *p = mp;
		while (*p) {
			const char *e = strchr(p, ':');
			size_t n = e ? (size_t)(e - p) : strlen(p);
			if (n > 0 && n < PATH_MAX) {
				char entry[PATH_MAX], root[PATH_MAX];
				memcpy(entry, p, n); entry[n] = 0;
				if (realpath(entry, root) && path_under(path, root)) return 1;
			}
			if (!e) break;
			p = e + 1;
		}
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* page resolution via native man -w                                   */
/* ------------------------------------------------------------------ */
typedef struct {
	char path[512];
	char name[128];
	char sec[16];
	int used; /* number of leading non-option tokens that formed the page name */
} resolved_page;

static int is_sec_ext(const char *s) {
	size_t l = strlen(s);
	if (l < 1 || l > 3) return 0;
	if (!isdigit((unsigned char)s[0])) return 0;
	for (size_t i = 1; i < l; i++)
		if (!isalpha((unsigned char)s[i])) return 0;
	return 1;
}
static void derive_name_sec(resolved_page *r) {
	const char *base = strrchr(r->path, '/');
	base = base ? base + 1 : r->path;
	char tmp[512];
	size_t tl = 0;
	for (const char *p = base; *p && tl < sizeof tmp - 1; p++) tmp[tl++] = *p;
	tmp[tl] = 0;
	static const char *suf[] = {".gz", ".bz2", ".xz", ".zst", ".lzma", ".lz", NULL};
	for (int i = 0; suf[i]; i++) {
		size_t l = strlen(suf[i]);
		if (tl > l && strcmp(tmp + tl - l, suf[i]) == 0) { tmp[tl - l] = 0; tl -= l; break; }
	}
	char *dot = strrchr(tmp, '.');
	if (dot && dot != tmp && is_sec_ext(dot + 1) && strlen(dot + 1) < sizeof r->sec) {
		strcpy(r->sec, dot + 1);
		*dot = 0;
	}
	if (!r->sec[0]) strcpy(r->sec, "?");
	strncpy(r->name, tmp, sizeof r->name - 1);
	r->name[sizeof r->name - 1] = 0;
}

static int memmem2(const char *h, size_t hl, const char *n) {
	size_t nl = strlen(n);
	if (nl == 0 || nl > hl) return 0;
	for (size_t i = 0; i + nl <= hl; i++)
		if (memcmp(h + i, n, nl) == 0) return 1;
	return 0;
}
static int man_gnu = -1;
static int man_probe_gnu(void) {
	if (man_gnu >= 0) return man_gnu;
	/* ask man itself whether it understands GNU long options */
	char *av[] = {"man", "--help", NULL};
	char *out = NULL;
	size_t ol;
	int st;
	if (run_cmd(av, &out, &ol, &st) < 0 || st != 0) {
		free(out);
		man_gnu = 0;
		return 0;
	}
	man_gnu = (out && memmem2(out, ol, "--no-hyphenation")) ? 1 : 0;
	free(out);
	return man_gnu;
}

static int try_man_w(const char *sec, const char *cand, resolved_page *r) {
	if (!safe_relative_path(cand, 1)) return 0;
	char *av[8];
	int n = 0;
	av[n++] = "man";
	av[n++] = "-w";
	char secbuf[8];
	if (sec && sec[0]) {
		av[n++] = man_probe_gnu() ? "-s" : "-S";
		snprintf(secbuf, sizeof secbuf, "%s", sec);
		av[n++] = secbuf;
	}
	av[n++] = (char *)cand;
	av[n++] = NULL;
	char *out = NULL;
	size_t outlen;
	int status;
	if (run_cmd(av, &out, &outlen, &status) < 0) return 0;
	if (status != 0) { free(out); return 0; }
	size_t i = 0;
	while (i < outlen && out[i] != '\n' && out[i] != ' ' && out[i] != '\t') i++;
	if (i == 0 || i >= sizeof r->path) { free(out); return 0; }
	out[i] = 0;
	char canon[PATH_MAX];
	if (!realpath(out, canon) || !man_root_allowed(canon) || strlen(canon) >= sizeof r->path) {
		free(out);
		return 0;
	}
	strcpy(r->path, canon);
	free(out);
	r->sec[0] = 0;
	r->name[0] = 0;
	derive_name_sec(r);
	return 1;
}

static int resolve_page(const char *query, const char *section, resolved_page *r, toks *tokens_out) {
	toks t;
	if (!split_tokens(query, &t)) return 0;
	char *nopts[16];
	int nn = 0;
	for (int i = 0; i < t.n && nn < 16; i++) {
		if (t.v[i][0] == '-') continue;
		int ok = 1;
		for (const char *p = t.v[i]; *p; p++)
			if (!pagechar((unsigned char)*p)) { ok = 0; break; }
		if (!ok) continue;
		nopts[nn++] = t.v[i];
	}
	if (nn == 0) { toks_free(&t); return 0; }
	int maxu = nn > 3 ? 3 : nn;
	for (int u = maxu; u >= 1; u--) {
		size_t clen = 0;
		for (int i = 0; i < u; i++) clen += strlen(nopts[i]) + 1;
		char *cand = malloc(clen + 1);
		char *c = cand;
		for (int i = 0; i < u; i++) {
			if (i) *c++ = '-';
			size_t l = strlen(nopts[i]);
			memcpy(c, nopts[i], l);
			c += l;
		}
		*c = 0;
		resolved_page r2;
		memset(&r2, 0, sizeof r2);
		if (try_man_w(section, cand, &r2)) {
			memcpy(r, &r2, sizeof r2);
			r->used = u;
			*tokens_out = t;
			t.v = NULL;
			free(cand);
			return 1;
		}
		free(cand);
	}
	toks_free(&t);
	return 0;
}

/* ------------------------------------------------------------------ */
/* rendering via native man                                            */
/* ------------------------------------------------------------------ */
static int render_page(const resolved_page *r, gbuf *out) {
	char *av[6];
	int n = 0;
	if (man_probe_gnu())
		av[n++] = "--no-hyphenation", av[n++] = "--no-justification";
	av[n++] = "-l";
	av[n++] = (char *)r->path;
	av[n++] = NULL;
	/* rearranged argv: man [GNU flags] -l PATH */
	char *avf[6];
	int nf = 0;
	avf[nf++] = "man";
	for (int i = 0; i < n; i++) avf[nf++] = av[i];
	char *o;
	size_t ol;
	int st;
	if (run_cmd(avf, &o, &ol, &st) < 0) return 0;
	if (st != 0) { free(o); return 0; }
	gbuf_free(out);
	out->p = o;
	out->len = ol;
	out->cap = ol + 1;
	return 1;
}

/* ------------------------------------------------------------------ */
/* line index + structure detection                                    */
/* ------------------------------------------------------------------ */
typedef struct {
	const char *s;
	size_t len;
	int indent;
	int blank;
} line;

static line *build_lines(const char *txt, size_t len, int *nout) {
	int cap = 64, n = 0;
	line *ls = malloc((size_t)cap * sizeof *ls);
	size_t i = 0;
	while (i <= len) {
		const char *start = txt + i;
		while (i < len && txt[i] != '\n') i++;
		if (n == cap) { cap *= 2; ls = realloc(ls, (size_t)cap * sizeof *ls); }
		ls[n].s = start;
		ls[n].len = (size_t)((txt + i) - start);
		int ind = 0;
		while (ind < (int)ls[n].len && ls[n].s[ind] == ' ') ind++;
		ls[n].indent = ind;
		ls[n].blank = (ind == (int)ls[n].len);
		n++;
		if (i < len) i++;
		else break;
	}
	*nout = n;
	return ls;
}

static size_t trimline(const line *ln, const char **out) {
	size_t l = ln->len - (size_t)ln->indent;
	const char *s = ln->s + ln->indent;
	while (l > 0 && (s[l - 1] == ' ' || s[l - 1] == '\t')) l--;
	*out = s;
	return l;
}

static int wb(char c) { return isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.'; }
static int has_alnum(const char *s) {
	for (; *s; s++)
		if (isalnum((unsigned char)*s)) return 1;
	return 0;
}
static int starts_word(const char *s, size_t l, const char *w) {
	size_t wl = strlen(w);
	if (l < wl) return 0;
	if (memcmp(s, w, wl)) return 0;
	if (l == wl) return 1;
	char c = s[wl];
	return c == ' ' || c == '\t' || c == '=' || c == 0;
}
static int has_word(const char *s, size_t l, const char *w) {
	size_t wl = strlen(w);
	if (wl == 0) return 0;
	for (size_t i = 0; i + wl <= l; i++) {
		if (memcmp(s + i, w, wl) == 0) {
			int bl = (i == 0) || !wb(s[i - 1]);
			int br = (i + wl == l) || !wb(s[i + wl]);
			if (bl && br) return 1;
		}
	}
	return 0;
}
static int grammar_marker(const char *s, size_t l) {
	for (size_t i = 0; i < l; i++)
		if (s[i] == '[' || s[i] == '{' || s[i] == '|' || s[i] == ']' || s[i] == '}')
			return 1;
	return 0;
}
static int is_diagram_line(const char *s, size_t l) {
	for (size_t i = 0; i + 2 < l; i++)
		if ((unsigned char)s[i] == 0xE2 && (unsigned char)s[i + 1] == 0x94) return 1;
	return 0;
}
static int upper_of(const char *s, size_t l, char *buf, size_t bsz) {
	if (l + 1 > bsz) return 0;
	for (size_t i = 0; i < l; i++) buf[i] = (char)toupper((unsigned char)s[i]);
	buf[l] = 0;
	return 1;
}

static const char *top_sections[] = {
	"NAME", "SYNOPSIS", "DESCRIPTION", "OPTIONS", "OPTION", "EXAMPLES", "EXAMPLE", "FILES",
	"SEE ALSO", "ENVIRONMENT", "RETURN VALUE", "RETURN VALUES", "ERRORS", "NOTES", "HISTORY",
	"STANDARDS", "BUGS", "AUTHOR", "AUTHORS", "COPYRIGHT", "VERSION", "CONFIGURATION",
	"PORTABILITY", "INVOCATION", "REPORTING BUGS", "EXIT STATUS", "EXIT VALUES", "EXIT CODES",
	NULL};

static int is_top_heading(const char *s, size_t l) {
	if (l == 0) return 0;
	char up[64];
	if (!upper_of(s, l, up, sizeof up)) return 0;
	for (int i = 0; top_sections[i]; i++)
		if (strcmp(up, top_sections[i]) == 0) return 1;
	return 0;
}
/* real top-level heading: a short line flush-left (indent 0) that is not a
 * page-title line.  page titles are right-aligned and end with "(N)", e.g.
 * "GNU grep 3.12-modified  ...  GREP(1)"; a tab means a title too.  this lets
 * the map show real headings (REGULAR EXPRESSIONS, CAVEATS, ...) that are
 * absent from the extraction whitelist. */
static int real_top_heading(const line *ln, const char *s, size_t l) {
	if (ln->indent != 0) return 0;
	if (l == 0 || l >= 45) return 0;
	if (s[l - 1] == ')') return 0;
	int alpha = 0;
	for (size_t i = 0; i < l; i++) {
		unsigned char c = (unsigned char)s[i];
		if (c == '\t') return 0;
		if (isalpha(c)) alpha = 1;
	}
	return alpha;
}

static int *build_headings(const line *ls, int n, int *outn) {
	int cap = 16, c = 0;
	int *head = malloc((size_t)cap * sizeof(int));
	for (int i = 0; i < n; i++) {
		const char *s;
		size_t l = trimline(&ls[i], &s);
		if (is_top_heading(s, l) || real_top_heading(&ls[i], s, l)) {
			if (c == cap) { cap *= 2; head = realloc(head, (size_t)cap * sizeof(int)); }
			head[c++] = i;
		}
	}
	*outn = c;
	return head;
}
static int find_section(const line *ls, int n, const int *heads, int nh, const char *name) {
	(void)n;
	for (int h = 0; h < nh; h++) {
		const char *s;
		size_t l = trimline(&ls[heads[h]], &s);
		char up[64];
		upper_of(s, l, up, sizeof up);
		if (strcmp(up, name) == 0) return heads[h];
	}
	return -1;
}
static int section_end(const int *heads, int nh, int start) {
	for (int h = 0; h < nh; h++)
		if (heads[h] > start) return heads[h];
	return INT_MAX;
}

/* interpret the trailing query tokens as a named section: try the longest
 * suffix first (so "grep exit status" -> EXIT STATUS, not STATUS), uppercased
 * to match the page heading; also try stripping a trailing 'S' so "examples"
 * matches an "EXAMPLE" heading.  returns the heading line index or -1. */
static int find_section_by_tokens(const line *ls, int n, const int *heads, int nh,
                                  char **anchors, int nan) {
	for (int len = nan; len >= 1; len--) {
		char buf[128];
		size_t bl = 0;
		int ok = 1;
		for (int i = nan - len; i < nan; i++) {
			size_t l = strlen(anchors[i]);
			if (bl + l + (i > nan - len ? 1 : 0) + 1 > sizeof buf) { ok = 0; break; }
			if (i > nan - len) buf[bl++] = ' ';
			memcpy(buf + bl, anchors[i], l);
			bl += l;
		}
		if (!ok) continue;
		buf[bl] = 0;
		char up[64];
		if (!upper_of(buf, bl, up, sizeof up)) continue;
		int h = find_section(ls, n, heads, nh, up);
		if (h >= 0) return h;
		size_t ul = strlen(up);
		if (ul > 1 && up[ul - 1] == 'S') {
			up[ul - 1] = 0;
			h = find_section(ls, n, heads, nh, up);
			if (h >= 0) return h;
		}
	}
	return -1;
}

static void emit_range(gbuf *b, const line *ls, int a, int e) {
	for (int i = a; i < e; i++) {
		gbuf_addn(b, ls[i].s, ls[i].len);
		gbuf_addch(b, '\n');
	}
}

/* emit a section but never exceed the hard budget: whole lines only, so a
 * huge section (grep ENVIRONMENT) is trimmed at a line boundary instead of
 * blowing past the 400-token guard. */
#define SECTION_BUDGET_BYTES 1000
#define OUTPUT_BUDGET_BYTES 1600
static void emit_section_budget(gbuf *b, const line *ls, int a, int e) {
	for (int i = a; i < e && b->len < SECTION_BUDGET_BYTES; i++) {
		if (ls[i].len + 1 > SECTION_BUDGET_BYTES - b->len) break;
		gbuf_addn(b, ls[i].s, ls[i].len);
		gbuf_addch(b, '\n');
	}
}

/* compact map of the page's real top-level headings, e.g.
 *   SECTIONS: NAME | SYNOPSIS | DESCRIPTION | OPTIONS | EXIT STATUS
 * only headings actually present in the page, deduplicated, one logical
 * line (capped so it never balloons past a few hundred bytes). */
static void emit_sections_line(gbuf *b, const line *ls, const int *heads, int nh) {
	char seen[64][40];
	int nseen = 0;
	int emitted = 0;
	size_t start = b->len;
	gbuf_adds(b, "SECTIONS:");
	for (int h = 0; h < nh; h++) {
		const char *s;
		size_t l = trimline(&ls[heads[h]], &s);
		if (l == 0 || l >= 40) continue;
		char up[40];
		if (!upper_of(s, l, up, sizeof up)) continue;
		int dup = 0;
		for (int i = 0; i < nseen; i++)
			if (strcmp(seen[i], up) == 0) { dup = 1; break; }
		if (dup) continue;
		if (nseen < 64) {
			memcpy(seen[nseen], up, l);
			seen[nseen][l] = 0;
			nseen++;
		}
		if (b->len - start > 380) break; /* keep the map compact */
		gbuf_addch(b, ' ');
		gbuf_addn(b, s, l);
		gbuf_adds(b, " |");
		emitted = 1;
	}
	if (emitted) {
		b->len -= 2; /* drop trailing " |" */
		b->p[b->len] = 0;
	}
	gbuf_addch(b, '\n');
}

/* ------------------------------------------------------------------ */
/* option-name recovery for failed option queries                     */
/* ------------------------------------------------------------------ */
static int edit_dist(const char *a, const char *b) {
	int la = (int)strlen(a), lb = (int)strlen(b);
	if (la >= 64 || lb >= 64) return 64;
	int dp[64][64];
	for (int i = 0; i <= la; i++) dp[i][0] = i;
	for (int j = 0; j <= lb; j++) dp[0][j] = j;
	for (int i = 1; i <= la; i++)
		for (int j = 1; j <= lb; j++) {
			int c = (a[i - 1] == b[j - 1]) ? 0 : 1;
			int m = dp[i - 1][j - 1] + c;
			if (dp[i - 1][j] + 1 < m) m = dp[i - 1][j] + 1;
			if (dp[i][j - 1] + 1 < m) m = dp[i][j - 1] + 1;
			dp[i][j] = m;
		}
	return dp[la][lb];
}

/* length of the longest common subsequence (order-preserving chars) */
static int lcs_len(const char *a, const char *b) {
	int la = (int)strlen(a), lb = (int)strlen(b);
	if (la >= 64 || lb >= 64) return 0;
	int dp[64][64];
	for (int i = 0; i <= la; i++) dp[i][0] = 0;
	for (int j = 0; j <= lb; j++) dp[0][j] = 0;
	for (int i = 1; i <= la; i++)
		for (int j = 1; j <= lb; j++)
			dp[i][j] = a[i - 1] == b[j - 1] ? dp[i - 1][j - 1] + 1
			                                : (dp[i - 1][j] > dp[i][j - 1] ? dp[i - 1][j]
			                                                                : dp[i][j - 1]);
	return dp[la][lb];
}

/* collect distinct option head tokens from lines in [a, e) */
#define MAX_OPT_CANDS 512
static int collect_option_names(const line *ls, int a, int e, char out[][64], int max) {
	int cnt = 0;
	for (int i = a; i < e && cnt < max; i++) {
		const char *x;
		size_t l = trimline(&ls[i], &x);
		if (l == 0 || x[0] != '-') continue;
		size_t pos = 0;
		while (pos < l) {
			while (pos < l && (x[pos] == ' ' || x[pos] == '\t' || x[pos] == ',')) pos++;
			if (pos >= l) break;
			if (x[pos] != '-') break; /* description begins */
			size_t t0 = pos;
			while (pos < l && x[pos] != ' ' && x[pos] != '\t' && x[pos] != ',') pos++;
			size_t tl = pos - t0;
			if (tl >= 64) tl = 63;
			/* skip prose references like "--retry. Normally, ..." that
			 * appear inside descriptions (token ends in sentence punct) */
			if (tl > 2 && (x[t0 + tl - 1] == '.' || x[t0 + tl - 1] == ';')) continue;
			char tok[64];
			memcpy(tok, x + t0, tl);
			tok[tl] = 0;
			int dup = 0;
			for (int c = 0; c < cnt; c++)
				if (strcmp(out[c], tok) == 0) { dup = 1; break; }
			if (!dup && cnt < max) {
				memcpy(out[cnt], tok, tl + 1);
				cnt++;
			}
		}
	}
	return cnt;
}

/* nearest real option names to a failed option query, capped at 5; only
 * names actually present in the page are ever returned. */
static void emit_nearest_options(gbuf *b, const line *ls, int n, const int *heads, int nh,
                                 const char *q) {
	(void)heads;
	(void)nh;
	/* The fixed matrices below are deliberately bounded.  For a very long
	 * unknown option, omit recovery suggestions rather than changing its
	 * similarity semantics by truncating it. */
	if (strlen(q) >= 64) return;
	/* scan the whole page: OPTIONS often continues in subsections (curl:
	 * OPTIONS then ALL OPTIONS), so section bounds are too narrow for a
	 * recovery hint. */
	int a = 0, e = n;
	char cand[MAX_OPT_CANDS][64];
	int nc = collect_option_names(ls, a, e, cand, MAX_OPT_CANDS);
	if (nc == 0) return;

	/* score = -edit distance, with a bonus for common prefix so a typed
	 * abbreviation like "--rcur" still ranks "--recursive" first. */
	int best[5];
	int nbest = 0;
	for (int k = 0; k < 5; k++) {
		int bi = -1, bs = INT_MIN;
		for (int i = 0; i < nc; i++) {
			int used = 0;
			for (int j = 0; j < nbest; j++)
				if (best[j] == i) { used = 1; break; }
			if (used) continue;
			int lcp = 0;
			while (q[lcp] && cand[i][lcp] && q[lcp] == cand[i][lcp]) lcp++;
			int sc = lcs_len(q, cand[i]) * 2 + lcp - edit_dist(q, cand[i]);
			if (sc > bs) { bs = sc; bi = i; }
		}
		if (bi < 0 || bs < -6) break; /* nothing close enough */
		best[nbest++] = bi;
	}
	if (nbest == 0) return;
	gbuf_adds(b, "NEARBY OPTIONS:");
	for (int k = 0; k < nbest; k++) {
		gbuf_addch(b, ' ');
		gbuf_adds(b, cand[best[k]]);
		if (k + 1 < nbest) gbuf_addch(b, ',');
	}
	gbuf_addch(b, '\n');
}

/* ------------------------------------------------------------------ */
/* fragment assembly with budget                                       */
/* ------------------------------------------------------------------ */
typedef struct {
	int a, b;
} range;
typedef struct {
	gbuf a; /* authoritative: never trimmed */
	range *opt;
	int nopt, cap;
} frag;

static void frag_init(frag *f) { memset(f, 0, sizeof *f); gbuf_init(&f->a); }
static void frag_free(frag *f) {
	gbuf_free(&f->a);
	free(f->opt);
	f->opt = NULL;
	f->nopt = f->cap = 0;
}
static void add_opt_range(frag *f, const line *ls, int a, int e) {
	if (a >= e) return;
	if (f->nopt == f->cap) {
		f->cap = f->cap ? f->cap * 2 : 8;
		f->opt = realloc(f->opt, (size_t)f->cap * sizeof *f->opt);
	}
	f->opt[f->nopt].a = a;
	f->opt[f->nopt].b = e;
	f->nopt++;
	(void)ls;
}
static size_t range_bytes(const line *ls, range r) {
	size_t s = 0;
	for (int i = r.a; i < r.b; i++) s += ls[i].len + 1;
	return s;
}

/* ------------------------------------------------------------------ */
/* option entry extraction                                             */
/* ------------------------------------------------------------------ */
/* match a single head token against a query option; allows =ARG and [=ARG]
 * suffixes so `--strip-components` matches `--strip-components=NUMBER`. */
static int opt_token_matches(const char *tok, size_t tl, const char *opt) {
	size_t ol = strlen(opt);
	if (tl < ol) return 0;
	if (memcmp(tok, opt, ol) != 0) return 0;
	if (tl == ol) return 1;
	char c = tok[ol];
	return c == '=' || c == '[';
}

/* does the head of an option entry line (the comma-separated option tokens
 * before the description) contain `opt`?  textual occurrences in the
 * description never match, so `-r, --recursive` answers both `-r` and
 * `--recursive`. */
static int opt_in_head(const char *s, size_t l, const char *opt) {
	size_t i = 0;
	while (i < l) {
		while (i < l && (s[i] == ' ' || s[i] == '\t' || s[i] == ',')) i++;
		if (i >= l) break;
		if (s[i] != '-') break; /* description begins */
		size_t t0 = i;
		while (i < l && s[i] != ' ' && s[i] != '\t' && s[i] != ',') i++;
		if (opt_token_matches(s + t0, i - t0, opt)) return 1;
	}
	return 0;
}

/* a line whose head starts with the option: a dedicated entry like
 * "-r, --recursive  ..." or "-fPIC  ...".  preferred over cluster lines
 * where the option is buried mid-head ("... -fpic -fPIC -fpie ..."). */
static int head_starts_with(const char *s, size_t l, const char *opt) {
	size_t i = 0;
	while (i < l && (s[i] == ' ' || s[i] == '\t' || s[i] == ',')) i++;
	if (i >= l || s[i] != '-') return 0;
	size_t t0 = i;
	while (i < l && s[i] != ' ' && s[i] != '\t' && s[i] != ',') i++;
	return opt_token_matches(s + t0, i - t0, opt);
}

static int option_entry(const line *ls, int n, const char *opt, int *start, int *end) {
	int s = -1;
	for (int i = 0; i < n; i++) {
		const char *x;
		size_t l = trimline(&ls[i], &x);
		if (head_starts_with(x, l, opt)) { s = i; break; }
	}
	if (s < 0)
		for (int i = 0; i < n; i++) {
			const char *x;
			size_t l = trimline(&ls[i], &x);
			if (opt_in_head(x, l, opt)) { s = i; break; }
		}
	if (s < 0) return 0;
	int eind = ls[s].indent;
	int e = s + 1;
	for (; e < n; e++) {
		const char *x;
		size_t l = trimline(&ls[e], &x);
		if (l == 0) continue;
		if (ls[e].indent <= eind && x[0] == '-') break;
		if (is_top_heading(x, l)) break;
		if (ls[e].indent <= eind && !grammar_marker(x, l) && l > 40) break;
	}
	*start = s;
	*end = e;
	return 1;
}

/* a bundled short-option token like "-rin" decomposes into single-letter
 * shorts ("-r -i -n"); only decompose when every letter is a real option
 * entry in the page, so unknown letters are never invented. */
static int decompose_short_bundle(const line *ls, int n, const char *tok, char *comps[8]) {
	if (tok[0] != '-' || tok[1] == '-' || tok[1] == 0) return 0;
	size_t l = strlen(tok);
	if (l < 3) return 0;
	int ncomps = 0;
	for (size_t i = 1; i < l; i++) {
		if (!isalpha((unsigned char)tok[i])) return 0;
		char one[3] = {'-', tok[i], 0};
		int s, e;
		if (!option_entry(ls, n, one, &s, &e)) return 0;
		if (ncomps < 8) {
			comps[ncomps] = strdup(one);
			if (comps[ncomps]) ncomps++;
		}
	}
	return ncomps > 1;
}

static int extract_option(const line *ls, int n, const char *opt, frag *f) {
	int s, e;
	if (!option_entry(ls, n, opt, &s, &e)) return 0;
	emit_range(&f->a, ls, s, e);

	char var[64];
	size_t ol = strlen(opt);
	int differs = 0;
	if (ol < sizeof var) {
		for (size_t i = 0; i < ol; i++) {
			char c = opt[i];
			var[i] = islower((unsigned char)c) ? (char)toupper((unsigned char)c)
			         : (isupper((unsigned char)c) ? (char)tolower((unsigned char)c) : c);
			if (var[i] != c) differs = 1;
		}
		var[ol] = 0;
	}
	if (differs) {
		int s2, e2;
		if (option_entry(ls, n, var, &s2, &e2) && s2 != s) add_opt_range(f, ls, s2, e2);
	}

	/* sibling cluster: lines sharing the option as a prefix plus '-', e.g. --delete-* */
	if (opt[0] == '-' && opt[1] == '-') {
		size_t pl = strlen(opt);
		int q = e, saw = 0;
		for (; q < n; q++) {
			const char *x;
			size_t xl = trimline(&ls[q], &x);
			if (xl == 0) { if (saw) break; continue; }
			if (ls[q].indent < ls[s].indent) break;
			if (xl > pl && memcmp(x, opt, pl) == 0 && x[pl] == '-') {
				add_opt_range(f, ls, q, q + 1);
				saw = 1;
			} else break;
		}
	}
	return 1;
}

/* ------------------------------------------------------------------ */
/* subcommand / statement anchor extraction                            */
/* ------------------------------------------------------------------ */
/* add the first prose paragraph after `from` that contains the anchor word */
static void add_statement_explanation(frag *f, const line *ls, int n, int from, const char *anchor) {
	for (int i = from; i < n; i++) {
		const char *s;
		size_t l = trimline(&ls[i], &s);
		if (l == 0) continue;
		if (is_top_heading(s, l)) return;
		if (grammar_marker(s, l)) continue;
		if (has_word(s, l, anchor)) {
			int p0 = i;
			while (p0 > 0 && !ls[p0 - 1].blank) p0--;
			int p1 = i + 1;
			while (p1 < n && !ls[p1].blank) p1++;
			if (p1 - i <= 12) add_opt_range(f, ls, p0, p1);
			return;
		}
	}
}

/* subcommand description block, e.g. "ip link set - change device attributes":
 * find the line headed "CMD ANCHOR - ..." and take the following option entries. */
static void add_subcommand_desc(frag *f, const line *ls, int n, int from, const char *cmd,
                                const char *anchor) {
	if (!cmd) return;
	for (int i = from; i < n; i++) {
		const char *x;
		size_t xl = trimline(&ls[i], &x);
		if (xl == 0) continue;
		if (is_top_heading(x, xl)) return;
		if (!starts_word(x, xl, cmd)) continue;
		if (!has_word(x, xl, anchor)) continue;
		const char *dash = strstr(x, " - ");
		if (!dash || dash == x) continue;
		size_t pre = (size_t)(dash - x);
		char prefix[128];
		if (pre >= sizeof prefix) continue;
		memcpy(prefix, x, pre);
		prefix[pre] = 0;
		if (!has_word(prefix, pre, anchor)) continue;
		int added = 0;
		for (int q = i + 1; q < n && added < 3; q++) {
			const char *y;
			size_t yl = trimline(&ls[q], &y);
			if (yl == 0) continue;
			if (is_top_heading(y, yl)) break;
			if (grammar_marker(y, yl)) continue;
			if (ls[q].indent > 0 && islower((unsigned char)y[0])) {
				int p0 = q;
				while (p0 > 0 && !ls[p0 - 1].blank) p0--;
				int p1 = q + 1;
				while (p1 < n && !ls[p1].blank) p1++;
				if (p1 - q <= 8) {
					add_opt_range(f, ls, p0, p1);
					added++;
					q = p1 - 1;
				}
			}
		}
		return;
	}
}

static int extract_anchor(const line *ls, int n, const int *heads, int nh, const char *anchor,
                          const char *cmd, frag *f) {
	int syn = find_section(ls, n, heads, nh, "SYNOPSIS");
	int syn_end = syn >= 0 ? section_end(heads, nh, syn) : -1;
	int best = -1, bestsc = INT_MIN;
	for (int i = 0; i < n; i++) {
		const char *s;
		size_t l = trimline(&ls[i], &s);
		if (l == 0) continue;
		if (is_top_heading(s, l)) continue;
		if (is_diagram_line(s, l)) continue;
		if (!has_word(s, l, anchor)) continue;
		int sc = 0;
		if (grammar_marker(s, l)) {
			sc += 70;
			if (starts_word(s, l, anchor)) sc += 30;
			if (syn >= 0 && i >= syn && (syn_end < 0 || i < syn_end)) sc += 40;
			if (l <= 100) sc += 10;
			if (ls[i].indent <= 6) sc += 10;
		} else {
			sc += 15;
			if (starts_word(s, l, anchor)) sc += 5;
		}
		if (l > 140) sc -= 20;
		if (sc > bestsc) { bestsc = sc; best = i; }
	}
	if (best < 0) return 0;

	const char *s;
	size_t l = trimline(&ls[best], &s);
	int g = grammar_marker(s, l);
	if (g) {
		/* grammar block: anchor line + contiguous grammar/continuation lines.
		 * whole lines only; a generous cap keeps huge grammars (ip-route) compact. */
		const int CAP = 20;
		int end = best + 1;
		for (; end < n && end - best <= CAP; end++) {
			const char *x;
			size_t xl = trimline(&ls[end], &x);
			if (xl == 0) {
				int k = end + 1;
				while (k < n && ls[k].blank) k++;
				if (k < n) {
					const char *y;
					size_t yl = trimline(&ls[k], &y);
					if (!grammar_marker(y, yl)) break;
				}
				continue;
			}
			if (is_top_heading(x, xl)) break;
			if (cmd && starts_word(x, xl, cmd)) {
				if (!has_word(x, xl, anchor)) break;
			}
			if (!grammar_marker(x, xl)) break;
		}
		emit_range(&f->a, ls, best, end);
		/* related material */
		if (starts_word(s, l, anchor))
			add_statement_explanation(f, ls, n, end, anchor);
		add_subcommand_desc(f, ls, n, best + 1, cmd, anchor);
	} else {
		int p0 = best;
		while (p0 > 0 && !ls[p0 - 1].blank) p0--;
		int p1 = best + 1;
		while (p1 < n && !ls[p1].blank) p1++;
		emit_range(&f->a, ls, p0, p1);
	}
	return 1;
}

/* ------------------------------------------------------------------ */
/* /usr/share/doc source (source="doc")                                */
/* ------------------------------------------------------------------ */
#define DOC_BUDGET_BYTES 1500 /* ~375 tokens; normal doc replies stay small */
#define DOC_MAX_FILES 48
#define DOC_MAX_READ (256u * 1024u)

typedef struct {
	char path[512];
	char name[128];
} docfile;

/* supported: plain, .txt, .md, and .html via the small tag stripper below.
 * legal/boilerplate changelogs are excluded from candidates. */
static int doc_file_kind(const char *name) {
	const char *base = strrchr(name, '/');
	base = base ? base + 1 : name;
	static const char *skip[] = {"LICENSE", "COPYING", "ChangeLog", "ChangeLog.", "AUTHORS",
	                             "INSTALL", "NEWS", "README.CHANGES", NULL};
	for (int i = 0; skip[i]; i++)
		if (strncasecmp(base, skip[i], strlen(skip[i])) == 0) return 0;
	const char *dot = strrchr(base, '.');
	if (!dot) return 1; /* no extension: plain text (README, FAQ, INTRO) */
	if (strcmp(dot, ".txt") == 0) return 1;
	if (strcmp(dot, ".md") == 0 || strcmp(dot, ".markdown") == 0) return 2;
	if (strcmp(dot, ".html") == 0 || strcmp(dot, ".htm") == 0) return 3;
	return 0;
}

/* cheap local tag stripper: drops <...> and decodes the common entities. */
static void doc_strip_html(const char *in, size_t len, gbuf *out) {
	int in_tag = 0;
	for (size_t i = 0; i < len; i++) {
		if (in_tag) {
			if (in[i] == '>') in_tag = 0;
			continue;
		}
		if (in[i] == '<') { in_tag = 1; continue; }
		if (in[i] == '&') {
			if (len - i >= 4 && strncmp(in + i, "&lt;", 4) == 0) { gbuf_addch(out, '<'); i += 3; continue; }
			if (len - i >= 4 && strncmp(in + i, "&gt;", 4) == 0) { gbuf_addch(out, '>'); i += 3; continue; }
			if (len - i >= 5 && strncmp(in + i, "&amp;", 5) == 0) { gbuf_addch(out, '&'); i += 4; continue; }
			if (len - i >= 6 && strncmp(in + i, "&nbsp;", 6) == 0) { gbuf_addch(out, ' '); i += 5; continue; }
			if (len - i >= 6 && strncmp(in + i, "&quot;", 6) == 0) { gbuf_addch(out, '"'); i += 5; continue; }
			if (len - i >= 5 && strncmp(in + i, "&#39;", 5) == 0) { gbuf_addch(out, '\''); i += 4; continue; }
			gbuf_addch(out, '&');
			continue;
		}
		gbuf_addch(out, in[i]);
	}
}

/* optional package-manager-assisted package-name lookup.  pacman today;
 * apk/others slot in as extra branches below.  absence of the helper is not
 * an error, it just falls through. */
static int pkg_owner_dir(const char *cmd, char *dir, size_t sz) {
	static const char *bindirs[] = {"/usr/bin/", "/bin/", "/usr/sbin/", "/sbin/", NULL};
	for (int i = 0; bindirs[i]; i++) {
		char path[512];
		snprintf(path, sizeof path, "%s%s", bindirs[i], cmd);
		if (access(path, X_OK) != 0) continue;
		char *av[] = {"pacman", "-Qqo", path, NULL};
		char *out = NULL;
		size_t ol;
		int st;
		if (run_cmd(av, &out, &ol, &st) < 0 || st != 0) { free(out); continue; }
		size_t l = 0;
		while (l < ol && out[l] != '\n' && out[l] != '\r' && out[l] != ' ') l++;
		if (l > 0 && l < sz - 1) {
			char pkg[128];
			memcpy(pkg, out, l);
			pkg[l] = 0;
			free(out);
			char root[PATH_MAX], candidate[PATH_MAX], canon[PATH_MAX];
			if (!realpath("/usr/share/doc", root) ||
			    snprintf(candidate, sizeof candidate, "%s/%s", root, pkg) >= (int)sizeof candidate)
				continue;
			struct stat st2;
			if (stat(candidate, &st2) == 0 && S_ISDIR(st2.st_mode) &&
			    canonical_under(candidate, root, canon, sizeof canon) &&
			    strlen(canon) + 1 <= sz) {
				strcpy(dir, canon);
				return 1;
			}
			return 0;
		}
		free(out);
	}
	return 0;
}

static int find_doc_dir(const char *pkg, char *dir, size_t sz) {
	char root[PATH_MAX];
	if (!safe_relative_path(pkg, 0) || !realpath("/usr/share/doc", root)) return 0;
	if (snprintf(dir, sz, "%s/%s", root, pkg) >= (int)sz) return 0;
	struct stat st;
	char canon[PATH_MAX];
	if (stat(dir, &st) == 0 && S_ISDIR(st.st_mode) &&
	    canonical_under(dir, root, canon, sizeof canon)) {
		if (strlen(canon) + 1 > sz) return 0;
		strcpy(dir, canon);
		return 1;
	}
	return pkg_owner_dir(pkg, dir, sz);
}

static int list_doc_files(const char *dir, docfile *files, int max) {
	DIR *d = opendir(dir);
	if (!d) return 0;
	struct dirent *de;
	int n = 0;
	while ((de = readdir(d)) && n < max) {
		if (de->d_name[0] == '.') continue;
		if (!doc_file_kind(de->d_name)) continue;
		size_t dl = strlen(dir), nl = strlen(de->d_name);
		if (dl + nl + 2 > sizeof files[n].path) continue;
		char candidate[PATH_MAX];
		if (snprintf(candidate, sizeof candidate, "%s/%s", dir, de->d_name) >= (int)sizeof candidate)
			continue;
		struct stat st;
		if (lstat(candidate, &st) != 0 || !S_ISREG(st.st_mode)) continue;
		char canon[PATH_MAX], root[PATH_MAX];
		if (!canonical_under(candidate, dir, canon, sizeof canon) ||
		    !realpath(dir, root) || !path_under(canon, root) ||
		    strlen(canon) + 1 > sizeof files[n].path) continue;
		strcpy(files[n].path, canon);
		strncpy(files[n].name, de->d_name, sizeof files[n].name - 1);
		files[n].name[sizeof files[n].name - 1] = 0;
		n++;
	}
	closedir(d);
	return n;
}

static int doc_read_file(const char *path, gbuf *out) {
	int flags = O_RDONLY;
#ifdef O_CLOEXEC
	flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
	flags |= O_NOFOLLOW;
#endif
	int fd = open(path, flags);
	if (fd < 0) return 0;
	struct stat st;
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) { close(fd); return 0; }
	FILE *f = fdopen(fd, "rb");
	if (!f) { close(fd); return 0; }
	char buf[8192];
	size_t r;
	while (out->len < DOC_MAX_READ && (r = fread(buf, 1, sizeof buf, f)) > 0) {
		size_t want = r;
		if (out->len + want > DOC_MAX_READ) want = DOC_MAX_READ - out->len;
		gbuf_addn(out, buf, want);
	}
	fclose(f);
	return out->len > 0;
}

static int has_word_ci(const char *s, size_t l, const char *w) {
	size_t wl = strlen(w);
	if (wl == 0) return 0;
	for (size_t i = 0; i + wl <= l; i++) {
		if (strncasecmp(s + i, w, wl) == 0) {
			int bl = (i == 0) || !wb(s[i - 1]);
			int br = (i + wl == l) || !wb(s[i + wl]);
			if (bl && br) return 1;
		}
	}
	return 0;
}

static int is_doc_heading(const char *s, size_t l) {
	if (l == 0) return 0;
	if (s[0] == '#' || s[0] == '=') return 1; /* md / rst style */
	if (s[l - 1] == ':') return 1;            /* "Topic:" plain style */
	return 0;
}

/* find the smallest paragraph containing the most query terms; prefer one
 * headed by a matching heading line.  whole lines only, budget capped.
 * returns the number of distinct query-term matches found, 0 if none. */
static int doc_find_paragraph(const line *ls, int n, const char **terms, int nt, gbuf *out) {
	int i = 0;
	int best = -1, bestsc = INT_MIN, besta = 0, bestb = 0, bestmatched = 0;
	while (i < n) {
		while (i < n && ls[i].blank) i++;
		if (i >= n) break;
		int a = i;
		while (i < n && !ls[i].blank) i++;
		int b = i;
		int matched = 0;
		size_t bytes = 0;
		for (int j = a; j < b; j++) {
			const char *s;
			size_t l = trimline(&ls[j], &s);
			for (int k = 0; k < nt; k++)
				if (has_word_ci(s, l, terms[k])) matched++;
			bytes += l;
		}
		if (matched == 0) continue;
		const char *s;
		size_t l = trimline(&ls[a], &s);
		int head = is_doc_heading(s, l);
		int sc = matched * 100000 + (head ? 50000 : 0) - (int)bytes - a;
		if (sc > bestsc) { bestsc = sc; best = a; besta = a; bestb = b; bestmatched = matched; }
	}
	(void)best;
	if (best < 0) return 0;
	for (int j = besta; j < bestb && out->len < DOC_BUDGET_BYTES; j++) {
		gbuf_addn(out, ls[j].s, ls[j].len);
		gbuf_addch(out, '\n');
	}
	return bestmatched;
}

/* case-insensitive filename score: file whose name contains a term wins. */
static int doc_file_score(const char *name, const char **terms, int nt) {
	char lc[128];
	size_t l = strlen(name);
	for (size_t i = 0; i < l && i < sizeof lc - 1; i++) lc[i] = (char)tolower((unsigned char)name[i]);
	lc[l] = 0;
	int sc = 0;
	for (int k = 0; k < nt; k++) {
		char tl[64];
		size_t tlk = strlen(terms[k]);
		if (tlk >= sizeof tl) tlk = sizeof tl - 1;
		for (size_t i = 0; i < tlk; i++) tl[i] = (char)tolower((unsigned char)terms[k][i]);
		tl[tlk] = 0;
		if (tlk && strstr(lc, tl)) sc += 2000;
	}
	if (strncmp(lc, "readme", 6) == 0) sc += 300;
	if (strncmp(lc, "faq", 3) == 0) sc += 200;
	if (strncmp(lc, "intro", 5) == 0) sc += 100;
	return sc;
}

static void doc_file_map(gbuf *out, const docfile *files, int nfiles) {
	gbuf_adds(out, "DOC FILES:");
	int shown = 0;
	for (int i = 0; i < nfiles && shown < 12; i++) {
		if (strncasecmp(files[i].name, "readme", 6) != 0 &&
		    strncasecmp(files[i].name, "faq", 3) != 0 &&
		    strncasecmp(files[i].name, "intro", 5) != 0)
			continue;
		gbuf_addch(out, ' ');
		gbuf_adds(out, files[i].name);
		gbuf_adds(out, " |");
		shown++;
	}
	for (int i = 0; i < nfiles && shown < 12; i++) {
		if (strncasecmp(files[i].name, "readme", 6) == 0 ||
		    strncasecmp(files[i].name, "faq", 3) == 0 ||
		    strncasecmp(files[i].name, "intro", 5) == 0)
			continue;
		gbuf_addch(out, ' ');
		gbuf_adds(out, files[i].name);
		gbuf_adds(out, " |");
		shown++;
	}
	if (nfiles > shown) gbuf_adds(out, " ...");
	size_t end = out->len;
	if (end > 0 && out->p[end - 1] == '|') out->len--; /* drop trailing " |" */
	out->p[out->len] = 0;
	gbuf_addch(out, '\n');
}

static int akaman_run_doc(const char *query, result *res) {
	toks t;
	memset(&t, 0, sizeof t);
	if (!split_tokens(query, &t)) {
		gbuf_adds(&res->text, "no local doc match for \"");
		gbuf_adds(&res->text, query);
		gbuf_adds(&res->text, "\"\n");
		res->status = 1;
		return 1;
	}
	char pkg[128] = "";
	for (int i = 0; i < t.n; i++) {
		if (t.v[i][0] == '-') continue;
		int ok = 1;
		for (const char *p = t.v[i]; *p; p++)
			if (!pagechar((unsigned char)*p)) { ok = 0; break; }
		if (ok && has_alnum(t.v[i])) {
			strncpy(pkg, t.v[i], sizeof pkg - 1);
			pkg[sizeof pkg - 1] = 0;
			break;
		}
	}
	char dir[512];
	if (!pkg[0] || !find_doc_dir(pkg, dir, sizeof dir)) {
		gbuf_adds(&res->text, "no local doc match for \"");
		gbuf_adds(&res->text, query);
		gbuf_adds(&res->text, "\"\n");
		res->status = 1;
		toks_free(&t);
		return 1;
	}

	const char *terms[8];
	int nt = 0;
	int seen_pkg = 0;
	for (int i = 0; i < t.n; i++) {
		int isopt = t.v[i][0] == '-';
		int ispage = 1;
		for (const char *p = t.v[i]; *p; p++)
			if (!pagechar((unsigned char)*p)) { ispage = 0; break; }
		if (!isopt && ispage && !seen_pkg && strcmp(t.v[i], pkg) == 0) { seen_pkg = 1; continue; }
		if (nt < 8) terms[nt++] = t.v[i];
	}

	strncpy(res->pagename, pkg, sizeof res->pagename - 1);
	res->pagename[sizeof res->pagename - 1] = 0;

	docfile files[DOC_MAX_FILES];
	int nfiles = list_doc_files(dir, files, DOC_MAX_FILES);
	if (nfiles == 0) {
		gbuf_adds(&res->text, "no local doc match for \"");
		gbuf_adds(&res->text, query);
		gbuf_adds(&res->text, "\"\n");
		res->status = 1;
		toks_free(&t);
		return 1;
	}

	if (nt == 0) {
		/* bare package query = map of the available doc files */
		gbuf_adds(&res->text, pkg);
		gbuf_adds(&res->text, "(doc)\n");
		doc_file_map(&res->text, files, nfiles);
		res->status = 0;
		toks_free(&t);
		return 0;
	}

	/* term query: scan every doc file once and keep the best paragraph.
	 * scoring is dominated by how many query terms the paragraph matches;
	 * filename match and README/FAQ/INTRO preference are tiebreakers. */
	int bestf = -1, bestsc = INT_MIN;
	gbuf best;
	gbuf_init(&best);
	size_t best_full = 0;
	for (int i = 0; i < nfiles; i++) {
		int fsc = doc_file_score(files[i].name, terms, nt);
		gbuf raw;
		gbuf_init(&raw);
		if (!doc_read_file(files[i].path, &raw)) { gbuf_free(&raw); continue; }
		gbuf content;
		gbuf_init(&content);
		if (doc_file_kind(files[i].name) == 3)
			doc_strip_html(raw.p, raw.len, &content);
		else
			gbuf_addn(&content, raw.p, raw.len);
		gbuf_free(&raw);
		int n = 0;
		line *ls = build_lines(content.p, content.len, &n);
		gbuf para;
		gbuf_init(&para);
		int pm = doc_find_paragraph(ls, n, terms, nt, &para);
		if (pm > 0) {
			int sc = pm * 100000 + fsc - (int)para.len;
			if (sc > bestsc) {
				bestsc = sc;
				bestf = i;
				gbuf_free(&best);
				best.p = para.p;
				best.len = para.len;
				best.cap = para.cap;
				para.p = NULL;
				best_full = content.len;
			}
		}
		gbuf_free(&para);
		free(ls);
		gbuf_free(&content);
	}
	toks_free(&t);

	if (bestf < 0) {
		gbuf_adds(&res->text, "no match for \"");
		gbuf_adds(&res->text, query);
		gbuf_adds(&res->text, "\" in ");
		gbuf_adds(&res->text, pkg);
		gbuf_adds(&res->text, "(doc)\n");
		doc_file_map(&res->text, files, nfiles);
		res->status = 2;
		gbuf_free(&best);
		return 0;
	}

	gbuf_adds(&res->text, pkg);
	gbuf_addch(&res->text, '/');
	gbuf_adds(&res->text, files[bestf].name);
	gbuf_addch(&res->text, '\n');
	gbuf_addn(&res->text, best.p, best.len);
	gbuf_free(&best);
	res->full_bytes = best_full;
	strncpy(res->pagesec, files[bestf].name, sizeof res->pagesec - 1);
	res->pagesec[sizeof res->pagesec - 1] = 0;
	res->status = 0;
	return 0;
}

/* ------------------------------------------------------------------ */
/* headers source (source="headers")                                   */
/* ------------------------------------------------------------------ */
/* Locate the system include root.  On macOS the SDK path is dynamic
 * (xcrun --show-sdk-path), falling back to the legacy root; everywhere
 * else /usr/include.  No hardcoded SDK path: xcrun is authoritative. */
static int find_include_root(char *root, size_t sz) {
	{
		char *av[] = {"xcrun", "--show-sdk-path", NULL};
		char *out = NULL;
		size_t ol;
		int st;
		int rc = run_cmd(av, &out, &ol, &st);
		if (rc >= 0 && st == 0) {
			size_t l = 0;
			while (l < ol && out[l] != '\n' && out[l] != '\r') l++;
			int ok = l > 0 && l < sz - (size_t)14;
			if (ok) {
				memcpy(root, out, l);
				root[l] = 0;
				struct stat st2;
				if (stat(root, &st2) != 0 || !S_ISDIR(st2.st_mode)) ok = 0;
			}
			free(out);
			if (ok) {
				size_t rl = strlen(root);
				snprintf(root + rl, sz - rl, "/usr/include");
				struct stat st2;
				return stat(root, &st2) == 0 && S_ISDIR(st2.st_mode);
			}
		}
	}
	/* fallbacks */
	static const char *cands[] = {
		"/usr/include",
		"/usr/local/include",
		NULL};
	for (int i = 0; cands[i]; i++) {
		struct stat st2;
		if (stat(cands[i], &st2) == 0 && S_ISDIR(st2.st_mode)) {
			snprintf(root, sz, "%s", cands[i]);
			return 1;
		}
	}
	return 0;
}

/* a header query names a concrete header first (stdio, stdio.h, sys/socket,
 * sys/socket.h); optional trailing tokens are symbols to find inside it. */
static int header_file_exists(const char *root, const char *hdr, char *path, size_t sz) {
	char rootcanon[PATH_MAX];
	if (!safe_relative_path(hdr, 1) || !realpath(root, rootcanon)) return 0;
	size_t rl = strlen(rootcanon), hl = strlen(hdr);
	if (rl + 1 + hl + 5 + 1 >= sz) return 0;
	memcpy(path, rootcanon, rl);
	path[rl] = '/';
	memcpy(path + rl + 1, hdr, hl);
	path[rl + 1 + hl] = 0;
	struct stat st;
	char canon[PATH_MAX];
	if (stat(path, &st) == 0 && S_ISREG(st.st_mode) &&
	    canonical_under(path, rootcanon, canon, sizeof canon) && strlen(canon) + 1 <= sz) {
		strcpy(path, canon);
		return 1;
	}
	/* assume ".h" when no dot is present */
	if (!strrchr(hdr, '.')) {
		memcpy(path + rl + 1 + hl, ".h", 3);
		if (stat(path, &st) == 0 && S_ISREG(st.st_mode) &&
		    canonical_under(path, rootcanon, canon, sizeof canon) && strlen(canon) + 1 <= sz) {
			strcpy(path, canon);
			return 1;
		}
	}
	return 0;
}

/* find the smallest declaration block mentioning every symbol term:
 * scan declaration-looking lines (start of a line, or after "(" "," ";"),
 * expand forward to the closing ';' (braces for struct/define bodies).
 * whole lines only, budget capped. */
static int find_header_block(const line *ls, int n, const char **terms, int nt, gbuf *out) {
	int best = -1, bestsc = INT_MIN;
	for (int i = 0; i < n; i++) {
		const char *s;
		size_t l = trimline(&ls[i], &s);
		if (l == 0) continue;
		int matched = 0;
		for (int k = 0; k < nt; k++)
			if (has_word(s, l, terms[k])) matched++;
		if (matched != nt) continue;
		/* score: strong if the line itself starts a declaration */
		int sc = matched * 100000;
		if (s[0] == '#') sc += 5000;             /* #define / #include */
		if (i == 0 || ls[i - 1].blank) sc += 20000; /* declaration start */
		if (l <= 100) sc += 5000;
		sc -= (int)l;
		if (sc > bestsc) { bestsc = sc; best = i; }
	}
	if (best < 0) return 0;
	int a = best;
	/* climb to the top of the declaration block: previous lines that are
	 * not blank and do not end in ';' '}' ')' belong to the same decl */
	while (a > 0) {
		const char *s;
		size_t l = trimline(&ls[a - 1], &s);
		if (l == 0) break;
		if (s[l - 1] == ';' || s[l - 1] == '}' || s[l - 1] == ')') break;
		a--;
		if (best - a > 6) break;
	}
	int b = best + 1;
	int brace = 0;
	for (int i = best; i < n; i++) {
		const char *x;
		size_t xl = trimline(&ls[i], &x);
		for (size_t j = 0; j < xl; j++) {
			if (x[j] == '{') brace++;
			else if (x[j] == '}') brace--;
		}
		b = i + 1;
		if (x[0] == '#') continue; /* #define body continues until backslash-\n or blank */
		if (brace > 0) continue;
		if (i > best && xl > 0 && x[xl - 1] == ';') break;
		if (i > best && xl == 0) break;
		if (i - best > 40) break;
	}
	for (int i = a; i < b; i++) {
		if (ls[i].len + 1 > DOC_BUDGET_BYTES - out->len) {
			out->len = 0;
			if (out->p) out->p[0] = 0;
			return 0;
		}
		gbuf_addn(out, ls[i].s, ls[i].len);
		gbuf_addch(out, '\n');
	}
	if (brace > 0) {
		out->len = 0;
		if (out->p) out->p[0] = 0;
		return 0;
	}
	return 1;
}

static int akaman_run_headers(const char *query, result *res) {
	char root[512];
	if (!find_include_root(root, sizeof root)) {
		gbuf_adds(&res->text, "no local include root found (xcrun and /usr/include)\n");
		res->status = 1;
		return 1;
	}
	toks t;
	memset(&t, 0, sizeof t);
	if (!split_tokens(query, &t)) {
		gbuf_adds(&res->text, "no header match for \"");
		gbuf_adds(&res->text, query);
		gbuf_adds(&res->text, "\"\n");
		res->status = 1;
		return 1;
	}
	/* first token = header name; rest = symbols */
	char hdr[256] = "";
	for (int i = 0; i < t.n; i++) {
		int ok = 1;
		for (const char *p = t.v[i]; *p; p++) {
			unsigned char c = (unsigned char)*p;
			if (c == '/') continue;
			if (!(isalnum(c) || c == '_' || c == '-' || c == '.')) { ok = 0; break; }
		}
		if (ok && has_alnum(t.v[i])) {
			strncpy(hdr, t.v[i], sizeof hdr - 1);
			hdr[sizeof hdr - 1] = 0;
			break;
		}
	}
	const char *terms[8];
	int nt = 0;
	int seenhdr = 0;
	for (int i = 0; i < t.n; i++) {
		if (!seenhdr && strcmp(t.v[i], hdr) == 0) { seenhdr = 1; continue; }
		if (nt < 8) terms[nt++] = t.v[i];
	}
	if (!hdr[0]) {
		gbuf_adds(&res->text, "no header match for \"");
		gbuf_adds(&res->text, query);
		gbuf_adds(&res->text, "\" (name a header, e.g. stdio.h)\n");
		res->status = 1;
		toks_free(&t);
		return 1;
	}

	char path[600];
	if (!header_file_exists(root, hdr, path, sizeof path)) {
		gbuf_adds(&res->text, "no such header: ");
		gbuf_adds(&res->text, hdr);
		gbuf_adds(&res->text, "\n");
		res->status = 1;
		toks_free(&t);
		return 1;
	}
	/* canonical header name: what sits under the include root */
	const char *rel = path + strlen(root) + 1;
	{
		char canon[256];
		strncpy(canon, rel, sizeof canon - 1);
		canon[sizeof canon - 1] = 0;
		strncpy(hdr, canon, sizeof hdr - 1);
		hdr[sizeof hdr - 1] = 0;
		(void)0;
	}

	strncpy(res->pagename, hdr, sizeof res->pagename - 1);
	res->pagename[sizeof res->pagename - 1] = 0;
	gbuf raw;
	gbuf_init(&raw);
	if (!doc_read_file(path, &raw)) {
		gbuf_adds(&res->text, "could not read ");
		gbuf_adds(&res->text, path);
		gbuf_adds(&res->text, "\n");
		res->status = 1;
		toks_free(&t);
		gbuf_free(&raw);
		return 1;
	}
	res->full_bytes = raw.len;
	int n = 0;
	line *ls = build_lines(raw.p, raw.len, &n);

	if (nt == 0) {
		/* bare header query: show the file's guard + a small excerpt */
		gbuf_adds(&res->text, "#include <");
		gbuf_adds(&res->text, hdr);
		gbuf_adds(&res->text, ">\n");
		int shown = 0;
		for (int i = 0; i < n && shown < 12; i++) {
			const char *s;
			size_t l = trimline(&ls[i], &s);
			if (l == 0) continue;
			if (s[0] == '\'' || s[0] == '/') continue; /* comment */
			gbuf_addn(&res->text, ls[i].s, ls[i].len);
			gbuf_addch(&res->text, '\n');
			shown++;
		}
		res->status = 0;
		free(ls);
		gbuf_free(&raw);
		toks_free(&t);
		return 0;
	}

	gbuf res2;
	gbuf_init(&res2);
	if (!find_header_block(ls, n, terms, nt, &res2)) {
		gbuf_adds(&res->text, "no match for \"");
		gbuf_adds(&res->text, query);
		gbuf_adds(&res->text, "\" in <");
		gbuf_adds(&res->text, hdr);
		gbuf_adds(&res->text, ">\n");
		res->status = 2;
		free(ls);
		gbuf_free(&res2);
		gbuf_free(&raw);
		toks_free(&t);
		return 0;
	}
	gbuf_adds(&res->text, "#include <");
	gbuf_adds(&res->text, hdr);
	gbuf_adds(&res->text, ">\n");
	gbuf_addn(&res->text, res2.p, res2.len);
	gbuf_free(&res2);
	res->status = 0;
	free(ls);
	gbuf_free(&raw);
	toks_free(&t);
	return 0;
}

/* ------------------------------------------------------------------ */
/* top-level entry point                                               */
/* ------------------------------------------------------------------ */
void result_free(result *res) { gbuf_free(&res->text); }

int akaman_run(const char *query, const char *section, const char *source, result *res) {
	memset(res, 0, sizeof *res);
	gbuf_init(&res->text);
	if (!query || !query[0]) {
		gbuf_adds(&res->text, "no local man match for empty query\n");
		res->status = 1;
		return 1;
	}
	if (strlen(query) > AKAMAN_MAX_QUERY_BYTES) {
		gbuf_adds(&res->text, "query too large\n");
		res->status = 1;
		return 1;
	}
	if (section && section[0] && isdigit((unsigned char)section[0]) && strlen(section) >= 8) {
		gbuf_adds(&res->text, "section selector too large\n");
		res->status = 1;
		return 1;
	}
	if (source && strcmp(source, "doc") == 0) return akaman_run_doc(query, res);
	if (source && strcmp(source, "headers") == 0) return akaman_run_headers(query, res);
	if (source && strcmp(source, "man") != 0) {
		gbuf_adds(&res->text, "unknown source \"");
		gbuf_adds(&res->text, source);
		gbuf_adds(&res->text, "\" (use \"man\", \"doc\" or \"headers\")\n");
		res->status = 1;
		return 1;
	}

	int logical_section = 0;
	if (section && section[0] && !isdigit((unsigned char)section[0])) logical_section = 1;

	toks allt;
	memset(&allt, 0, sizeof allt);
	resolved_page rp;
	memset(&rp, 0, sizeof rp);
	if (!resolve_page(query, logical_section ? NULL : section, &rp, &allt)) {
		gbuf_adds(&res->text, "no local man match for \"");
		gbuf_adds(&res->text, query);
		gbuf_adds(&res->text, "\"\n");
		res->status = 1;
		return 1;
	}

	gbuf page;
	gbuf_init(&page);
	if (!render_page(&rp, &page)) {
		gbuf_adds(&res->text, "no local man match for \"");
		gbuf_adds(&res->text, query);
		gbuf_adds(&res->text, "\"\n");
		res->status = 1;
		gbuf_free(&page);
		toks_free(&allt);
		return 1;
	}
	res->full_bytes = page.len;
	strncpy(res->pagename, rp.name, sizeof res->pagename - 1);
	res->pagename[sizeof res->pagename - 1] = 0;
	strncpy(res->pagesec, rp.sec, sizeof res->pagesec - 1);
	res->pagesec[sizeof res->pagesec - 1] = 0;

	int n = 0;
	line *ls = build_lines(page.p, page.len, &n);
	int nh = 0;
	int *heads = build_headings(ls, n, &nh);

	const char *cmd = NULL;
	for (int i = 0; i < allt.n; i++) {
		if (allt.v[i][0] == '-') continue;
		int ok = 1;
		for (const char *p = allt.v[i]; *p; p++)
			if (!pagechar((unsigned char)*p)) { ok = 0; break; }
		if (ok) { cmd = allt.v[i]; break; }
	}

	char *opts[8];
	int nop = 0;
	char *anchors[8];
	int nan = 0;
	int seen = 0;
	for (int i = 0; i < allt.n; i++) {
		int isopt = allt.v[i][0] == '-';
		int ispage = 1;
		for (const char *p = allt.v[i]; *p; p++)
			if (!pagechar((unsigned char)*p)) { ispage = 0; break; }
		if (!isopt && ispage) {
			if (seen < rp.used) { seen++; continue; }
		}
		if (isopt && nop < 8) opts[nop++] = allt.v[i];
		else if (!isopt && ispage && nan < 8 && has_alnum(allt.v[i])) anchors[nan++] = allt.v[i];
	}

	frag f;
	frag_init(&f);
	int matched = 0;

	if (logical_section) {
		int h = find_section(ls, n, heads, nh, section);
		if (h >= 0) {
			int e = section_end(heads, nh, h);
			if (e > n) e = n;
			emit_section_budget(&f.a, ls, h, e);
			matched = 1;
		}
	} else if (nop == 0 && nan == 0) {
		int h = find_section(ls, n, heads, nh, "SYNOPSIS");
		if (h < 0) h = find_section(ls, n, heads, nh, "DESCRIPTION");
		if (h >= 0) {
			int e = section_end(heads, nh, h);
			if (e > n) e = n;
			emit_range(&f.a, ls, h, e);
			matched = 1;
		}
		/* bare query = map: append the page's real heading inventory */
		emit_sections_line(&f.a, ls, heads, nh);
	} else if (nop > 0) {
		for (int oi = 0; oi < nop && !matched; oi++) {
			if (extract_option(ls, n, opts[oi], &f)) { matched = 1; break; }
			char *comps[8] = {0};
			if (decompose_short_bundle(ls, n, opts[oi], comps)) {
				for (int ci = 0; comps[ci]; ci++) {
					if (extract_option(ls, n, comps[ci], &f)) matched = 1;
					free(comps[ci]);
				}
			}
		}
	} else if (nan > 0) {
		int h = find_section_by_tokens(ls, n, heads, nh, anchors, nan);
		if (h >= 0) {
			int e = section_end(heads, nh, h);
			if (e > n) e = n;
			emit_section_budget(&f.a, ls, h, e);
			matched = 1;
		} else if (extract_anchor(ls, n, heads, nh, anchors[nan - 1], cmd, &f)) {
			matched = 1;
		}
	}

	if (matched && (f.a.len == 0 || f.a.len > OUTPUT_BUDGET_BYTES)) matched = 0;
	if (!matched) {
		gbuf_adds(&res->text, "no match for \"");
		gbuf_adds(&res->text, query);
		gbuf_adds(&res->text, "\" in ");
		gbuf_adds(&res->text, rp.name);
		gbuf_addch(&res->text, '(');
		gbuf_adds(&res->text, rp.sec);
		gbuf_adds(&res->text, ")\n");
		/* tiny recovery hint: the page's available headings */
		emit_sections_line(&res->text, ls, heads, nh);
		/* option-looking query: suggest real nearby option names only */
		if (nop > 0) emit_nearest_options(&res->text, ls, n, heads, nh, opts[0]);
		res->status = 2;
	} else {
		gbuf_adds(&res->text, rp.name);
		gbuf_addch(&res->text, '(');
		gbuf_adds(&res->text, rp.sec);
		gbuf_adds(&res->text, ")\n");
		gbuf_addn(&res->text, f.a.p, f.a.len);
		for (int i = 0; i < f.nopt; i++) {
			range rr = f.opt[i];
			size_t ub = range_bytes(ls, rr);
			if (res->text.len <= OUTPUT_BUDGET_BYTES && ub <= OUTPUT_BUDGET_BYTES - res->text.len)
				emit_range(&res->text, ls, rr.a, rr.b);
		}
	}

	frag_free(&f);
	free(heads);
	free(ls);
	gbuf_free(&page);
	toks_free(&allt);
	return res->status;
}
