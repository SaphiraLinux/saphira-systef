# Akaman MCP Server Test Report

Date: 2026-08-11

## Result

`make test` completed successfully:

```text
217 passed, 0 failed
```

The build and test command exercised the CLI, MCP stdio transport, and webMCP
HTTP transport.

## Coverage

- Native man-page discovery, sections, subcommands, options, bundled options,
  and progressive disclosure.
- Documentation and system-header sources, including traversal and symlink
  escape checks.
- MCP initialize, ping, tools/list, tools/call, malformed requests, invalid
  UTF-8, oversized requests, and CLI/MCP response consistency.
- HTTP authentication, unauthorized response size, valid calls, method
  rejection, and invalid bind-address handling.
- Token-budget and benchmark checks.
- Existing shell metacharacter and command-substitution injection checks.

## SQL injection and XSS checks

Added tests cover these SQL injection-shaped CLI payloads:

- `' OR '1'='1`
- `" OR 1=1 --`
- `x' UNION SELECT NULL--`

They were rejected with no-match responses. The same style of SQLi input was
tested through the documentation source.

An XSS-shaped payload, `"><script>alert(1)</script>`, was tested through the
CLI and MCP transports and through the headers source. It was treated as data,
returned a rejection/no-match response, and produced valid JSON with MCP text
content. No command or file was created by the injection tests.

Akaman does not contain a SQL database or HTML renderer, so the SQLi checks are
input-handling and non-execution tests rather than database-query injection
tests; the XSS checks verify that the server does not execute or corrupt the
payload and that MCP framing remains valid.

## Conclusion

No failures were observed in the tested scope. The updated suite and README
document the added SQLi/XSS coverage.
