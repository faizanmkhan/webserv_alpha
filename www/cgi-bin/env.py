#!/usr/bin/env python3
import os
print("Content-Type: text/plain\r\n\r\n", end="")
for k in sorted(os.environ):
    print("%s=%s" % (k, os.environ[k]))
