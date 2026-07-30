---
trigger: glob
globs: **/*.{c,h}
description: FreeRTOS 任务管理规范、任务注册与业务逻辑解耦及入口函数放置规则
---

# FreeRTOS 任务管理规范

> 编辑 C/H 文件涉及 FreeRTOS 任务创建、任务函数定义时自动生效

## 核心原则

**freertos.c 是"任务注册表"，不是业务逻辑容器。** 所有任务实现必须放到对应的 `app_xxx.c` 模块中。

## freertos.c 的职责（仅限以下内容）

| 允许 | 禁止 |
|------|------|
| `osThreadDef` + `osThreadCreate` 任务注册 | 任务函数体的实现 |
| RTOS 内核回调（StackOverflowHook 等） | 业务逻辑、外设初始化 |
| `#include` 各任务模块头文件 | 直接调用驱动/BSP 层 API |

## 任务函数放置规则

```
freertos.c                          → 只注册任务（一行创建）
Application/XXX/app_xxx.c           → 任务入口函数 + 模块私有数据/回调
Application/XXX/app_xxx.h           → 仅导出任务入口函数声明
```

### 正确示例

```c
/* freertos.c — 只做注册 */
#include "app_gui.h"
#include "app_sensor.h"

/* USER CODE BEGIN RTOS_THREADS */
osThreadDef(guiTask, app_gui_task, osPriorityAboveNormal, 0, 1024);
osThreadCreate(osThread(guiTask), NULL);

osThreadDef(sensorTask, app_sensor_task, osPriorityNormal, 0, 512);
osThreadCreate(osThread(sensorTask), NULL);
/* USER CODE END RTOS_THREADS */
```

```c
/* app_sensor.c — 任务实现在独立模块 */
#include "app_sensor.h"
#include "sht3x.h"

void app_sensor_task(void const *argument)
{
    (void)argument;
    sht3x_init();
    for (;;)
    {
        sht3x_process();
        osDelay(1000);
    }
}
```

### 错误示例（禁止）

```c
/* freertos.c 中直接写任务实现 — 禁止 */
void StartTestTask(void const *argument)
{
    w25qxx_init();
    lcd_init();
    sht3x_init();
    for (;;) { sht3x_process(); osDelay(1000); }
}
```

## 任务模块文件规范

### 头文件（app_xxx.h）

```c
#ifndef __APP_XXX_H__
#define __APP_XXX_H__

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  XXX 任务入口
 * @param  argument: 未使用
 */
void app_xxx_task(void const *argument);

#ifdef __cplusplus
}
#endif

#endif /* __APP_XXX_H__ */
```

### 源文件（app_xxx.c）结构

1. 头文件包含（含日志三行接入）
2. 私有变量（Queue、静态缓冲等）
3. 私有函数（回调、内部逻辑）
4. 导出函数（任务入口 `app_xxx_task`）

## 任务命名约定

| 项目 | 约定 | 示例 |
|------|------|------|
| 任务入口函数 | `app_模块_task` | `app_gui_task` |
| osThreadDef 名称 | `模块Task`（驼峰） | `guiTask` |
| 文件名 | `app_模块.c/.h` | `app_gui.c` |

## 硬件初始化的放置

- **外设初始化**（LCD、Flash、传感器）不应散落在 freertos.c
- 两种合理方式：
  1. 在各任务模块内部初始化自己依赖的外设
  2. 创建专门的 `app_system_init()` 统一处理，由某个任务在启动时调用
- 如果任务间有初始化依赖，使用事件标志组（EventGroup）或信号量同步，禁止用 `osDelay` 硬等待

## CubeMX 兼容性

- freertos.c 会被 CubeMX 重新生成，任务创建代码必须放在 `USER CODE BEGIN/END` 区域内
- 任务实现放在外部文件中，天然不受 CubeMX 覆盖影响
