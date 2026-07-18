#!/usr/bin/env python3
import sys
data = sys.stdin.read()
print("Content-Type: text/plain\r\n\r\n", end="")
print("You sent %d bytes: %s" % (len(data), data))
