# agent-skills

个人 AI Agent 配置中央仓库：统一管理 skills 与 rules，作为所有工具（Claude Code / Antigravity 等）的单一权威源。

## 目录结构

```
agent-skills/
├── skills/     # Agent Skills（每个子目录一个 skill，含 SKILL.md）
└── rules/      # 规则文件（双 frontmatter，兼容 Claude Code 与 Windsurf/Antigravity 系）
```

## Skills 分发

通过 [skills.sh](https://www.skills.sh/) CLI 全局安装：

```powershell
# 全局安装本仓库全部 skill
npx skills add 20-D-20/agent-skills -g -a claude-code -s '*' -y

# 内容更新后重拉
npx skills update
```

> Windows 上 skills.sh 对 claude-code **强制 copy 模式**（与 symlink 权限无关），
> 因此"改仓库"与"生效"是两步：push 后必须再跑一次 `add`/`update`。

### 新增 skill 到仓库并分发：`add-skill.ps1`

把"拉取 → 入库 → commit → push → 分发"包成一条命令：

```powershell
# 收录 skills.sh 上的公开 skill
.\add-skill.ps1 -Source someone/repo -Skill cool-skill

# 分发自己写在 skills/<name> 下的新 skill
.\add-skill.ps1 -Skill my-new-skill -Local

# push 前本地测试（从本地路径分发，不推远程）
.\add-skill.ps1 -Skill my-new-skill -Local -NoPush
```

### Skill 分类

- **通用**：doc-writer, grill-me, grill-with-docs, handoff, logic-extractor, prototype, staged-dev, tdd, to-issues, to-prd, write-a-skill
- **嵌入式/项目向**：git-commit, easylogger, embedded-doc（按需装到具体项目）

## Rules 分发

rules 是项目规范，**不装全局**——复制进目标项目的 `.claude/rules/` 与 `.agent/rules/` 并提交进该项目的 git：

```powershell
Copy-Item rules\* <project>\.claude\rules\ -Force
Copy-Item rules\* <project>\.agent\rules\ -Force
```

### frontmatter 约定

每个 rule 同时携带两套条件加载键，各工具只认自己的：

```yaml
---
trigger: glob          # Windsurf/Antigravity 系
globs: **/*.{c,h}
paths:                 # Claude Code
  - "**/*.{c,h}"
---
```

修改 rule 时两套键必须同步维护。

## 维护约定

- 本仓库是唯一编辑入口；不要直接改各工具目录下的副本，改完这里再分发
- `rules/architecture-maintain.md` 为 MH3500C 项目专属，其余 rules 为嵌入式通用规范
