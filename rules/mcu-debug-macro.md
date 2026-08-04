---
trigger: glob
globs: **/*.{c,h}
paths:
  - "**/*.{c,h}"
description: 模块级调试宏规范：debug_config.h 集中开关管理、DBG_PRINT 输出后端抽象、分层命名与新增模块步骤
---

# 🔍 模块级调试宏规范

> 编辑 C 文件涉及调试输出时自动生效

## 用途定位

编译期开关控制的轻量调试跟踪，关闭后**代码与格式化字符串一并从固件中消失**。

## 集中管理文件

全工程唯一开关文件 `debug_config.h`，放在与 `main.h` 同级目录。

**新增调试宏前必须先查看 `main.h` 同级目录**：已存在 `debug_config.h` 就在其中增补，只有确认不存在时才新建，禁止重复创建或另起文件名。

文件固定四段结构：

| 段 | 内容 | 说明 |
|----|------|------|
| 1 | `DEBUG_MASTER_ENABLE` | 全局总开关，`#ifndef` 包裹，可被 `-D` 覆盖 |
| 2 | `DBG_PRINT` | 输出后端抽象，默认 `printf`，老工程只改这一行 |
| 3 | 模块开关总表 | 纯 `#define`，按层分组，一屏看完全工程状态 |
| 4 | 模块调试宏 | `#if` 块，**分组与顺序严格对应第 3 段** |

新建工程时照此模板创建：

```c
/**
 * @file    debug_config.h
 * @brief   全工程模块级调试开关集中管理
 */

#ifndef __DEBUG_CONFIG_H__
#define __DEBUG_CONFIG_H__

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================
 * 1. 全局总开关
 * ----------------------------------------------------------
 * 1 = 允许各模块开关生效；0 = 全工程调试输出一律关闭。
 * 用 #ifndef 包裹，Release 构建可加 -DDEBUG_MASTER_ENABLE=0
 * 覆盖，无需改动本文件。
 * ========================================================== */
#ifndef DEBUG_MASTER_ENABLE
#define DEBUG_MASTER_ENABLE     1
#endif

/* ==========================================================
 * 2. 输出后端
 * ----------------------------------------------------------
 * 默认走 printf（需工程已完成重定向，如 SEGGER RTT）。
 * 无法使用 printf 的老工程只改这一行：
 *   #define DBG_PRINT(fmt, ...)  SEGGER_RTT_printf(0, fmt, ##__VA_ARGS__)
 *   #define DBG_PRINT(fmt, ...)  Uart_Printf(fmt, ##__VA_ARGS__)
 * ========================================================== */
#define DBG_PRINT(fmt, ...)     printf(fmt, ##__VA_ARGS__)

/* ==========================================================
 * 3. 模块开关总表
 * ----------------------------------------------------------
 * 全工程调试状态一屏可见；按层分组，组内按模块名排序。
 * ========================================================== */

/* --- Application --- */
#define DBG_APP_STORAGE         0
#define DBG_APP_MODBUS          0

/* --- Service --- */
#define DBG_SVC_RECSTORE        0

/* --- BSP --- */
#define DBG_BSP_FLASH           0

/* --- Driver --- */
#define DBG_DRV_UART            0

/* ==========================================================
 * 4. 模块调试宏
 * ----------------------------------------------------------
 * 分组与模块顺序必须与第 3 段严格一一对应，便于比对遗漏。
 * 新增模块照下方 5 行模板复制，勿手写变体。
 * ========================================================== */

/* --- Application --- */
#if (DEBUG_MASTER_ENABLE && DBG_APP_STORAGE)
#define APP_STORAGE_DBG(fmt, ...)   DBG_PRINT("[APP/STORAGE] " fmt, ##__VA_ARGS__)
#else
#define APP_STORAGE_DBG(fmt, ...)   ((void)0)
#endif

#if (DEBUG_MASTER_ENABLE && DBG_APP_MODBUS)
#define APP_MODBUS_DBG(fmt, ...)    DBG_PRINT("[APP/MODBUS] " fmt, ##__VA_ARGS__)
#else
#define APP_MODBUS_DBG(fmt, ...)    ((void)0)
#endif

/* --- Service --- */
#if (DEBUG_MASTER_ENABLE && DBG_SVC_RECSTORE)
#define SVC_RECSTORE_DBG(fmt, ...)  DBG_PRINT("[SVC/RECSTORE] " fmt, ##__VA_ARGS__)
#else
#define SVC_RECSTORE_DBG(fmt, ...)  ((void)0)
#endif

/* --- BSP --- */
#if (DEBUG_MASTER_ENABLE && DBG_BSP_FLASH)
#define BSP_FLASH_DBG(fmt, ...)     DBG_PRINT("[BSP/FLASH] " fmt, ##__VA_ARGS__)
#else
#define BSP_FLASH_DBG(fmt, ...)     ((void)0)
#endif

/* --- Driver --- */
#if (DEBUG_MASTER_ENABLE && DBG_DRV_UART)
#define DRV_UART_DBG(fmt, ...)      DBG_PRINT("[DRV/UART] " fmt, ##__VA_ARGS__)
#else
#define DRV_UART_DBG(fmt, ...)      ((void)0)
#endif

#ifdef __cplusplus
}
#endif

#endif /* __DEBUG_CONFIG_H__ */
```

## 命名规范

| 项目 | 格式 | 示例 |
|------|------|------|
| 模块开关 | `DBG_<层>_<模块>` | `DBG_APP_STORAGE` |
| 调试宏 | `<层>_<模块>_DBG` | `APP_STORAGE_DBG(...)` |
| 输出 TAG | `[<层>/<模块>] ` | `"[APP/STORAGE] "` |

- 层前缀：`APP` / `SVC` / `BSP` / `DRV`，对应分层架构
- 同一模块在三处必须使用**完全相同的词根**，禁止 `STORAGE_MANAGER` 与 `STORAGE_MGR` 混用
- TAG 一律**大写加斜杠**

## 新增模块步骤

1. 第 3 段对应层分组内添加 `#define DBG_<层>_<模块>  0`（**默认 0**）
2. 第 4 段**相同位置**复制 5 行模板，替换开关名、宏名、TAG
3. 模块 `.c` 顶部 `#include "debug_config.h"`
4. 需要输出时临时把第 3 段开关改 1，调试完改回 0

```c
/* storage_manager.c */
#include "debug_config.h"

APP_STORAGE_DBG("SaveRecord: FileNo=%lu, Size=%lu\n", u32FileNo, u32Size);
```

## 使用约束

- **禁止在中断服务程序中使用**
- **禁止在高频循环中使用**，必要时加计数器降频
- 内容用英文，避免终端乱码；必须携带关键变量值
- 格式化遵循代码规范：地址 `0x%02X`，返回值 `ret=%d`，大小 `size=%u`

## 禁止事项

- **禁止 `#ifdef DEBUG_XXX`**：`#define DEBUG_XXX 0` 同样满足 `#ifdef`，开关将永久失效。一律用 `#if`
- **禁止关闭态展开为空**：必须为 `((void)0)`，否则 `if (x) MODULE_DBG("...");` 会吞掉下一条语句
- **禁止在模块 `.c` 内自行定义调试宏**，一切定义集中于 `debug_config.h`
- **禁止硬编码 `printf`**，一律经由 `DBG_PRINT`
