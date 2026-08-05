#!/usr/bin/env python3
"""Push all local git-tracked text files to GitHub via Git Data API. Requires GH_TOKEN."""
from __future__ import annotations
import base64, json, os, subprocess, sys, urllib.error, urllib.request
from pathlib import Path

OWNER, REPO, BRANCH = "dbcyyds", "MemDbg", "main"
ROOT = Path(__file__).resolve().parents[1]
TOKEN = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN")
if not TOKEN:
    sys.exit("Set GH_TOKEN (GitHub PAT with repo scope)")

def api(method: str, url: str, data=None):
    body = None if data is None else json.dumps(data).encode()
    req = urllib.request.Request(
        url, data=body, method=method,
        headers={
            "Authorization": f"Bearer {TOKEN}",
            "Accept": "application/vnd.github+json",
            "X-GitHub-Api-Version": "2022-11-28",
            "User-Agent": "MemDbg-push-all",
            "Content-Type": "application/json",
        },
    )
    try:
        with urllib.request.urlopen(req) as r:
            return json.loads(r.read().decode())
    except urllib.error.HTTPError as e:
        err = e.read().decode()
        raise SystemExit(f"HTTP {e.code} {url}: {err[:500]}") from e

base = f"https://api.github.com/repos/{OWNER}/{REPO}"
ref = api("GET", f"{base}/git/ref/heads/{BRANCH}")
head_sha = ref["object"]["sha"]
commit = api("GET", f"{base}/git/commits/{head_sha}")
base_tree = commit["tree"]["sha"]
print("base", head_sha[:8], "tree", base_tree[:8])

files = subprocess.check_output(["git", "-C", str(ROOT), "ls-files"], text=True).splitlines()
# include b64 font
extra = ROOT / "data/memdbg_cjk_subset.ttf.b64"
if extra.is_file() and "data/memdbg_cjk_subset.ttf.b64" not in files:
    files.append("data/memdbg_cjk_subset.ttf.b64")

tree_items = []
for rel in files:
    p = ROOT / rel
    if not p.is_file():
        continue
    if p.suffix.lower() in {".ttf", ".png", ".gif"}:
        print("skip binary", rel)
        continue
    raw = p.read_bytes()
    # prefer utf-8 text; for any binary remaining use base64
    try:
        text = raw.decode("utf-8")
        blob = api("POST", f"{base}/git/blobs", {"content": text, "encoding": "utf-8"})
    except UnicodeDecodeError:
        blob = api("POST", f"{base}/git/blobs", {
            "content": base64.b64encode(raw).decode("ascii"),
            "encoding": "base64",
        })
    mode = "100755" if p.stat().st_mode & 0o111 else "100644"
    tree_items.append({"path": rel, "mode": mode, "type": "blob", "sha": blob["sha"]})
    print("blob", rel, blob["sha"][:8])

# GitHub limits tree size; if too many, we still try
tree = api("POST", f"{base}/git/trees", {"base_tree": base_tree, "tree": tree_items})
print("new tree", tree["sha"][:8], "entries", len(tree_items))
new_commit = api("POST", f"{base}/git/commits", {
    "message": "feat: import complete MemDbg source tree",
    "tree": tree["sha"],
    "parents": [head_sha],
})
print("commit", new_commit["sha"], new_commit.get("html_url"))
api("PATCH", f"{base}/git/refs/heads/{BRANCH}", {"sha": new_commit["sha"]})
print("DONE pushed to", f"https://github.com/{OWNER}/{REPO}")
