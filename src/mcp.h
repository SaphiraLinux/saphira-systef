#ifndef AKAMAN_MCP_H
#define AKAMAN_MCP_H

#include <stddef.h>
#include "core.h"

#define MCP_MAX_HEADER_BYTES 16384u
#define MCP_MAX_REQUEST_BYTES (64u * 1024u)
#define MCP_MAX_QUERY_BYTES AKAMAN_MAX_QUERY_BYTES

/* Static tools/list result: keep description + schema as small as possible.
 * This is the entire MCP surface: one tool. The `stats` field is opt-in and
 * costs one line of schema: an agent that wants to know how much context it
 * just saved asks for it, and every other call stays byte-identical. That
 * matters for a tool whose entire claim is context reduction -- an agent
 * cannot hold that claim to account if the only way to measure it is a CLI
 * flag it cannot reach. */
#define AKAMAN_TOOLS_LIST_JSON                                                              \
	"{\"tools\":[{\"name\":\"man\",\"description\":\"Get minimal local man-page context " \
	"for command syntax or C symbols (source=api).\",\"inputSchema\":{\"type\":\"object\",\"properties\":{"           \
	"\"query\":{\"type\":\"string\"},\"section\":{\"type\":\"string\"},"                   \
	"\"source\":{\"type\":\"string\",\"enum\":[\"man\",\"doc\",\"headers\",\"api\"]},"            \
	"\"stats\":{\"type\":\"boolean\"}},"                                                   \
	"\"required\":[\"query\"]}}]}"

int conf_load(const char *path, char *key, size_t keysz);
int run_stdio(void);
int run_http(const char *addr, const char *api_key);
int run_bench(void);

#endif
