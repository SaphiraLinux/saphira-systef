"""Offline functional coverage using executable CLI/MCP and private fixtures."""
import gzip
import json
import os
from pathlib import Path
import subprocess
import http.client
import socket
import time
import sys

binary, scratch = sys.argv[1:]
base = Path(scratch) / 'fixture'
inc, docs, helpers = (base / n for n in ('include', 'doc', 'bin'))
for d in (inc, docs, helpers):
    d.mkdir(parents=True, exist_ok=True)
env = dict(os.environ, SYSREF_INCLUDE_PATH=str(inc), SYSREF_DOC_PATH=str(docs),
           SYSREF_LIBRARY_PATH=str(base / 'lib'), PATH=f'{helpers}:/usr/bin:/bin', MANPATH=str(base), SYSREF_DOC_ROOT=str(docs))
checks = 0

def check(ok, name):
    global checks
    assert ok, name
    checks += 1
    print('ok', name)

def run(source, *query, expected=0, custom=None, page=None):
    args = [binary, '--source', source, *query]
    if page is not None:
        args += ['--page', str(page)]
    p = subprocess.run(args, env=custom or env,
                       capture_output=True, text=True, timeout=30)
    check(p.returncode == expected, f'{source} {query} exit {p.returncode}: {p.stderr}')
    return p.stdout + p.stderr

(inc / 'mpfr.h').write_text('/* mpfr_subnormalize is not a declaration here */\n'
                           '#ifndef TEST_MPFR\n#define TEST_MPFR\n'
                           '__MPFR_DECLSPEC int mpfr_subnormalize(mpfr_ptr, int, mpfr_rnd_t);\n'
                           '#endif\n')
signature = '__MPFR_DECLSPEC int mpfr_subnormalize(mpfr_ptr, int, mpfr_rnd_t);'
s = run('headers', 'mpfr.h', 'mpfr_subnormalize')
check(f'{inc}/mpfr.h:4\n' in s and signature in s, 'mpfr exact declaration and line')
check('/*' not in s and '#ifndef' not in s, 'no banner or unrelated preprocessor lines')
check(signature in run('headers', 'mpfr_subnormalize'), 'symbol-only')
check(signature in run('headers', 'mpfr.h mpfr_subnormalize'), 'quoted query')
check('Header available' in run('headers', 'mpfr.h'), 'bare header preserved')
check('Header available' in run('headers', 'mpfr'), 'extensionless header preserved')
check('Missing header' in run('headers', 'absent.h', 'absent', expected=1), 'missing header')
check('Symbol not found' in run('headers', 'mpfr.h', 'future_function', expected=1), 'installed header missing symbol')
(inc / 'other.h').write_text('int mpfr_subnormalize(double);\n')
s = run('headers', 'mpfr_subnormalize')
check(s.count('SOURCE:') == 2, 'all matching definitions')
(inc / 'multi.h').write_text('/* ignored */\nextern int\nmultiline(\n int x,\n int y\n);\n'
                           '#if FEATURE\nint variant(int);\n#else\nint variant(double);\n#endif\n'
                           'typedef struct item { int x; } item;\n'
                           'enum colour { RED, BLUE };\n'
                           '#define LIMIT(x) \\\n ((x) + 1)\n'
                           'int use_only(item *p);\n')
s = run('headers', 'multiline')
check('multi.h:2' in s and 'extern int\nmultiline(\n int x,\n int y\n);' in s, 'complete multiline')
check(run('headers', 'variant').count('SOURCE:') == 2, 'conditional variants retained')
check('typedef struct item' in run('headers', 'item'), 'typedef structure')
check('enum colour' in run('headers', 'colour'), 'enum tag')
check('enum colour' in run('headers', 'RED'), 'enumerator')
s = run('headers', 'LIMIT')
check('((x) + 1)' in s and 'use_only' not in s, 'multiline macro ends correctly')
check(run('headers', 'item').count('SOURCE:') == 1, 'parameter type use excluded')
check('Invalid' in run('headers', '$(id)', expected=1), 'invalid symbol rejected')
check('relative' in run('headers', '../secret.h', 'x', expected=1), 'path escape rejected')
(inc / 'escape.h').symlink_to('/etc/passwd')
check('Missing header' in run('headers', 'escape.h', 'root', expected=1), 'symlink escape rejected')

# Neutral fixtures prove generic C resolution independent of MPFR names.
(inc / 'widget.h').write_text('#ifndef WIDGET_H\n#define WIDGET_H\n'
                              'extern int\nwidget_open(\n int fd,\n int flags\n);\n'
                              '#if WIDGET_V2\nint widget_mode(int);\n#else\nint widget_mode(long);\n#endif\n'
                              'typedef struct widget_state { int fd; } widget_state;\n'
                              'enum widget_colour { WIDGET_RED, WIDGET_BLUE };\n'
                               '#define WIDGET_CLAMP(x) \\\n ((x) < 0 ? 0 : (x))\n'
                               '#define WIDGET_VERSION 2\n'
                               'int widget_use(widget_state *p);\n'
                               '#endif\n')
(inc / 'guard.h').write_text('#ifndef GUARD_H\n#define GUARD_H\n'
                             '#ifdef __USE_GUARD\nint guarded_open(int fd);\n#endif\n'
                             '#endif\n')
s = run('headers', 'widget_open')
check('widget.h:3' in s and 'extern int\nwidget_open(\n int fd,\n int flags\n);' in s, 'neutral multiline function')
check(run('headers', 'widget_mode').count('SOURCE:') == 2, 'neutral conditional variants retained')
check('typedef struct widget_state' in run('headers', 'widget_state'), 'neutral typedef structure')
check('enum widget_colour' in run('headers', 'widget_colour'), 'neutral enum tag')
check('enum widget_colour' in run('headers', 'WIDGET_RED'), 'neutral enumerator')
s = run('headers', 'WIDGET_CLAMP')
check('((x) < 0 ? 0 : (x))' in s and 'widget_use' not in s, 'neutral multiline macro ends correctly')
check(run('headers', 'widget_state').count('SOURCE:') == 1, 'neutral parameter type use excluded')
s = run('headers', 'widget.h', 'widget_open')
check('widget.h:3' in s and 'widget_open' in s, 'neutral header+symbol query')
s = run('api', 'widget.h', 'widget_open')
check('KIND: function declaration (declared extern in header)' in s, 'function kind with linkage keyword')
s = run('api', 'WIDGET_CLAMP')
check('KIND: function-like macro' in s and 'cannot be taken' in s, 'function-like macro kind')
s = run('api', 'WIDGET_VERSION')
check('KIND: object-like macro' in s, 'object-like macro kind')
s = run('api', 'guard.h', 'guarded_open')
check('GUARDED BY: #ifdef __USE_GUARD' in s, 'conditional guard quoted verbatim')
check('feature_test_macros(7)' in s, 'feature-test manual cross-referenced')

(docs / 'mpfr-4.2').mkdir()
(docs / 'mpfr-4.2' / 'api.md').write_text('# mpfr_subnormalize\nint mpfr_subnormalize (mpfr_t X, int T, mpfr_rnd_t RND)\nArguments: x is rounded.\nReturn: documented ternary result.\n\nUnrelated text.\n')
s = run('api', 'mpfr.h', 'mpfr_subnormalize')
check('api.md:1' in s and 'documented ternary result' in s, 'version-directory local documentation')
check('not established' in s and 'no separate parameter table inferred' in s, 'uncertainty explicit')
check('SYMBOL: mpfr_subnormalize' in s, 'lifecycle symbol line')
check('HEADER: ' in s and 'mpfr.h:' in s, 'lifecycle header line with file and line')
check('PACKAGE: ' in s, 'lifecycle package line')
check('ARGUMENTS: ' in s and 'STATUS: declaration found.' in s, 'lifecycle arguments and status')
check('Detailed local manual: quoted above' in s, 'manual quoted status when documented')
check("1: header 'mpfr_ptr' | documented 'mpfr_t X'" in s, 'parameters contrast header and documented forms')
check("3: header 'mpfr_rnd_t' | documented 'mpfr_rnd_t RND'" in s, 'parameters final position named by manual')
check('matched positionally; not inferred' in s, 'parameters provenance explicit')
(docs / 'mpfr-4.2' / 'api.md').unlink()
with gzip.open(docs / 'mpfr.info.gz', 'wt') as f:
    f.write('Node: Rounding\nmpfr_subnormalize: Returns a documented status.\n\n')
check('mpfr.info.gz:2' in run('api', 'mpfr_subnormalize'), 'compressed Info discovery')
(docs / 'mpfr.info.gz').unlink()
(docs / 'manual.html').write_text('<h2>mpfr_subnormalize</h2>\n<p>Documented HTML contract.</p>\n')
s = run('api', 'mpfr_subnormalize')
check('Documented HTML contract' in s, 'HTML discovery')
check('<h2>' not in s and '<p>' not in s, 'HTML tags stripped, text quoted')
(docs / 'manual.html').unlink()
s = run('api', 'mpfr_subnormalize')
check('DOCUMENTATION: unavailable' in s and 'provider unknown' in s, 'missing docs no invented package')
check('ARGUMENTS: unavailable' in s and 'do not infer from types' in s, 'arguments honestly unavailable')
check('STATUS: declaration found.' in s and 'Detailed local manual: unavailable.' in s, 'honest manual status')
check('PARAMETERS: no documented signature' in s, 'parameters honestly absent without docs')
empty = base / 'empty'
empty.mkdir()
s = run('api', 'mpfr_subnormalize', expected=1, custom=dict(env, SYSREF_INCLUDE_PATH=str(empty)))
check('missing development package versus absent library unresolved' in s, 'system without MPFR')

# Adapter commands are stubs: prove exact ownership/file lookup, never install.
# Paths containing "foreign" resolve to a different owner so suppression
# (prose-only match from another package) is exercisable in-fixture.
(helpers / 'pacman').write_text('#!/bin/sh\ncase "$1" in\n-Qo) case "$2" in\n*foreign*) echo "$2 is owned by other-pkg 9.9";;\n*) echo "$2 is owned by fixture-dev 1.2";;\nesac;;\n-F) echo "fixture-dev 1.2"; echo " usr/include/absent.h";;\n*) exit 1;;\nesac\n')
(helpers / 'pacman').chmod(0o755)
s = run('api', 'mpfr_subnormalize')
check('fixture-dev 1.2' in s and 'not documentation provider' in s, 'verified owner/version distinct from docs provider')
s = run('headers', 'absent.h', 'absent', expected=1)
check('candidate provider' in s and 'fixture-dev' in s, 'missing development header cached metadata candidate')

(docs / 'fixture').mkdir()
(docs / 'fixture' / 'README.md').write_text('# Usage\nfixture_symbol performs the documented operation.\n')
check('README.md' in run('doc', 'fixture'), 'existing documentation file map')
check('documented operation' in run('doc', 'fixture', 'fixture_symbol'), 'existing focused documentation query')

# A library export distinguishes installed runtime from missing development files.
lib = base / 'lib'
lib.mkdir()
(lib / 'libfixture.so.1').write_text('test-only stand-in')
(helpers / 'nm').write_text('#!/bin/sh\necho "mpfr_subnormalize T 123 12"\n')
(helpers / 'nm').chmod(0o755)
s = run('api', 'mpfr_subnormalize', expected=1, custom=dict(env, SYSREF_INCLUDE_PATH=str(empty)))
check('runtime library is installed' in s and 'Development declaration unavailable' in s, 'runtime installed development header missing')
(lib / 'libfixture.so.1').unlink()
# A verified cached file listing establishes a doc provider without assuming -doc.
(helpers / 'pacman').write_text('#!/bin/sh\ncase "$1" in\n-Qo) case "$2" in\n*foreign*) echo "$2 is owned by other-pkg 9.9";;\n*) echo "$2 is owned by fixture-dev 1.2";;\nesac;;\n-F) echo "reference-manuals 1.2"; echo " usr/share/man/man3/mpfr_subnormalize.3.gz";;\n*) exit 1;;\nesac\n')
s = run('api', 'mpfr_subnormalize')
check('DOCUMENTATION FILE PROVIDER' in s and 'reference-manuals' in s, 'missing documentation provider from cached file list')
check('installation state unknown' in s, 'doc package absence not inferred from missing file')
(docs / 'foreign-prose.txt').write_text('The widget_open routine is fast and reliable.\n')
(docs / 'foreign-code.txt').write_text('widget_open(fd, flags) works here.\n')
s = run('api', 'widget.h', 'widget_open')
check('fast and reliable' not in s, 'foreign prose match suppressed, not quoted')
check('widget_open(fd, flags)' in s, 'foreign code-context match still quoted')
check('Omitted 1 low-relevance' in s and 'prose-only from other packages' in s, 'suppression reported with count')
(docs / 'foreign-prose.txt').unlink()
(docs / 'foreign-code.txt').unlink()
for i, pad in ((1, 'a'), (2, 'b'), (3, 'c'), (4, 'd')):
    (docs / f'big{i}.txt').write_text('big_sym ' + pad * 680 + '\n')
s = run('api', 'big_sym', expected=1)
check(s.count('DOCUMENT: ') == 3, 'page 1 quotes first window of excerpts')
check('page=1' in s and 'total_pages=2' in s and 'next_page=2' in s, 'page 1 navigation')
check('Additional documentation matches on later pages' in s, 'later-page hint')
check('DOCUMENTATION: 4 local excerpt(s) total, 3 quoted on this page.' in s, 'excerpt count reflects paging')
s = run('api', 'big_sym', expected=1, page=2)
check(s.count('DOCUMENT: ') == 1, 'page 2 quotes remaining excerpt')
check('page=2' in s and 'total_pages=2' in s, 'page 2 navigation')
check('next_page' not in s, 'no next page on last page')
s = run('api', 'big_sym', expected=1, page=3)
check('out of range (1-2)' in s, 'page out of range names valid range')
for i in (1, 2, 3, 4):
    (docs / f'big{i}.txt').unlink()
s = run('doc', 'fixture')
check('page=1' in s and 'total_pages=1' in s, 'doc map page navigation')
s = run('doc', 'fixture', expected=1, page=2)
check('out of range (1-1)' in s, 'doc map out of range names valid range')
s = run('man', 'demo', expected=2, page=0)
check('invalid --page value' in s, 'invalid page rejected')
(docs / 'same-package.txt').write_text('mpfr_subnormalize has a fixture contract.\n')
check('same installed package/version' in run('api', 'mpfr_subnormalize'), 'version-associated local reference')
(docs / 'same-package.txt').unlink()
(docs / 'widget-doc.txt').write_text('widget_open has a fixture contract.\nextern int\nwidget_open(\n int fd,\n int flags\n);\n')
check('PARAMETERS: header and documented forms agree.' in run('api', 'widget.h', 'widget_open'), 'parameters agree when doc quotes header form')
(docs / 'widget-doc.txt').unlink()

# Exact-duplicate excerpts add no information: only the first is quoted.
(docs / 'dup-a.txt').write_text('dupe_sym has a duplicated fixture contract.\n')
(docs / 'dup-b.txt').write_text('dupe_sym has a duplicated fixture contract.\n')
s = run('api', 'dupe_sym', expected=1)
check(s.count('DOCUMENT: ') == 1, 'duplicate excerpts deduplicated')
check('DOCUMENTATION: 1 local excerpt' in s, 'excerpt count reflects deduplication')
(docs / 'dup-a.txt').unlink()
(docs / 'dup-b.txt').unlink()

# Cross-hints route a wrong-verb guess to the right tool.
s = run('man', 'zzz_no_such_page_ever', expected=1)
check('use the api tool' in s and 'doc tool' in s, 'man miss hints at api and doc')
s = run('doc', 'definitely_no_such_pkg_zzz', expected=1)
check('man tool' in s and 'api tool' in s, 'doc miss hints at man and api')
s = run('api', 'zzz_nothere_symbol_at_all', expected=1)
check('man tool' in s and 'doc tool' in s, 'api miss hints at man and doc')

# Existing man source still uses native discovery and rendering.
(helpers / 'man').write_text('#!/bin/sh\ncase "$*" in\n*zzz_no_such_page_ever*) exit 1;;\n*"-w"*dualsec*|*dualsec*"-w"*) echo "'+str(base / 'man1/dualsec.1')+'"; echo "'+str(base / 'man3/dualsec.3')+'";;\n*"-w"*) echo "'+str(base / 'demo.1')+'";;\n*) printf "DEMO(1)\\n\\nNAME\\n demo - fixture\\n\\nSYNOPSIS\\n demo [OPTION]\\n\\nDESCRIPTION\\n Local fixture description.\\n";;\nesac\n')
(helpers / 'man').chmod(0o755)
(base / 'demo.1').write_text('.TH DEMO 1\n')
(base / 'man1').mkdir()
(base / 'man3').mkdir()
(base / 'man1' / 'dualsec.1').write_text('.TH DUALSEC 1\n')
(base / 'man3' / 'dualsec.3').write_text('.TH DUALSEC 3\n')
s = run('man', 'demo')
check('SYNOPSIS' in s and 'demo' in s, 'existing native man query')
s = run('man', 'dualsec')
check('Also manual sections: 1, 3' in s, 'multi-section serving discloses the choice')
s = run('man', 'demo')
check('page=1' in s and 'total_pages=1' in s, 'man page navigation')
s = run('man', 'demo', expected=1, page=2)
check('out of range (1-1)' in s, 'man page out of range names valid range')

# Three discoverable MCP tools sharing one handler, framed and newline transports.
def rpc(method, params=None, framed=False):
    req = json.dumps(dict(jsonrpc='2.0', id=1, method=method, params=params or {}))
    data = (f'Content-Length: {len(req)}\r\n\r\n{req}' if framed else req+'\n').encode()
    p = subprocess.run([binary, '--mcp'], input=data, capture_output=True, env=env, timeout=30)
    raw = p.stdout.split(b'\r\n\r\n', 1)[-1]
    return json.loads(raw)
init = rpc('initialize', {'protocolVersion': '2025-03-26'})
check(init['result'].get('instructions') == 'Local Linux documentation, manual pages and C API reference.', 'initialize advertises server purpose')
check(init['result']['serverInfo']['name'] == 'sysref', 'initialize server identity')
schema = rpc('tools/list')['result']['tools']
check([t['name'] for t in schema] == ['man', 'doc', 'api'], 'exactly three MCP tools')
by_name = {t['name']: t for t in schema}
check(by_name['man']['description'] == 'Look up Linux manual pages, commands and options.', 'man help text')
check(by_name['doc']['description'] == 'Find installed package documentation, guides and examples.', 'doc help text')
check(by_name['api']['description'] == 'Find C declarations, API contracts and header ownership.', 'api help text')
check('api' in by_name['man']['inputSchema']['properties']['source']['enum'], 'legacy source selector retained on man')
check(by_name['doc']['inputSchema']['required'] == ['query'], 'doc query required')
check(by_name['api']['inputSchema']['required'] == [], 'api symbol/header optional with server-side validation')
for framed in (False, True):
    response = rpc('tools/call', {'name': 'api', 'arguments': {'symbol': 'mpfr_subnormalize'}}, framed)
    check(signature in response['result']['content'][0]['text'], f'MCP api tool framed={framed}')
response = rpc('tools/call', {'name': 'api', 'arguments': {'symbol': 'mpfr_subnormalize', 'header': 'mpfr.h'}})
check(signature in response['result']['content'][0]['text'] and 'HEADER: ' in response['result']['content'][0]['text'], 'MCP api header+symbol')
response = rpc('tools/call', {'name': 'api', 'arguments': {}})
check('error' in response, 'MCP api rejects empty arguments')
response = rpc('tools/call', {'name': 'doc', 'arguments': {'query': 'fixture fixture_symbol', 'stats': True}})
check('documented operation' in response['result']['content'][0]['text'], 'MCP doc tool')
check('[sysref]' in response['result']['content'][0]['text'], 'MCP stats footer on doc tool')
response = rpc('tools/call', {'name': 'man', 'arguments': {'query': 'demo', 'page': 2}})
check('out of range (1-1)' in response['result']['content'][0]['text'], 'MCP man page out of range')
response = rpc('tools/call', {'name': 'api', 'arguments': {'symbol': 'mpfr_subnormalize', 'page': 99}})
check('out of range' in response['result']['content'][0]['text'], 'MCP api page out of range')
response = rpc('tools/call', {'name': 'doc', 'arguments': {'query': 'fixture', 'page': 'two'}})
check('error' in response, 'MCP page rejects non-integer')
response = rpc('tools/call', {'name': 'man', 'arguments': {'query': 'mpfr_subnormalize', 'source': 'api'}})
check(signature in response['result']['content'][0]['text'], 'MCP legacy man source=api')
s = rpc('tools/call', {'name': 'man', 'arguments': {'query': 'demo'}})
check('SYNOPSIS' in s['result']['content'][0]['text'], 'MCP default man compatibility')
s = rpc('tools/call', {'name': 'bogus', 'arguments': {'query': 'x'}})
check('error' in s, 'MCP unknown tool rejected')

# HTTP shares the same handler and retains bearer authentication.
conf = base / 'sysref.conf'
conf.write_text('APIKEY=fixture-key\n')
with socket.socket() as sock:
    sock.bind(('127.0.0.1', 0))
    port = sock.getsockname()[1]
server = subprocess.Popen([binary, f'--http=127.0.0.1:{port}', '--conf', str(conf)], env=env,
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    for attempt in range(100):
        try:
            conn = http.client.HTTPConnection('127.0.0.1', port, timeout=1)
            conn.connect()
            break
        except ConnectionRefusedError:
            time.sleep(.01)
    req = json.dumps(dict(jsonrpc='2.0', id=2, method='tools/call', params=dict(name='api', arguments=dict(symbol='mpfr_subnormalize'))))
    conn.request('POST', '/mcp', req, {'Authorization': 'Bearer fixture-key'})
    response = conn.getresponse()
    check(response.status == 200 and signature in json.loads(response.read())['result']['content'][0]['text'], 'HTTP api tool and existing bearer auth')
    conn.close()
finally:
    server.terminate()
    server.wait(timeout=5)

# Optional installed-system evidence, independent of whether MPFR is present.
if Path('/usr/include/mpfr.h').is_file():
    s = run('headers', 'mpfr.h', 'mpfr_subnormalize', custom=dict(env, SYSREF_INCLUDE_PATH='/usr/include'))
    check('mpfr_subnormalize' in s and 'SOURCE: /usr/include/mpfr.h:' in s, 'installed MPFR declaration')

# Representative system headers prove generic /usr/include resolution.
if Path('/usr/include/stdio.h').is_file():
    s = run('headers', 'stdio.h', 'printf', custom=dict(env, SYSREF_INCLUDE_PATH='/usr/include'))
    check('SOURCE: /usr/include/stdio.h:' in s and 'printf' in s, 'system stdio printf declaration')
if Path('/usr/include/string.h').is_file():
    s = run('headers', 'string.h', 'strlen', custom=dict(env, SYSREF_INCLUDE_PATH='/usr/include'))
    check('SOURCE: /usr/include/string.h:' in s and 'strlen' in s, 'system string strlen declaration')
print(f'{checks} checks, 0 failed')
