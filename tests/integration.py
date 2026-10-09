"""Exercises WinHTTP and real native window controls against an isolated local Memos fixture."""
import json
import pathlib
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class Fixture(ThreadingHTTPServer):
    def __init__(self, prefix, desktop=False):
        super().__init__(("127.0.0.1", 0), Handler)
        self.prefix = prefix
        self.desktop = desktop
        self.refreshes = 0
        self.creates = []
        self.cookie = ""
        self.token = ""
        self.errors = []
        self.base = f"http://127.0.0.1:{self.server_port}/notes/"


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def send_json(self, status, body, cookie=None):
        payload = json.dumps(body, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        if cookie:
            self.send_header("Set-Cookie", cookie)
        self.end_headers()
        try:
            self.wfile.write(payload)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass

    def do_GET(self):
        path = self.path.removeprefix("/notes/")
        if path == self.server.prefix + "/instance/profile":
            self.send_json(200, {"version": "0.26.1" if self.server.prefix == "api/v1" else "0.32.0"})
        elif path == self.server.prefix + "/auth/me":
            if self.headers.get("Authorization") != "Bearer " + self.server.token:
                self.send_json(401, {})
            else:
                self.send_json(200, {"user": {"name": "users/alice", "displayName": "Alice"}})
        else:
            self.send_json(404, {})

    def do_POST(self):
        try:
            self.post()
        except Exception as error:
            self.server.errors.append(str(error))
            self.send_json(500, {})

    def post(self):
        assert self.path.startswith("/notes/"), "Reverse-proxy base path was lost"
        path = self.path.removeprefix("/notes/")
        data = json.loads(self.rfile.read(int(self.headers.get("Content-Length", "0"))) or "{}")
        assert self.headers.get("Origin") == self.server.base.removesuffix("/notes/"), "Origin header missing"
        if path == self.server.prefix + "/auth/signin":
            assert data == {"passwordCredentials": {"username": "alice", "password": "test-password"}}, "Credentials have wrong shape"
            self.server.token = "token-0"
            self.server.cookie = "refresh-0"
            self.send_json(200, {"accessToken": self.server.token}, "memos_refresh=refresh-0; Path=/notes/; HttpOnly; Max-Age=86400")
        elif path == self.server.prefix + "/auth/refresh":
            assert self.headers.get("Cookie") == "memos_refresh=" + self.server.cookie, "Refresh cookie missing or not rotated"
            self.server.refreshes += 1
            self.server.token = f"token-{self.server.refreshes}"
            self.server.cookie = f"refresh-{self.server.refreshes}"
            self.send_json(200, {"accessToken": self.server.token}, f"memos_refresh={self.server.cookie}; Path=/notes/; HttpOnly; Max-Age=86400")
        elif path == self.server.prefix + "/memos":
            self.server.creates.append(data)
            assert data["visibility"] == "PRIVATE", "Visibility changed"
            if data["content"] == "FAIL":
                self.send_json(500, {})
            elif data["content"] == "TIMEOUT":
                time.sleep(0.8)
                self.send_json(200, {"name": "memos/fixture"})
            elif self.server.desktop and len(self.server.creates) == 1:
                self.send_json(500, {})
            elif self.server.token == "token-0":
                self.send_json(401, {})
            else:
                assert self.headers.get("Authorization") == "Bearer " + self.server.token, "Bearer token missing"
                self.send_json(200, {"name": "memos/fixture"})
        elif path == self.server.prefix + "/auth/signout":
            self.send_json(200, {}, "memos_refresh=; Path=/notes/; Max-Age=0")
        else:
            self.send_json(404, {})


def run_fixture(prefix, desktop, executable):
    server = Fixture(prefix, desktop)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        if not desktop:
            subprocess.run([executable, "--api", server.base], check=True, timeout=45)
            assert server.refreshes == 2, f"Unexpected refresh count: {server.refreshes}"
            assert len(server.creates) == 4, "Failed or timed-out writes were retried"
            assert server.creates[0] == {"content": "你好 📝\r\n#测试\r\n- [ ] 待办", "visibility": "PRIVATE"}
        else:
            output = pathlib.Path(__file__).resolve().parent.parent / "test-output"
            output.mkdir(exist_ok=True)
            failure = output / "failure.txt"
            failure.unlink(missing_ok=True)
            with tempfile.TemporaryDirectory(prefix="memoquick-ui-") as data:
                result = subprocess.run([executable, "--smoke", server.base, "--data-dir", data, "--output", str(output)], timeout=45)
                assert result.returncode == 0, failure.read_text("utf-8") if failure.exists() else "Native UI smoke test failed"
                position_file = pathlib.Path(data) / "window.json"
                expected = json.loads(position_file.read_text("utf-8"))
                subprocess.run([executable, "--check-position", "--data-dir", data, "--output", str(output)], check=True, timeout=15)
                restored = json.loads((output / "position.json").read_text())
                assert (restored["x"], restored["y"]) == (expected["x"], expected["y"]), "Restart lost window position"
                position_file.write_text(json.dumps({"x": -500000, "y": 500000}), "utf-8")
                subprocess.run([executable, "--check-position", "--data-dir", data, "--output", str(output)], check=True, timeout=15)
                restored = json.loads((output / "position.json").read_text())
                assert restored["workLeft"] <= restored["x"] < restored["right"] <= restored["workRight"], "Restored window is horizontally offscreen"
                assert restored["workTop"] <= restored["y"] < restored["bottom"] <= restored["workBottom"], "Restored window is vertically offscreen"
            assert len(server.creates) == 3, "Desktop submission count incorrect"
            assert server.refreshes == 1
            print("Native desktop: login, encrypted draft, failure retention, refresh, close-on-success and show handler passed")
            print("Window position: drag persistence, hide/show, restart and offscreen recovery passed")
            print((output / "metrics.json").read_text())
        assert not server.errors, server.errors
    finally:
        server.shutdown()
        server.server_close()


if __name__ == "__main__":
    run_fixture("api/v1", False, sys.argv[1])
    run_fixture("api", False, sys.argv[1])
    run_fixture("api/v1", True, sys.argv[2])
