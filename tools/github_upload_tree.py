
import json, os, sys, urllib.request
from pathlib import Path

token = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN")
if not token:
    print("Set GH_TOKEN", file=sys.stderr)
    sys.exit(1)

def api(method, url, data=None):
    req = urllib.request.Request(url, data=data, method=method, headers={
        "Authorization": f"Bearer {token}",
        "Accept": "application/vnd.github+json",
        "X-GitHub-Api-Version": "2022-11-28",
        "User-Agent": "MemDbg-uploader",
        "Content-Type": "application/json",
    })
    with urllib.request.urlopen(req) as r:
        return json.loads(r.read().decode())

owner, repo = "dbcyyds", "MemDbg"
root = Path("/data/data/com.termux/files/home/vk_imgui_float")
# get ref
ref = api("GET", f"https://api.github.com/repos/{owner}/{repo}/git/ref/heads/main")
sha = ref["object"]["sha"]
commit = api("GET", f"https://api.github.com/repos/{owner}/{repo}/git/commits/{sha}")
base_tree = commit["tree"]["sha"]

# collect files
files = []
for line in (root/"out/push_order.txt").read_text().splitlines():
    p = root/line
    if not p.is_file():
        continue
    if p.suffix in (".ttf",".png",".gif"):
        continue
    try:
        content = p.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        continue
    # create blob
    blob = api("POST", f"https://api.github.com/repos/{owner}/{repo}/git/blobs",
               json.dumps({"content": content, "encoding": "utf-8"}).encode())
    files.append({"path": line, "mode": "100644", "type": "blob", "sha": blob["sha"]})
    print("blob", line, blob["sha"][:8])

# tree
tree = api("POST", f"https://api.github.com/repos/{owner}/{repo}/git/trees",
           json.dumps({"base_tree": base_tree, "tree": files}).encode())
new_commit = api("POST", f"https://api.github.com/repos/{owner}/{repo}/git/commits",
    json.dumps({
        "message": "feat: import full MemDbg source tree",
        "tree": tree["sha"],
        "parents": [sha],
    }).encode())
api("PATCH", f"https://api.github.com/repos/{owner}/{repo}/git/refs/heads/main",
    json.dumps({"sha": new_commit["sha"]}).encode())
print("DONE", new_commit["sha"], new_commit.get("html_url"))
