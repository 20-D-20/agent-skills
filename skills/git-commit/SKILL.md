---
name: git-commit
description: Generate a Git commit message following this embedded-systems project's Conventional Commits style — <type>[scope] subject in Chinese, with module-specific scopes and a Why-focused body. Use when the user asks to write, generate, or format a commit message, or wants help choosing the right type/scope for a change in this repo.
---

# Git Commit Skill

## 概述

本 skill 用于生成符合项目规范的 Git 提交消息，专为嵌入式项目定制。

## 提交消息格式

```
<type>[scope]: <subject>

<body>

<footer>
```

## 提交类型 (Type)

| 类型       | 用途        | 示例                            |
| ---------- | ----------- | ------------------------------- |
| **feat**   | 新增功能    | `feat[pwm]: 实现SPWM波形生成`   |
| **fix**    | 修复缺陷    | `fix[uart]: 修复DMA接收丢包问题`|
| **docs**   | 文档更新    | `docs[api]: 补充HAL接口说明`    |
| **style**  | 代码格式化  | `style[drivers]: 调整代码缩进`  |
| **refactor** | 代码重构  | `refactor[app]: 提取公共初始化函数` |
| **test**   | 测试相关    | `test[can]: 新增CAN协议单元测试`|
| **chore**  | 构建/工具   | `chore[cmake]: 更新编译配置`    |
| **perf**   | 性能优化    | `perf[isr]: 减少中断响应延迟`   |
| **ci**     | CI/CD配置   | `ci: 添加GitHub Actions工作流`  |
| **revert** | 撤销提交    | `revert: <hash> - 原因说明`     |

## 范围 (Scope)

使用方括号 `[scope]` 标识受影响的模块：

- **驱动层**: `uart`, `spi`, `i2c`, `can`, `gpio`, `timer`, `adc`, `pwm`
- **应用层**: `app`, `task`, `event`, `config`, `location`, `sampling`, `storage`
- **系统层**: `rtos`, `memory`, `interrupt`, `clock`
- **中间件**: `screen`, `hmi`, `protocol`, `printer`
- **工具链**: `cmake`, `makefile`, `git`
- **文档**: `readme`, `api`, `changelog`

## 主题 (Subject) 规则

- 简洁明了，**不超过 50 字符**
- 以**动词**开头：添加、修复、重构、优化、更新、实现、移除
- 使用**中文**，技术术语保留英文
- **不加句号**，不用过去式

### 示例

✅ 正确：
- `feat[uart]: 添加DMA发送功能`
- `fix[can]: 修复接收缓冲区溢出 #MH3052-42`
- `refactor[storage]: 优化索引查询算法`

❌ 错误：
- `Added DMA send feature` (英文)
- `fix: 修复了一些问题。` (有句号，模糊)
- `feat: 新功能` (缺少 scope)

## 正文 (Body)

**何时需要**：多行更改、复杂逻辑、需要解释原因

**格式**：
- 与主题空一行
- 每行不超过 72 字符
- 说明**改动内容**和**改动原因**

```
feat[can]: 实现CAN错误自动恢复机制

- 添加错误状态监控，检测Bus-Off故障
- 实现自动复位恢复逻辑，避免人工干预
- 新增错误统计计数器用于调试诊断

修复了长时间运行后CAN通信中断的问题，提高系统可靠性。
```

## 分支命名规范

| 分支类型 | 格式                           | 示例                          |
| -------- | ------------------------------ | ----------------------------- |
| 功能分支 | `feat/<description>`           | `feat/uart-dma-support`       |
| 修复分支 | `fix/<issue-id>-<description>` | `fix/MH3052-42-can-overflow`  |
| 热修复   | `hotfix/<version>-<description>` | `hotfix/v1.2.1-uart-timeout` |
| 发布分支 | `release/<version>`            | `release/v1.2.0`              |

## 使用说明

当用户请求生成 Git 提交消息时：

1. **分析变更文件**：确定涉及的模块和改动类型
2. **确定 Type**：根据改动性质选择合适的类型
3. **确定 Scope**：根据改动文件所属模块确定范围
4. **生成 Subject**：中文动词开头，简洁描述改动
5. **判断 Body**：复杂改动需添加详细说明

### 输出模板

```
<type>[<scope>]: <subject>

- <改动说明1>
- <改动说明2>
- <改动说明3>

<补充说明（可选）>
```

## 禁止事项

- **禁止**在提交消息末尾添加 `Co-Authored-By`、`Signed-off-by` 或任何 AI 工具署名行

- 提交消息只包含项目相关内容，不引入任何工具链或协作者元数据