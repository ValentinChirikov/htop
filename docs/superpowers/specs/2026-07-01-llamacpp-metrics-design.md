# llama.cpp throughput meter

Date: 2026-07-01
Status: Approved

## Goal

Add an htop meter that shows a local llama.cpp server's token throughput and
request state, scraped from its Prometheus `/metrics` endpoint. Enabled via an
environment variable that also carries the endpoint URL.

## Enablement

- Env var `HTOP_LLAMACPP_METRICS_URL`, e.g.
  `http://127.0.0.1:2233/metrics`.
- Presence enables scraping; unset → meter is inert and renders `N/A`.
- Only plaintext `http://` is supported; `https://` (or an unparseable URL)
  is treated as unsupported → `N/A`.

## Module

New `linux/LlamaCpp.c` / `linux/LlamaCpp.h`, registered in
`linux/Platform.c` `Platform_meterTypes[]` (same pattern as `IPMIPower`).
Added to `Makefile.am` (platform_linux headers + sources).

## Data flow

1. Parse the env URL once into host / port / path (default path `/metrics`).
2. Scrape, throttled to at most once per ~900 ms (cached reading reused
   otherwise):
   - `getaddrinfo(host, port)`
   - non-blocking `connect()`, `poll()` with a ~300 ms timeout
   - send `GET <path> HTTP/1.0\r\nHost: <host>\r\nConnection: close\r\n\r\n`
   - read the response with a bounded total timeout
3. Parse the Prometheus text body: for each target metric name, find the line
   beginning `"<name> "` and read the trailing number.

All socket operations are non-blocking / time-bounded so the UI never stalls.
On any failure the meter renders `N/A`.

## Displayed metrics

| Field       | Prometheus metric                   | Rendered as   |
|-------------|-------------------------------------|---------------|
| tg          | `llamacpp:predicted_tokens_seconds` | `tg 45.2`     |
| pp          | `llamacpp:prompt_tokens_seconds`    | `pp 658.8`    |
| processing  | `llamacpp:requests_processing`      | `req 2/0`     |
| deferred    | `llamacpp:requests_deferred`        | (the `/0`)    |
| max tokens  | `llamacpp:n_tokens_max`             | `ntok 48088`  |

Text render: `tg 45.2 pp 658.8 t/s  req 2/0  ntok 48088`, caption `llama`.

Text-only meter (`defaultMode = TEXT_METERMODE`, supported modes = text only):
five heterogeneous values make a bar/graph meaningless.

## Non-goals

- No TLS/HTTPS, no auth, no redirects (localhost plaintext scrape only).
- No new build dependency (no libcurl); raw sockets only.
- No historical graphing of throughput.

## Testing

Endpoint is live. Verification is manual: seed an htoprc with the LlamaCpp
meter in text mode and `HTOP_LLAMACPP_METRICS_URL` set, capture the rendered
line, and compare against a direct `curl` of `/metrics`. Also confirm the meter
shows `N/A` when the env var is unset.
