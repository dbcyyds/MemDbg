# 发布到 GitHub

仓库已创建：https://github.com/dbcyyds/MemDbg

本地已完成 initial commit。若 `git push` 提示需要登录，在 Termux 执行：

```bash
gh auth login -h github.com -p https -w
# 或
export GH_TOKEN=ghp_你的个人访问令牌

cd ~/vk_imgui_float
git remote set-url origin https://github.com/dbcyyds/MemDbg.git
git push -u origin main
```

也可使用本仓库 `tools/github_upload_tree.py`（需 `GH_TOKEN`）上传树。
