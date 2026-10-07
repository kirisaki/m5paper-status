"""Live API test; removes only its own records and restores the original display.

Run explicitly: python3 tests/device_history_smoke.py --reset-port /dev/ttyUSB0
Requires pyserial only when --reset-port is provided.
"""
import argparse
import json
from pathlib import Path
import time
import urllib.error
import urllib.request


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", default="http://paper.local")
    parser.add_argument("--config", default="config.local.json")
    parser.add_argument("--reset-port")
    args = parser.parse_args()
    config = json.loads(Path(args.config).read_text())
    token = config.get("api_tokens", {}).get("device", "")
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

    def request(path, method="GET", body=None, expected=200, text=False):
        headers = {"Content-Type": "text/plain; charset=utf-8"}
        if token:
            headers["Authorization"] = "Bearer " + token
        data = None if body is None else body.encode("utf-8")
        req = urllib.request.Request(args.url + path, data=data, headers=headers, method=method)
        try:
            response = opener.open(req, timeout=20)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            raw = response.read().decode("utf-8")
            assert response.status == expected, (method, path, response.status, raw)
            return raw if text else json.loads(raw)

    def wait_ready():
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            try:
                health = request("/api/health")
                assert health["storage_ready"], "History storage unavailable"
                if health["clock_ready"]:
                    return
            except (urllib.error.URLError, TimeoutError, ConnectionResetError):
                pass
            time.sleep(1)
        raise RuntimeError("Device did not become ready")

    def reset():
        import serial
        with serial.Serial(args.reset_port, 115200, timeout=1) as port:
            port.dtr = False
            port.rts = True
            time.sleep(0.1)
            port.rts = False
            time.sleep(1)
        wait_ready()

    wait_ready()
    original = request("/api/message", text=True)
    initial = request("/api/messages")
    assert initial["total"] <= initial["capacity"] - 3, "Not enough unused history slots for a non-evicting test"
    if initial["messages"]:
        assert initial["messages"][0]["text"] == original
    created = []
    try:
        for query in ("limit=0", "limit=51", "before=-1", "before=4294967296", "limit=x"):
            request("/api/messages?" + query, expected=400)
        request("/api/messages", method="DELETE", expected=405)
        request("/api/messages/not-an-id", expected=400)
        request("/api/messages/4294967295", expected=404)
        texts = ["履歴テスト A\n引用\"とタブ\t", "履歴テスト B", "あ" * 1365 + "!"]
        for body in texts:
            posted = request("/api/message", method="POST", body=body, expected=202)
            created.append(posted["id"])
            item = request("/api/messages/" + str(posted["id"]))
            assert item["text"] == body and item["bytes"] == len(body.encode())
            assert isinstance(item["received_at"], int) and item["received_at"] > 1700000000
        duplicate = request("/api/message", method="POST", body=texts[-1])
        assert duplicate["status"] == "unchanged" and duplicate["id"] == created[-1]
        page = request("/api/messages?limit=2")
        assert [entry["id"] for entry in page["messages"]] == list(reversed(created[1:]))
        assert page["next_before"] == created[1] and page["total"] == initial["total"] + 3
        following = request("/api/messages?limit=2&before=" + str(page["next_before"]))
        assert following["messages"][0]["id"] == created[0]
        # A rejected DELETE carrying a body must never mutate history.
        request("/api/messages/" + str(created[-1]), method="DELETE", body="unexpected", expected=400)
        assert request("/api/message", text=True) == texts[-1]
        request("/api/messages/" + str(created[1]), method="DELETE")
        assert request("/api/message", text=True) == texts[-1]
        request("/api/messages/" + str(created[1]), expected=404)
        request("/api/messages/" + str(created[1]), method="DELETE", expected=404)
        request("/api/messages/" + str(created[2]), method="DELETE")
        assert request("/api/message", text=True) == texts[0]
        if args.reset_port:
            reset()
            assert request("/api/message", text=True) == texts[0]
            assert request("/api/messages/" + str(created[0]))["text"] == texts[0]
            request("/api/messages/" + str(created[1]), expected=404)
        print("PASS: UTF-8/full-size posts, timestamps, deduplication, pagination, GET and DELETE" +
              (", reboot persistence" if args.reset_port else ""), flush=True)
    finally:
        for ident in reversed(created):
            request("/api/messages/" + str(ident), method="DELETE",
                    expected=200 if any(x["id"] == ident for x in request("/api/messages?limit=50")["messages"]) else 404)
        assert request("/api/message", text=True) == original
        assert request("/api/messages")["total"] == initial["total"]
        print("Restored original display and history count", flush=True)


if __name__ == "__main__":
    main()
