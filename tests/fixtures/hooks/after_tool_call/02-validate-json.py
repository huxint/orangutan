#!/usr/bin/env python3

import json
import sys


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


payload = json.load(sys.stdin)
if payload.get("event") != "after_tool_call":
    fail("unexpected event")
if not payload.get("timestamp"):
    fail("missing timestamp")
if payload.get("tool_name") != "demo":
    fail("unexpected tool_name")
tool_input = payload.get("tool_input")
if not isinstance(tool_input, dict) or tool_input.get("value") != "from-provider":
    fail("unexpected tool_input")
if payload.get("tool_result") != "tool output":
    fail("unexpected tool_result")
if payload.get("is_error") is not False:
    fail("unexpected is_error")

print("validated after_tool_call", file=sys.stderr)
