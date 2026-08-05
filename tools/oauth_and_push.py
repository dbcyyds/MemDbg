#!/usr/bin/env python3
from __future__ import annotations
import json, os, subprocess, sys, time, urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OAUTH = ROOT / "out" / "device_oauth.json"
TOKEN_PATH = ROOT / "out" / ".gh_token"
CLIENT_ID = "Iv1.e7b89e013f801f03"  # official gh CLI OAuth app

def main() -> int:
    d = json.loads(OAUTH.read_text())
    device_code = d["device_code"]
    interval = int(d.get("interval", 5))
    expires = int(d.get("expires_in", 900))
    print("WAIT", d.get("user_code"), flush=True)
    start = time.time()
    token = None
    while time.time() - start < expires - 15:
        time.sleep(interval)
        body = (
            f"client_id={CLIENT_ID}&device_code={device_code}"
            f"&grant_type=urn:ietf:params:oauth:grant-type:device_code"
        ).encode()
        req = urllib.request.Request(
            "https://github.com/login/oauth/access_token",
            data=body, method="POST",
            headers={"Accept": "application/json",
                     "Content-Type": "application/x-www-form-urlencoded"},
        )
        try:
            with urllib.request.urlopen(req) as r:
                resp = json.loads(r.read().decode())
        except Exception as e:
            print("err", e, flush=True)
            continue
        if "access_token" in resp:
            token = resp["access_token"]
            print("GOT_TOKEN", flush=True)
            break
        err = resp.get("error")
        print(err, flush=True)
        if err == "slow_down":
            interval += 5
        if err in ("expired_token", "access_denied", "unsupported_grant_type"):
            break
    if not token:
        print("NO_TOKEN", flush=True)
        return 1
    TOKEN_PATH.write_text(token)
    TOKEN_PATH.chmod(0o600)
    env = {**os.environ, "GH_TOKEN": token, "GITHUB_TOKEN": token}
    print("RUNNING github_push_missing.py", flush=True)
    r = subprocess.run(
        [sys.executable, str(ROOT / "tools" / "github_push_missing.py")],
        env=env, text=True)
    print("missing_exit", r.returncode, flush=True)
    # git push full local history as backup
    url = f"https://x-access-token:{token}@github.com/dbcyyds/MemDbg.git"
    r2 = subprocess.run(
        ["git", "-C", str(ROOT), "push", "--force", url, "main:main"],
        env=env, text=True, capture_output=True)
    print(r2.stdout, flush=True)
    print(r2.stderr, flush=True)
    print("git_exit", r2.returncode, flush=True)
    print("ALL_DONE", flush=True)
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
