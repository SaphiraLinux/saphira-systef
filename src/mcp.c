#include "mcp.h"
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <sys/time.h>
#include <fcntl.h>

/* ------------------------------------------------------------------ */
/* config: /etc/akaman/akamcp.conf  (APIKEY=...)                         */
/* ------------------------------------------------------------------ */
int conf_load(const char *path, char *key, size_t keysz) {
	FILE *f = fopen(path, "r");
	if (!f) return 0;
	char line[1024];
	key[0] = 0;
	while (fgets(line, sizeof line, f)) {
		char *p = line;
		while (*p == ' ' || *p == '\t') p++;
		if (*p == '#' || *p == 0 || *p == '\n') continue;
		size_t l = strlen(p);
		while (l > 0 && (p[l - 1] == '\n' || p[l - 1] == '\r')) p[--l] = 0;
		if (strncmp(p, "APIKEY=", 7) == 0) {
			strncpy(key, p + 7, keysz - 1);
			key[keysz - 1] = 0;
			fclose(f);
			return 1;
		}
	}
	fclose(f);
	return 0;
}

/* ------------------------------------------------------------------ */
/* JSON-RPC helpers                                                    */
/* ------------------------------------------------------------------ */
static void jsonrpc_result(gbuf *out, const char *idraw, size_t idlen, const char *result_json) {
	gbuf_adds(out, "{\"jsonrpc\":\"2.0\",\"id\":");
	gbuf_addn(out, idraw, idlen);
	gbuf_adds(out, ",\"result\":");
	gbuf_adds(out, result_json);
	gbuf_adds(out, "}");
}
static void jsonrpc_error(gbuf *out, const char *idraw, size_t idlen, int code, const char *msg) {
	gbuf_adds(out, "{\"jsonrpc\":\"2.0\",\"id\":");
	gbuf_addn(out, idraw, idlen);
	gbuf_adds(out, ",\"error\":{\"code\":");
	char tmp[32];
	snprintf(tmp, sizeof tmp, "%d", code);
	gbuf_adds(out, tmp);
	gbuf_adds(out, ",\"message\":");
	json_escape(out, msg, strlen(msg));
	gbuf_adds(out, "}}");
}
static void bad_id(gbuf *out, int code, const char *msg) {
	jsonrpc_error(out, "null", 4, code, msg);
}

/* parse one MCP JSON-RPC message; writes a response into resp if one is due */
static int handle_mcp(const char *body, size_t len, gbuf *resp) {
	if (len > MCP_MAX_REQUEST_BYTES) {
		bad_id(resp, -32600, "request too large");
		return 1;
	}
	jv *root = json_parse(body, len);
	if (!root) {
		bad_id(resp, -32700, "parse error");
		return 1;
	}
	jv *id = jv_get(root, "id");
	jv *method = jv_get(root, "method");
	if (!method) {
		jsonrpc_error(resp, id && id->raw ? id->raw : "null", id && id->raw ? id->rawlen : 4,
		              -32600, "invalid request");
		jv_free(root);
		return 1;
	}
	if (method->t != 3 || !method->str) {
		jsonrpc_error(resp, id && id->raw ? id->raw : "null", id && id->raw ? id->rawlen : 4,
		              -32600, "method must be a string");
		jv_free(root);
		return 1;
	}
	const char *m = method->str;

	if (strcmp(m, "notifications/initialized") == 0) {
		jv_free(root);
		return 0; /* notification: no response */
	}
	if (!id || !id->raw) {
		jv_free(root);
		return 0; /* other notifications: silent */
	}

	if (strcmp(m, "initialize") == 0) {
		const char *pv = "2025-03-26";
		jv *params = jv_get(root, "params");
		if (params) {
			jv *p = jv_get(params, "protocolVersion");
			if (p && p->str) pv = p->str;
		}
		gbuf r;
		gbuf_init(&r);
		gbuf_adds(&r, "{\"protocolVersion\":");
		json_escape(&r, pv, strlen(pv));
		gbuf_adds(&r,
		          ",\"capabilities\":{\"tools\":{\"listChanged\":false}},"
		          "\"serverInfo\":{\"name\":\"akaman\",\"version\":\"0.1.0\"}}");
		jsonrpc_result(resp, id->raw, id->rawlen, r.p);
		gbuf_free(&r);
	} else if (strcmp(m, "ping") == 0) {
		jsonrpc_result(resp, id->raw, id->rawlen, "{}");
	} else if (strcmp(m, "tools/list") == 0) {
		jsonrpc_result(resp, id->raw, id->rawlen, AKAMAN_TOOLS_LIST_JSON);
	} else if (strcmp(m, "tools/call") == 0) {
		jv *params = jv_get(root, "params");
		jv *args = params ? jv_get(params, "arguments") : NULL;
		jv *name = params ? jv_get(params, "name") : NULL;
		jv *q = args ? jv_get(args, "query") : NULL;
		jv *sec = args ? jv_get(args, "section") : NULL;
		jv *src = args ? jv_get(args, "source") : NULL;
		if ((name && (name->t != 3 || strcmp(name->str ? name->str : "", "man") != 0)) ||
		    !q || !q->str) {
			jsonrpc_error(resp, id->raw, id->rawlen, -32602, "invalid tool call arguments");
			jv_free(root);
			return 1;
		}
		if (strlen(q->str) > MCP_MAX_QUERY_BYTES) {
			jsonrpc_error(resp, id->raw, id->rawlen, -32602, "query too large");
			jv_free(root);
			return 1;
		}
		result res;
		memset(&res, 0, sizeof res);
		akaman_run(q->str, (sec && sec->str) ? sec->str : NULL,
		           (src && src->str) ? src->str : NULL, &res);
		gbuf txt;
		gbuf_init(&txt);
		gbuf_adds(&txt, "{\"content\":[{\"type\":\"text\",\"text\":");
		json_escape(&txt, res.text.p ? res.text.p : "", res.text.len);
		gbuf_adds(&txt, "}],\"isError\":false}");
		jsonrpc_result(resp, id->raw, id->rawlen, txt.p);
		gbuf_free(&txt);
		result_free(&res);
	} else {
		jsonrpc_error(resp, id->raw, id->rawlen, -32601, "method not found");
	}
	jv_free(root);
	return 1;
}

/* ------------------------------------------------------------------ */
/* stdio transport (MCP over stdio)                                    */
/* ------------------------------------------------------------------ */
static int find_content_length(const char *h, size_t hl, size_t *outlen) {
	*outlen = 0;
	for (size_t i = 0; i + 15 < hl; i++) {
		if (strncasecmp(h + i, "content-length:", 15) == 0) {
			size_t p = i + 15;
			while (p < hl && (h[p] == ' ' || h[p] == '\t')) p++;
			size_t v = 0;
			size_t start = p;
			while (p < hl && isdigit((unsigned char)h[p])) {
				if (v > (SIZE_MAX - (size_t)(h[p] - '0')) / 10) return -1;
				v = v * 10 + (size_t)(h[p] - '0');
				p++;
			}
			if (p == start || (p < hl && h[p] != '\r' && h[p] != '\n' && h[p] != ' ' && h[p] != '\t'))
				return -1;
			*outlen = v;
			return 1;
		}
	}
	return 0;
}

/* Extract one complete JSON-RPC message from the input buffer into body.
 * MCP stdio framing is newline-delimited JSON (each message one line);
 * LSP-style Content-Length framing is accepted as a fallback.
 * Returns 1 on success (*ndjson set to the framing used), 0 if more data
 * is needed. Never blocks: the caller feeds it as bytes arrive. */
static int read_one_message(gbuf *in, gbuf *body, int *ndjson) {
	if (in->len > MCP_MAX_REQUEST_BYTES + MCP_MAX_HEADER_BYTES) return -1;
	size_t i = 0;
	while (i < in->len && (in->p[i] == ' ' || in->p[i] == '\t')) i++;
	*ndjson = (i < in->len && in->p[i] == '{');

	if (*ndjson) {
		size_t nl = i;
		while (nl < in->len && in->p[nl] != '\n') nl++;
		if (nl >= in->len) return 0; /* partial line: wait for the newline */
		gbuf_addn(body, in->p + i, nl - i);
		memmove(in->p, in->p + nl + 1, in->len - (nl + 1));
		in->len -= (nl + 1);
		in->p[in->len] = 0;
		return 1;
	}

	/* Content-Length framing: headers until a blank line, then the body */
	size_t hs = 0, found = 0, hl = 0;
	for (hs = 0; hs + 1 < in->len; hs++) {
		if (in->p[hs] == '\r' && in->p[hs + 1] == '\n') {
			if (hs + 3 < in->len && in->p[hs + 2] == '\r' && in->p[hs + 3] == '\n') {
				hl = hs + 4; found = 1; break;
			}
		} else if (in->p[hs] == '\n' && in->p[hs + 1] == '\n') {
			hl = hs + 2; found = 1; break;
		}
	}
	if (!found) return 0;
	if (hl > MCP_MAX_HEADER_BYTES) return -1;
	size_t clen = 0;
	int cl = find_content_length(in->p, hl, &clen);
	if (cl != 1 || clen == 0 || clen > MCP_MAX_REQUEST_BYTES) return -1;
	if (clen > SIZE_MAX - hl || in->len < hl + clen) return 0;
	gbuf_addn(body, in->p + hl, clen);
	memmove(in->p, in->p + hl + clen, in->len - (hl + clen));
	in->len -= (hl + clen);
	in->p[in->len] = 0;
	return 1;
}

static int write_all(int fd, const char *p, size_t len) {
	for (size_t off = 0; off < len;) {
		ssize_t n = write(fd, p + off, len - off);
		if (n > 0) off += (size_t)n;
		else if (n < 0 && errno == EINTR) continue;
		else return 0;
	}
	return 1;
}

int run_stdio(void) {
	gbuf in;
	gbuf_init(&in);
	char t[8192];
	for (;;) {
		ssize_t n = read(0, t, sizeof t);
		if (n <= 0) break;
		gbuf_addn(&in, t, (size_t)n);
		if (in.failed) { gbuf_free(&in); return 1; }
		for (;;) {
			gbuf body;
			gbuf_init(&body);
			int ndjson = 0;
			int m = read_one_message(&in, &body, &ndjson);
			if (m < 0) { gbuf_free(&body); gbuf_free(&in); return 1; }
			if (m <= 0) { gbuf_free(&body); break; }
			gbuf resp;
			gbuf_init(&resp);
			if (handle_mcp(body.p, body.len, &resp)) {
				if (ndjson) {
					if (!write_all(1, resp.p ? resp.p : "", resp.len) || !write_all(1, "\n", 1)) {
						gbuf_free(&body); gbuf_free(&resp); gbuf_free(&in); return 1;
					}
				} else {
					char hl[64];
					int hlw = snprintf(hl, sizeof hl, "Content-Length: %zu\r\n\r\n", resp.len);
					if (hlw < 0 || !write_all(1, hl, (size_t)hlw) ||
					    !write_all(1, resp.p ? resp.p : "", resp.len)) {
						gbuf_free(&body); gbuf_free(&resp); gbuf_free(&in); return 1;
					}
				}
			}
			gbuf_free(&body);
			gbuf_free(&resp);
		}
	}
	gbuf_free(&in);
	return 0;
}

/* ------------------------------------------------------------------ */
/* webMCP: minimal HTTP transport with bearer API key                  */
/* ------------------------------------------------------------------ */
static int read_http_request(int c, char **body, size_t *bodylen, char *auth, size_t authsz) {
	gbuf hd;
	gbuf_init(&hd);
	char ch;
	ssize_t n;
	int found = 0;
	for (;;) {
		n = read(c, &ch, 1);
		if (n <= 0) break;
		gbuf_addch(&hd, ch);
		if (hd.len >= 4 && memcmp(hd.p + hd.len - 4, "\r\n\r\n", 4) == 0) { found = 1; break; }
		if (hd.len > MCP_MAX_HEADER_BYTES) break;
	}
	if (!found) { gbuf_free(&hd); return -1; }
	/* request line */
	char method[16], path[256];
	method[0] = path[0] = 0;
	{
		size_t i = 0;
		while (i < hd.len && hd.p[i] != ' ' && i < sizeof method - 1) { method[i] = hd.p[i]; i++; }
		method[i] = 0;
		while (i < hd.len && hd.p[i] == ' ') i++;
		size_t j = 0;
		while (i < hd.len && hd.p[i] != ' ' && j < sizeof path - 1) { path[j] = hd.p[i]; i++; j++; }
		path[j] = 0;
	}
	if (strcmp(method, "POST") != 0) {
		gbuf_free(&hd);
		/* signal non-POST so caller can 405 */
		return 0;
	}
	size_t clen = 0;
	if (find_content_length(hd.p, hd.len, &clen) != 1 ||
	    clen == 0 || clen > MCP_MAX_REQUEST_BYTES) {
		gbuf_free(&hd);
		return -1;
	}
	auth[0] = 0;
	/* extract Authorization header */
	for (size_t i = 0; i + 14 < hd.len; i++) {
		if (strncasecmp(hd.p + i, "authorization:", 14) == 0) {
			size_t p = i + 14;
			while (p < hd.len && hd.p[p] == ' ') p++;
			size_t q = p;
			while (q < hd.len && hd.p[q] != '\r' && hd.p[q] != '\n') q++;
			size_t al = q - p;
			if (al >= authsz) al = authsz - 1;
			memcpy(auth, hd.p + p, al);
			auth[al] = 0;
			break;
		}
	}
	gbuf_free(&hd);
	gbuf b;
	gbuf_init(&b);
	while (b.len < clen) {
		char t[8192];
		size_t want = clen - b.len;
		if (want > sizeof t) want = sizeof t;
		n = read(c, t, want);
		if (n <= 0) break;
		gbuf_addn(&b, t, (size_t)n);
	}
	if (b.len < clen) {
		gbuf_free(&b);
		return -1;
	}
	*body = b.p;
	*bodylen = b.len;
	(void)path;
	return 1;
}

static void http_send(int c, int code, const char *reason, const char *body, size_t bodylen) {
	char hdr[256];
	int hw = snprintf(hdr, sizeof hdr,
	                  "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n"
	                  "Content-Length: %zu\r\nConnection: close\r\n\r\n",
	                  code, reason, bodylen);
	/* Responses are small, but still handle short writes correctly. */
	for (size_t off = 0; off < (size_t)hw;) {
		ssize_t n = write(c, hdr + off, (size_t)hw - off);
		if (n > 0) off += (size_t)n;
		else if (n < 0 && errno == EINTR) continue;
		else break;
	}
	for (size_t off = 0; off < bodylen;) {
		ssize_t n = write(c, body + off, bodylen - off);
		if (n > 0) off += (size_t)n;
		else if (n < 0 && errno == EINTR) continue;
		else break;
	}
}

static void set_cloexec(int fd) {
	int flags = fcntl(fd, F_GETFD);
	if (flags >= 0) (void)fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

int run_http(const char *addr, const char *api_key) {
	char host[256] = "127.0.0.1";
	int port = 8931;
	const char *colon = strrchr(addr, ':');
	if (colon) {
		size_t hl = (size_t)(colon - addr);
		if (hl > 0 && hl < sizeof host) {
			memcpy(host, addr, hl);
			host[hl] = 0;
		}
		port = atoi(colon + 1);
		if (port <= 0 || port > 65535) port = 8931;
	} else {
		port = atoi(addr);
		if (port <= 0 || port > 65535) port = 8931;
	}
	struct in_addr bind_addr;
	if (inet_pton(AF_INET, host, &bind_addr) != 1) {
		fprintf(stderr, "akaman: invalid IPv4 bind address: %s\n", host);
		return 1;
	}

	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) { perror("socket"); return 1; }
	set_cloexec(fd);
	int on = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
	struct sockaddr_in sa;
	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_port = htons((unsigned short)port);
	sa.sin_addr = bind_addr;
	if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0) { perror("bind"); close(fd); return 1; }
	if (listen(fd, 16) < 0) { perror("listen"); close(fd); return 1; }
	fprintf(stderr, "akaman webMCP listening on %s:%d (api key: %s)\n", host, port,
	        api_key && api_key[0] ? "required" : "none");

	for (;;) {
		int c = accept(fd, NULL, NULL);
		if (c < 0) {
			if (errno == EINTR) continue;
			break;
		}
		set_cloexec(c);
		struct timeval tv = {5, 0};
		(void)setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
		char *body = NULL;
		size_t bodylen = 0;
		char auth[512];
		int r = read_http_request(c, &body, &bodylen, auth, sizeof auth);
		if (r == 0) {
			const char *m = "{\"error\":\"method not allowed\"}";
			http_send(c, 405, "Method Not Allowed", m, strlen(m));
		} else if (r < 0) {
			const char *m = "{\"error\":\"bad request\"}";
			http_send(c, 400, "Bad Request", m, strlen(m));
		} else {
			/* api key gate */
			int authed = 1;
			if (api_key && api_key[0]) {
				if (strncmp(auth, "Bearer ", 7) != 0 || strcmp(auth + 7, api_key) != 0)
					authed = 0;
			}
			if (!authed) {
				const char *m = "{\"error\":\"unauthorized\"}";
				http_send(c, 401, "Unauthorized", m, strlen(m));
			} else {
				gbuf resp;
				gbuf_init(&resp);
				handle_mcp(body, bodylen, &resp);
				http_send(c, 200, "OK", resp.p, resp.len);
				gbuf_free(&resp);
			}
		}
		free(body);
		close(c);
	}
	close(fd);
	return 0;
}

/* ------------------------------------------------------------------ */
/* benchmark                                                           */
/* ------------------------------------------------------------------ */
typedef struct {
	const char *q;
	const char *s;
	const char *label;
} bench_case;

int run_bench(void) {
	static const bench_case cases[] = {
		{"grep", NULL, "grep"},
		{"grep -r", NULL, "grep -r"},
		{"grep --recursive", NULL, "grep --recursive"},
		{"grep examples", NULL, "grep examples"},
		{"grep exit status", NULL, "grep exit status"},
		{"grep environment", NULL, "grep environment"},
		{"passwd files", NULL, "passwd files"},
		{"find -mtime", NULL, "find -mtime"},
		{"curl --retry", NULL, "curl --retry"},
		{"ip link set", NULL, "ip link set"},
		{"ip route add", NULL, "ip route add"},
		{"nft masquerade", NULL, "nft masquerade"},
		{"rsync --delete", NULL, "rsync --delete"},
		{"gcc -fPIC", NULL, "gcc -fPIC"},
		{"tar --strip-components", NULL, "tar --strip-components"},
		{"open", "2", "open(2)"},
		{"sudo troubleshooting", "doc", "doc sudo troubleshooting"},
		{"bash readline", "doc", "doc bash readline"},
		{"bash", "doc", "doc bash (bare)"},
		{"stdio.h printf", "headers", "headers stdio printf"},
		{"sys/socket connect", "headers", "headers sys/socket"},
		{"stdlib malloc", "headers", "headers stdlib malloc"},
		{NULL, NULL, NULL},
	};
	printf("%-22s %-14s %10s %11s %11s %11s %8s\n", "case", "page", "full B", "full tok",
	       "ret B", "ret tok", "reduct%");
	size_t tot_full = 0, tot_ret = 0;
	for (int i = 0; cases[i].q; i++) {
		result res;
		memset(&res, 0, sizeof res);
		const char *src = NULL;
		if (cases[i].s && strcmp(cases[i].s, "doc") == 0) src = "doc";
		else if (cases[i].s && strcmp(cases[i].s, "headers") == 0) src = "headers";
		const char *sec = src ? NULL : cases[i].s;
		int st = akaman_run(cases[i].q, sec, src, &res);
		char page[160];
		snprintf(page, sizeof page, "%s(%s)", res.pagename, res.pagesec);
		size_t ft = EST_TOKENS(res.full_bytes);
		size_t rt = EST_TOKENS(res.text.len);
		double red = res.full_bytes ? 100.0 * (1.0 - (double)res.text.len / (double)res.full_bytes)
		                            : 0.0;
		char redbuf[24];
		if (red > 99.9) snprintf(redbuf, sizeof redbuf, ">99.9%%");
		else snprintf(redbuf, sizeof redbuf, "%.1f%%", red);
		printf("%-22s %-14s %10zu %11zu %11zu %11zu %7s  %s\n", cases[i].label, page,
		       res.full_bytes, ft, res.text.len, rt, redbuf, st ? "(not found)" : "");
		tot_full += res.full_bytes;
		tot_ret += res.text.len;
		result_free(&res);
	}
	(void)tot_full;
	(void)tot_ret;
	printf("\nstatic MCP tool schema: %zu bytes, ~%zu tokens\n", sizeof(AKAMAN_TOOLS_LIST_JSON) - 1,
	       EST_TOKENS(sizeof(AKAMAN_TOOLS_LIST_JSON) - 1));
	printf("(per call: one tools/list payload + one small JSON-RPC envelope)\n");
	return 0;
}
