# akaman

Minimal local man-page context for AI coding agents.

Feeding a coding agent a complete man page costs thousands of tokens for a
page whose useful part is a few hundred bytes. `akaman` answers one question —
*"what is the exact syntax of `X`?"* — by extracting the smallest useful
fragment from the **native man pages already installed on the host**, and
serves that fragment over MCP or the command line.

Single static C binary, no dependencies, no database, no network. Native `man`
is authoritative: `akaman` never re-implements man's discovery or rendering.

## Results

| case                  | page       | full bytes | full tokens | returned | tokens | reduction |
| --------------------- | ---------- | ---------: | ----------: | -------: | -----: | --------: |
| `grep` (bare synopsis + map) | `grep(1)` | 31692 | 7923 | 324 | 81 | 99.0% |
| `grep -r`             | `grep(1)` | 31692 | 7923 | 477 | 120 | 98.5% |
| `grep --recursive`    | `grep(1)` | 31692 | 7923 | 313 | 79 | 99.0% |
| `grep -rin`           | `grep(1)` | 31692 | 7923 | ~915 | ~230 | 97.1% |
| `grep examples`       | `grep(1)` | 31692 | 7923 | 802 | 201 | 97.5% |
| `grep exit status`    | `grep(1)` | 31692 | 7923 | 274 | 69 | 99.1% |
| `grep environment`    | `grep(1)` | 31692 | 7923 | 1039 | 260 | 96.7% |
| `passwd files`        | `passwd(1)` | 7822 | 1956 | 192 | 48 | 97.5% |
| `find -mtime`         | `find(1)` | 94380 | 23595 | 248 | 62 | 99.7% |
| `curl --retry`        | `curl(1)` | 318804 | 79701 | 1271 | 318 | 99.6% |
| `ip link set`         | `ip-link(8)` | 89462 | 22366 | 874 | 219 | 99.0% |
| `ip route add`        | `ip-route(8)` | 45360 | 11340 | 864 | 216 | 98.1% |
| `nft masquerade`      | `nft(8)`  | 241221 | 60306 | 934 | 234 | 99.6% |
| `rsync --delete`      | `rsync(1)` | 260103 | 65026 | 428 | 107 | 99.8% |
| `gcc -fPIC`           | `gcc(1)`  | 1466248 | 366562 | 497 | 125 | >99.9% |
| `tar --strip-components` | `tar(1)` | 43836 | 10959 | 114 | 29 | 99.7% |
| `open(2)`             | `open(2)` | 46338 | 11585 | 773 | 194 | 98.3% |
| `doc sudo troubleshooting` | `sudo/TROUBLESHOOTING.md` | 16596 | 4149 | 100 | 25 | 99.4% |
| `doc bash readline`   | `bash/bashref.html` | 148115 | 37029 | 213 | 54 | 99.9% |
| `doc bash` (bare)     | `bash(doc)` | — | — | 105 | 27 | — |
| `headers stdio printf` | `stdio.h` | 34258 | 8565 | 108 | 27 | 99.7% |
| `headers sys/socket`  | `sys/socket.h` | 12417 | 3105 | 189 | 48 | 98.5% |
| `headers stdlib malloc` | `stdlib.h` | 42963 | 10741 | 126 | 32 | 99.7% |

Token estimate is `ceil(bytes / 4)`. Targets: bare synopsis 20–150, focused
option 40–200, named section 50–300, command explanation 50–250; hard guard
≤ 400.

## Progressive disclosure

A bare command query returns the compact SYNOPSIS plus a single-line map of
the page's actual sections, so an agent knows what else exists without
guessing section names:

```
$ akaman grep
grep(1)
SYNOPSIS
     grep [OPTION]... PATTERNS [FILE]...
...
SECTIONS: NAME | SYNOPSIS | DESCRIPTION | OPTIONS | REGULAR EXPRESSIONS | EXIT STATUS | ENVIRONMENT | NOTES | COPYRIGHT | BUGS | Reporting Bugs | EXAMPLE | SEE ALSO
```

Heading names are taken verbatim from the page (real top-level headings are
indexed, not just a fixed whitelist), de-duplicated, and kept on one logical
line. A specific query — `akaman grep -r`, `akaman "grep examples"`,
`akaman "grep exit status"` — returns only its exact fragment and never the
map. A query that matches nothing returns a compact no-match message plus the
available headings; option-looking queries (starting `-`/`--`) additionally
list up to five real nearby option names collected from the same page, never
invented:

```
$ akaman "curl --retr"
curl(1)
no match for "curl --retr" in curl(1)
SECTIONS: NAME | SYNOPSIS | DESCRIPTION | URL | GLOBBING | ...
NEARBY OPTIONS: --retry, --retry-delay, --netrc, --referer, --rate
```

## `/usr/share/doc` source (`source="doc"`)

A second, explicit documentation source reads the package documentation
already installed on the host, via the same single tool and one extra
optional field: `source = "man" | "doc" | "headers"` (default `man`). A failed
`man` lookup is never silently rerouted into another source; the man path is
unchanged.

```
$ akaman --source doc "sudo troubleshooting"
sudo/TROUBLESHOOTING.md
> You can specify the editor to use in visudo in the sudoers file.
> See the 'editor' and 'env_editor' entries in the sudoers manual.
```

Supported content: plain text, README/FAQ/INTRO, `.txt`, `.md`, and `.html`
(via a small local tag stripper). No PDF/XML, no database, no index, no
embeddings, no AI summaries, no network. A bare package query (`akaman
--source doc bash`) returns the available doc-file map. Query terms are
matched against filenames, headings and exact terms in the text; the smallest
complete paragraph matching the most terms wins. Package-manager-assisted
resolution (pacman today, apk tomorrow) is optional and never required.

Size: the binary grew from 907,400 B to 915,592 B (+8,192 B, +0.9%) for the
whole feature.

## System headers source (`source="headers"`)

Reads C/C++ system headers for API/type definitions. The include root is
resolved dynamically — on macOS via `xcrun --show-sdk-path` (the active SDK,
e.g. `/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk`) so `usr/include`
is never hardcoded, falling back to the legacy root; everywhere else
`/usr/include`. Query form is `header symbol...`:

```
$ akaman --source headers "stdio.h printf"
#include <stdio.h>
/* Maximum length of printf output for a NaN.  */
# define _PRINTF_NAN_LEN_MAX 4
```

```
$ akaman --source headers "sys/socket connect"
#include <sys/socket.h>
extern int connect (int __fd, __CONST_SOCKADDR_ARG __addr, socklen_t __len);
```

Headers may be named with or without `.h` (`stdio` == `stdio.h`) and with
subdirectories (`sys/socket.h`). A bare header query shows its include-guard
plus a small excerpt. Results are whole declaration blocks, budget capped.
No database, no index, no networks, same 95%+ token reduction as man.

## Cross-platform

The C is POSIX-portable (verified under clang/`-std=gnu11`). The Makefile only
uses `-static` on Linux/BSD; macOS builds dynamic (override with
`make STATIC=1`). At runtime the `man` invocation is probed once: GNU man-db
flags (`--no-hyphenation`, `-s`) are used when the local `man` accepts them,
otherwise BSD/macOS-compatible calls (`man -l PATH`, `-S` for sections) are
used automatically.

## Build

Requires only a C compiler. The binary is statically linked by default on
Linux/BSD (dynamic on macOS) so the same file runs across glibc, musl and BSD
hosts.

```sh
make            # build ./akaman
make test       # run the 162-test suite (requires man-db + curl)
make bench      # print the benchmark table
make clean
```

## Command line

```sh
akaman QUERY [SECTION]            # print fragment to stdout
akaman -s 2 open                  # numeric section = man section
akaman "ip link set"              # resolves to ip-link(8)
akaman --source doc "sudo troubleshooting"   # /usr/share/doc
akaman --source doc bash          # bare package = doc-file map
akaman --stats "rsync --delete"   # also print token metrics to stderr
akaman --bench                    # benchmark table
```

Query forms (in priority order, `source="man"`):

| query                         | returns                                        |
| ----------------------------- | ---------------------------------------------- |
| `command`                     | SYNOPSIS (bare query)                          |
| `command -opt` / `command --opt` | the exact option entry (matches alias forms, e.g. `-r` and `--recursive` both hit `-r, --recursive`) |
| `command -rin`                | bundled short options decomposed (`-r -i -n`) — only when every letter is a real option |
| `command <section-name>`      | the named section, e.g. `grep examples`, `grep exit status`, `passwd files` |
| `command subcommand`          | the subcommand's grammar/statement block, e.g. `ip link set` |

Query forms (`source="doc"`):

| query                         | returns                                        |
| ----------------------------- | ---------------------------------------------- |
| `package`                     | doc-file map, e.g. `akaman --source doc bash` |
| `package term...`             | smallest complete paragraph matching the terms, e.g. `akaman --source doc "sudo troubleshooting"` |

Exit codes: `0` match, `1` no page / render failure, `2` usage error.
No-match output is one compact line, e.g.
`no local man match for "no_such_page"`.

## MCP endpoint

`akaman` exposes a single tool, `man(query, section?, source?)`, over two transports:

- **stdio** (`--mcp`) — for agents that launch local MCP servers (codex,
  opencode, Claude, ...).
- **webMCP HTTP** (`--http[=ADDR]` / `--web[=ADDR]`) — JSON-RPC over HTTP
  with bearer-key auth, for network clients.

Tool schema (static, ~67 tokens):

```json
{
  "name": "man",
  "description": "Get minimal local man-page context for command syntax.",
  "inputSchema": {
    "type": "object",
    "properties": {
      "query": { "type": "string" },
      "section": { "type": "string" },
      "source": { "type": "string", "enum": ["man", "doc", "headers"] }
    },
    "required": ["query"]
  }
}
```

### codex

```sh
codex mcp add akaman -- /usr/local/bin/akaman --mcp
```

### opencode

Add the server to `opencode.json` (project or
`~/.config/opencode/opencode.json`) under `mcp`:

```json
{
  "$schema": "https://opencode.ai/config.json",
  "mcp": {
    "akaman": {
      "type": "local",
      "command": ["/usr/local/bin/akaman", "--mcp"],
      "enabled": true
    }
  }
}
```

Notes:

- `command` is an array of strings — never a single string. The binary path
  should be absolute.
- `type` is required (`"local"` for a spawned process, `"remote"` for HTTP).
- `environment` may be added to set env vars for the spawned process.
- Config is read at startup and is **not** hot-reloaded: after saving the
  change, quit and restart opencode.

### webMCP HTTP

The HTTP transport requires an API key. Create `/etc/akaman/akamcp.conf`
(any path, overridable with `--conf`):

```
# akaman webMCP config
APIKEY=sekrit-123
```

First `APIKEY=` line wins; blank lines and `#` comments are ignored. Then:

The installer creates or preserves this file with mode `0600`; it does not
overwrite an existing key. Rotate the key if the file or its contents have
ever been exposed.

```sh
akaman --http=127.0.0.1:8931     # default address; --web and --http are aliases
```

Authenticate every JSON-RPC request with a bearer key and POST:

```sh
curl -s -X POST -H 'Content-Type: application/json' \
     -H 'Authorization: Bearer sekrit-123' \
     -d '{"jsonrpc":"2.0","id":1,"method":"tools/list"}' \
     http://127.0.0.1:8931/

curl -s -X POST -H 'Content-Type: application/json' \
     -H 'Authorization: Bearer sekrit-123' \
     -d '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"man","arguments":{"query":"tar --strip-components"}}}' \
     http://127.0.0.1:8931/
```

Responses: `401` on a missing/wrong key, `405` for non-POST, `400` for a
malformed request.

### Protocol support

`initialize`, `ping`, `tools/list`, `tools/call`, `notifications/initialized`
(JSON-RPC 2.0 over stdio, Content-Length framed).

## How it works

1. **Resolve** the query against native `man -w`, probing candidate page names
   most-specific first (`cmd-sub-op`, `cmd-sub`, `cmd`) — so `ip link set`
   resolves to `ip-link(8)`, not `ip(8)`.
2. **Render** the page with
   `man --no-hyphenation --no-justification -l <path>` under `LC_ALL=C`
   (clean plain text, no ANSI/backspace bytes).
3. **Extract** the smallest fragment that answers the query, in priority order:
   named section (`grep exit status` → EXIT STATUS), option entry (alias-aware:
   `-r` and `--recursive` both resolve to the `-r, --recursive` entry, with its
   sibling cluster e.g. `--delete` → `--delete-*`; bundled shorts like `-rin`
   decompose to `-r -i -n` only when every letter is a real option), then the
   grammar block / statement for a subcommand anchor.
4. **Budget**: the authoritative fragment is never trimmed; optional ranges are
   appended only while `ceil(bytes/4)` stays ≤ 800, whole lines only; whole
   sections are capped at ~250 tokens so a huge section (grep ENVIRONMENT)
   stays under the guard.

No shell is ever invoked — subprocesses run via `execvp` with an argv array,
and queries containing shell metacharacters cannot execute anything (covered
by tests).

## Tests

`make test` runs `tests/test.sh` (217 tests): page discovery, section
selection, subcommand resolution, option extraction (short/long alias forms),
bundled short-option decomposition, named-section lookup, punctuation
preservation, exclusion of unrelated content, compact failure messages,
injection resistance (including SQL injection- and XSS-shaped input), token
budget, CLI==MCP output equality, the MCP protocol, and webMCP HTTP auth.

## Layout

```
Makefile        build/test/bench targets
src/main.c      CLI entry point and flag parsing
src/core.c      resolution, rendering, extraction, budget
src/mcp.c       MCP stdio + HTTP transports, config, benchmark
src/json.c      minimal JSON parser/emitter
tests/test.sh   test suite
```
