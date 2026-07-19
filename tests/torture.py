#!/usr/bin/env python3
"""
Multithreaded torture / conformance test for webserv.

Spawns many worker threads that fire a mix of well-formed and deliberately
malformed requests at the running server and assert the status code the
server answers with. Concurrency is the point: it flushes out shared-state
races, fd leaks, and one-slow-client-blocks-everyone bugs that a sequential
script never reaches.

Usage:
    ./webserv config/default.conf          # in one terminal
    python3 tests/torture.py [host] [port] [seconds] [threads]

Exit code is 0 only if every assertion passed and the server stayed up.
Uses the standard library only (raw sockets, so we can send illegal bytes).
"""

import socket
import sys
import threading
import time

HOST     = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT     = int(sys.argv[2]) if len(sys.argv) > 2 else 8080
DURATION = float(sys.argv[3]) if len(sys.argv) > 3 else 10.0
THREADS  = int(sys.argv[4]) if len(sys.argv) > 4 else 50


def send_raw(payload, read_all=False, timeout=5.0):
    """Send raw bytes on a fresh connection, return (status_int, raw_response).
       status_int is 0 if the server sent nothing parseable."""
    s = socket.create_connection((HOST, PORT), timeout=timeout)
    try:
        s.sendall(payload)
        data = b""
        while True:
            chunk = s.recv(4096)
            if not chunk:
                break
            data += chunk
            if not read_all and b"\r\n" in data:
                # We only need the status line for most assertions.
                break
    finally:
        s.close()
    status = 0
    if data.startswith(b"HTTP/"):
        try:
            status = int(data.split(b" ", 2)[1])
        except (IndexError, ValueError):
            status = 0
    return status, data


def req(method, path, headers=None, body=b""):
    """Build a well-formed HTTP/1.1 request."""
    if isinstance(body, str):
        body = body.encode()
    lines = ["%s %s HTTP/1.1" % (method, path), "Host: %s" % HOST]
    if headers:
        for k, v in headers.items():
            lines.append("%s: %s" % (k, v))
    if body:
        lines.append("Content-Length: %d" % len(body))
    return ("\r\n".join(lines) + "\r\n\r\n").encode() + body


# ---- individual checks; each returns (label, ok_bool) --------------------

def check_get_ok():
    st, _ = send_raw(req("GET", "/"))
    return "GET / -> 200", st == 200

def check_not_found():
    st, _ = send_raw(req("GET", "/does/not/exist/here"))
    return "GET missing -> 404", st == 404

def check_method_not_allowed():
    st, _ = send_raw(req("POST", "/", body="x"))
    return "POST / -> 405", st == 405

def check_upload_delete_cycle(tid):
    path = "/uploads/torture_%d_%d.txt" % (tid, threading.get_ident() % 100000)
    st1, _ = send_raw(req("POST", path, body="payload-%d" % tid))
    st2, _ = send_raw(req("DELETE", path))
    return "POST+DELETE cycle -> 201,204", st1 == 201 and st2 == 204

def check_cgi_get():
    st, _ = send_raw(req("GET", "/cgi-bin/hello.py"), read_all=True)
    return "GET CGI -> 200", st == 200

def check_cgi_post():
    st, _ = send_raw(req("POST", "/cgi-bin/echo_post.py", body="name=hi"),
                     read_all=True)
    return "POST CGI -> 200", st == 200

def check_bad_version():
    st, _ = send_raw(b"GET / HTTP/2.0\r\nHost: x\r\n\r\n")
    return "bad version -> 505", st == 505

def check_bad_content_length():
    st, _ = send_raw(b"POST /uploads/x HTTP/1.1\r\nHost: x\r\n"
                     b"Content-Length: abc\r\n\r\n")
    return "non-numeric CL -> 400", st == 400

def check_length_required():
    st, _ = send_raw(b"POST /uploads/x HTTP/1.1\r\nHost: x\r\n\r\n")
    return "POST no length -> 411", st == 411

def check_uri_too_long():
    payload = b"GET /" + b"a" * 9000 + b" HTTP/1.1\r\nHost: x\r\n\r\n"
    st, _ = send_raw(payload)
    return "giant URI -> 414", st == 414

def check_garbage():
    # Any non-crash answer is acceptable; server must not die.
    st, _ = send_raw(b"\x00\x01\x02 GARBAGE \r\n\r\n")
    return "garbage -> handled", st != 0

def check_keep_alive():
    # Two requests down one socket; both must come back 200.
    s = socket.create_connection((HOST, PORT), timeout=5.0)
    try:
        oks = 0
        for _ in range(2):
            s.sendall(req("GET", "/"))
            data = b""
            while b"\r\n\r\n" not in data:
                chunk = s.recv(4096)
                if not chunk:
                    break
                data += chunk
            if b" 200 " in data.split(b"\r\n", 1)[0]:
                oks += 1
            # drain body if Content-Length present
            head = data.split(b"\r\n\r\n", 1)[0].lower()
            if b"content-length:" in head:
                clen = int(head.split(b"content-length:")[1].split(b"\r\n")[0])
                have = len(data.split(b"\r\n\r\n", 1)[1]) if b"\r\n\r\n" in data else 0
                while have < clen:
                    chunk = s.recv(4096)
                    if not chunk:
                        break
                    have += len(chunk)
        return "keep-alive 2x -> 200", oks == 2
    finally:
        s.close()


CHECKS = [
    check_get_ok, check_not_found, check_method_not_allowed,
    check_cgi_get, check_cgi_post, check_bad_version,
    check_bad_content_length, check_length_required,
    check_uri_too_long, check_garbage, check_keep_alive,
]

# ---- worker loop ---------------------------------------------------------

stop_at = 0.0
lock = threading.Lock()
totals = {}          # label -> [passed, failed]
errors = []


def record(label, ok):
    with lock:
        slot = totals.setdefault(label, [0, 0])
        slot[0 if ok else 1] += 1


def worker(tid):
    i = 0
    while time.time() < stop_at:
        try:
            fn = CHECKS[i % len(CHECKS)]
            if fn is check_upload_delete_cycle:
                label, ok = fn(tid)
            else:
                label, ok = fn()
            record(label, ok)
            # interleave the upload/delete cycle occasionally
            if i % 5 == 0:
                l2, o2 = check_upload_delete_cycle(tid)
                record(l2, o2)
        except Exception as e:               # a connection error counts as failure
            with lock:
                errors.append("thread %d: %r" % (tid, e))
        i += 1


def main():
    global stop_at
    # Fail fast if the server isn't up.
    try:
        send_raw(req("GET", "/"))
    except OSError as e:
        print("Cannot reach %s:%d (%s). Start webserv first." % (HOST, PORT, e))
        return 1

    print("Torturing %s:%d with %d threads for %.0fs ..."
          % (HOST, PORT, THREADS, DURATION))
    stop_at = time.time() + DURATION
    pool = [threading.Thread(target=worker, args=(t,)) for t in range(THREADS)]
    for t in pool:
        t.start()
    for t in pool:
        t.join()

    print("\n%-28s %8s %8s" % ("check", "passed", "failed"))
    print("-" * 46)
    total_fail = 0
    for label in sorted(totals):
        p, f = totals[label]
        total_fail += f
        flag = "" if f == 0 else "  <-- FAIL"
        print("%-28s %8d %8d%s" % (label, p, f, flag))

    if errors:
        print("\nConnection errors (%d), first few:" % len(errors))
        for e in errors[:5]:
            print("  ", e)

    # Server must still answer after the storm.
    try:
        st, _ = send_raw(req("GET", "/"))
        alive = (st == 200)
    except OSError:
        alive = False
    print("\nserver alive after storm:", "YES" if alive else "NO")

    ok = (total_fail == 0 and not errors and alive)
    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
