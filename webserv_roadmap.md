# Webserv — A Beginner's Complete Roadmap

*A step-by-step plan for building a HTTP server in C++98 (42 project), written for someone who has never done socket programming.*

---

## How to use this document

Work through the phases **in order**. Each phase has: a goal, concepts to learn first, what to build, how to test it, and a "done when" checklist. Do not move on until the checklist passes. Phases 0–2 are throwaway learning code; from Phase 3 onward you build the real project.

Rough total time: **6–9 weeks** part-time solo, faster in a team (suggested split: config parser / core event loop / HTTP layer / CGI).

---

## 1. The big picture — what are you actually building?

A web server is a program that:

1. **Listens** on one or more TCP ports (e.g., `8080`).
2. Accepts **connections** from clients (browsers, `curl`, `telnet`).
3. Reads a **plain-text request** from each connection.
4. Figures out what the client wants (a file? run a script? upload something?).
5. Writes back a **plain-text response** (possibly with binary content like an image attached).

That's it. HTTP is just structured text over a TCP connection.

```
   Browser                         Your webserv
      |                                 |
      |--- TCP connect to :8080 ------->|  accept()
      |--- "GET /index.html HTTP/1.1" ->|  recv(), parse
      |                                 |  find ./www/index.html
      |<-- "HTTP/1.1 200 OK ... <html>" |  send()
      |                                 |
```

The hard part of this project is **not** HTTP. It's doing all of the above for *hundreds of clients at once, in a single thread, without ever blocking* — using one `poll()` loop.

### What a raw HTTP exchange looks like

Request (every line ends with `\r\n`, headers end with an empty line):

```
GET /index.html HTTP/1.1\r\n
Host: localhost:8080\r\n
User-Agent: curl/8.5.0\r\n
Accept: */*\r\n
\r\n
```

Response:

```
HTTP/1.1 200 OK\r\n
Content-Type: text/html\r\n
Content-Length: 44\r\n
\r\n
<html><body><h1>It works!</h1></body></html>
```

If you understand this exchange deeply, half the project makes sense already.

---

## 2. Socket programming crash course (read this before Phase 1)

### What is a socket?

A socket is just a **file descriptor** — a small integer, like the ones you get from `open()` — except it represents a network connection instead of a file. You `read()`/`recv()` and `write()`/`send()` on it.

### The server-side lifecycle (memorize this)

```
socket()      → create an fd ("install a phone line")
setsockopt()  → SO_REUSEADDR ("allow reusing the number right after hanging up")
bind()        → attach it to an IP:port ("give the line a phone number")
listen()      → start queueing incoming calls
accept()      → answer ONE call → returns a NEW fd for that specific client
recv()/send() → talk with that client on the new fd
close()       → hang up
```

Key insight for beginners: the **listening socket** and each **client socket** are different fds. The listener only ever produces new client fds via `accept()`; you never `recv()` on the listener itself.

### Blocking vs non-blocking

By default, `recv()` **blocks**: your whole program freezes until data arrives. With one client that's fine; with two, client B starves while you wait for client A. The subject forbids this.

`fcntl(fd, F_SETFL, O_NONBLOCK)` makes calls return immediately instead of waiting. But now you need a way to know *when* an fd is ready — that's `poll()`.

### poll() — the restaurant analogy

Imagine a restaurant with **one waiter** (your single thread). A blocking waiter stands at one table until those guests finish everything. A `poll()` waiter carries a pager board: *"table 3 wants to order (`POLLIN`), table 7 is ready for the bill (`POLLOUT`)"* — and only visits tables that are ready. Nobody is ever ignored, and one waiter serves the whole room.

```c
struct pollfd p;
p.fd      = some_fd;
p.events  = POLLIN | POLLOUT;   // what you want to know about
poll(array_of_pollfds, count, timeout_ms);
// afterwards, p.revents tells you what actually happened
```

- `POLLIN` on a **listener** = a new client is waiting → `accept()`.
- `POLLIN` on a **client** = bytes arrived → `recv()` once.
- `POLLOUT` on a **client** = the OS can take more outgoing bytes → `send()` once.
- `POLLHUP` / `POLLERR` = connection is dead → clean up.

`poll()` works on both Linux and macOS, matches the subject wording, and is the simplest to reason about — **use `poll()`** unless you have a strong reason to use `epoll`/`kqueue`.

### Partial reads and writes (this will bite you)

TCP is a **byte stream**, not a message system. One `recv()` may return half a request, or two requests glued together. One `send()` may accept only part of your response. Therefore every client needs:

- a **read buffer** you keep appending to until the parser says "request complete", and
- a **write buffer** + offset you keep draining across multiple `POLLOUT` events.

Never assume one `recv()` = one request or one `send()` = whole response.

---

## 3. Ground rules from the subject (grade-killers — memorize)

1. **One `poll()`** (or equivalent) drives *all* socket/pipe I/O — listeners, clients, and CGI pipes together, monitoring reads and writes.
2. **Never** `recv`/`send`/`read`/`write` on a socket or pipe without `poll()` having reported readiness first. (Regular disk files are exempt — plain `read()`/`write()` on files is fine.)
3. **Never check `errno` after a read or write** to decide behavior. Use return values only: `> 0` = got bytes, `0` = peer closed (for recv), `-1` = treat the connection as dead and clean it up. No `if (errno == EAGAIN)` — ever.
4. `fork()` is allowed **only for CGI**.
5. The server must **never crash** (even out of memory) and never leak fds.
6. Requests must **never hang forever** → implement timeouts (use `poll()`'s timeout argument).
7. **Accurate status codes** and **default error pages** built in.
8. C++98 only, `-Wall -Wextra -Werror`, no external/Boost libraries, only the allowed functions list.
9. Config file as `argv[1]` (or a default path if none given).
10. Must work in a **real browser**, and behavior can be compared against NGINX.

Print this list and tape it to your monitor.

---

## 4. Target architecture

```
main()
 ├── Config            (parsed once from the .conf file)
 └── EventLoop / ServerManager
      ├── Listeners: one non-blocking socket per unique host:port
      ├── std::vector<pollfd>   ← the ONE poll set
      ├── std::map<int, Client> ← state per connection fd
      └── loop:
            poll(...)
            ├── listener POLLIN  → accept() → new Client
            ├── client  POLLIN   → recv once → feed RequestParser
            │                      → request complete? → route → build Response
            ├── client  POLLOUT  → send pending bytes once
            ├── CGI pipe POLLIN  → read script output
            ├── CGI pipe POLLOUT → write request body to script
            └── timeout          → close idle clients, kill stuck CGI
```

Each `Client` owns: read buffer, an incremental `Request` parser (state machine), a `Response` (headers + body + send offset), a state (`READING`, `HANDLING`, `CGI_RUNNING`, `WRITING`, `CLOSING`), and a last-activity timestamp.

### Suggested repository layout

```
webserv/
├── Makefile
├── README.md
├── config/
│   ├── default.conf
│   └── eval.conf
├── www/                     # demo site for the evaluation
│   ├── index.html  style.css  img/
│   ├── errors/404.html  errors/500.html
│   ├── uploads/             # upload_store target
│   └── cgi-bin/hello.py  echo_post.py  slow.py
├── src/
│   ├── main.cpp
│   ├── config/   Lexer  Parser  ServerConfig  LocationConfig
│   ├── core/     EventLoop  Listener  Client
│   ├── http/     Request  RequestParser  Response  StatusCodes  Mime
│   ├── handlers/ StaticHandler  Upload  Delete  Autoindex  Redirect
│   └── cgi/      CgiHandler
└── tests/        Python test scripts
```

---

## 5. The step-by-step build plan

### Phase 0 — Setup & study (2–3 days)

**Goal:** understand HTTP by touching it, and set up the skeleton.

**Do:**
- Create the git repo, `Makefile` skeleton (`$(NAME)=webserv`, `all`, `clean`, `fclean`, `re`, no relinking), empty `main.cpp`, README skeleton.
- Watch HTTP happen for real:
  - `python3 -m http.server 8000` in one terminal, then in another: `telnet localhost 8000` and *hand-type* `GET / HTTP/1.1`, `Host: localhost`, empty line. Read the raw response.
  - `curl -v http://example.com` — study every header line.
  - If you can, run NGINX and poke it the same way; it's your reference implementation.
- Read **Beej's Guide to Network Programming**, chapters 1–7 (free online). This is the single best socket resource for beginners.
- Skim RFC 1945 (HTTP/1.0) — it's short and readable.

**Done when:** you can hand-type a valid request in telnet and explain every line of the response you get back.

---

### Phase 1 — Hello, socket (2–3 days, throwaway code)

**Goal:** your first working server — blocking, single-client, hardcoded response. This is a learning exercise, not final code.

**Build:** `socket()` → `setsockopt(SO_REUSEADDR)` → `bind()` to port 8080 → `listen()` → loop: `accept()` → `recv()` → print the raw request → `send()` a fixed valid response → `close()`.

The fixed response (get the `\r\n` and `Content-Length` exactly right):

```
HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 13\r\n\r\nHello, world!
```

**Test:** open `http://localhost:8080` in a browser; run `curl -v localhost:8080`. Restart the server twice in a row — without `SO_REUSEADDR` you'll see `Address already in use` (understand why: the old socket lingers in TIME_WAIT).

**Done when:** the browser shows your text, and you can explain: why `accept()` returns a *new* fd, what `bind` does, what the backlog argument of `listen` is.

---

### Phase 2 — The event loop (5–7 days) ← the heart of the project

**Goal:** rebuild Phase 1 as a non-blocking, single-`poll()` server that handles many clients at once. This loop is the skeleton everything else hangs on — take your time.

**Learn first:** re-read section 2 above (poll, partial reads/writes). 

**Build, in this order:**
1. Make the listener non-blocking (`fcntl(fd, F_SETFL, O_NONBLOCK)`), put it in a `std::vector<pollfd>` with `POLLIN`.
2. Main loop: `poll(&fds[0], fds.size(), timeout_ms)`.
3. Listener `POLLIN` → `accept()`, set the new fd non-blocking, create a `Client` object, add to the poll set with `POLLIN`.
4. Client `POLLIN` → `recv()` **once** into a temp buffer:
   ```c
   n = recv(fd, buf, sizeof(buf), 0);
   if (n > 0)  append n bytes to client.read_buffer;
   else        close_and_cleanup(fd);   // n == 0 or -1. Do NOT touch errno.
   ```
5. First make it an **echo server** (send back whatever arrives). Then: when the read buffer contains `\r\n\r\n`, queue the hardcoded HTTP response into the client's write buffer and start monitoring `POLLOUT` for that fd.
6. Client `POLLOUT` → `send()` **once** from `write_buffer + offset`; advance the offset by the return value; when fully sent, either reset the client for the next request (keep-alive) or close.
7. Cleanup helper: `close(fd)`, remove from the pollfd vector, erase the `Client`. Every exit path must go through it.

**Rules check:** you are now living under rules 1–3 from section 3. One `recv`/`send` per readiness event; decisions based on return values only.

**Test:**
- Two terminals running `curl` simultaneously; then `for i in $(seq 1 100); do curl -s localhost:8080 & done; wait`.
- Connect with telnet, type *nothing*, and confirm other clients still get served.
- Check for fd leaks: `ls /proc/$(pidof webserv)/fd | wc -l` stays stable after many requests.

**Done when:** 100 parallel curls all succeed, a silent telnet client blocks nobody, and fd count is stable.

---

### Phase 3 — Configuration file (4–6 days)

**Goal:** parse an NGINX-inspired config into clean C++ structures. Pure string work — no sockets involved, easy to unit test.

**Design a grammar like:**

```nginx
server {
    listen 127.0.0.1:8080;
    server_name mysite;              # optional (virtual hosts are out of scope)
    error_page 404 /errors/404.html;
    client_max_body_size 10M;

    location / {
        methods GET;
        root ./www;
        index index.html;
        autoindex off;
    }
    location /uploads {
        methods GET POST DELETE;
        root ./www;
        upload_store ./www/uploads;
        autoindex on;
    }
    location /cgi-bin {
        methods GET POST;
        root ./www;
        cgi_ext .py /usr/bin/python3;
    }
    location /old {
        return 301 /;
    }
}

server {
    listen 8081;
    root ./site2;                    # a second site on another port
    ...
}
```

**Build:**
1. Tokenizer: split into words, `{`, `}`, `;`, strip comments.
2. Recursive parser → `std::vector<ServerConfig>`, each holding `std::vector<LocationConfig>`.
3. Validation with clear error messages (unknown directive, missing `;`, duplicate listen, invalid port, unreadable root...). A bad config must print an error and exit cleanly — never crash.
4. Defaults for anything omitted (e.g., body size limit, index).
5. A debug function that pretty-prints the parsed config.

**Done when:** every deliberately broken config you write produces a clear error; valid configs (including multiple `server` blocks and ports) load and print correctly.

---

### Phase 4 — HTTP request parser (5–8 days)

**Goal:** an **incremental** parser: you feed it bytes as they arrive; it consumes what it can and reports `INCOMPLETE`, `COMPLETE`, or `ERROR(status_code)`.

**Why incremental?** Because of partial reads — the request line may arrive in three pieces across three `POLLIN` events. A state machine handles this naturally:

```
STATE: REQUEST_LINE → HEADERS → BODY (Content-Length or chunked) → DONE
```

**Build:**
1. Request line: method, target, version. Split target into path + query string; percent-decode the path (`%20` → space).
2. Headers into a map with **case-insensitive** keys (`Host`, `host`, `HOST` are equal). Detect end of headers at the empty `\r\n` line.
3. Body: if `Content-Length: N`, read exactly N bytes. If `Transfer-Encoding: chunked`, decode chunks (`size-in-hex\r\n data \r\n ... 0\r\n\r\n`). You *must* support chunked — CGI depends on un-chunking.
4. Enforce limits **while reading**, not after: body over `client_max_body_size` → `413`; absurd header block → `431`; huge URI → `414`. Don't buffer a 2 GB body before noticing it's too big.
5. Error cases → correct codes: garbage request line → `400`; unknown/unsupported version → `505`; HTTP/1.1 without `Host` → `400`.

**Test:** unit-test the parser with raw strings — feed a valid request one byte at a time; feed two pipelined requests in one buffer; feed every malformed case in your table. No sockets needed.

**Done when:** your parser passes a written table of ~20 good/bad inputs, including byte-by-byte delivery and chunked bodies.

---

### Phase 5 — Routing & responses (3–5 days)

**Goal:** connect config + request → the right handler → a correct response.

**Build:**
1. **Server selection:** match by the listener's host:port the client connected to (and `server_name` vs `Host` header if you implement it; otherwise first server for that port wins — NGINX behavior).
2. **Location selection:** longest-prefix match of the path against `location` blocks (`/uploads/cat.png` matches `/uploads` over `/`).
3. Checks in order: method allowed? → `405` + `Allow:` header. `return` directive? → `301`/`302` + `Location:` header.
4. `Response` class: status line, headers (`Content-Type`, `Content-Length`, `Connection`, optionally `Date`, `Server`), body → serialize into the client's write buffer.
5. **Error pages:** one function `makeError(code, server)` used *everywhere* — serves the configured page if it exists, else a built-in HTML fallback. Never send an empty 4xx/5xx.

**Done when:** curl to various routes returns the right code, `-X PATCH` gets a 405 with `Allow`, `/old` redirects (curl `-L` follows it), and an unknown path shows your custom 404 page.

---

### Phase 6 — GET: static files (4–6 days)

**Goal:** serve a full static website — the subject's core deliverable.

**Build:**
1. **Path mapping** exactly as the subject describes: location `/kapouet` with root `/tmp/www` → request `/kapouet/pouic/toto/pouet` → file `/tmp/www/pouic/toto/pouet`.
2. **Security:** normalize the path and reject anything escaping the root (`GET /../../etc/passwd` → `403`/`404`). Test this explicitly — evaluators will.
3. `stat()` the result: regular file → serve it; directory → try the `index` file, else `autoindex` listing if enabled, else `403`; nothing → `404`; no read permission → `403`.
4. **Autoindex:** generate an HTML listing with `opendir`/`readdir`/`closedir`.
5. **MIME types** from extension: html, css, js, png, jpg, gif, ico, svg, txt, pdf → correct `Content-Type`; unknown → `application/octet-stream`. (Wrong MIME = browser shows CSS as text.)
6. Read the file into the response body — plain `read()` is fine here (disk files are exempt from poll) — then let the existing `POLLOUT` machinery stream it out in chunks. Test with a file larger than your send buffer (e.g., a 50 MB file) to prove partial sends work.

**Done when:** a real multi-page demo site with CSS and images renders perfectly in Chrome/Firefox, autoindex works on one directory, and traversal attempts fail safely.

---

### Phase 7 — POST: uploads (4–6 days)

**Goal:** clients can upload files, stored in the location's `upload_store`.

**Build:**
1. Raw-body upload first: `curl -X POST --data-binary @photo.jpg localhost:8080/uploads/photo.jpg` → write body to the store → `201 Created` (+ `Location:` header).
2. Then `multipart/form-data`, which is what **browser forms** send: parse the `boundary` from `Content-Type`, extract each part's `filename` and content. Not strictly demanded by the subject, but you need a browser upload demo at evaluation — implement it.
3. Enforce `client_max_body_size` → `413` (already wired in Phase 4 — now test it per-route).
4. A simple upload form page for the demo site:
   ```html
   <form action="/uploads" method="POST" enctype="multipart/form-data">
     <input type="file" name="file"><input type="submit" value="Upload">
   </form>
   ```

**Done when:** uploading a photo via the browser form stores it, you can GET it back and see it, and an oversized upload cleanly returns 413.

---

### Phase 8 — DELETE (1–2 days)

**Goal:** the third mandatory method.

**Build:** resolve the path like GET; file exists and is writable → delete → `204 No Content`; doesn't exist → `404`; is a directory or lacks permission → `403`.

**Test:** `curl -X POST` an upload, `curl -X DELETE` it, `curl` GET it → 404. Also DELETE on a route that doesn't allow it → `405`.

**Done when:** the upload→delete→404 cycle works and forbidden cases return the right codes.

---

### Phase 9 — CGI (6–10 days — the hardest phase)

**Goal:** execute scripts (Python and/or php-cgi) for matching extensions, fully integrated into the poll loop.

**What CGI is:** instead of sending a file, the server *runs a program*. The request is handed over via **environment variables** + the body on the program's **stdin**; whatever the program prints to **stdout** (its own headers, then a body) becomes the response.

**Build:**
1. On a request whose path matches `cgi_ext`: create two `pipe()`s (server→child stdin, child stdout→server), `fork()`; in the child: `dup2` the pipe ends onto fds 0 and 1, `chdir()` to the script's directory (subject requires correct relative paths), `execve(interpreter, {interpreter, script_path, NULL}, envp)`.
2. Build `envp` with the CGI meta-variables (RFC 3875): `REQUEST_METHOD`, `SCRIPT_NAME`, `SCRIPT_FILENAME`, `PATH_INFO`, `QUERY_STRING`, `CONTENT_TYPE`, `CONTENT_LENGTH`, `SERVER_PROTOCOL=HTTP/1.1`, `GATEWAY_INTERFACE=CGI/1.1`, `SERVER_NAME`, `SERVER_PORT`, `REMOTE_ADDR`, plus every request header as `HTTP_HEADER_NAME` (dashes→underscores, uppercased).
3. In the parent: both pipe ends are **non-blocking and inside your one poll loop** (pipes count as "I/O that can wait" — bypassing poll here is a grade of 0). `POLLOUT` on the stdin pipe → write the (already **un-chunked**) body, then close it so the script sees **EOF**. `POLLIN` on the stdout pipe → collect output until EOF.
4. Parse CGI output: header lines (`Content-Type:`, optional `Status: 404 Not Found`), blank line, body. No `Content-Length` from the script? Then EOF marks the end — you already have the full body, so set `Content-Length` yourself.
5. `waitpid(pid, &st, WNOHANG)` — never block on the child. Keep a start timestamp; on timeout `kill()` it and respond `504 Gateway Timeout`. **This is your "requests never hang" guarantee for CGI.**

**Test scripts to write** (in `www/cgi-bin/`): `hello.py` (prints a header + HTML), `env.py` (dumps all env vars — great for debugging), `echo_post.py` (reads stdin, echoes it back), `slow.py` (`sleep(9999)` — must trigger your 504 while other clients keep working).

**Done when:** a browser form POST is echoed back by `echo_post.py`, a chunked request (`curl -T file -H "Transfer-Encoding: chunked" ...`) reaches the script un-chunked, and `slow.py` returns 504 without freezing the server.

---

### Phase 10 — Multi-server & robustness (3–5 days)

**Goal:** the polish that makes it production-ish.

**Build:**
1. Multiple `server` blocks on **different ports serving different content** — an explicit subject requirement.
2. **Keep-alive:** after a response, reset the client's parser and serve the next request on the same connection; honor `Connection: close` (and default-close for HTTP/1.0).
3. **Idle timeouts:** give `poll()` a timeout (e.g., 1000 ms); on each wakeup, close clients idle past N seconds and reap/kill expired CGI.
4. `signal(SIGPIPE, SIG_IGN)` — otherwise a client vanishing mid-`send()` kills your whole server.
5. Sweep for crash-proofing: every `new`/container growth that could throw, every syscall return value, every parser edge case.

**Done when:** two different sites load from two ports simultaneously; a browser reloading many times reuses connections; abandoned connections evaporate after the timeout.

---

### Phase 11 — Testing & hardening (4–6 days, plus continuously)

**Goal:** the subject says it plainly — *resilience is key*.

**Torture with telnet:** type a request one character per second (tests partial reads + timeout); send garbage bytes; send a 1 MB header line; open a connection and walk away.

**Stress:** `siege -b -c 50 -t 60S http://localhost:8080/` — availability should stay ~100%, memory flat (watch with `top`), fd count flat.

**Leaks:** `valgrind --leak-check=full --track-fds=yes ./webserv config/default.conf`, then run your test suite against it.

**Write a Python test script** (the subject encourages tests in another language): spawn 50 threads doing mixed GET/POST/DELETE/CGI requests including malformed ones, assert status codes. Keep it in `tests/` — it doubles as an evaluation demo.

**Compare with NGINX** whenever unsure what a correct response looks like (codes, headers, redirect behavior).

**Done when:** an hour of mixed stress + torture leaves the server responsive with flat memory/fd usage and zero crashes.

---

### Phase 12 — README & defense prep (1–2 days)

The subject imposes a strict README format:

```markdown
*This project has been created as part of the 42 curriculum by yourlogin.*

## Description
What webserv is, its goal, a short overview of the architecture.

## Instructions
How to compile (make), run (./webserv config/default.conf), and test.

## Resources
- Links: Beej's guide, RFCs, MDN, NGINX docs...
- **AI usage:** exactly which tasks and which parts of the project AI
  helped with (required by the subject — keep notes as you go!).
```

Also prepare for the oral evaluation: a cheat-sheet of demo commands (curl lines for every feature), the eval config + demo site ready, and — most importantly — make sure you can **explain every line of your code**. Evaluators will ask you to justify the event loop, the errno rule, and your CGI design.

---

## 6. Classic mistakes that fail evaluations

- **Looping recv/send "until EAGAIN"** — that *is* checking errno. One read/write per readiness event; `poll()` (level-triggered) will re-notify you if more remains.
- **Ignoring partial `send()`** — it can write 10 bytes of your 5 MB response. Always keep an offset.
- **Closed fd left in the poll set** → `POLLNVAL` storms or ghost events. One cleanup function, used everywhere.
- **CGI pipes outside the poll loop** — explicit grade-0 territory ("I/O that can wait... must be driven by a single poll").
- **Forgetting to close the CGI stdin pipe** after writing the body — the script waits for EOF forever → your 504 fires or the client hangs.
- **Not un-chunking before CGI** — the script must never see chunk framing.
- **Buffering an unbounded body before checking the size limit** — enforce 413 *while* reading.
- **Path traversal** (`/../../etc/passwd`) not blocked.
- **SIGPIPE** not ignored — one rude client kills the server.
- **No `SO_REUSEADDR`** — server won't restart during your own demo.
- **Assuming text** — images are binary; use length-tracked buffers (`std::string::append(buf, n)`), never `strlen`-style logic on payloads.
- **`Content-Length` off by even one byte** — browsers hang waiting or truncate. Count carefully.
- **Blocking `waitpid`** on a CGI child while other clients starve — use `WNOHANG`.

---

## 7. Testing toolkit (keep this handy)

```bash
# Basics
curl -v http://localhost:8080/
curl -v http://localhost:8080/does_not_exist            # your 404 page
curl -v -X PATCH http://localhost:8080/                 # 405 + Allow
curl -v -L http://localhost:8080/old                    # redirect

# Body / uploads
curl -v -X POST --data-binary @big.jpg localhost:8080/uploads/big.jpg
curl -v -X POST --data "$(head -c 20000000 /dev/zero | tr '\0' 'a')" localhost:8080/uploads/x   # 413
curl -v -X DELETE localhost:8080/uploads/big.jpg

# CGI
curl -v "localhost:8080/cgi-bin/env.py?a=1&b=2"
curl -v -X POST -d "name=hello" localhost:8080/cgi-bin/echo_post.py
curl -v -T file.txt -H "Transfer-Encoding: chunked" localhost:8080/cgi-bin/echo_post.py
curl -v localhost:8080/cgi-bin/slow.py                  # expect 504, server alive

# Raw / torture
telnet localhost 8080          # hand-type requests, slowly, or not at all
printf 'GET / HTTP/1.1\r\nHost: x\r\n\r\n' | nc localhost 8080

# Stress & leaks
siege -b -c 50 -t 60S http://localhost:8080/
valgrind --leak-check=full --track-fds=yes ./webserv config/default.conf
ls /proc/$(pidof webserv)/fd | wc -l
```

---

## 8. Resources

- **Beej's Guide to Network Programming** — the beginner bible for sockets (free, beej.us).
- **RFC 1945** (HTTP/1.0, short and readable — the subject's suggested baseline), **RFC 7230 & 7231** (HTTP/1.1 message syntax & semantics), **RFC 3875** (CGI — the env-variable list lives here).
- **MDN Web Docs → HTTP** — friendliest explanation of methods, status codes, headers, MIME types.
- **NGINX beginner's guide** — for config-file inspiration and as the behavior reference.
- `man 2 socket bind listen accept poll fcntl send recv` and `man 7 ip tcp`.

---

## 9. Suggested timeline (solo, part-time)

| Weeks | Phases |
|-------|--------|
| 1     | 0–1: study, telnet experiments, first blocking server |
| 2     | 2: the poll event loop (don't rush this) |
| 3     | 3–4: config parser, request parser |
| 4     | 5–6: routing, responses, static site |
| 5     | 7–8: uploads, DELETE |
| 6–7   | 9: CGI |
| 8     | 10–11: multi-server, hardening, stress tests |
| 9     | 12: README, eval prep, buffer for surprises |

Good luck — the day a real browser renders your site through code you wrote from raw sockets is genuinely one of the best moments in the 42 curriculum.
