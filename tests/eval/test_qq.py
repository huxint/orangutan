#!/usr/bin/env python3
"""Exercise the live runner's admission/recovery before any external service.

Usage: python3 tests/eval/test_qq.py /absolute/path/to/eval-qq
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

BINARY = str(Path(sys.argv.pop(1)).resolve())


class Admission(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="oran-qq-eval-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.workspace = self.root / "workspace"
        self.workspace.mkdir()
        self.state = self.root / "state"
        self.state.mkdir(mode=0o700)
        self.config = self.root / "config.json"
        self.config.write_text(json.dumps({
            "strict_config": True,
            "runtime": {"prompt": {"active_tools": []}},
            "profiles": {"fixture": {
                "protocol": "anthropic_messages", "provider": "anthropic",
                "model": "fixture", "base_url": "http://127.0.0.1:1",
                "api_key_env": "ORAN_QQ_TEST_KEY"}},
            "routes": {"default": {"primary": "fixture", "fallbacks": []}},
        }))
        self.write("qq-credentials.json", {
            "app_id": "1234", "app_secret": "fixture", "user_openid": "owner"})
        self.event = {"op": 0, "t": "C2C_MESSAGE_CREATE", "d": {
            "id": "event-1", "content": "hello", "author": {"user_openid": "owner"}}}
        self.write("event.json", self.event)
        self.journal = {
            "app": "1234", "owner": "owner", "workspace": str(self.workspace),
            "session": "0123456789abcdef0123456789abcdef", "pending": None,
            "delivered": [],
        }

    def write(self, name, value):
        path = self.state / name
        path.write_text(json.dumps(value))
        path.chmod(0o600)

    def run_event(self):
        return subprocess.run(
            [BINARY, str(self.config), str(self.workspace), str(self.state), "--no-typing"],
            env={**os.environ, "ORAN_QQ_TEST_KEY": "fixture"},
            capture_output=True, text=True, timeout=20)

    def test_foreign_sender_is_rejected_without_journal(self):
        self.event["d"]["author"]["user_openid"] = "stranger"
        self.write("event.json", self.event)
        self.assertEqual(self.run_event().returncode, 1)
        self.assertFalse((self.state / "live-journal.json").exists())

    def test_missing_owner_is_rejected(self):
        self.write("qq-credentials.json", {
            "app_id": "1234", "app_secret": "fixture", "user_openid": ""})
        self.assertEqual(self.run_event().returncode, 1)
        self.assertFalse((self.state / "live-journal.json").exists())

    def test_pending_delivery_is_preserved(self):
        self.journal["pending"] = {"id": "earlier", "answer": "preserve me", "send_inflight": True}
        self.write("live-journal.json", self.journal)
        before = (self.state / "live-journal.json").read_bytes()
        self.assertEqual(self.run_event().returncode, 1)
        self.assertEqual((self.state / "live-journal.json").read_bytes(), before)

    def test_duplicate_never_calls_model_or_sends(self):
        self.journal["delivered"] = [{"id": "event-1"}]
        self.write("live-journal.json", self.journal)
        result = self.run_event()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("duplicate already delivered", result.stdout)

    def test_changed_binding_is_rejected(self):
        self.journal["owner"] = "different-owner"
        self.write("live-journal.json", self.journal)
        self.assertEqual(self.run_event().returncode, 1)

    def test_oversized_journal_is_preserved(self):
        self.journal["delivered"] = [{"id": "earlier", "text": "x" * (4 * 1024 * 1024)}]
        self.write("live-journal.json", self.journal)
        before = (self.state / "live-journal.json").read_bytes()
        self.assertEqual(self.run_event().returncode, 1)
        self.assertEqual((self.state / "live-journal.json").read_bytes(), before)

    def test_denied_provider_preserves_intake_and_blocks_replay(self):
        # The strict default provider permission fails before any model HTTP.
        self.assertEqual(self.run_event().returncode, 1)
        before = (self.state / "live-journal.json").read_bytes()
        journal = json.loads(before)
        self.assertEqual(journal["pending"]["text"], "hello")
        self.assertIsNone(journal["pending"]["answer"])
        self.assertEqual(self.run_event().returncode, 1)
        self.assertEqual((self.state / "live-journal.json").read_bytes(), before)


if __name__ == "__main__":
    unittest.main()
