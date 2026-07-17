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
