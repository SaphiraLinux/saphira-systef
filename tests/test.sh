#!/usr/bin/env bash
# akaman test suite. Run with: make test
set -u

cd "$(dirname "$0")/.."
BIN="$PWD/akaman"

pass=0
fail=0

t() {
	local name="$1" rc="$2" got="$3" want="$4"
	if [ "$got" = "$want" ]; then
		pass=$((pass + 1))
		printf 'PASS  %s\n' "$name"
	else
		fail=$((fail + 1))
		printf 'FAIL  %s\n  got: %s\n  exp: %s\n' "$name" "$got" "$want"
	fi
}

t_contains() {
	local name="$1" hay="$2" needle="$3"
	case "$hay" in
		*"$needle"*) pass=$((pass + 1)); printf 'PASS  %s\n' "$name" ;;
		*) fail=$((fail + 1)); printf 'FAIL  %s\n  need: %s\n  in: %s\n' "$name" "$needle" "$hay" ;;
	esac
}

t_not_contains() {
	local name="$1" hay="$2" needle="$3"
	case "$hay" in
		*"$needle"*) fail=$((fail + 1)); printf 'FAIL  %s\n  must NOT contain: %s\n  in: %s\n' "$name" "$needle" "$hay" ;;
		*) pass=$((pass + 1)); printf 'PASS  %s\n' "$name" ;;
	esac
}

run() { "$BIN" "$@" 2>&1; }
run_s() { "$BIN" -s "$@" 2>&1; }

# --- native discovery ---------------------------------------------------
out=$(run ls)
t "ls resolves to ls(1)" "$?" "0" "0"
t "ls header" "$(printf '%s' "$out" | head -1)" "ls(1)" "ls(1)"

out=$(run tar)
t "tar header" "$(printf '%s' "$out" | head -1)" "tar(1)" "tar(1)"

# --- section selection --------------------------------------------------
out=$(run_s 2 open)
t "open -s 2 header" "$(printf '%s' "$out" | head -1)" "open(2)" "open(2)"
t_contains "open(2) synopsis includes fcntl" "$out" "#include <fcntl.h>"

out=$(run open 2)
t "open positional section 2" "$(printf '%s' "$out" | head -1)" "open(2)" "open(2)"

out=$(run_s 9 open)
t "nonexistent section fails" "$?" "1" "1"

# --- subcommand-page resolution ----------------------------------------
out=$(run "ip link set")
t "ip link set -> ip-link(8)" "$(printf '%s' "$out" | head -1)" "ip-link(8)" "ip-link(8)"
t_contains "ip link set grammar" "$out" "ip link { set | change }"
t_contains "ip link set up/down" "$out" "{ up | down }"
t_not_contains "ip link set excludes unrelated types" "$out" "vxlan"
t_not_contains "ip link set excludes other subcommands" "$out" "ip-route"

out=$(run "ip route add")
t "ip route add -> ip-route(8)" "$(printf '%s' "$out" | head -1)" "ip-route(8)" "ip-route(8)"
t_contains "ip route add grammar" "$out" "ip route { add | del"

# --- option extraction --------------------------------------------------
out=$(run "rsync --delete")
t_contains "rsync --delete entry" "$out" "--delete                 delete extraneous"
t_contains "rsync sibling cluster" "$out" "--delete-excluded"

out=$(run "tar --strip-components")
t_contains "tar --strip-components syntax" "$out" "--strip-components=NUMBER"
t_contains "tar description kept" "$out" "Strip NUMBER leading components"

out=$(run "gcc -fPIC")
t_contains "gcc -fPIC entry" "$out" "-fPIC"
t_contains "gcc -fPIC explanation" "$out" "position-independent code"

# --- focused option lookup (short + long alias forms) -------------------
out=$(run "grep -r")
t "grep -r -> grep(1)" "$(printf '%s' "$out" | head -1)" "grep(1)" "grep(1)"
t_contains "grep -r short alias entry" "$out" "-r, --recursive"
t_contains "grep -r explains recursion" "$out" "Read all files under each directory"

out=$(run "grep --recursive")
t_contains "grep --recursive long alias entry" "$out" "-r, --recursive"

out=$(run "find -mtime")
t_contains "find -mtime entry" "$out" "-mtime n"

out=$(run "tar --strip-components")
t_contains "tar --strip-components syntax" "$out" "--strip-components=NUMBER"

out=$(run "curl --retry")
t_contains "curl --retry entry" "$out" "--retry <num>"

# --- bundled short-option decomposition ----------------------------------
out=$(run 'grep -rin "error" /var/log')
t "grep -rin -> grep(1)" "$(printf '%s' "$out" | head -1)" "grep(1)" "grep(1)"
t_contains "bundle r decomposed" "$out" "-r, --recursive"
t_contains "bundle i decomposed" "$out" "-i, --ignore-case"
t_contains "bundle n decomposed" "$out" "-n, --line-number"

# --- named-section lookup ------------------------------------------------
out=$(run "grep examples")
t_contains "grep examples -> EXAMPLE section" "$out" "EXAMPLE"
t_contains "grep examples has example cmd" "$out" 'grep -n --'

out=$(run "grep exit status")
t_contains "grep exit status -> EXIT STATUS" "$out" "EXIT STATUS"
t_contains "grep exit status value 2" "$out" "2 if an error occurred"

out=$(run "grep environment")
t_contains "grep environment -> ENVIRONMENT" "$out" "ENVIRONMENT"

out=$(run "passwd files")
t_contains "passwd files -> FILES" "$out" "FILES"
t_contains "passwd files has /etc/shadow" "$out" "/etc/shadow"

# --- progressive disclosure ----------------------------------------------
# bare query = map
out=$(run "grep")
t_contains "bare grep synopsis" "$out" "SYNOPSIS"
t_contains "bare grep heading map" "$out" "SECTIONS:"
t_contains "bare grep maps real headings" "$out" "REGULAR EXPRESSIONS"
t_contains "bare grep maps EXIT STATUS" "$out" "EXIT STATUS"
t "bare grep map is one logical line" "$(printf '%s' "$out" | grep -c '^SECTIONS:')" "1" "1"

# specific query = exact fragment, no map
out=$(run "grep -r")
t_not_contains "grep -r has no SECTIONS map" "$out" "SECTIONS:"

out=$(run "grep examples")
t_not_contains "grep examples has no SECTIONS map" "$out" "SECTIONS:"
t_contains "grep examples returns EXAMPLE only" "$out" "EXAMPLE"

# failed named query returns available headings
out=$(run "grep NO_SUCH_SECTION")
t_contains "failed named query keeps compact message" "$out" 'no match for "grep NO_SUCH_SECTION"'
t_contains "failed named query lists headings" "$out" "SECTIONS:"
t_contains "failed named query has OPTIONS heading" "$out" "OPTIONS"

# failed option query returns only real nearby option names
out=$(run "grep --rcur")
t_contains "failed option query message" "$out" "no match for"
t_contains "nearby options include --recursive" "$out" "--recursive"
t_not_contains "nearby options never invent" "$out" "--frobnicate"

out=$(run "grep -xyz")
t_contains "bundled garbage decomposes to real opts" "$out" "-x"
t_contains "bundled garbage has -z" "$out" "-z"
t_not_contains "bundled garbage has no invented option" "$out" "--frobnicate"

# no additional MCP tools or fields are introduced
mcp_schema=$(printf 'Content-Length: %d\r\n\r\n{"jsonrpc":"2.0","id":9,"method":"tools/list"}' "$(printf '%s' '{"jsonrpc":"2.0","id":9,"method":"tools/list"}' | wc -c)" | "$BIN" --mcp)
t_contains "only the man tool exists" "$mcp_schema" '"name":"man"'
t_not_contains "no extra MCP tool introduced" "$mcp_schema" '"name":"sections"'
t_not_contains "no extra schema field" "$mcp_schema" '"sections"'
t_contains "schema has source enum" "$mcp_schema" '"source":{"type":"string","enum":["man","doc","headers"]}'

# --- /usr/share/doc source ----------------------------------------------
out=$(run --source doc bash)
t "doc bare package exit 0" "$?" "0" "0"
t_contains "doc bare header" "$out" "bash(doc)"
t_contains "doc bare file map" "$out" "DOC FILES:"

out=$(run --source doc "sudo troubleshooting")
t "doc term query exit 0" "$?" "0" "0"
t_contains "doc term file header" "$out" "sudo/TROUBLESHOOTING.md"
t_contains "doc term content" "$out" "sudo"

out=$(run --source doc "sudo visudo editor")
t "doc multi-term query exit 0" "$?" "0" "0"
t_contains "doc multi-term best file" "$out" "TROUBLESHOOTING.md"
t_contains "doc multi-term visudo" "$out" "visudo"

out=$(run --source doc "definitely_no_such_pkg_zzz")
t "doc missing package exit 1" "$?" "1" "1"
t_contains "doc missing message" "$out" "no local doc match"

out=$(run --source doc "bash zzzzz_nothere")
t "doc no term match exit 0" "$?" "0" "0"
t_contains "doc no-term message" "$out" "no match for"
t_contains "doc no-term recovery map" "$out" "DOC FILES:"

# doc never silently falls back to man
out=$(run --source doc "grep -r")
t_contains "doc does not fall back to man" "$out" "no local doc match"

# doc replies stay within the ~400-token budget
toks=$("$BIN" --stats --source doc "sudo troubleshooting" 2>&1 >/dev/null | sed -n 's/^returned tokens(est): //p')
[ -n "$toks" ] || toks=-1
t "doc budget under 400" "$(test "$toks" -le 400 && echo yes || echo no)" "yes" "yes"

# unknown source is rejected
out=$(run --source web "bash")
t "unknown source exit 1" "$?" "1" "1"
t_contains "unknown source message" "$out" "unknown source"

# --- system headers source ----------------------------------------------
out=$(run --source headers "stdio printf")
t "headers symbol query exit 0" "$?" "0" "0"
t_contains "headers include line" "$out" "#include <stdio.h>"
t_contains "headers printf block" "$out" "printf"

out=$(run --source headers "sys/socket connect")
t "headers subdir query exit 0" "$?" "0" "0"
t_contains "headers subdir include" "$out" "#include <sys/socket.h>"
t_contains "headers connect block" "$out" "extern int connect"

out=$(run --source headers stdio.h)
t "headers bare header exit 0" "$?" "0" "0"
t_contains "headers bare include" "$out" "#include <stdio.h>"

out=$(run --source headers "no_such_header_zzz x")
t "headers missing header exit 1" "$?" "1" "1"
t_contains "headers missing header message" "$out" "no such header"

out=$(run --source headers "stdio.h zzz_nothere")
t "headers no symbol match exit 1" "$?" "1" "1"
t_contains "headers no symbol message" "$out" "no match for"
t_contains "headers no symbol names header" "$out" "stdio.h"

out=$(run --source headers "string.h strlen")
t_headers=$(printf '%s' "$out" | grep -c '^#include')
t "headers single include line" "$t_headers" "1" "1"

# headers replies stay within the ~400-token budget
toks=$("$BIN" --stats --source headers "stdio printf" 2>&1 >/dev/null | sed -n 's/^returned tokens(est): //p')
[ -n "$toks" ] || toks=-1
t "headers budget under 400" "$(test "$toks" -le 400 && echo yes || echo no)" "yes" "yes"

# --- punctuation preservation ------------------------------------------
out=$(run "ip link set")
t_not_contains "punctuation brackets kept" "$out" "up down"  # { } must survive

# --- compact failures ---------------------------------------------------
out=$(run "definitely_no_such_page_zzz")
t "missing page exit 1" "$?" "1" "1"
t_contains "missing page message" "$out" "no local man match"

out=$(run "")
t "empty query exit 1" "$?" "1" "1"

# --- injection resistance ----------------------------------------------
rm -f /tmp/akaman_pwn /tmp/akaman_pwn2
out=$(run 'ls; touch /tmp/akaman_pwn')
t "shell metachar query exit 1" "$?" "1" "1"
t "no file created by ; injection" "$(test -f /tmp/akaman_pwn && echo yes || echo no)" "no" "no"
out=$(run '$(touch /tmp/akaman_pwn2)')
t "command substitution query exit 1" "$?" "1" "1"
t "no file created by $() injection" "$(test -f /tmp/akaman_pwn2 && echo yes || echo no)" "no" "no"

# SQL injection-shaped input must remain inert. Akaman has no database, but
# these payloads still exercise tokenisation, page resolution, and error
# handling at the public CLI boundary.
for q in "' OR '1'='1" "\" OR 1=1 --" "x' UNION SELECT NULL--"; do
	out=$(run "$q")
	t "SQLi CLI payload is rejected: $q" "$?" "1" "1"
	t_contains "SQLi CLI payload returns no-match: $q" "$out" "no local man match"
done

# XSS-shaped input is data, not markup or a command. CLI output may reflect
# the query in its diagnostic, while MCP must preserve it as valid JSON text.
xss_query='\"><script>alert(1)</script>'
out=$(run "$xss_query")
t "XSS CLI payload is rejected" "$?" "1" "1"
t_contains "XSS CLI payload returns no-match" "$out" "no local man match"
t_contains "XSS CLI payload is not executed" "$out" "<script>alert(1)</script>"

xss_mcp=$(printf '%s\n' '{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"man","arguments":{"query":"\"><script>alert(1)</script>"}}}' | "$BIN" --mcp 2>&1)
t_contains "XSS MCP payload returns a response" "$xss_mcp" '"result":{"content"'
printf '%s\n' "$xss_mcp" | jq -e . >/dev/null
t "XSS MCP response is valid JSON" "$?" "0" "0"
t_contains "XSS MCP response uses text content" "$xss_mcp" '"type":"text"'

# SQLi and XSS payloads must also be inert through the doc and headers source
# selectors, which take different filesystem/query paths.
out=$(run --source doc "' OR 1=1 --")
t "SQLi doc payload is rejected" "$?" "1" "1"
t_contains "SQLi doc payload returns no-match" "$out" "no local doc match"
out=$(run --source headers '<script>alert(1)</script>')
t "XSS headers payload is rejected" "$?" "1" "1"
t_contains "XSS headers payload returns no-match" "$out" "no header match"

# stray path tokens must not become anchors (ls / matched lowpan line)
out=$(run "ip link set ls /")
t_contains "slash token is not an anchor" "$out" "no match"
t_not_contains "slash token does not leak to lowpan" "$out" "lowpan"

# --- budget ------------------------------------------------------------
for q in "ip link set" "ip route add" "nft masquerade" "rsync --delete" "gcc -fPIC" "tar --strip-components" "grep -r" "grep --recursive" "grep examples" "grep exit status" "grep environment" "passwd files" 'grep -rin "error" /var/log' "find -mtime" "curl --retry" "grep"; do
	toks=$("$BIN" --stats "$q" 2>&1 >/dev/null | sed -n 's/^returned tokens(est): //p')
	[ -n "$toks" ] || toks=-1
	if [ "$toks" -le 400 ]; then
		pass=$((pass + 1)); printf 'PASS  budget %-24s %s tokens\n' "$q" "$toks"
	else
		fail=$((fail + 1)); printf 'FAIL  budget %-24s %s tokens > 400\n' "$q" "$toks"
	fi
done
toks=$("$BIN" --stats -s 2 open 2>&1 >/dev/null | sed -n 's/^returned tokens(est): //p')
[ -n "$toks" ] || toks=-1
if [ "$toks" -le 400 ]; then
	pass=$((pass + 1)); printf 'PASS  budget %-24s %s tokens\n' "open(2)" "$toks"
else
	fail=$((fail + 1)); printf 'FAIL  budget %-24s %s tokens > 400\n' "open(2)" "$toks"
fi
# bare queries stay compact: synopsis + one-line map < 150 tokens
for q in "grep" "ls" "find" "man"; do
	toks=$("$BIN" --stats "$q" 2>&1 >/dev/null | sed -n 's/^returned tokens(est): //p')
	[ -n "$toks" ] || toks=-1
	if [ "$toks" -le 150 ]; then
		pass=$((pass + 1)); printf 'PASS  bare budget %-24s %s tokens\n' "$q" "$toks"
	else
		fail=$((fail + 1)); printf 'FAIL  bare budget %-24s %s tokens > 150\n' "$q" "$toks"
	fi
done

# --- CLI == MCP consistency ---------------------------------------------
mcp_text() {
	local body="$1" resp t
	resp=$(printf 'Content-Length: %d\r\n\r\n%s' "${#body}" "$body" | "$BIN" --mcp)
	t=${resp#*\"text\":\"}
	t=${t%%\"*}
	printf '%s' "$t" | sed 's/\\n/\n/g'
}
for q in "ip link set" "nft masquerade" "tar --strip-components" "grep -r" "grep exit status"; do
	cli=$(run "$q")
	mcp=$(mcp_text "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"man\",\"arguments\":{\"query\":\"$q\"}}}")
	t "CLI==MCP for '$q'" "$(printf '%s' "$cli" | md5sum | cut -d' ' -f1)" "$(printf '%s' "$mcp" | md5sum | cut -d' ' -f1)" "$(printf '%s' "$cli" | md5sum | cut -d' ' -f1)"
done
for q in "sudo troubleshooting"; do
	cli=$(run --source doc "$q")
	mcp=$(mcp_text "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"man\",\"arguments\":{\"query\":\"$q\",\"source\":\"doc\"}}}")
	t "CLI==MCP doc for '$q'" "$(printf '%s' "$cli" | md5sum | cut -d' ' -f1)" "$(printf '%s' "$mcp" | md5sum | cut -d' ' -f1)" "$(printf '%s' "$cli" | md5sum | cut -d' ' -f1)"
done
for q in "stdio printf"; do
	cli=$(run --source headers "$q")
	mcp=$(mcp_text "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"man\",\"arguments\":{\"query\":\"$q\",\"source\":\"headers\"}}}")
	t "CLI==MCP headers for '$q'" "$(printf '%s' "$cli" | md5sum | cut -d' ' -f1)" "$(printf '%s' "$mcp" | md5sum | cut -d' ' -f1)" "$(printf '%s' "$cli" | md5sum | cut -d' ' -f1)"
done

# --- MCP protocol -------------------------------------------------------
mcp_call() {
	printf 'Content-Length: %d\r\n\r\n%s' "${#1}" "$1" | "$BIN" --mcp
}
mcp_json() {
	local resp
	resp=$(mcp_call "$1")
	printf '%s' "${resp#*$'\r\n\r\n'}"
}
out=$(mcp_call '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26"}}')
t_contains "initialize server name" "$out" '"name":"akaman"'
t_contains "initialize protocol echo" "$out" '"protocolVersion":"2025-03-26"'

out=$(mcp_call '{"jsonrpc":"2.0","id":2,"method":"tools/list"}')
t_contains "tools/list has man" "$out" '"name":"man"'
t_contains "tools/list schema has query" "$out" '"query"'

out=$(mcp_call '{"jsonrpc":"2.0","id":3,"method":"ping"}')
t_contains "ping result" "$out" '"result":{}'

out=$(mcp_call '{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"man","arguments":{}}}')
t_contains "missing query error" "$out" '-32602'

out=$(mcp_call '{"jsonrpc":"2.0","id":5,"method":"bogus"}')
t_contains "unknown method error" "$out" '-32601'

# malformed method values must return valid errors, never crash
for method in null 1 '[]' '{}' true; do
	out=$(mcp_json "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":$method}")
	t_contains "non-string method $method rejected" "$out" '"code":-32600'
	printf '%s\n' "$out" | jq -e . >/dev/null
	t "non-string method $method is valid JSON" "$?" "0" "0"
done

# malformed JSON numbers must not be accepted and echoed into invalid JSON
for num in '1.' '1e' '01'; do
	out=$(mcp_json "{\"jsonrpc\":\"2.0\",\"id\":$num,\"method\":\"ping\"}")
	t_contains "malformed number $num parse error" "$out" '"code":-32700'
	printf '%s\n' "$out" | jq -e . >/dev/null
	t "malformed number $num response is valid JSON" "$?" "0" "0"
done

# long unknown options must recover without entering the fixed-size scorer
long_opt=$(printf '%*s' 200 x | tr ' ' a)
out=$(run "grep --$long_opt")
t "long unknown option is safe" "$?" "1" "1"
t_contains "long unknown option reports no match" "$out" "no match for"

# source roots must reject absolute paths and parent traversal
out=$(run "../../../etc/passwd")
t "man traversal rejected" "$?" "1" "1"
t_not_contains "man traversal does not disclose passwd" "$out" "root:x:"
out=$(run --source headers "../../etc/passwd")
t "header traversal rejected" "$?" "1" "1"
t_not_contains "header traversal does not disclose passwd" "$out" "root:x:"
out=$(run --source doc "../../../etc")
t "doc traversal rejected" "$?" "1" "1"
t_not_contains "doc traversal does not enumerate etc" "$out" "DOC FILES:"
if [ -L /usr/share/doc/semver/README.md ]; then
	out=$(run --source doc "semver README")
	t "doc symlink escape rejected" "$?" "1" "1"
	t_not_contains "doc symlink target is not disclosed" "$out" "/usr/lib/node_modules"
fi

# header results must satisfy every requested symbol
out=$(run --source headers "stdio printf zzz_nothere")
t_contains "header missing term reports no match" "$out" "no match for"
t_not_contains "header missing term returns no declaration" "$out" "_PRINTF_NAN_LEN_MAX"

out=$(run -s 123456789 open)
t "overlong numeric section rejected" "$?" "1" "1"
t_contains "overlong numeric section message" "$out" "section selector too large"

# oversized and overflowing Content-Length values are rejected without hangs
out=$(printf 'Content-Length: 18446744073709551576\r\n\r\n{}' | "$BIN" --mcp 2>&1)
t "overflowing Content-Length rejected" "$?" "1" "1"
large_body=$(printf '%*s' 70000 x | tr ' ' x)
out=$(printf '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"arguments":{"query":"%s"}}}\n' "$large_body" | "$BIN" --mcp 2>&1)
t_contains "oversized NDJSON request rejected" "$out" "request too large"

# invalid UTF-8 in a JSON request must yield a valid parse-error response
out=$(python3 -c 'import sys; sys.stdout.buffer.write(b"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\",\"x\":\""+bytes([255])+b"\"}\n")' | "$BIN" --mcp 2>&1)
t_contains "invalid UTF-8 rejected" "$out" '"code":-32700'
printf '%s\n' "$out" | jq -e . >/dev/null
t "invalid UTF-8 response is valid JSON" "$?" "0" "0"

out=$(run --stats "gcc -fPIC")
t_not_contains "GCC reduction is not rounded to 100.0" "$out" "100.0%"
t_contains "GCC reduction uses lower bound" "$out" ">99.9%"

# --- webMCP HTTP --------------------------------------------------------
if command -v curl >/dev/null 2>&1; then
	conf="$PWD/tests/.http.conf"
	printf '# comment\n\nAPIKEY=sekrit-123\n' > "$conf"
	port=$((20000 + ($$ % 10000)))
	"$BIN" --http="127.0.0.1:$port" --conf "$conf" 2>/dev/null &
	srv=$!
	sleep 0.4
	hdr='Content-Type: application/json'
	url="http://127.0.0.1:$port/"

	out=$(curl -s -o /dev/null -w '%{http_code}' -X POST -H "$hdr" \
		-d '{"jsonrpc":"2.0","id":1,"method":"tools/list"}' "$url")
	t "http no auth 401" "$out" "401" "401"

	out=$(curl -s -o /dev/null -w '%{http_code}' -X POST -H "$hdr" -H 'Authorization: Bearer wrong' \
		-d '{"jsonrpc":"2.0","id":1,"method":"tools/list"}' "$url")
	t "http bad key 401" "$out" "401" "401"
	http_body="/tmp/akaman-http-body-$$"
	out=$(curl -s -o "$http_body" -w '%{http_code} %{size_download}' -X POST -H "$hdr" -H 'Authorization: Bearer wrong' \
		-d '{"jsonrpc":"2.0","id":1,"method":"tools/list"}' "$url")
	t "http 401 body length is exact" "$out" "401 24" "401 24"
	rm -f "$http_body"

	out=$(curl -s -X POST -H "$hdr" -H 'Authorization: Bearer sekrit-123' \
		-d '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"man","arguments":{"query":"tar --strip-components"}}}' "$url")
	t_contains "http tools/call works" "$out" "--strip-components=NUMBER"

	out=$(curl -s -o /dev/null -w '%{http_code}' "$url")
	t "http GET 405" "$out" "405" "405"
	out=$("$BIN" --http="not-an-ip:$port" --conf "$conf" 2>&1)
	t "invalid HTTP bind address fails" "$?" "1" "1"
	t_contains "invalid HTTP bind message" "$out" "invalid IPv4 bind address"

	kill "$srv" 2>/dev/null
	wait "$srv" 2>/dev/null
	rm -f "$conf"
fi

# --- CLI errors ---------------------------------------------------------
out=$(run --bogus)
t "unknown flag exit 2" "$?" "2" "2"
out=$(run)
t "no args exit 2" "$?" "2" "2"

# --- installed manual page ----------------------------------------------
# These prove the man page works through native man after 'make install'
# and that akaman can retrieve its own locally installed manual.  They are
# skipped unless the page is actually installed so 'make test' stays green.
if man -w akaman >/dev/null 2>&1; then
	out=$(man -w akaman 2>&1)
	t "man -w akaman resolves" "$?" "0" "0"
	t_contains "man -w akaman finds local path" "$out" "akaman.1"

	out=$(man akaman 2>&1)
	t "man akaman exit 0" "$?" "0" "0"
	t_contains "man akaman NAME section" "$out" "akaman - minimal local man-page context for AI agents"
	t_contains "man akaman OPTIONS" "$out" "SYNOPSIS"

	out=$(run akaman)
	t "akaman self lookup exit 0" "$?" "0" "0"
	t_contains "akaman self lookup header" "$out" "akaman(1)"
	t_contains "akaman self lookup synopsis" "$out" "SYNOPSIS"

	out=$(run "akaman --source")
	t "akaman self option entry exit 0" "$?" "0" "0"
	t_contains "akaman self option entry" "$out" "--source"
	t_contains "akaman self option sources" "$out" "man"
	t_contains "akaman self option doc" "$out" "doc"
	t_contains "akaman self option headers" "$out" "headers"

	out=$(run "akaman examples")
	t "akaman self examples exit 0" "$?" "0" "0"
	t_contains "akaman self examples section" "$out" "EXAMPLES"
	t_contains "akaman self examples cmd" "$out" "akaman -s 2 open"
else
	printf 'SKIP  installed man-page tests (run make install first)\n'
fi

# --- benchmark ----------------------------------------------------------
out=$(run --bench)
t "benchmark exit 0" "$?" "0" "0"
t_contains "benchmark table" "$out" "reduct%"
t_contains "benchmark case" "$out" "ip link set"

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
