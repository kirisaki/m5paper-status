#!/usr/bin/env python3
"""Create a dedicated device login without changing the desktop Codex session.

Protocol: openai/codex, codex-rs/login/src/device_code_auth.rs.
Only the one-time user code is printed; OAuth tokens are stored with mode 0600.
"""
import base64
import json
import os
from pathlib import Path
import time
import urllib.error
import urllib.parse
import urllib.request

CLIENT_ID = "app_EMoamEEZ73f0CkXaXp7hrann"
BASE = "https://auth.openai.com"
ROOT = Path(__file__).resolve().parents[1]


def post(path, data, form=False):
    raw = urllib.parse.urlencode(data).encode() if form else json.dumps(data).encode()
    request = urllib.request.Request(BASE + path, data=raw, headers={
        "Content-Type": "application/x-www-form-urlencoded" if form else "application/json",
        "User-Agent": "m5paper-status/0.1",
    })
    with urllib.request.urlopen(request, timeout=30) as response:
        return json.load(response)


def main():
    target = ROOT / "credentials" / "codex-paper.json"
    if target.exists():
        raise SystemExit("Dedicated credentials already exist; move that file aside to create a new login.")
    device = post("/api/accounts/deviceauth/usercode", {"client_id": CLIENT_ID})
    code = device.get("user_code", device.get("usercode"))
    if not code:
        raise SystemExit("Device login did not return a user code")
    print("Open: " + BASE + "/codex/device", flush=True)
    print("One-time code: " + code, flush=True)
    deadline = time.monotonic() + 900
    while time.monotonic() < deadline:
        try:
            grant = post("/api/accounts/deviceauth/token", {
                "device_auth_id": device["device_auth_id"], "user_code": code})
            break
        except urllib.error.HTTPError as error:
            if error.code not in (403, 404):
                raise SystemExit("Device login failed: HTTP " + str(error.code)) from None
            time.sleep(max(3, int(device.get("interval", 5))))
    else:
        raise SystemExit("Device login expired; run this script again")
    tokens = post("/oauth/token", {
        "grant_type": "authorization_code", "client_id": CLIENT_ID,
        "code": grant["authorization_code"], "code_verifier": grant["code_verifier"],
        "redirect_uri": BASE + "/deviceauth/callback",
    }, form=True)
    # Decode only to retrieve identity metadata from the HTTPS token response.
    payload = tokens["id_token"].split(".")[1]
    claims = json.loads(base64.urlsafe_b64decode(payload + "=" * (-len(payload) % 4)))
    account = claims.get("https://api.openai.com/auth", {}).get("chatgpt_account_id")
    if not account or not tokens.get("refresh_token"):
        raise SystemExit("Login did not return account identity and refresh credentials")
    output = {"access_token": tokens["access_token"], "refresh_token": tokens["refresh_token"],
              "account_id": account, "expires_at": int(time.time()) + int(tokens["expires_in"])}
    target.parent.mkdir(parents=True, exist_ok=True)
    fd = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "w") as file:
        json.dump(output, file)
    print("Dedicated credentials saved to credentials/codex-paper.json (tokens hidden)", flush=True)


if __name__ == "__main__":
    try:
        main()
    except urllib.error.HTTPError as error:
        raise SystemExit("Authentication HTTP error: " + str(error.code)) from None
