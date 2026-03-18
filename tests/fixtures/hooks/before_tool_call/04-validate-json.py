#!/usr/bin/env python3

import json
import sys


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


payload = json.load(sys.stdin)
if payload.get("event") != "before_tool_call":
    fail("unexpected event")
if not payload.get("timestamp"):
    fail("missing timestamp")
if payload.get("tool_name") != "demo":
    fail("unexpected tool_name")
tool_input = payload.get("tool_input")
if not isinstance(tool_input, dict) or tool_input.get("value") != "from-provider":
    fail("unexpected tool_input")

print("validated before_tool_call", file=sys.stderr)
