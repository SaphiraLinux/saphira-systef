#ifndef SYSREF_MCP_H
#define SYSREF_MCP_H

#include <stddef.h>
#include "core.h"

#define MCP_MAX_HEADER_BYTES 16384u
#define MCP_MAX_REQUEST_BYTES (64u * 1024u)
#define MCP_MAX_QUERY_BYTES SYSREF_MAX_QUERY_BYTES

/* Static tools/list result. Descriptions are the discovery metadata: each
 * one says when to use the tool in 5-10 words, nothing more. `man` keeps
 * its legacy `source=` selector for compatibility (marked legacy in the
 * manual, not here); new agents use the dedicated `doc` and `api` tools.
 * The `stats` field stays opt-in on all three so the common call pays
 * nothing for it. `format` selects the text rendering; the same evidence
 * is always also returned as standard structuredContent, which conforms
 * to the declared outputSchema (envelope; verb-specific data is an
 * object or null). */
#define SYSREF_OUTPUT_SCHEMA \
"\"outputSchema\":{\"type\":\"object\",\"properties\":{" \
"\"tool\":{\"type\":\"string\"}," \
"\"query\":{\"type\":\"string\"}," \
"\"format\":{\"type\":\"string\"}," \
"\"status\":{\"type\":\"integer\"}," \
"\"page\":{\"type\":\"integer\"}," \
"\"total_pages\":{\"type\":\"integer\"}," \
"\"next_page\":{\"type\":[\"integer\",\"null\"]}," \
"\"data\":{\"type\":[\"object\",\"null\"]}}," \
"\"required\":[\"tool\",\"query\",\"format\",\"status\",\"page\",\"total_pages\",\"next_page\",\"data\"]," \
"\"additionalProperties\":true}"
#define SYSREF_TOOLS_LIST_JSON \
"{\"tools\":[" \
"{\"name\":\"man\"," \
"\"description\":\"Look up Linux manual pages, commands and options.\"," \
"\"inputSchema\":{\"type\":\"object\",\"properties\":{" \
"\"query\":{\"type\":\"string\"}," \
"\"section\":{\"type\":\"string\"}," \
"\"source\":{\"type\":\"string\",\"enum\":[\"man\",\"doc\",\"headers\",\"api\"]}," \
"\"page\":{\"type\":\"integer\"}," \
"\"format\":{\"type\":\"string\",\"enum\":[\"text\",\"markdown\",\"json\"]}," \
"\"stats\":{\"type\":\"boolean\"}}," \
"\"required\":[\"query\"]}," \
SYSREF_OUTPUT_SCHEMA "}," \
"{\"name\":\"doc\"," \
"\"description\":\"Find installed package documentation, guides and examples.\"," \
"\"inputSchema\":{\"type\":\"object\",\"properties\":{" \
"\"query\":{\"type\":\"string\"}," \
"\"page\":{\"type\":\"integer\"}," \
"\"format\":{\"type\":\"string\",\"enum\":[\"text\",\"markdown\",\"json\"]}," \
"\"stats\":{\"type\":\"boolean\"}}," \
"\"required\":[\"query\"]}," \
SYSREF_OUTPUT_SCHEMA "}," \
"{\"name\":\"api\"," \
"\"description\":\"Find C declarations, API contracts and header ownership.\"," \
"\"inputSchema\":{\"type\":\"object\",\"properties\":{" \
"\"symbol\":{\"type\":\"string\"}," \
"\"header\":{\"type\":\"string\"}," \
"\"page\":{\"type\":\"integer\"}," \
"\"per_page\":{\"type\":\"integer\"}," \
"\"format\":{\"type\":\"string\",\"enum\":[\"text\",\"markdown\",\"json\"]}," \
"\"stats\":{\"type\":\"boolean\"}}," \
"\"required\":[]}," \
SYSREF_OUTPUT_SCHEMA "}" \
"]}"

int conf_load(const char *path, char *key, size_t keysz);
int run_stdio(void);
int run_http(const char *addr, const char *api_key);
int run_bench(void);

#endif
