# AKAMAN Context Reduction

## Purpose

AI agents can already access local system documentation. The problem is that
feeding an entire man page or documentation file into model context is wasteful
when the agent usually needs only one syntax block, option, section, or
declaration.

AKAMAN performs that retrieval locally and returns only the smallest useful
authoritative fragment. This is context reduction, not binary or text
compression: the source documentation is left unchanged, while the amount of
documentation placed in model context is reduced.

## Headline benchmark

The representative benchmark contains 13 queries.

| Measure | Result |
| --- | ---: |
| Source documentation | 1,210,101 estimated tokens |
| AKAMAN returned | 1,937 estimated tokens |
| Tokens avoided | 1,208,164 |
| Overall context reduction | 99.84% |
| Source bytes | 4,840,396 |
| Returned bytes | 7,723 |

The token figures are estimated using the methodology below.

## Benchmark table

| Query | Page | Source estimated tokens → AKAMAN estimated tokens | Context reduction |
| --- | --- | ---: | ---: |
| `bash` | `bash(1)` | 104475 → 112 | 99.9% |
| `gcc` | `gcc(1)` | 366562 → 130 | >99.9% |
| `gcc -fPIC` | `gcc(1)` | 366562 → 125 | >99.9% |
| `curl` | `curl(1)` | 79701 → 71 | 99.9% |
| `curl --retry` | `curl(1)` | 79701 → 318 | 99.6% |
| `rsync --delete` | `rsync(1)` | 65026 → 107 | 99.8% |
| `grep -r` | `grep(1)` | 7923 → 120 | 98.5% |
| `find -mtime` | `find(1)` | 23595 → 62 | 99.7% |
| `tar --strip-components` | `tar(1)` | 10959 → 29 | 99.7% |
| `ip link set` | `ip-link(8)` | 22366 → 219 | 99.0% |
| `ip route add` | `ip-route(8)` | 11340 → 216 | 98.1% |
| `nft masquerade` | `nft(8)` | 60306 → 234 | 99.6% |
| `open(2)` | `open(2)` | 11585 → 194 | 98.3% |

Both GCC rows retain the supplied `>99.9%` notation.

## Practical examples

For `rsync --delete`, the full local `rsync(1)` manual is approximately
65,026 estimated tokens. AKAMAN returns 107 estimated tokens covering:

- `--delete`
- `--delete-before`
- `--delete-during`
- `--delete-delay`
- `--delete-after`
- `--delete-excluded`

That is a 99.8% context reduction.

For `gcc -fPIC`, the full local `gcc(1)` manual is approximately 366,562
estimated tokens. AKAMAN returns 125 estimated tokens, a reduction of >99.9%.

## How it works

```text
Local documentation
        ↓
      AKAMAN
        ↓
smallest authoritative fragment
        ↓
     AI model
```

Reading a large man page locally does not consume model-context tokens. Only
the returned AKAMAN fragment enters the model context.

## Current sources

| Source | Documentation |
| --- | --- |
| `man` | Installed man pages |
| `doc` | Installed `/usr/share/doc` documentation |
| `headers` | Installed C/C++ system headers |

The installed system remains authoritative. AKAMAN uses no copied
documentation corpus, vector database, embeddings, cloud lookup, or required
AI-generated summary.

## Token estimate methodology

**Estimated tokens = ceil(bytes / 4)**

This is a consistent comparative estimate, not an exact tokenizer measurement
for any particular model. It should not be interpreted as an exact GPU or
token-billing savings figure.

## Performance

For the representative 13-query benchmark:

- Average query time: approximately 0.307 seconds
- Median query time: approximately 0.217 seconds

## Validation

The current verified test result is:

- 162 tests
- 162 passed
- 0 failed

Coverage includes man-page resolution, focused option extraction, named
sections, bundled short options, progressive disclosure, `/usr/share/doc`,
system headers, token budgets, malformed input, command-injection cases,
CLI/MCP parity, MCP protocol handling, and HTTP authentication behaviour.

The documentation already knows the answer. AKAMAN makes sure the model only receives the part it actually needs.
