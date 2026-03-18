#!/usr/bin/env python3

import json
import sys
import time


MODE = sys.argv[1] if len(sys.argv) > 1 else "normal"


TOOLS = [
    {
        "name": "echo",
        "description": "Echo a message back",
        "inputSchema": {
            "type": "object",
            "properties": {
                "message": {"type": "string"},
            },
            "required": ["message"],
        },
    },
    {
        "name": "structured",
        "description": "Return structured content",
        "inputSchema": {
            "type": "object",
            "properties": {},
            "required": [],
        },
    },
]


def send(message):
    sys.stdout.write(json.dumps(message) + "\n")
    sys.stdout.flush()


for raw_line in sys.stdin:
    line = raw_line.strip()
    if not line:
        continue

    message = json.loads(line)
    method = message.get("method")

    if method == "initialize":
        if MODE == "handshake-timeout":
            time.sleep(11)
            continue

        send(
            {
                "jsonrpc": "2.0",
                "id": message["id"],
                "result": {
                    "protocolVersion": "2024-11-05",
                    "capabilities": {"tools": {}},
                    "serverInfo": {"name": "mock-server", "version": "1.0.0"},
                },
            }
        )
        continue

    if method == "notifications/initialized":
        continue

    if method == "tools/list":
        send(
            {
                "jsonrpc": "2.0",
                "id": message["id"],
                "result": {"tools": TOOLS},
            }
        )
        continue

    if method == "tools/call":
        if MODE == "tool-timeout":
            time.sleep(2.5)
            continue

        if MODE == "crash-on-call":
            sys.exit(1)

        name = message.get("params", {}).get("name", "")
        arguments = message.get("params", {}).get("arguments", {})

        if name == "echo":
            send(
                {
                    "jsonrpc": "2.0",
                    "id": message["id"],
                    "result": {
                        "content": [
                            {
                                "type": "text",
                                "text": arguments.get("message", ""),
                            }
                        ]
                    },
                }
            )
            continue

        if name == "structured":
            send(
                {
                    "jsonrpc": "2.0",
                    "id": message["id"],
                    "result": {
                        "structuredContent": {"ok": True, "mode": MODE},
                    },
                }
            )
            continue

        send(
            {
                "jsonrpc": "2.0",
                "id": message["id"],
                "result": {
                    "isError": True,
                    "content": [
                        {
                            "type": "text",
                            "text": f"unknown tool: {name}",
                        }
                    ],
                },
            }
        )
