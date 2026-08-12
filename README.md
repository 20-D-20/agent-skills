# agent-skills

个人 AI Agent 配置中央仓库：统一维护 skills 与 rules，再分发到 Linux / Windows 上已安装的 Agent。

## 目录结构

```text
agent-skills/
├── skills/              # Agent Skills，每个子目录包含 SKILL.md
├── rules/               # 复制到具体项目的规则文件
├── scripts/lib.sh       # Linux 脚本公共逻辑
├── setup-linux.sh       # Linux 首次初始化
├── sync-skills.sh       # 同步全部 skills
├── add-skill.sh         # 收录并分发一个 skill
├── remove-skill.sh      # 本机卸载或从中央仓库删除
├── sync-rules.sh        # 向一个或多个项目同步 rules
├── add-skill.ps1        # Windows 收录与分发
├── remove-skill.ps1     # Windows 删除
└── sync-rules.ps1       # Windows rules 同步
```

## Linux 首次部署

前置条件：

- Git
- Node.js 18 或更高版本（含 npm / npx）
- GNU `readlink`
- 至少一个已经安装或初始化过的 Agent，例如 Claude Code、Codex、Cursor、Gemini CLI 或 OpenCode

脚本只检查这些依赖，不会执行 `apt`、`sudo` 或自动安装软件。

```bash
git clone https://github.com/20-D-20/agent-skills.git
cd agent-skills
./setup-linux.sh
```

初始化脚本会同步仓库里的全部 skills。它先用本机命令和配置目录做安全预检，再由 skills CLI 完成实际的 Agent 识别。若没有检测到任何 Agent，脚本会中止，避免无人值守模式意外选择全部 Agent。

需要安装便捷命令时：

```bash
./setup-linux.sh --install-commands
```

它会在 `~/.local/bin` 创建以下符号链接，且不会覆盖已有的不同文件：

- `agent-skills-setup`
- `agent-skills-sync`
- `agent-skills-add`
- `agent-skills-remove`
- `agent-rules-sync`

只有确实希望覆盖 skills CLI 支持的全部 Agent 时，才显式使用 `--all-agents`。

## Skills 日常同步

默认从当前检出的仓库内容同步全部 skills：

```bash
./sync-skills.sh
```

先对中央仓库执行 fast-forward 更新再同步：

```bash
./sync-skills.sh --pull
```

同步是增量更新：会新增或覆盖同名 skill，不会自动删除本机多余的 skill。删除必须使用 `remove-skill.sh` 明确执行。

Linux 上 skills CLI 通常把内容维护在 `~/.agents/skills`，再向各 Agent 目录创建符号链接；Windows 使用目录联接（junction）。如果链接创建失败，CLI 可能回退为复制，因此仓库更新后仍建议执行一次同步。

Antigravity IDE 的全局技能目录是 `~/.gemini/config/skills`。当前 `skills@1` 会把 Antigravity 归入通用 Agent，却只写入 `~/.agents/skills`；因此 Linux 维护脚本检测到 Antigravity 后，会额外把本仓库 skills 增量复制到其官方全局目录。该兼容同步会新增或覆盖同名文件，不会删除目录中额外存在的 skill；通过 `remove-skill.sh` 卸载时会同时删除对应副本。

## 收录一个 skill

从外部仓库收录：

```bash
./add-skill.sh --source someone/repo --skill cool-skill
```

外部内容会先导入临时隔离目录，验证存在 `SKILL.md` 后才写入本仓库，不会在收录阶段直接激活到全局 Agent 目录。请只使用可信来源。

收录已经写在 `skills/<name>` 下的本地 skill：

```bash
./add-skill.sh --local --skill my-new-skill
```

默认流程是“收录 → 独立 commit → push → 全局分发”。push 前测试可使用：

```bash
./add-skill.sh --local --skill my-new-skill --no-push
```

脚本只提交目标 `skills/<name>` 路径，不会带入工作区或暂存区中的其他变更。如果当前分支在 upstream 之前已经存在旧的未推送提交，默认流程会拒绝代为 push；此时请手动处理，或使用 `--no-push`。

## 删除一个 skill

只从本机 Agent 目录卸载，不修改中央仓库：

```bash
./remove-skill.sh cool-skill
```

同时从中央仓库删除、commit、push，再从本机卸载：

```bash
./remove-skill.sh cool-skill --from-repo
```

中央删除默认要求输入 skill 名称确认。自动化环境可以增加 `--yes`；只创建本地删除提交时增加 `--no-push`。仓库删除的内容仍可从 Git 历史恢复。

## Rules 分发

rules 不全局安装，而是复制到目标项目的 `.claude/rules/` 与 `.agent/rules/`：

```bash
./sync-rules.sh /path/to/project-a /path/to/project-b
```

同名文件会覆盖，目标目录里额外存在的文件不会删除。同步后应在目标项目检查并提交变更。

每个 rule 同时携带两套条件加载键，各工具读取自己支持的字段：

```yaml
---
trigger: glob
globs: "**/*.{c,h}"
paths:
  - "**/*.{c,h}"
---
```

修改 rule 时应同步维护两套键。

## Windows

直接用 skills CLI 同步全部 skills，并让它自动识别本机 Agent：

```powershell
npx -y skills@1 add 20-D-20/agent-skills -g -s '*' -y
```

收录外部或本地 skill：

```powershell
.\add-skill.ps1 -Source someone/repo -Skill cool-skill
.\add-skill.ps1 -Skill my-new-skill -Local
.\add-skill.ps1 -Skill my-new-skill -Local -NoPush
```

明确同步到全部 Agent 时增加 `-AllAgents`。Windows 脚本同样会做零 Agent 防护、隔离导入、目标路径独立提交和旧提交 push 防护。

删除一个 skill：

```powershell
.\remove-skill.ps1 -Skill cool-skill
.\remove-skill.ps1 -Skill cool-skill -FromRepo
.\remove-skill.ps1 -Skill cool-skill -FromRepo -NoPush -Yes
```

行为与 `remove-skill.sh` 一致：默认只从本机全局 Agent 目录卸载；`-FromRepo` 会同时从中央仓库删除、创建独立提交并默认 push（要求交互式输入 skill 名称确认，或传 `-Yes` 跳过）；`-NoPush` 只能与 `-FromRepo` 一起使用。

向一个或多个项目同步 rules：

```powershell
.\sync-rules.ps1 -ProjectPath "D:\project-a", "D:\project-b"
```

## CLI 版本与遥测

维护脚本默认调用 `skills@1`，锁定主版本并接收兼容更新。需要精确版本或主动测试新版时可以覆盖：

```bash
SKILLS_CLI_PACKAGE=skills@1.5.9 ./sync-skills.sh
SKILLS_CLI_PACKAGE=skills@latest ./sync-skills.sh
```

```powershell
$env:SKILLS_CLI_PACKAGE = "skills@1.5.9"
.\add-skill.ps1 -Skill my-skill -Local
```

所有维护脚本都会设置 `DISABLE_TELEMETRY=1`。

## Git 认证与卡死防护

skills CLI 在自己的 TUI 里执行 `git clone`，交互式认证提示（SSH key passphrase、未知 host key、HTTPS 用户名密码）会被 TUI 吞掉，表现为永久卡在 `Cloning repository…`。脚本做了两层防护：

- 调用 CLI 时统一设置 `GIT_SSH_COMMAND="ssh -o BatchMode=yes"` 与 `GIT_TERMINAL_PROMPT=0`，认证失败立即报错退出，而不是等待一个看不见的输入。若外部已设置 `GIT_SSH_COMMAND`（例如自定义 ssh 或 plink），脚本不覆盖。
- 分发是只读 clone，`add-skill` 会把 GitHub 的 SSH origin（`git@github.com:owner/repo.git`）转成 `owner/repo` 简写走 HTTPS，完全绕开 SSH 认证。非 GitHub 地址原样使用。

私有仓库必须走 SSH 时，用环境变量保留原始 origin，并确保 ssh-agent 已加载密钥：

```bash
eval "$(ssh-agent -s)" && ssh-add ~/.ssh/id_ed25519_github
AGENT_SKILLS_KEEP_ORIGIN_URL=1 ./add-skill.sh --local --skill my-skill
```

```powershell
$env:AGENT_SKILLS_KEEP_ORIGIN_URL = "1"
```

注意 `git push` 不受影响，仍在前台运行，可以正常交互输入 passphrase。

## 维护约定

- 本仓库是唯一编辑入口；不要直接修改各 Agent 目录中的副本。
- skills 同步不负责删除；删除必须显式调用 `remove-skill.sh`。
- rules 始终按项目复制，不做全局安装或符号链接。
- `rules/architecture-maintain.md` 为 MH3500C 项目专属，其余 rules 为嵌入式通用规范。
