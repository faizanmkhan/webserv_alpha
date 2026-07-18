*This project has been created as part of the 42 curriculum by faikhan, iskaraag, and mkwizera.*

## Description

TODO: one paragraph — what webserv is (an HTTP/1.1 server written from
scratch in C++98) and a short overview of the architecture (single
poll()-driven event loop, nginx-style config file, static files, uploads,
CGI).

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

## HTTP status codes

Codes the server returns, and when. ✅ = implemented, ⏳ = planned in a
later phase.

| Code | Meaning | When webserv returns it |
|------|---------|-------------------------|
| ✅ `200 OK` | Success | A GET that serves a static file, directory index, or autoindex listing. |
| ✅ `201 Created` | Resource created | A successful POST upload; includes a `Location:` header pointing at the stored file. |
| ⏳ `204 No Content` | Success, empty body | A successful DELETE (Phase 8). |
| ✅ `301 Moved Permanently` | Permanent redirect | A location configured with `return 301 <target>`; sends a `Location:` header. |
| ✅ `302 Found` | Temporary redirect | A location configured with `return 302 <target>`. |
| ✅ `400 Bad Request` | Malformed request | Unparseable request line/headers, or an upload with no filename in the path. |
| ⏳ `411 Length Required` | Missing length | A POST with neither `Content-Length` nor chunked encoding (Phase 9). |
| ✅ `403 Forbidden` | Access denied | Path traversal (`..`), a directory with no index and autoindex off, uploads to a route without `upload_store`, or an unreadable file. |
| ✅ `404 Not Found` | No such resource | The requested path does not exist, or no location matches it. |
| ✅ `405 Method Not Allowed` | Method not permitted | The method is not in the location's allowed `methods`; includes an `Allow:` header listing what is permitted. |
| ✅ `413 Payload Too Large` | Body too big | The request body exceeds `client_max_body_size`; rejected before the body is buffered. |
| ⏳ `414 URI Too Long` | Request target too long | The request line exceeds the configured limit (Phase 11 hardening). |
| ⏳ `431 Request Header Fields Too Large` | Headers too big | The header block exceeds the configured limit (Phase 11 hardening). |
| ✅ `500 Internal Server Error` | Server-side failure | An upload's file write failed, or a CGI process misbehaves (Phase 9). |
| ✅ `501 Not Implemented` | Unsupported method | A method the server does not implement (e.g. DELETE until Phase 8, PUT/HEAD). |
| ⏳ `505 HTTP Version Not Supported` | Bad HTTP version | A request whose version is not `HTTP/1.1` (Phase 11 hardening). |

Every 4xx/5xx is served either from a configured `error_page` file or a
generated HTML fallback, so an error response is never empty.

## Resources

- Beej's Guide to Network Programming
- RFC 1945 (HTTP/1.0), RFC 7230 & 7231 (HTTP/1.1), RFC 3875 (CGI)
- NGINX documentation

### AI usage

TODO: keep `AI_Notes.md` updated each session; summarize here which tasks
AI helped with (explaining concepts, reviewing code, writing tests) and
which parts were written by hand.
