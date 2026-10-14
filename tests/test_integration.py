"""Exercise real HTTP requests and CLI/config precedence without CEC hardware."""
import http.server
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
INFO = {"response_code": 0, "model_name": "R-N602", "device_id": "ABC123"}


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        path = self.path.rsplit("/", 1)[-1]
        code = 200
        body = json.dumps(INFO).encode()
        if path == "invalid-json":
            body = b"<html>not a MusicCast device</html>"
        elif path == "api-error":
            body = b'{"response_code": 4}'
        elif path == "missing-code":
            body = b'{"device_id": "ABC123", "model_name": "R-N602"}'
        elif path == "http-error":
            code = 503
        elif path == "oversized":
            body = b"x" * (1024 * 1024 + 1)
        elif path == "timeout":
            time.sleep(3.5)
        elif path == "getStatus":
            body = json.dumps(self.server.zone_status).encode()
        elif path == "getDeviceInfo" and self.server.device_ids:
            body = json.dumps(dict(INFO, device_id=self.server.device_ids.pop(0))).encode()
        elif path == "setVolume?volume=up":
            self.server.volume_requests += 1
        self.send_response(code)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass


class IntegrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        cls.address = "127.0.0.1:" + str(cls.server.server_port)
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join()

    def cli(self, *args):
        return subprocess.run([str(ROOT / "cec"), *args], capture_output=True,
                              text=True, timeout=8)

    def setUp(self):
        self.server.zone_status = {"response_code": 4}
        self.server.device_ids = []
        self.server.volume_requests = 0

    def bridge(self, mode):
        return subprocess.run([str(ROOT / "tests/bridge_test"), self.address, mode],
                              capture_output=True, text=True, timeout=8)

    def test_recovery_waits_for_original_receiver(self):
        self.server.device_ids = ["OTHER", "ABC123"]
        result = self.bridge("reconnect")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.server.volume_requests, 1)

    def test_invalid_zone_status_is_safe(self):
        for status in ({"response_code": 4}, {"response_code": 0},
                       {"response_code": 0, "power": "on", "sleep": 0,
                        "volume": 0, "max_volume": 0, "mute": False, "input": "optical1"}):
            with self.subTest(status=status):
                self.server.zone_status = status
                self.assertEqual(self.bridge("status").returncode, 1)

    def test_valid_zone_status(self):
        self.server.zone_status = {"response_code": 0, "power": "on", "sleep": 0,
                                  "volume": 90, "max_volume": 161, "mute": False,
                                  "input": "optical1"}
        self.assertEqual(self.bridge("status").returncode, 0)

    def request(self, path):
        # LAN APIs must work even when the service inherits a proxy environment.
        env = dict(os.environ, http_proxy="http://127.0.0.1:1", no_proxy="")
        return subprocess.run([str(ROOT / "tests/network_test"), self.address, path],
                              capture_output=True, text=True, timeout=6, env=env)

    def test_probe_verifies_yamaha_api(self):
        result = self.request("probe")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "ABC123")

    def test_invalid_http_responses(self):
        for path in ("invalid-json", "api-error", "missing-code", "http-error", "oversized"):
            with self.subTest(path=path):
                self.assertNotEqual(self.request(path).returncode, 0)

    def test_timeout_is_bounded(self):
        start = time.monotonic()
        self.assertNotEqual(self.request("timeout").returncode, 0)
        self.assertLess(time.monotonic() - start, 4.5)

    def test_help_requires_no_network_or_config(self):
        result = self.cli("--config", "/missing/config", "--help")
        self.assertEqual(result.returncode, 0)
        self.assertIn("--discover", result.stdout)

    def test_invalid_options_fail_before_connecting(self):
        for args in (("--volume", "-1"), ("--volume", "2147483648"),
                     ("--discovery-timeout", "0"), ("--discovery-timeout", "31"),
                     ("--input", "optical1&power=on"), ("--config", "/missing/config")):
            with self.subTest(args=args):
                result = self.cli(*args)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("Error:", result.stderr)

    def test_fixed_address_checks_device_identity(self):
        result = self.cli("--address", self.address, "--device-id", "OTHER")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("OTHER was not found", result.stderr)
        self.assertNotIn("Opening", result.stdout)

    def test_cli_overrides_config(self):
        with tempfile.NamedTemporaryFile(mode="w") as config:
            config.write("address = 127.0.0.1:1\nvolume = -1\ndevice-id = OTHER\n")
            config.flush()
            result = self.cli("--config", config.name, "--address", self.address,
                              "--volume", "70")
        self.assertIn("OTHER was not found", result.stderr)

    def test_config_values_are_used(self):
        with tempfile.NamedTemporaryFile(mode="w") as config:
            config.write(f"address = {self.address}\ndevice-id = OTHER\n")
            config.flush()
            result = self.cli("--config", config.name)
        self.assertIn("OTHER was not found", result.stderr)

    def test_unknown_config_option_fails(self):
        with tempfile.NamedTemporaryFile(mode="w") as config:
            config.write("typo-address = auto\n")
            config.flush()
            result = self.cli("--config", config.name)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("typo-address", result.stderr)


if __name__ == "__main__":
    unittest.main()
