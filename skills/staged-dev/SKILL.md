---
name: staged-dev
description: 通过显式 planner、implementer 或 reviewer 身份，为长周期、高风险、跨模块或预计多轮返工的软件任务运行上下文隔离的三阶段开发工作流，并使用仓库本地文件完成阶段交接。仅当用户明确调用 $staged-dev 时使用；执行 staged task 时必须指定角色，解释、审查或配置本 skill 以及查询 task 状态时不要求角色。实施前必须取得用户批准，并在独立 Codex 会话中完成方案、实现与审查。
---

# 分阶段开发 / Staged Development

每个执行会话只承担一个角色。将 `workflow.json` 和已批准的 `PLAN.md` 视为跨会话契约，不依赖其他会话中的隐含上下文。

## 语言策略

- 用户交互和计划正文使用简体中文，技术术语保留 English。
- 路径、命令、identifier、API、Git 信息及状态值保留原文。
- 关键业务代码注释使用中文，同时服从项目已有规范。
- 不翻译 `planner`、`implementer`、`reviewer`、`PASS`、`CHANGES_REQUESTED`、`BLOCKED`。

## 解析调用

- 执行 staged task 时要求用户显式指定 `planner`、`implementer` 或 `reviewer`；缺少角色时停止并询问。
- 解释、审查或配置本 skill 时不进入阶段工作流，也不要求角色。
- 查询状态可直接使用 `$staged-dev status <task-id>`；缺少 task ID 时停止并询问。
- `planner` 可以使用已有 task ID，也可以生成简短的小写连字符 ID。
- `implementer` 和 `reviewer` 必须提供准确的已有 task ID。
- 使用 `git rev-parse --show-toplevel` 确定仓库根目录。
- 任务文档只能写入 `<repo>/.codex/tasks/<task-id>/`。
- 除非用户另行要求，否则不得启动 tmux、创建 worktree、commit、push 或修改 Git branch。

本 skill 是大型或高风险任务的显式严格流程。小型、单文件且可直接验证的修改，优先使用普通 Plan mode 加独立 review，不为此 skill 增加 `lite/strict` 分支。

## 状态辅助脚本

所有状态转换必须调用 `scripts/workflow_state.py`。使用可用的 Python 3，并相对于本 `SKILL.md` 解析脚本路径。

```text
python workflow_state.py init --repo <repo> --task-id <id> --title <title>
python workflow_state.py status --repo <repo> --task-id <id>
python workflow_state.py approve --repo <repo> --task-id <id> --user-approved
python workflow_state.py revise-plan --repo <repo> --task-id <id>
python workflow_state.py begin-implementation --repo <repo> --task-id <id>
python workflow_state.py complete-implementation --repo <repo> --task-id <id>
python workflow_state.py begin-review --repo <repo> --task-id <id>
python workflow_state.py complete-review --repo <repo> --task-id <id> --verdict <pass|changes_requested|blocked>
python workflow_state.py block --repo <repo> --task-id <id> --reason <reason>
```

禁止手工编辑 `workflow.json`。`begin-implementation` 和 `begin-review` 可在对应活动状态下重复调用以恢复中断会话。其他脚本错误应报告具体不一致并停止，不得绕过状态机。

仅在初始化 task、恢复中断或 blocked task、或排查状态转换错误时读取 `references/workflow-protocol.md`；正常下游阶段不要重复加载该文档。

## Planner

1. 任务不存在时运行 `init`；任务存在时先运行 `status`。
2. 初始化、恢复或状态异常时阅读 `references/workflow-protocol.md`。
3. 检查仓库并澄清行为、范围、约束、风险和验证方式；除任务文档外保持只读。
4. 使用中文填写 `PLAN.md` 全部必填章节，移除全部 `<!-- 必填 ... -->` 或 `<!-- REQUIRED ... -->` 标记。新功能可将“当前行为”明确填写为不适用。
5. 解决所有重要决策后，将“待确认问题 / Open Questions”填写为 `- 无。`。
6. 展示方案后停止，不得修改源码。
7. 只有用户明确批准当前方案后，才能运行 `approve --user-approved`。批准要求 non-task working tree 干净；不得为了通过检查而擅自 reset、clean、stash 或丢弃修改。
8. 报告 task ID、已批准 plan version 和 plan hash，然后停止。

已批准方案需要变化时，先取得用户指示，再运行 `revise-plan`，修改方案并重新申请批准。

## Implementer

1. 运行 `status`，然后运行 `begin-implementation`；中断后重复运行同一命令恢复当前 cycle。
2. 默认只读取 `PLAN.md` 和 `workflow.json`；返工时增加最新 Review，按需检查源码，不主动加载历史报告。
3. 作为该任务唯一的源码写入者，不得修改 `PLAN.md`。
4. 只实现已批准范围，并执行与风险匹配的验证。
5. 使用中文填写当前 `implementations/implementation-NNN.md`，移除全部必填标记，保留准确的命令、路径、identifier 和原始结果摘要。
6. 运行 `complete-implementation`，由脚本记录可校验的 implementation snapshot，报告结果并停止。

方案与代码冲突、必须扩大范围或环境导致无法可靠继续时，不得静默重设计。运行 `block --reason <reason>`，报告 `BLOCKED` 并返回 Planner。

## Reviewer

1. 运行 `status`，然后运行 `begin-review`；中断后重复运行同一命令恢复当前 cycle。
2. 阅读 `PLAN.md`、最新 Implementation Report 以及真实 Git diff/status；不得把 Implementation Report 本身当作通过证据。
3. 检查全部 acceptance criteria、回归风险、方案偏差和测试声明，执行合适的只读检查或测试。
4. 默认不得修改源码。使用中文填写当前 `reviews/review-NNN.md`，移除全部必填标记。
5. 使用固定结论 `PASS`、`CHANGES_REQUESTED` 或 `BLOCKED`，随后用对应的小写 verdict 运行 `complete-review` 并停止。

若状态漂移或环境问题导致无法完成 Review Report，运行 `block --reason <reason>`。`CHANGES_REQUESTED` 时让新的 Implementer 会话继续同一 task ID；`BLOCKED` 时返回 Planner；`PASS` 时仅报告独立审查通过，不自动 commit 或 merge。

## 边界限制

- 持久的仓库规范放在 `AGENTS.md`，task 文档只记录本任务事实。
- 不得把完整对话、探索过程或原始构建日志复制进交接文档。
- Planner 和 Reviewer 不得修改源码；Implementer 不得重写已批准方案。
- 同一 checkout 中不得使用多个并行写入 Agent。
- 批准到 Review 完成期间不得改变 Git `HEAD`；通过后再 commit。
- `.codex/tasks/` 仅供本地使用；未跟踪文档不会自动出现在其他 Git worktree 中。
