"""Exercises WinHTTP and real native window controls against an isolated local Memos fixture."""
import json
import base64
import pathlib
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit, parse_qs


class Fixture(ThreadingHTTPServer):
    def __init__(self, prefix, desktop=False, connect=False):
        super().__init__(("127.0.0.1", 0), Handler)
        self.prefix = prefix
        self.desktop = desktop
        self.connect = connect
        self.refreshes = 0
        self.recovery_failures = 0
        self.profile_failures = 0
        self.suppress_cookie = False
        self.creates = []
        self.cookie = ""
        self.token = ""
        self.errors = []
        self.uploads = {}
        self.files = []
        self.upload_rejected = False
        self.upload_failures = 0
        self.base = f"http://127.0.0.1:{self.server_port}/notes/"


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def send_json(self, status, body, cookie=None):
        payload = json.dumps(body, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        if cookie and not self.server.suppress_cookie:
            self.send_header("Set-Cookie", cookie)
        self.end_headers()
        try:
            self.wfile.write(payload)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass

    def do_GET(self):
        parsed = urlsplit(self.path)
        path = parsed.path.removeprefix("/notes/")
        query = parse_qs(parsed.query)
        if path.startswith("reverse"):
            assert not self.headers.get("Authorization") and not self.headers.get("Cookie"), "Geocoding received Memos credentials"
            assert "MemoQuick/" in self.headers.get("User-Agent", "")
            assert query["format"] == ["json"] and query["accept-language"] == ["zh-CN"]
            if path == "reverse":
                self.send_json(200, {"display_name": "中国，广东省，深圳市，南山区"})
            elif path == "reverse-empty":
                self.send_json(200, {})
            elif path == "reverse-large":
                self.send_json(200, {"display_name": "x" * 70000})
            elif path == "reverse-redirect":
                self.send_response(302)
                self.send_header("Location", "/notes/reverse")
                self.send_header("Content-Length", "0")
                self.end_headers()
            else:
                self.send_json(503, {})
        elif path == "cookie-headers":
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", "2")
            self.send_header("Set-Cookie", "memos_refresh=header-probe; Path=/; HttpOnly; Expires=Wed, 01 Jan 2098 00:00:00 GMT")
            self.send_header("Set-Cookie", "auxiliary=second; Path=/; Max-Age=86400")
            self.end_headers()
            self.wfile.write(b"{}")
        elif path == self.server.prefix + "/instance/profile":
            if self.server.profile_failures:
                self.server.profile_failures -= 1
                self.send_json(503, {})
                return
            self.send_json(200, {"version": "0.26.1" if self.server.prefix == "api/v1" else "0.32.0"})
        elif path == self.server.prefix + "/auth/me":
            if self.headers.get("Authorization") != "Bearer " + self.server.token:
                self.send_json(401, {})
            else:
                self.send_json(200, {"user": {"name": "users/7", "displayName": "Alice"}})
        elif path == self.server.prefix + "/memos":
            filter_text = query.get("filter", [""])[0]
            if self.server.prefix == "api/v1" and filter_text.startswith("creator =="):
                self.send_json(400, {})
                return
            expected = 'creator_id == 7' if self.server.prefix == "api/v1" else 'creator == "users/7"'
            assert filter_text.startswith(expected), "Search was not scoped to the current user"
            assert self.headers.get("Authorization") == "Bearer " + self.server.token
            if not self.server.desktop:
                assert filter_text == expected + ' && content.contains("灵感\\\" &")', "Search escaping changed"
            if query.get("pageToken", [""])[0]:
                assert query["pageToken"] == ["next +/="], "Pagination token encoding failed"
                self.send_json(200, {"memos": [{"name":"memos/reference-2","creator":"users/7","snippet":"参考二"}]})
            else:
                self.send_json(200, {"memos":[{"name":"memos/reference-1","creator":"users/7","snippet":"参考一"},
                    {"name":"memos/other","creator":"users/99","snippet":"其他账号"},
                    {"name":"memos/archived","creator":"users/7","snippet":"已归档","state":"ARCHIVED"}],"nextPageToken":"next +/="})
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
        if path.startswith("memos.api.v1."):
            if not self.server.connect:
                self.send_json(404, {})
                return
            assert self.headers.get("Connect-Protocol-Version") == "1"
            if path == "memos.api.v1.InstanceService/GetInstanceProfile":
                self.send_json(200, {"version": "0.31.0"})
                return
            procedures = {"SignIn":"signin", "RefreshToken":"refresh", "GetCurrentUser":"me", "SignOut":"signout"}
            path = self.server.prefix + "/auth/" + procedures[path.rsplit("/", 1)[1]]
        elif self.server.connect and "/auth/" in path:
            raise AssertionError("Authentication used the REST gateway instead of Connect")
        if path == self.server.prefix + "/auth/me":
            assert self.headers.get("Authorization") == "Bearer " + self.server.token
            self.send_json(200, {"user": {"name":"users/7", "displayName":"Alice"}})
            return
        if path == self.server.prefix + "/auth/signin":
            assert data == {"passwordCredentials": {"username": "alice", "password": "test-password"}}, "Credentials have wrong shape"
            self.server.token = "token-0"
            self.server.cookie = "refresh-0"
            self.send_json(200, {"accessToken": self.server.token}, "memos_refresh=refresh-0; Path=/notes/; HttpOnly; Expires=Wed, 01 Jan 2098 00:00:00 GMT")
        elif path == self.server.prefix + "/auth/refresh":
            assert self.headers.get("Cookie") == "memos_refresh=" + self.server.cookie, "Refresh cookie missing or not rotated"
            if self.server.recovery_failures:
                self.server.recovery_failures -= 1
                self.send_json(503, {})
                return
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
                if "attachments" in data:
                    assert all(set(a) == {"name"} for a in data["attachments"]), "Attachment references have wrong shape"
                    assert len(data["attachments"]) == 2
                    if data["content"].startswith("剪贴板粘贴"):
                        assert data["attachments"] == [{"name":"attachments/fixture-3"},{"name":"attachments/fixture-4"}]
                        assert "/notes/file/attachments/fixture-3/image" in data["content"]
                        assert "location" not in data and not data.get("relations")
                    else:
                        assert data["relations"] == [{"relatedMemo":{"name":"memos/reference-1"},"type":"REFERENCE"}]
                        assert data["location"] == ({"placeholder":"赤道","latitude":0,"longitude":0} if not self.server.desktop else {"placeholder":"书桌","latitude":22.3,"longitude":114.2})
                        assert "/notes/file/attachments/fixture-1/image" in data["content"], "Inline image used the wrong URL or lost the subpath"
                self.send_json(200, {"name": "memos/fixture"})
        elif path == self.server.prefix + "/attachments:upload":
            if self.server.prefix == "api/v1":
                self.send_json(404, {})
                return
            if not self.server.upload_rejected:
                self.server.upload_rejected = True
                self.send_json(401, {})
                return
            assert self.headers.get("Authorization") == "Bearer " + self.server.token
            if "spec" in data:
                spec = data["spec"]
                if spec["attachment"]["filename"] == "fail.bin":
                    self.server.upload_failures += 1
                    self.send_json(500,{})
                    return
                upload_id = str(len(self.server.uploads) + 1)
                self.server.uploads[upload_id] = {"metadata":spec["attachment"],"size":int(spec["totalSize"]),"bytes":bytearray()}
                self.send_json(200,{"uploadId":upload_id,"committedSize":"0","maxChunkSize":2*1024*1024})
            else:
                upload = self.server.uploads[data["uploadId"]]
                assert int(data["writeOffset"]) == len(upload["bytes"]), "Chunk offset was not preserved"
                chunk = base64.b64decode(data["data"],validate=True)
                assert len(chunk) <= 2*1024*1024
                upload["bytes"].extend(chunk)
                response = {"committedSize":str(len(upload["bytes"]))}
                if data["finishWrite"]:
                    assert len(upload["bytes"]) == upload["size"]
                    response["attachment"] = self.accept_file(upload["metadata"],bytes(upload["bytes"]))
                self.send_json(200,response)
        elif path == self.server.prefix + "/attachments":
            if not self.server.upload_rejected:
                self.server.upload_rejected = True
                self.send_json(401,{})
                return
            assert self.headers.get("Authorization") == "Bearer " + self.server.token
            if self.server.desktop and data["filename"] == "voice.wav" and not self.server.upload_failures:
                self.server.upload_failures += 1
                self.send_json(500,{})
                return
            if data["filename"] == "fail.bin":
                self.server.upload_failures += 1
                self.send_json(500,{})
                return
            self.send_json(200,self.accept_file(data,base64.b64decode(data["content"],validate=True)))
        elif path == self.server.prefix + "/auth/signout":
            self.send_json(200, {}, "memos_refresh=; Path=/notes/; Max-Age=0")
        else:
            self.send_json(404, {})

    def accept_file(self, metadata, data):
        if metadata["filename"] == "图[片].png":
            assert data == b"x"*(2*1024*1024+17), "Uploaded binary changed"
        if metadata["type"] == "audio/wav":
            assert data.startswith(b"RIFF") and data[8:12] == b"WAVE", "Recording is not a WAV attachment"
        if metadata["filename"] == "剪贴图.png":
            assert data.startswith(b"\x89PNG\r\n\x1a\n") and b"IEND" in data, "Clipboard image was not encoded as PNG"
        if metadata["filename"] == "说明.pdf":
            assert data == b"%PDF-1.7\nclipboard fixture", "Clipboard virtual file bytes changed"
        self.server.files.append((metadata["filename"],data))
        return {"name":f"attachments/fixture-{len(self.server.files)}","filename":metadata["filename"],"type":metadata["type"],"externalLink":"https://storage.invalid/private"}


def run_fixture(prefix, desktop, executable, connect=False):
    server = Fixture(prefix, desktop, connect)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        if not desktop:
            subprocess.run([executable, "--api", server.base], check=True, timeout=45)
            assert server.refreshes == 3, f"Unexpected refresh count: {server.refreshes}"
            assert len(server.creates) == 5, "Failed or timed-out writes were retried"
            assert len(server.files) == 2 and server.upload_failures == 1, "Upload failure was retried or attachments were duplicated"
            assert server.creates[0] == {"content": "你好 📝\r\n#测试\r\n- [ ] 待办", "visibility": "PRIVATE"}
        else:
            output = pathlib.Path(__file__).resolve().parent.parent / "test-output"
            output.mkdir(exist_ok=True)
            failure = output / "failure.txt"
            failure.unlink(missing_ok=True)
            with tempfile.TemporaryDirectory(prefix="memoquick-ui-") as data:
                result = subprocess.run([executable, "--smoke", server.base, "--data-dir", data, "--output", str(output)], timeout=45)
                assert result.returncode == 0, (failure.read_text("utf-8") if failure.exists() else "Native UI smoke test failed", server.errors)
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
            assert len(server.creates) == 5, "Desktop submission count incorrect"
            assert server.refreshes == 2
            assert len(server.files) == 4, "Desktop attachments were lost or duplicated"
            assert server.upload_failures == 1, "Partial upload failure was not exercised"
            print("Native desktop: login, encrypted draft, failure retention, refresh, close-on-success and show handler passed")
            print("Window position: drag persistence, hide/show, restart and offscreen recovery passed")
            print((output / "metrics.json").read_text())
        assert not server.errors, server.errors
    finally:
        server.shutdown()
        server.server_close()


def run_session_recovery(executable, application):
    # GIVEN DPAPI state from an exited process, WHEN background startup encounters
    # temporary service failures, THEN it retries and restores without opening UI.
    server = Fixture("api/v1", connect=True)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        # GIVEN a successful token response without a refresh cookie, WHEN signing
        # in, THEN reject the non-restorable session rather than reporting success.
        server.suppress_cookie = True
        with tempfile.TemporaryDirectory(prefix="memoquick-no-cookie-") as data:
            result = subprocess.run([executable, "--session-seed", server.base, data], capture_output=True, timeout=30)
            assert result.returncode == 1, "Sign-in silently accepted a missing refresh cookie"
            assert not (pathlib.Path(data) / "private.bin").exists(), "Missing refresh cookie created a session"
        server.suppress_cookie = False
        with tempfile.TemporaryDirectory(prefix="memoquick-session-") as data:
            subprocess.run([executable, "--session-seed", server.base, data], check=True, timeout=30)
            encrypted = (pathlib.Path(data) / "private.bin").read_bytes()
            assert b"refresh-0" not in encrypted and b"test-password" not in encrypted
            server.profile_failures = 1
            server.recovery_failures = 1
            report = pathlib.Path(data) / "session-restored.txt"
            for _ in range(2):
                report.unlink(missing_ok=True)
                subprocess.run([application, "--check-session", "--data-dir", data, "--output", data], check=True, timeout=30)
                assert report.exists(), "Background restart did not restore session and draft"
            assert server.refreshes == 2, "Refresh rotation did not survive the second process restart"
            assert not server.creates, "Session recovery submitted a memo"
            assert not server.errors, server.errors
        print("Background restart: Connect cookies, DPAPI, draft recovery, 503 retries and cookie rotation passed")
    finally:
        server.shutdown()
        server.server_close()


if __name__ == "__main__":
    run_fixture("api/v1", False, sys.argv[1])
    run_fixture("api", False, sys.argv[1])
    run_fixture("api/v1", False, sys.argv[1], connect=True)
    run_fixture("api/v1", True, sys.argv[2])
    run_session_recovery(sys.argv[1], sys.argv[2])
