---
name: easylogger
description: 基于 EasyLogger 的模块日志接入规范，实现统一 TAG 管理和分级日志输出
---

# EasyLogger 模块日志接入 Skill

## 触发条件

当用户要求为某个**模块/文件添加日志**时使用本 Skill，包括但不限于：
- 为已有模块接入 EasyLogger 日志
- 新建模块时配套添加日志支持
- 调整模块的日志级别或输出格式

**不适用于**：EasyLogger 框架本身的移植或底层 port 层修改。

---

## 核心原则

> **每个 `.c` 文件独立定义 `LOG_TAG`，所有 TAG 必须在 `sys_log.h` 中集中注册。**

```
sys_log.h（TAG 注册表 + 初始化接口）
    │
    │  集中定义所有模块的 LOG_TAG_xxx 常量
    ▼
各模块 .c 文件
    │
    │  #include "sys_log.h"
    │  #define LOG_TAG  LOG_TAG_XXX   ← 引用注册表常量
    │  #include <elog.h>              ← 必须在 LOG_TAG 之后
    ▼
EasyLogger 核心（elog.h / elog.c）
    │
    │  宏展开时自动使用当前文件的 LOG_TAG
    ▼
elog_port.c（底层输出实现）
```

---

## 实施步骤

### 第一步：在 TAG 注册表中注册新模块

打开 `sys_log.h`，在 TAG 注册表区域添加新模块的 TAG 定义。

**规则：**
- TAG 常量命名：`LOG_TAG_模块名`（全大写）
- TAG 字符串值：小写、简短、可区分（建议不超过 12 字符）
- TAG 按功能层级分组（APP / Middleware / Driver / BSP）
- **禁止** TAG 字符串重复

**TAG 注册表示例：**

```c
/* sys_log.h */
#ifndef __SYS_LOG_H__
#define __SYS_LOG_H__

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * 日志 TAG 注册表（所有模块的 LOG_TAG 必须在此集中注册）
 * ================================================================ */

/* --- Application Layer --- */
#define LOG_TAG_MAIN        "main"
#define LOG_TAG_TASK_MGR    "task_mgr"

/* --- Middleware Layer --- */
#define LOG_TAG_STORAGE     "storage"
#define LOG_TAG_FATFS       "fatfs"

/* --- Driver Layer --- */
#define LOG_TAG_UART        "uart"
#define LOG_TAG_SPI         "spi"

/* --- BSP Layer --- */
#define LOG_TAG_GPIO        "gpio"

/* ================================================================ */

void bsp_logger_init(void);
void bsp_logger_test(void);

#ifdef __cplusplus
}
#endif

#endif /* __SYS_LOG_H__ */
```

### 第二步：在目标 `.c` 文件中接入日志

在需要添加日志的 `.c` 文件**头部**（所有函数定义之前），按以下**固定顺序**添加 3 行代码：

```c
/* === 日志接入（必须在所有函数定义之前） === */
#include "sys_log.h"
#define LOG_TAG    LOG_TAG_XXX    /* 替换为注册表中的常量 */
#include <elog.h>
```

**关键约束：**
- `#define LOG_TAG` **必须在** `#include <elog.h>` **之前**（EasyLogger 的宏展开机制要求）
- `#include "sys_log.h"` **必须在** `#define LOG_TAG` **之前**（否则常量未定义）
- 这 3 行代码不可更改顺序，不可拆分到不同位置
- 每个 `.c` 文件只能有一个 `LOG_TAG`

### 第三步：使用日志 API 输出日志

**API 选择指南：**

| API | 级别 | 使用场景 | 示例 |
|:----|:-----|:---------|:-----|
| `log_a(...)` | Assert | 致命错误，系统不可恢复 | `log_a("stack overflow detected");` |
| `log_e(...)` | Error | 操作失败，需关注 | `log_e("flash write failed, ret=%d", s32Ret);` |
| `log_w(...)` | Warn | 异常但可恢复的情况 | `log_w("retry count: %d", u8Retry);` |
| `log_i(...)` | Info | 关键流程节点 | `log_i("system init complete");` |
| `log_d(...)` | Debug | 调试细节 | `log_d("reg[0x%02X] = 0x%04X", u8Reg, u16Val);` |
| `log_v(...)` | Verbose | 高频次详细跟踪 | `log_v("tick=%lu", u32Tick);` |

**日志内容规范：**

1. **语言**：日志内容使用**英文**（避免中文编码在嵌入式终端乱码）
2. **格式**：关键变量使用明确的格式化标识
   - 地址/寄存器：`0x%02X` / `0x%04X`
   - 返回值/错误码：`ret=%d` / `err=%d`
   - 大小/长度：`size=%u` / `len=%u`
   - 布尔状态：`%s` 配合 `"true"/"false"`
3. **关键位置**必须添加日志：
   - 模块初始化成功/失败
   - 关键操作的返回值判断处
   - 异常分支和错误处理路径
   - 状态机切换点

**完整模块示例：**

```c
/**
 * @file    app_sensor.c
 * @brief   传感器应用模块
 */

#include "app_sensor.h"
#include "sys_log.h"
#define LOG_TAG    LOG_TAG_SENSOR
#include <elog.h>

int32_t app_sensor_init(void)
{
    log_i("sensor init start");

    int32_t s32Ret = sensor_hw_init();
    if (s32Ret != 0)
    {
        log_e("sensor hw init failed, ret=%d", s32Ret);
        return s32Ret;
    }

    uint16_t u16DevId = sensor_read_id();
    if (u16DevId != SENSOR_EXPECTED_ID)
    {
        log_e("sensor ID mismatch: got=0x%04X, expected=0x%04X",
              u16DevId, SENSOR_EXPECTED_ID);
        return -1;
    }

    log_i("sensor init ok, ID=0x%04X", u16DevId);
    return 0;
}

void app_sensor_process(void)
{
    int32_t s32Temp = sensor_read_temperature();
    log_d("temperature raw=%d", s32Temp);

    if (s32Temp > SENSOR_TEMP_ALARM)
    {
        log_w("temperature alarm: %d > %d", s32Temp, SENSOR_TEMP_ALARM);
    }
}
```

### 第四步：配置模块日志级别（可选）

在 `bsp_logger_init()` 中使用 `elog_set_filter_tag_lvl()` 对特定模块设置独立的日志级别：

```c
void bsp_logger_init(void)
{
    elog_init();

    /* 格式配置 ... */

    /* ========== 模块级日志过滤 ========== */
    /* 注意：最多同时配置 ELOG_FILTER_TAG_LVL_MAX_NUM 个 TAG
     * 当前值为 5，如需更多请修改 elog_cfg.h */
    elog_set_filter_tag_lvl(LOG_TAG_UART,    ELOG_LVL_WARN);
    elog_set_filter_tag_lvl(LOG_TAG_SENSOR,  ELOG_LVL_DEBUG);

    elog_start();
}
```

> **提示**：`ELOG_FILTER_TAG_LVL_MAX_NUM` 的值决定了可同时设置独立级别的 TAG 数量上限，
> 需根据项目模块数量在 `elog_cfg.h` 中调整。

---

## 自检清单

在输出代码**之前**，必须逐项核查：

### 头文件顺序检查
- [ ] `#include "sys_log.h"` 在 `#define LOG_TAG` 之前
- [ ] `#define LOG_TAG` 在 `#include <elog.h>` 之前
- [ ] 三行日志接入代码位于文件头部，所有函数定义之前

### TAG 规范检查
- [ ] `LOG_TAG` 使用的是 `sys_log.h` 中已注册的常量（`LOG_TAG_XXX`），而非硬编码字符串
- [ ] TAG 注册表中**无重复** TAG 字符串
- [ ] TAG 分组正确（按 APP / Middleware / Driver / BSP 分层）

### 日志内容检查
- [ ] 日志内容使用**英文**
- [ ] 关键变量值已包含在日志输出中（让日志具有可诊断性）
- [ ] 初始化函数包含成功和失败两个分支的日志
- [ ] 错误处理路径包含 `log_e` 或 `log_w` 级别日志
- [ ] 无高频循环中使用 `log_i` 以上级别（避免日志风暴，高频场景用 `log_d` 或 `log_v`）

### 级别选择检查
- [ ] Assert/Error 级别仅用于真正的异常和故障
- [ ] Info 级别仅用于关键流程节点（初始化、状态变化等）
- [ ] 周期性数据读取使用 Debug 或 Verbose 级别
- [ ] 如需对该模块独立控制级别，已在 `bsp_logger_init()` 中配置 `elog_set_filter_tag_lvl()`

---

## 反面示例（⚠️ 严禁出现）

```c
/* ❌ 错误：LOG_TAG 直接硬编码字符串，未引用注册表 */
#define LOG_TAG    "sensor"
#include <elog.h>

/* ❌ 错误：顺序反了，elog.h 在 LOG_TAG 之前 */
#include <elog.h>
#define LOG_TAG    LOG_TAG_SENSOR

/* ❌ 错误：日志使用中文 */
log_e("传感器初始化失败");

/* ❌ 错误：在高频循环中使用 log_i */
while (1)
{
    log_i("adc value = %d", adc_read());   /* 日志风暴！应该用 log_d 或 log_v */
    osDelay(10);
}

/* ❌ 错误：日志内容无诊断价值 */
log_e("error");           /* 没有错误码、没有上下文，无法定位问题 */

/* ❌ 错误：一个 .c 文件中定义多个 LOG_TAG */
#define LOG_TAG    LOG_TAG_UART
#include <elog.h>
/* ... 若干代码 ... */
#undef LOG_TAG
#define LOG_TAG    LOG_TAG_SPI    /* 不允许！一个文件一个 TAG */
```
