#!/usr/bin/env python3
body = "hi"
print("Content-Type: text/plain\r")
print("Content-Length: %d\r" % len(body))
print("\r")
print(body, end="")
