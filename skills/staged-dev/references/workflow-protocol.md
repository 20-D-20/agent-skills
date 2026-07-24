# 工作流协议 / Workflow Protocol

仅在初始化 task、恢复中断或 blocked task、或排查状态错误时读取本文档。`workflow.json` 是机器可读状态来源，已批准的 `PLAN.md` 是人类可读契约。

## 状态转换

```text
draft --用户明确批准且 working tree 干净--> approved
approved --开始实现--> implementing
implementing --完成实现报告和 snapshot--> implemented
implemented --开始审查且 snapshot 未变化--> reviewing
reviewing --通过--> passed
reviewing --要求修改--> changes_requested
changes_requested --snapshot 未变化且开始返工--> implementing (next cycle)
implementing/reviewing --无法可靠继续--> blocked
blocked/approved/changes_requested --修订方案--> draft (next plan version)
```

`begin-implementation` 在 `implementing` 状态、`begin-review` 在 `reviewing` 状态可幂等恢复当前 cycle。不得绕过其他失败的状态转换。

## 批准与 baseline

批准前必须：

- 删除 `PLAN.md` 中全部必填标记。
- 将“待确认问题 / Open Questions”填写为 `- 无。`；旧任务的 `- None.` 仍兼容。
- 获得用户对当前方案的明确批准。
- 保持 non-task working tree 干净。

批准操作记录 Git commit、plan version、批准时间和 `PLAN.md` SHA-256。Implementer 必须从相同 `HEAD` 和干净 working tree 开始。不得为了通过检查而 reset、clean、stash、丢弃或覆盖用户修改。

批准后到 Review 完成前不得改变 `HEAD`。如果需要保留 partial implementation 后重新规划，先转为 `blocked`，再由用户决定如何处理现有修改；新的方案只有在 working tree 恢复干净后才能重新批准。

## Implementation snapshot

完成实现时，脚本计算 tracked diff 与所有 non-ignored untracked files 的内容摘要，并保存为 `implementation_snapshot_sha256`。以下操作必须验证该摘要未变化：

- 开始或恢复 Reviewer。
- Reviewer 完成结论。
- `CHANGES_REQUESTED` 后开始下一轮 Implementer。

如果 Reviewer 执行测试后留下 non-ignored 文件变化，应先报告具体文件和原因，不得静默通过 snapshot 检查。

## 异常恢复

- `implementing` 中断：新的 Implementer 重复运行 `begin-implementation`，继续现有 cycle 和报告。
- `reviewing` 中断：新的 Reviewer 重复运行 `begin-review`，继续现有 cycle 和报告。
- 实现或审查无法可靠继续：运行 `block --reason <reason>`，再返回 Planner。
- baseline、plan hash 或 snapshot 不一致：停止并让用户决定如何恢复，不得自动修改 Git 状态。

旧报告属于审计记录，不是默认上下文。Implementer 返工时只增加最新 Review；Reviewer 只读取当前方案、最新 Implementation Report 和真实 diff。
