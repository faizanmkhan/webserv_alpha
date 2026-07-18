# AI Notes

Running log of what AI (Claude Code) helped with each session, for the README's AI-usage section.

## 2026-07-17 — Phase 1 & Phase 2

- **Phase 1 (blocking server):** AI reviewed my socket/accept/recv/send code line-by-line and caught bugs I introduced myself — using the listening fd instead of `accept()`'s returned fd (closed my own listener), a buffer over-read from a hardcoded `send()` length, and a `Content-Length` mismatch — then quizzed me on the socket lifecycle for oral-defense prep.
- **Architecture decision:** discussed `poll()` vs `epoll()` tradeoffs, including why edge-triggered epoll's typical "read until EAGAIN" pattern would violate the project's no-`errno` rule; I chose epoll (level-triggered), and AI updated the project's constraint notes to match.
- **Phase 2 (epoll event loop):** AI explained the `epoll_ctl`/`epoll_wait` registration model conceptually, then reviewed my incremental build, diagnosing three real bugs I wrote (an unconditional `accept()` clobbering the client fd every iteration, a cleanup call sitting outside the dispatch branches that killed the listener, and a compile-breaking unused variable), and verified the finished loop end-to-end with `curl`, `telnet`, and a concurrent-client test.

## 2026-07-17 — Phase 3 (config parser)

- Working under a self-imposed 2-day deadline, so the collaboration balance shifted: AI wrote more scaffolding directly (`ConfigTypes.hpp` structs, `Lexer.hpp`/`ConfigParser.hpp` declarations, and `ConfigParser`'s token-navigation helpers + recursive-descent structure for `server`/`location` blocks), while I focused on the directive-specific parsing logic layered on top.
- Tokenizer (`Lexer.cpp`): I attempted the character-scanning loop myself after understanding the algorithm conceptually, got stuck assembling it, and AI provided the full working implementation, which I typed in, built, and verified against a real config file myself.
- Directive parsing: AI worked through one directive (`listen`) with me in detail so I understood the pattern (read directive name → consume its value token(s) → store into the struct → expect the terminating `;`), then gave worked examples for the rest (`server_name`, `error_page`, `client_max_body_size`, `methods`, `root`, `index`, `autoindex`, `upload_store`, `cgi_ext`, `return`), which I typed in and tested incrementally, one directive at a time.
- AI found and fixed a real bug in code it had written for me: `current()`/`advance()` could read past the end of the token vector on a truncated config file. Confirmed as genuine memory corruption (not just a hang) by rebuilding under AddressSanitizer, which is a tool/technique I hadn't used before this session.
- Discussed `struct` vs `class` for `ServerConfig`/`LocationConfig` — kept as public-field structs since the parser fills them incrementally and there's no invariant to protect yet; agreed to revisit as `class` once they gain real behavior (e.g. path-matching in Phase 5).

## 2026-07-17 — Phase 4 (config-driven epoll event loop)

- **Recovering context:** while orienting for today, AI noticed Phase 3's `main.cpp` rewrite had silently dropped Phase 2's working epoll loop entirely (no socket code left in `main.cpp` at all). Chose to rebuild it fresh rather than resurrect the old version sitting in `playground/phase2.cpp`.
- **New constraint surfaced:** converting the config's `host` string into a bindable address can't use `inet_addr`/`inet_pton` — neither is on the allowed-functions list. AI explained `getaddrinfo`/`gai_strerror` as the sanctioned pair (the latter exists specifically because `getaddrinfo` doesn't set `errno`), which hadn't come up before since Phase 1/2 just hardcoded `INADDR_ANY`.
- Working under the same self-imposed deadline as Phase 3, the collaboration balance shifted further: AI wrote `src/server/Listener.cpp` (`createListeningSocket`) and `src/server/EventLoop.cpp` (`runEventLoop`) close to fully, rather than the incremental step-by-step drafting used earlier; I typed both in, wired `main.cpp` myself, and tested with `curl`.
- Core new design idea versus Phase 2 (which only ever had one socket): a `std::map<int, size_t>` built at startup distinguishes listening fds from client fds inside the single epoll instance's dispatch loop. Also: accepted client fds need their own explicit `fcntl(O_NONBLOCK)` call since they don't inherit non-blocking mode from the listening socket.
- AI flagged two deliberate simplifications left unresolved on purpose, not oversights: `send()`'s return value is currently ignored (fine for the tiny canned response, wrong once real response bodies arrive — will need buffering + `EPOLLOUT`/`MOD`), and `epoll_wait` returning -1 unconditionally throws (fine with no signals in the picture yet, will need reconsidering once CGI/`waitpid` land).
- Verified: clean build under `-Wall -Wextra -Werror -std=c++98`; and, since the earlier single-socket test wouldn't have caught a bug in the multi-socket dispatch logic specifically, built a throwaway two-`server`-block config and confirmed both ports respond independently through the one epoll instance.

## 2026-07-18 — HTTP layer: request parsing, routing, static GET (roadmap Phases 4–6)

> **Phase-numbering note for the team:** the "Phase 1–4" labels in the entries above are our *ad-hoc session labels*, and they drifted from `webserv_roadmap.md`. What we called "Phase 4" above (config-driven multi-socket listeners) was really finishing the roadmap's **Phase 2**. This session's work maps to the roadmap's **Phases 4, 5, and 6**. From here on we track `webserv_roadmap.md`'s numbering. Current state: roadmap Phases 0–3 done, 4 mostly done (body parsing deferred), 5 and 6 done.

### Roadmap Phase 4 — HTTP request parser (`src/http/HttpRequest.*`) — PARTIAL
- **What:** `parseRequest()` takes the raw bytes up to `\r\n\r\n` and fills an `HttpRequest` struct: splits the request line into method / target / version, then parses each header line into a `std::map`.
- **Why these decisions:**
  - **Header names are lowercased** at parse time — HTTP header names are case-insensitive (`Host` == `host`), so normalising once means one clean `headers["content-length"]` lookup everywhere instead of case-juggling later.
  - **Query string is split off the path** (`/page?a=1` → path `/page`, query `a=1`). Without this, static file lookups break — the server would `stat("./www/page?a=1")` and 404. The raw query is kept for CGI's `QUERY_STRING` later.
  - **The path is percent-decoded** (`%20`→space, `%2e`→`.`) **inside the parser, before any validation.** This is a deliberate security ordering ("decode/canonicalise first, then validate"): the traversal check downstream must see the *real* path, or an attacker just encodes `..` as `%2e%2e` to slip past it. AI demonstrated both bugs live (a `?query` URL returning 404, and `%2e%2e` traversal not being blocked) before we fixed them.
- **Persistent per-connection read buffer:** because TCP is a byte stream, one `recv()` may deliver half a request. Each connection keeps a `std::string buffer` (in `ClientConnection`) that survives across `epoll_wait` iterations; we only parse once it contains `\r\n\r\n`. Verified by splitting a request across two writes with a `sleep` in between.
- **Deferred on purpose (see "Known gaps" below):** request **body** reading (`Content-Length`), **chunked** decoding, request-size **limits** (413/431/414), and a couple of status codes (505, missing-`Host` 400).

### Roadmap Phase 5 — Routing & responses (`src/http/Router.*`, `src/http/RequestHandler.*`) — DONE
- **Architecture split (important for the defense):** `EventLoop.cpp` is *only* socket/epoll plumbing — it never mentions HTTP status codes. All HTTP decisions live in `RequestHandler.cpp`'s `handleRequest()`, which returns a finished response *string* that the loop just ships. Easier to reason about and to test.
- **Location matching** (`matchLocation`) is **longest-prefix**, not first-match: for `/uploads/cat.png`, `/uploads` must win over `/`. First-match would let a `/` block (listed first) swallow everything.
- **`handleRequest` pipeline, in order:** traversal guard (`..`) → match location → `return` redirect (301/302 + `Location:`) → method allowed? (`405` + **`Allow:` header** listing the location's methods) → serve. The `Allow` list is built from the *same* `loc.methods` used for the check, so it can never disagree with the decision.
- **Error pages funnel through one `errorPage()` function** with a two-step fallback: serve the configured `error_page` file (resolved against the location's `root`) if it reads, else a generated HTML stub — guaranteeing a 4xx/5xx is *never* empty (a subject requirement). Limitation: when there's no matched location (a traversal 403 / no-catch-all 404) there's no `root` to resolve against, so those use the stub.

### Roadmap Phase 6 — GET static files (`RequestHandler.cpp`) — DONE
- **Path mapping:** filesystem path = `location.root + request.path` (e.g. `./www` + `/uploads/hello.txt`).
- **Security:** any decoded path containing `..` → 403 (tested with plain, fully-encoded, and mixed-encoding traversal attempts — all blocked).
- **`stat()` dispatch:** regular file → serve it; directory → prefer the `index` file if it exists, else an **autoindex** listing if `autoindex on`, else 403; missing → 404.
- **Autoindex** builds an HTML directory listing with `opendir`/`readdir`/`closedir`; links are written **absolute** (`/uploads/notes.txt`) so they resolve correctly whether or not the request had a trailing slash.
- **MIME types** from file extension (html/css/js/png/…); unknown → `application/octet-stream`.
- **Response writing via `EPOLLOUT` (this replaced the Phase-2 inline `send`):** once the response string is built we store it in the connection's `writeBuffer` and flip the fd from `EPOLLIN` to `EPOLLOUT` with `epoll_ctl(MOD)`; we only `send()` when epoll reports the socket writable (subject rule: never write without readiness), and on a **partial send** we erase the bytes that went out and keep the rest for the next `EPOLLOUT`. Proven by downloading a 2 MB file intact — a single `send()` can't hold that, so it exercised the partial-write loop.

### What AI did vs. what we wrote
- Under the 2-day deadline, AI wrote the newer modules (`Listener`, `EventLoop`, `RequestHandler`, `Router`) close to fully and explained each design decision; we typed them in, wired `main.cpp`, and ran every test. The earlier request-*parser* string logic we wrote ourselves with AI reviewing and correcting (e.g. a broken header-value trim that used `&&` instead of `||` and modified the wrong variable).
- AI also **audited the whole codebase against the subject constraints** and produced the roadmap-vs-reality status map, and caught the phase-numbering drift documented above.

### Known gaps / why deferring them is safe
- **Body reading + `client_max_body_size`** → this *is* the first step of Phase 7 (POST), so it isn't really "deferred", it's next.
- **Chunked decoding** → required for Phase 7 uploads and mandatory for Phase 9 (CGI must receive an un-chunked body). Does not affect GET.
- **Unbounded read buffer (DoS) + 413/431/414 limits** → the one real robustness hole: a client that never sends `\r\n\r\n` grows memory forever. Features work without it, but evaluators test it, so we close body-size in Phase 7 and header/idle caps in Phase 11 hardening.
- **Timeouts and `SIGPIPE` ignore** → Phase 10 items, both grade-relevant, not yet done.

## 2026-07-18 (session 2) — Roadmap Phase 4 finish: request-body reading + `client_max_body_size`

- **Deadline update:** the self-imposed deadline was extended to **Monday 2026-07-20** (weekend added as working days), so the pace eased slightly — still teach-first, but scaffolding help stays on the table.
- **The core new idea — a two-stage completeness check** in `EventLoop.cpp`. The old loop had a *one-stage* rule ("saw `\r\n\r\n` → request done"), correct only for bodiless GETs. Now:
  - **Stage 1:** is the whole header block here? (`buffer.find("\r\n\r\n")`) — if not, wait for the next `EPOLLIN`.
  - **Stage 2:** parse the headers, read `Content-Length`, and only dispatch once `buffer.size() >= headerEnd + contentLength`; otherwise keep reading. `headerEnd = pos + 4` (the 4 separator bytes `\r\n\r\n`) is the first body byte.
- **`client_max_body_size` → `413 Payload Too Large`**, enforced **in the event loop, before routing** — the whole point is to refuse an oversized upload *up front* from the `Content-Length` header, without ever buffering the body into memory. AI made the case that rejecting as early as you have enough info to reject is both a memory-safety and an anti-DoS decision.
- **Small refactor:** extracted a `flipToWrite(epfd, fd)` helper (the `EPOLLOUT` `MOD` dance now happens in 4 places), and added a `std::string body` field to `HttpRequest`.
- **What AI did vs. what I wrote:** I typed the `HttpRequest.body` field myself; when the branching logic in the read path got hard to track mentally, AI wrote the full revised `EventLoop.cpp` read branch (I read the annotated version rather than diffing it in my head). I ran all tests.
- **Verified with curl:** a small `--data` POST returns (doesn't hang) → body consumed whole; an over-limit body returns `413`; GET still serves normally. Confirmed the `413` fires *before* the `405` method check, proving the ordering.
- **Oral-defense drill** on the new code. Two answers worth keeping: (1) an incomplete body (client promises 5000, sends 3000, goes silent) currently **hangs the connection forever** — this is *why the subject mandates timeouts*, a Phase 10 item; (2) the `<` (strict) in the Stage-2 wait condition is what makes an exactly-complete body count as *done* rather than waiting one read too long.
- **Still deferred (unchanged):** chunked decoding (→ Phase 9/CGI), `411 Length Required` for a POST with no length, `400` on non-numeric `Content-Length`, `414`/`431`. Known `TODO(phase10)`: the read buffer isn't shrunk after dispatch — harmless while responses send `Connection: close`, a real bug once keep-alive lands.

## 2026-07-18 (session 2 cont.) — Roadmap Phase 7: POST uploads (raw + multipart)

- **Stage 1 — raw-body upload** (`serveUpload` in `RequestHandler.cpp`): writes the already-buffered `req.body` to `upload_store + "/" + basename(URL path)`, returns `201 Created` + `Location`. Filename comes from the **last path segment**. Added a `writeFile` helper — the mirror of Phase 6's `readFile`, and the same "**regular files are exempt from epoll**" point applies: a plain synchronous `write()` loop, no epoll registration (only sockets and CGI pipes go in epoll). Failures decided from the return value only (no `errno`).
- **Stage 2 — multipart/form-data** (`parseMultipart`): a browser `<form>` doesn't send raw bytes, it sends a *named-parts envelope* so a form can carry multiple labelled fields with metadata (the original filename lives in the part's `Content-Disposition`, not the URL). Parser: pull `boundary=` from `Content-Type`, find the part with `filename="..."`, and slice the bytes between that part's blank line (`\r\n\r\n`) and the next boundary (`\r\n--BOUNDARY`). Those exact `\r\n` offsets are what keep binary uploads byte-perfect. `serveUpload` now branches on `Content-Type`: multipart → `parseMultipart`, else → raw body.
- **Security (defense point):** the multipart filename is **attacker-controlled**, so it's stripped to basename before use — same guard as the URL path, plus the existing `..` traversal check in `handleRequest`. Two independent layers keep every write inside `upload_store`.
- **Status codes wired:** `201` (+`Location`), `403` (uploads disabled / no `upload_store`), `400` (malformed multipart / no filename), `500` (write failed). Added a full **HTTP status-code reference table** to `README.md` (implemented ✅ vs planned ⏳) and filled in the README **Testing** section with the curl upload commands.
- **Verified:** raw upload (`curl --data-binary`) and multipart upload (`curl -F`) both round-trip a large binary (the `webserv` executable itself) with `cmp … && echo IDENTICAL` — byte-exact. Browser demo page added at `www/upload.html` for the evaluation's "must work from a real browser" requirement.
- **AI vs. me:** AI wrote `parseMultipart` and the revised `serveUpload` with per-line commentary and the `\r\n`-boundary reasoning; I typed them in, built, and ran every curl/browser test. Design discussion (why multipart exists, where the branch lives, filename trust) was worked through as Q&A first.

## 2026-07-18 (session 2 cont.) — Roadmap Phase 8: DELETE

- **Constraint flag (important for the defense):** AI caught that the subject *mandates* DELETE but its allowed-function list (subject.md) contains **no file-removal primitive** — no `unlink`, no `remove`. With only the listed functions, deleting a file is literally impossible, so the list must be read as governing *external/system* functions while a removal call is implied by DELETE being required. We chose **`std::remove`** (`<cstdio>`, C++98 **standard library**) over POSIX `unlink` specifically because it's easier to defend as "standard C++, not an external system call." Defense line ready for the eval.
- **`serveDelete`** reuses GET's path mapping (`loc.root + req.path`), then: `stat` miss → `404`; a directory → `403` (we don't delete dirs); `access(W_OK)` fail → `403`; `std::remove` fail → `403`; success → **`204 No Content`** (empty body — a 204 must carry none). The `..` traversal guard in `handleRequest` protects it for free, same as uploads.
- **Nuance noted:** deleting a file actually needs write permission on the *parent directory*, not the file — but `std::remove` fails (→ our 403) if the dir blocks it, so the outcome is correct regardless.
- **Verified:** the full lifecycle `upload → DELETE (204) → GET (404)`; plus `404` on a missing target, `405` (with `Allow`) when DELETE hits a GET-only route — proving the method check runs *before* any deletion — and `403` on a directory.
- **Milestone:** all three subject-mandatory methods (GET, POST, DELETE) now work. Next: Phase 9 (CGI).
