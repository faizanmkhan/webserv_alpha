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
