---
trigger: glob
globs: **/*.{c,h}
description: EasyLogger 日志接入规范、TAG 注册与日志级别选择要求
---

# EasyLogger 日志接入规范

> 编辑 C 文件涉及日志输出时自动生效

## 三行接入（顺序不可变）

```c
#include "sys_log.h"
#define LOG_TAG    LOG_TAG_XXX    /* 引用 sys_log.h 注册表常量 */
#include <elog.h>
```

- 三行必须位于文件头部，所有函数定义之前
- 每个 `.c` 文件只能有一个 `LOG_TAG`
- **禁止**硬编码字符串，必须引用 `LOG_TAG_XXX` 常量

## TAG 注册规则

在 `sys_log.h` 的 TAG 注册表区域添加，按层级分组：

- 命名：`LOG_TAG_模块名`（全大写）
- 值：小写、简短、不超过 12 字符
- 分组：`APP / Middleware / Driver / BSP`
- **禁止** TAG 字符串重复

## 日志级别选择

| API | 级别 | 适用场景 |
|-----|------|----------|
| `log_a` | Assert | 致命错误，系统不可恢复 |
| `log_e` | Error | 操作失败，需关注 |
| `log_w` | Warn | 异常但可恢复 |
| `log_i` | Info | 关键流程节点（初始化、状态切换） |
| `log_d` | Debug | 调试细节 |
| `log_v` | Verbose | 高频详细跟踪 |

**禁止**在高频循环中使用 `log_i` 及以上级别，高频场景用 `log_d` 或 `log_v`。

## 日志内容规范

- **语言**：英文（避免终端乱码）
- **格式化**：地址 `0x%02X`，返回值 `ret=%d`，大小 `size=%u`
- **可诊断性**：必须包含关键变量值，禁止无上下文的 `log_e("error")`

## 必须添加日志的位置

- 模块初始化成功/失败
- 关键操作的返回值判断处
- 异常分支和错误处理路径
- 状态机切换点

## 模块级日志过滤（可选）

在 `bsp_logger_init()` 中通过 `elog_set_filter_tag_lvl(LOG_TAG_XXX, ELOG_LVL_xxx)` 配置，受 `ELOG_FILTER_TAG_LVL_MAX_NUM` 上限约束。
