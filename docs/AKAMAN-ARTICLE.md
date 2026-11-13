# Akaman: The MCP Server That Gives Coding Agents the Right Man Page Fragment

Coding agents rarely need an entire manual page. They usually need one fact:
the syntax for an option, the grammar for a subcommand, the meaning of an exit
status, or the declaration for a C function. Akaman is a small MCP server built
around that observation. It searches the native documentation already installed
on the machine and returns the smallest useful fragment to the agent.

That makes Akaman less like a documentation chatbot and more like a precise
local reference tool. It does not copy a documentation corpus, build an index,
call a cloud service, use embeddings, or invent a summary. The installed man
pages, package documentation, and system headers remain the authority.

## The headline number: less context, same source

Akaman's benchmark uses a simple, consistent estimate of four bytes per token.
Across the current benchmark, the reduction is substantial:

| Query | Source estimate | Akaman result | Reduction |
| --- | ---: | ---: | ---: |
| `gcc -fPIC` | 366,562 tokens | 125 | >99.9% |
| `rsync --delete` | 65,026 | 107 | 99.8% |
| `curl --retry` | 79,701 | 318 | 99.6% |
| `find -mtime` | 23,595 | 62 | 99.7% |
| `ip route add` | 11,340 | 216 | 98.1% |
| `tar --strip-components` | 10,959 | 29 | 99.7% |

The result is not text compression. The original documentation is untouched;
Akaman simply avoids placing irrelevant material into the model context. In a
13-query representative benchmark, the measured/documented total is approximately
1,210,101 source tokens versus 1,937 returned tokens: a 99.84% reduction.
These are estimates based on bytes divided by four, not a measured model
tokenizer or a verified invoice-level token saving.

## What an agent can ask for

The interface is intentionally narrow: one MCP tool named `man`, with a query,
an optional section, and an optional source. A bare query such as `grep`
returns the synopsis and a compact map of real headings. A focused query such
as `grep -r` returns the matching option entry. Queries such as `ip link set`,
`grep examples`, and `open(2)` resolve to subcommand grammar, named sections,
and system-call documentation respectively.

Akaman also exposes two explicit local sources beyond man pages:

- `doc` searches installed package documentation such as Markdown, text, and
  HTML files under `/usr/share/doc`.
- `headers` extracts declaration blocks from system headers, for example
  `stdio.h printf` or `sys/socket.h connect`.

The same small MCP surface works over stdio for local agent integrations and
over HTTP with bearer-key authentication for network clients.

## How good is it to use from Codex?

From a code-review and direct MCP smoke-test perspective, Akaman is good at the part that matters most:
getting a precise answer with very little interaction overhead. The tool has
one clearly named operation, a small schema, predictable text output, and
useful recovery information when a query misses. The progressive-disclosure
behaviour is especially effective: the first call can show the real section
names, and the next call can request exactly one of them.

The design also fits coding-agent workflows well. It is a single C binary with
no runtime dependency on Python, a database, a network connection, or a
separate documentation service. It delegates man-page discovery and rendering
to the host's native `man`, which avoids creating a second, potentially stale
documentation system. Focused answers are capped around 400 estimated tokens,
so an agent can use the tool repeatedly without allowing reference material to
take over the conversation.

This review did not run a long autonomous Codex task with Akaman enabled, so it
cannot honestly claim that an agent has already saved a specific number of
tokens in production use. It can claim that the tool returned much smaller
reference payloads in the benchmark and direct checks.

The current source is also refreshingly inspectable: four C source files, a
single MCP tool, explicit size limits, JSON validation, traversal checks, and a
shell test suite. The live build produced a 927,880-byte binary on this host.

## Security and reliability signals

The test suite exercises malformed JSON-RPC, invalid UTF-8, oversized requests,
path traversal, command-substitution-shaped input, SQL-injection-shaped input,
XSS-shaped input, MCP/CLI response parity, and HTTP authentication behaviour.
The implementation has no SQL database or HTML renderer, so the SQLi and XSS
cases are appropriately treated as inert-input tests rather than claims of
database or browser security.

There is one important status note: the repository's test report records 217
passing tests, while a fresh run during this review produced 216 passes and one
failure in the authenticated HTTP `tools/call` check for `tar
--strip-components`. The CLI and stdio MCP paths returned the expected
fragment, and the benchmark completed, but the HTTP transport should be
investigated before advertising the suite as completely green.

## The trade-off

Akaman is deliberately not a general documentation search engine. It only
knows what is installed locally, and its answer quality depends on the host's
man pages and package docs. It also returns source fragments rather than
explaining them in natural language. That is a feature when the agent needs
authoritative syntax, but users wanting tutorials, cross-version comparisons,
or broad conceptual guidance will need another tool.

## Verdict

Akaman solves a narrow problem unusually well. It gives coding agents local,
authoritative reference material while spending tens or hundreds of tokens
instead of thousands—or, in the GCC example, hundreds of thousands. Its small
surface, zero-network default, native documentation strategy, and strong input
handling make it easy to understand and easy to trust.

The remaining HTTP test failure keeps the verdict provisional for the complete
transport story. For local Codex use over stdio, however, Akaman already looks
like a highly practical MCP server: fast enough, minimal enough, and focused on
exactly the context an agent needs.
