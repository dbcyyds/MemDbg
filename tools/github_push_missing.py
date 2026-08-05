#!/usr/bin/env python3
"""Upload missing text files to GitHub via Contents API. Requires GH_TOKEN."""
from __future__ import annotations
import base64, json, os, subprocess, sys, time, urllib.error, urllib.request
from pathlib import Path

OWNER, REPO, BRANCH = "dbcyyds", "MemDbg", "main"
ROOT = Path(__file__).resolve().parents[1]
TOKEN = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN")
if not TOKEN:
    sys.exit("Set GH_TOKEN")

def api(method, url, data=None):
    body = None if data is None else json.dumps(data).encode()
    req = urllib.request.Request(url, data=body, method=method, headers={
        "Authorization": f"Bearer {TOKEN}",
        "Accept": "application/vnd.github+json",
        "X-GitHub-Api-Version": "2022-11-28",
        "User-Agent": "MemDbg-push-missing",
        "Content-Type": "application/json",
    })
    for attempt in range(5):
        try:
            with urllib.request.urlopen(req) as r:
                raw = r.read()
                return json.loads(raw.decode()) if raw else {}
        except urllib.error.HTTPError as e:
            err = e.read().decode()
            if e.code in (409, 422) and attempt < 4:
                time.sleep(1 + attempt)
                # refresh and retry for conflicts
                continue
            if e.code == 403 and "rate" in err.lower():
                time.sleep(30)
                continue
            raise SystemExit(f"HTTP {e.code} {url}: {err[:400]}") from e
        except Exception as e:
            if attempt < 4:
                time.sleep(2)
                continue
            raise

base = f"https://api.github.com/repos/{OWNER}/{REPO}"

def remote_files():
    tree = api("GET", f"{base}/git/trees/main?recursive=1")
    return {t["path"] for t in tree.get("tree", []) if t.get("type") == "blob"}

def put_file(path: str, content: bytes, message: str):
    # get sha if exists
    sha = None
    try:
        meta = api("GET", f"{base}/contents/{path}?ref={BRANCH}")
        if isinstance(meta, dict):
            sha = meta.get("sha")
    except SystemExit as e:
        if "404" not in str(e):
            pass
    # Use base64 always for reliability
    payload = {
        "message": message,
        "content": base64.b64encode(content).decode("ascii"),
        "branch": BRANCH,
    }
    if sha:
        payload["sha"] = sha
    return api("PUT", f"{base}/contents/{path}", payload)

remote = remote_files()
print("remote", len(remote), flush=True)

files = subprocess.check_output(["git", "-C", str(ROOT), "ls-files"], text=True).splitlines()
b64font = ROOT / "data/memdbg_cjk_subset.ttf.b64"
if b64font.is_file() and "data/memdbg_cjk_subset.ttf.b64" not in files:
    files.append("data/memdbg_cjk_subset.ttf.b64")

ok = fail = skip = 0
for rel in files:
    p = ROOT / rel
    if not p.is_file():
        continue
    if p.suffix.lower() in {".ttf", ".png", ".gif"}:
        print("skip bin", rel, flush=True)
        skip += 1
        continue
    if rel in remote:
        # still update to ensure full content (optional skip)
        skip += 1
        continue
    raw = p.read_bytes()
    try:
        put_file(rel, raw, f"feat: add {rel}")
        print("OK", rel, len(raw), flush=True)
        ok += 1
        remote.add(rel)
        time.sleep(0.35)  # rate limit soft
    except Exception as e:
        print("FAIL", rel, e, flush=True)
        fail += 1
        time.sleep(1)

print(f"DONE ok={ok} fail={fail} skip={skip}", flush=True)
# final count
print("remote_final", len(remote_files()), flush=True)
