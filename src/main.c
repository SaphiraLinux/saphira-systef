#include "core.h"
#include "mcp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int parse_page(const char *s) {
	if (!s || !*s) return -1;
	long v = 0;
	for (const char *p = s; *p; p++) {
		if (*p < '0' || *p > '9') return -1;
		v = v * 10 + (*p - '0');
		if (v > 1000000) return -1;
	}
	if (v < 1) return -1;
	return (int)v;
}

static const char *usage =
    "sysref - minimal local man-page context for AI agents\n"
    "\n"
    "usage:\n"
    "  sysref QUERY [SECTION]         query native man pages (source=man)\n"
    "                                 SECTION numeric = man section, alphabetic = named section\n"
    "  sysref -s SECTION QUERY        same\n"
    "  sysref --source doc QUERY      query /usr/share/doc (README/FAQ/.txt/.md)\n"
    "  sysref --source headers QUERY  query system headers (stdio.h printf)\n"
    "  sysref --source api SYMBOL    declaration and local API documentation\n"
    "  sysref --source man QUERY      query native man pages (default)\n"
    "  sysref --stats QUERY [SECTION] print result plus token metrics\n"
    "  sysref --mcp                   MCP stdio server (tools: man, doc, api)\n"
    "  sysref --http[=ADDR] | --web[=ADDR]   webMCP HTTP server (default 127.0.0.1:8931)\n"
    "                                 requires APIKEY in /etc/sysref/sysref.conf\n"
    "  sysref --conf PATH             config file path (APIKEY for webMCP)\n"
    "  sysref --page N                  show page N of a multi-page reply (default 1)\n"
    "  sysref --bench                 benchmark table\n"
    "\n"
    "  QUERY forms: command [option] | command [section-name] | command subcommand\n"
    "    e.g. \"grep -r\"      -> the -r, --recursive option entry\n"
    "         \"grep -rin\"    -> bundled short options decomposed (-r -i -n)\n"
    "         \"grep examples\" -> the EXAMPLE section\n"
    "         \"grep exit status\" -> the EXIT STATUS section\n"
    "         \"passwd files\"  -> the FILES section\n"
    "\n"
    "  A bare command query returns the SYNOPSIS plus a one-line SECTIONS map\n"
    "  (\"SECTIONS: A | B | ...\") for progressive disclosure. A query that\n"
    "  matches nothing returns the available headings; option-like queries\n"
    "  (starting -/--) additionally list the nearest real option names.\n"
    "\n"
    "examples:\n"
    "  sysref \"ip link set\"\n"
    "  sysref \"rsync --delete\"\n"
    "  sysref -s 2 open\n";

int main(int argc, char **argv) {
	const char *conf = "/etc/sysref/sysref.conf";
	int mode = 0; /* 0 cli, 1 mcp, 2 http, 3 bench */
	char httpaddr[256] = "127.0.0.1:8931";
	const char *query = NULL;
	const char *pos[64];
	int npos = 0;
	gbuf joined;
	gbuf_init(&joined);
	const char *section = NULL;
	const char *source = NULL;
	int stats = 0;
	int page = 1;

	for (int i = 1; i < argc; i++) {
		char *a = argv[i];
		if (strcmp(a, "--mcp") == 0) mode = 1;
		else if (strcmp(a, "--bench") == 0) mode = 3;
		else if (strcmp(a, "--stats") == 0) stats = 1;
		else if (strncmp(a, "--source=", 9) == 0) source = a + 9;
		else if (strcmp(a, "--source") == 0 && i + 1 < argc) source = argv[++i];
		else if (strncmp(a, "--page=", 7) == 0) {
			page = parse_page(a + 7);
			if (page < 1) { fputs("sysref: invalid --page value (positive integer required)\n", stderr); return 2; }
		} else if (strcmp(a, "--page") == 0 && i + 1 < argc) {
			page = parse_page(argv[++i]);
			if (page < 1) { fputs("sysref: invalid --page value (positive integer required)\n", stderr); return 2; }
		}
		else if (strcmp(a, "--conf") == 0 && i + 1 < argc) conf = argv[++i];
		else if (strcmp(a, "--web") == 0 || strcmp(a, "--http") == 0) {
			mode = 2;
			if (i + 1 < argc && argv[i + 1][0] != '-')
				strncpy(httpaddr, argv[++i], sizeof httpaddr - 1);
		} else if (strncmp(a, "--http=", 7) == 0) {
			mode = 2;
			strncpy(httpaddr, a + 7, sizeof httpaddr - 1);
		} else if (strncmp(a, "--web=", 6) == 0) {
			mode = 2;
			strncpy(httpaddr, a + 6, sizeof httpaddr - 1);
		} else if (strcmp(a, "-s") == 0 && i + 1 < argc) {
			section = argv[++i];
		} else if (a[0] == '-' && a[1]) {
			fprintf(stderr, "sysref: unknown option: %s\n", a);
			fputs(usage, stderr);
			return 2;
		} else {
			if (npos == 64) { fputs("sysref: too many arguments\n", stderr); return 2; }
			pos[npos++] = a;
		}
	}

	if (source && strcmp(source, "man") != 0) {
		for (int i = 0; i < npos; i++) {
			if (i) gbuf_addch(&joined, ' ');
			gbuf_adds(&joined, pos[i]);
		}
		query = joined.p;
	} else {
		if (npos > (section ? 1 : 2)) { fputs("sysref: too many arguments\n", stderr); return 2; }
		if (npos) query = pos[0];
		if (npos == 2) section = pos[1];
	}

	if (mode == 1) return run_stdio();
	if (mode == 2) {
		char key[512] = {0};
		if (!conf_load(conf, key, sizeof key)) {
			fprintf(stderr, "sysref: webMCP requires APIKEY defined in %s\n", conf);
			return 1;
		}
		return run_http(httpaddr, key);
	}
	if (mode == 3) return run_bench();
	if (!query) {
		fputs(usage, stderr);
		return 2;
	}

	result res;
	memset(&res, 0, sizeof res);
	int st = sysref_run(query, section, source, page, &res);
	if (res.text.p) write(1, res.text.p, res.text.len);
	if (stats) {
		double reduction = res.full_bytes ? 100.0 * (1.0 - (double)res.text.len / (double)res.full_bytes) : 0.0;
		fprintf(stderr,
		        "page: %s(%s)\nfull bytes: %zu\nreturned bytes: %zu\nfull tokens(est): %zu\n"
		        "returned tokens(est): %zu\n",
		        res.pagename, res.pagesec, res.full_bytes, res.text.len,
		        EST_TOKENS(res.full_bytes), EST_TOKENS(res.text.len));
		if (reduction > 99.9) fprintf(stderr, "reduction: >99.9%%\n");
		else fprintf(stderr, "reduction: %.1f%%\n", reduction);
	}
	result_free(&res);
	gbuf_free(&joined);
	return st ? 1 : 0;
}
