#!/usr/bin/env python3

import json
import sys


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


payload = json.load(sys.stdin)
if payload.get("event") != "session_end":
    fail("unexpected event")
if not payload.get("timestamp"):
    fail("missing timestamp")
if payload.get("session_id") != "session-123":
    fail("unexpected session_id")
if payload.get("message_count") != 7:
    fail("unexpected message_count")

print("validated session_end", file=sys.stderr)
