# HTTPS downloads

`jb/download.h` downloads one HTTPS resource into a file on Linux and Windows.
Enable `JHBASE_HOST=ON` and `JHBASE_DOWNLOAD=ON`, prepare the pinned curl
checkout described in [dependencies](dependencies.md#optional-https-downloads),
then link `jb::download`. The module is written in C11 over the libcurl easy
API; no curl type or header appears in `jb/download.h`. It serves host tools,
not firmware.

## Initialization

Call `jb_download_global_init()` once before the threads that download start,
and `jb_download_global_cleanup()` once after they have finished. Every
successful init needs one cleanup; cleanup also waits for the worker threads
of abandoned attempts described below. Init checks the linked library and
returns `HAL_EUNSUPPORTED` when it lacks HTTPS, a TLS backend, asynchronous
name resolution or thread-safe initialization. Between the two calls,
independent `jb_download_file` calls may run on different threads.

`jb_download_info(&info)` reports the linked libcurl without initialization:
the version text, `version_number` as `0xMMmmpp`, the TLS backend (`Schannel`
or `OpenSSL/<version>`) and the `https`, `async_dns` and `threadsafe` flags.
A library built from an upstream git tag reports its version text with a
`-DEV` suffix, for example `8.23.0-DEV`; `version_number` names the release.

## Downloading a file

`jb_download_file(url, destination, &options, &result)` fetches `url` and
stores the response body at `destination`:

- `url` must be an `https` URL. Other schemes, also ones libcurl does not
  build, give `HAL_EPERM`; a malformed URL gives `HAL_EINVAL`. Both checks
  happen before any connection or file access.
- `destination` is an absolute UTF-8 path. Missing parent directories are
  created. The body goes into a temporary file next to it and replaces the
  destination atomically only after a complete transfer with a final 2xx
  status. A failure, a cancellation or another attempt deletes the temporary
  file, and the destination keeps its previous content. Identical bytes leave
  an existing file untouched.
- `options.connect_timeout_ms`, `options.timeout_ms` and
  `options.max_attempts` are required. `max_bytes` limits the body of one
  attempt; 0 allows only an empty body. `max_redirects` limits the redirects
  of one attempt; 0 refuses every redirect.
- `result` is cleared first and describes the last attempt: stored bytes,
  attempts started, the final HTTP status, the proxy's answer to `CONNECT`
  in `proxy_status`, the libcurl result code in `transfer_error` and a short
  UTF-8 `message`.

```c
jb_download_options_t options;
memset(&options, 0, sizeof(options));
options.max_bytes = UINT64_C(512) << 20;
options.connect_timeout_ms = 15000;
options.timeout_ms = 600000;
options.retry_delay_ms = 2000;
options.max_attempts = 4;
options.max_redirects = 5;
jb_download_result_t result;
const hal_status_t status = jb_download_file(
    "https://example.org/tool-1.2.tar.gz", "/home/me/.cache/tools/tool-1.2.tar.gz", &options,
    &result);
if (status != HAL_OK)
  fprintf(stderr, "download failed (%d, HTTP %u, curl %d): %s\n", (int)status,
          (unsigned)result.http_status, (int)result.transfer_error, result.message);
```

The module checks no content. Verify the size and SHA-256 of a downloaded
archive before using it.

## TLS and trust

Certificate and host name verification are always on, for the first URL and
for every redirect, and redirects may lead only to `https` URLs. A redirect to
plain HTTP ends the call with `HAL_EPERM` before the plain server is contacted.

`options.ca_file = NULL` uses the system store, also for an `https` proxy. On
Windows Schannel uses the certificate store of the user and the machine. On
Linux libcurl uses the CA bundle and directory of the distribution that
configure found when the library was built, for example
`/etc/ssl/certs/ca-certificates.crt`; install `ca-certificates` before
configuring. Otherwise `ca_file` names a PEM bundle that replaces the system
store, including the Linux CA directory, for the server and for an `https`
proxy.

Schannel also checks the revocation of every certificate in the chain and
fetches the CRLs named in them, so a chain without reachable revocation data
is refused. OpenSSL on Linux checks no revocation.

A rejected certificate, host name or revocation status gives `HAL_EAUTH`.
An unreadable `ca_file` gives `HAL_EINVAL`. Other TLS handshake failures give
`HAL_EPROTO`. None of them starts another attempt.

## Attempts, timeouts and cancellation

`timeout_ms` is the budget of the whole call. It covers name resolution,
connections, transfers and the waits between attempts, and every attempt gets
the time that remains. `connect_timeout_ms` limits one connection including
DNS and the TLS handshake. Name resolution runs on a libcurl helper thread,
so the limits also bound a slow DNS server: when an attempt ends during a
lookup, the call returns at once and the helper thread keeps waiting for the
system resolver on its own, then ends.

Another attempt follows only a transient failure: a failed name resolution or
connection, a connection limit that expired while the budget still had time,
an interrupted transfer, or HTTP 408, 429, 500, 502, 503 or 504 from the server
or from a proxy that refuses the `CONNECT` tunnel. The call waits
`retry_delay_ms` first, or longer when the server sends `Retry-After`. When the
wait would reach the end of the budget, the call returns the last failure
instead. Exhausted attempts report that failure: `HAL_EAGAIN` for the HTTP
statuses above, `HAL_ETIMEOUT`, `HAL_EIO` or `HAL_ENOENT` for the others.

Final HTTP failures end the call at once: 404 and 410 give `HAL_ENOENT`; 401,
403 and 407 give `HAL_EAUTH`, also when the proxy answers them to `CONNECT`;
any other status outside 2xx, including a 304 without a redirect, gives
`HAL_EPROTO`. A body larger than `max_bytes` gives `HAL_EOVERFLOW`, and an
announced size over the limit ends the attempt before the body is read, also
with `max_bytes` 0. Too many redirects give `HAL_EOVERFLOW` as well. A failed
write of the destination gives the file API status, and `result.bytes` then
includes the bytes that the failed write stored before its error.

libcurl runs each attempt on a worker thread, while the calling thread keeps
both callbacks and the limits. `options.canceled` is polled there before each
attempt, at least every 50 ms while an attempt or a delay between attempts
runs, and once more after a complete body arrived, before it is published;
`true` gives `HAL_ECANCELED`. `options.progress` receives the attempt number,
the bytes stored by that attempt and the announced size (0 when unknown)
whenever one of them changed.

The calling thread also ends an attempt at the deadline and, until TLS is up,
at the connection limit, whatever libcurl is doing. A stopped worker normally
ends within 100 ms and the call removes its temporary file. When a blocking
system call holds the worker longer, for example Schannel fetching a CRL from
a server that does not answer or a stuck disk write, the call returns anyway
and abandons the attempt: the worker deletes its temporary file once the
system call returns, and `jb_download_global_cleanup` waits for it.

## Proxy

`options.proxy = NULL` connects directly and ignores the `https_proxy`,
`ALL_PROXY` and similar environment variables. A proxy URL such as
`http://proxy.example:3128` sends every connection through that proxy; HTTPS
goes through a `CONNECT` tunnel, and `no_proxy` does not apply. Credentials
may be part of the proxy URL, for example `http://user:secret@proxy:3128`, and
use Basic authentication; a proxy that accepts only another method answers
407, which gives `HAL_EAUTH`. The proxy URL may use `http`, `https`, `socks4`,
`socks4a`, `socks5` or `socks5h`; without a scheme it is an HTTP proxy,
whatever its host name. Another scheme or a malformed proxy URL gives
`HAL_EINVAL` before any connection.

## Signals and threads

On Linux and other POSIX systems each worker thread blocks `SIGPIPE` and
consumes one raised on it; the caller's signal mask and the process
disposition stay unchanged. libcurl already sends with `MSG_NOSIGNAL` where
the system offers it, and its name resolution threads inherit the blocked
signal from the worker.

The module keeps no state between calls: no cookies, no HSTS or alt-svc
cache, and it never reads `.netrc`. Each attempt opens a new connection.
