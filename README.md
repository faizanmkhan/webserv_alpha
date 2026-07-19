*This project has been created as part of the 42 curriculum by faikhan, iskaraag, and mkwizera.*

## Description

webserv is an HTTP/1.1 server written from scratch in C++98. A **single,
level-triggered `epoll` instance** drives *all* I/O — listening sockets,
client sockets, and CGI pipes — through one `epoll_wait()` per loop
iteration; every socket and pipe fd is non-blocking, and read/write
decisions are made from return values alone (never `errno`). Behaviour is
configured by an nginx-style config file: multiple `listen host:port`
pairs, custom and default error pages, `client_max_body_size`, and
per-location rules for allowed methods, redirects, root mapping, autoindex,
uploads, and CGI by extension. It serves **static files** (with autoindex
and MIME types), handles **GET / POST / DELETE**, accepts **uploads** (raw
and `multipart/form-data`) from a real browser, un-chunks
`Transfer-Encoding: chunked` request bodies, and runs **CGI** scripts
(Python/PHP-CGI) — forked children whose stdin/stdout pipes are driven by
the same epoll loop, with a timeout that guarantees no request hangs
forever. Connections are **persistent** (HTTP/1.1 keep-alive, honouring
`Connection: close` and HTTP/1.0 defaults), **idle connections are reaped**
after a timeout, `SIGPIPE` is ignored so a client vanishing mid-`send()`
can't kill the server, and any per-connection exception (including
out-of-memory) is contained to that one connection rather than crashing the
process. Multiple `server` blocks on **different ports serve different
content** through the single epoll loop.

## Instructions

### Compilation

```
make
```

### Usage

```
./webserv [configuration file]
```

If no configuration file is given, a default path is used.

### Testing

Start the server with the demo config, then run the commands below from
another terminal:

```
./webserv config/default.conf
```

**Static GET**

```
curl -v http://127.0.0.1:8080/            # index.html
curl -v http://127.0.0.1:8080/uploads/    # autoindex listing
```

**POST upload (raw body)** — writes the request body to the location's
`upload_store` and returns `201 Created` with a `Location` header:

```
# upload text, then read it back off disk and over HTTP
curl -v --data-binary "hello upload" http://127.0.0.1:8080/uploads/test.txt
cat www/uploads/test.txt
curl -v http://127.0.0.1:8080/uploads/test.txt

# upload a binary file (e.g. an image)
curl -v --data-binary @some.png http://127.0.0.1:8080/uploads/some.png
```

**Body size limit** — an upload larger than `client_max_body_size` is
rejected with `413` before the body is buffered:

```
curl -v --data-binary @big.iso http://127.0.0.1:8080/uploads/big.iso
```

**Method not allowed** — `/` only permits GET, so a POST there returns
`405` with an `Allow` header:

```
curl -v --data "x" http://127.0.0.1:8080/
```

**DELETE** — removes a file under a location that permits DELETE and
returns `204 No Content`; the full lifecycle:

```
curl -v --data-binary "temp" http://127.0.0.1:8080/uploads/gone.txt  # 201
curl -v -X DELETE http://127.0.0.1:8080/uploads/gone.txt             # 204
curl -v http://127.0.0.1:8080/uploads/gone.txt                       # 404
```

### CGI

The `/cgi-bin` location runs a script for a matching extension
(`cgi_ext .py /usr/bin/python3`). The scripts live in `www/cgi-bin/`
(`hello.py`, `echo_post.py`, `slow.py`). The server hands the request to
the script via environment variables + stdin and turns the script's stdout
into the response; the child's pipes are driven by the same epoll loop as
the sockets.

**GET CGI** — runs the script and returns its output:

```
curl -v http://127.0.0.1:8080/cgi-bin/hello.py
curl -v "http://127.0.0.1:8080/cgi-bin/env.py?a=1&b=2"   # QUERY_STRING=a=1&b=2
```

**POST to CGI** — the request body is delivered on the script's stdin:

```
curl -v -X POST -d "name=hello" http://127.0.0.1:8080/cgi-bin/echo_post.py
```

**Chunked request → CGI** — the body is un-chunked *before* the script
sees it, so the CGI never receives chunk framing (identical result to the
`Content-Length` form above):

```
curl -v -X POST -H "Transfer-Encoding: chunked" \
     --data-binary "name=hello" http://127.0.0.1:8080/cgi-bin/echo_post.py
```

**CGI timeout** — a script that never finishes is killed after the
configured limit and answered with `504`, while other clients keep being
served throughout (the whole point of driving the CGI pipes through epoll):

```
curl -v http://127.0.0.1:8080/cgi-bin/slow.py    # 504 after the timeout
```

**Missing script / broken CGI** — a non-existent script returns `404`; a
script that produces no output returns `502 Bad Gateway`:

```
curl -v http://127.0.0.1:8080/cgi-bin/nope.py    # 404
```

### Keep-alive & connection handling

HTTP/1.1 connections are persistent: after a response the parser resets and
the next request is served on the **same** socket. `curl` reuses the
connection for two requests (`Re-using existing connection`), and an
explicit `Connection: close` is honoured:

```
curl -v http://127.0.0.1:8080/ http://127.0.0.1:8080/           # Re-using existing connection
curl -v -H "Connection: close" http://127.0.0.1:8080/           # Closing connection
curl -v --http1.0 http://127.0.0.1:8080/                        # served, then closed (HTTP/1.0 default)
```

Idle connections are reaped after a timeout, so an abandoned or slow client
cannot hold an fd forever (slow-loris defence). Open a raw socket and walk
away — it disappears on its own, while normal requests keep working:

```
( exec 3<>/dev/tcp/127.0.0.1/8080; sleep 40 ) &   # idle socket is closed after CLIENT_TIMEOUT
```

### Multiple servers on different ports

Two `server` blocks on different ports serve different content through the
single epoll loop:

```
./webserv config/two_sites.conf
curl -s http://127.0.0.1:8080/    # content from ./www
curl -s http://127.0.0.1:8081/    # content from ./www2
```

### Robustness

`SIGPIPE` is ignored, and any exception on one connection (including
out-of-memory) is contained to that connection instead of terminating the
server. A burst of concurrent and malformed requests leaves it responsive:

```
for i in $(seq 1 200); do curl -s -o /dev/null http://127.0.0.1:8080/ & done; wait
printf 'GARBAGE \r\n\r\n' | nc -q1 127.0.0.1 8080
curl -s http://127.0.0.1:8080/    # still serving
```

## HTTP status codes

Codes the server returns, and when. ✅ = implemented, ⏳ = planned in a
later phase.

| Code | Meaning | When webserv returns it |
|------|---------|-------------------------|
| ✅ `200 OK` | Success | A GET that serves a static file, directory index, or autoindex listing. |
| ✅ `201 Created` | Resource created | A successful POST upload; includes a `Location:` header pointing at the stored file. |
| ✅ `204 No Content` | Success, empty body | A successful DELETE; the response carries no body. |
| ✅ `301 Moved Permanently` | Permanent redirect | A location configured with `return 301 <target>`; sends a `Location:` header. |
| ✅ `302 Found` | Temporary redirect | A location configured with `return 302 <target>`. |
| ✅ `400 Bad Request` | Malformed request | Unparseable request line/headers, an upload with no filename in the path, or a malformed chunked body (bad chunk size). |
| ⏳ `411 Length Required` | Missing length | A POST with neither `Content-Length` nor chunked encoding (Phase 9). |
| ✅ `403 Forbidden` | Access denied | Path traversal (`..`), a directory with no index and autoindex off, uploads to a route without `upload_store`, or an unreadable file. |
| ✅ `404 Not Found` | No such resource | The requested path does not exist, or no location matches it. |
| ✅ `405 Method Not Allowed` | Method not permitted | The method is not in the location's allowed `methods`; includes an `Allow:` header listing what is permitted. |
| ✅ `413 Payload Too Large` | Body too big | The request body exceeds `client_max_body_size`; rejected before the body is buffered. |
| ⏳ `414 URI Too Long` | Request target too long | The request line exceeds the configured limit (Phase 11 hardening). |
| ⏳ `431 Request Header Fields Too Large` | Headers too big | The header block exceeds the configured limit (Phase 11 hardening). |
| ✅ `500 Internal Server Error` | Server-side failure | An upload's file write failed, or a CGI child could not be started (`pipe`/`fork` failed). |
| ✅ `501 Not Implemented` | Unsupported method | A method the server does not implement (e.g. PUT/HEAD). |
| ✅ `502 Bad Gateway` | Bad upstream response | A CGI script produced no output at all (e.g. the interpreter/script failed to run). |
| ✅ `504 Gateway Timeout` | Upstream too slow | A CGI child ran past the configured timeout; the server `kill()`s it and answers 504, while other clients keep being served. |
| ⏳ `505 HTTP Version Not Supported` | Bad HTTP version | A request whose version is not `HTTP/1.1` (Phase 11 hardening). |

Every 4xx/5xx is served either from a configured `error_page` file or a
generated HTML fallback, so an error response is never empty.

## Resources

- Beej's Guide to Network Programming
- RFC 1945 (HTTP/1.0), RFC 7230 & 7231 (HTTP/1.1), RFC 3875 (CGI)
- NGINX documentation

### AI usage

AI (Claude Code) was used as a mentor and pair-programmer: explaining the
concepts and design decisions before any code (sockets, the epoll model,
the pipe↔epoll relationship for CGI), reviewing and correcting code we
wrote ourselves, scaffolding some modules under deadline (which we typed in
and tested), and writing verification tests. Every design decision was
worked through as Q&A for oral-defense readiness. A per-session log of what
AI helped with versus what was written by hand is kept in
[`AI_Notes.md`](AI_Notes.md).
