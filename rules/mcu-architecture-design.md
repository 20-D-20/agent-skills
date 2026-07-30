---
trigger: glob
globs: **/*.{c,h}
description: 嵌入式分层架构设计原则、模块解耦策略、接口设计模板
---

# 🏗️ 架构设计

## 分层架构模型（对应项目实际目录）

```
┌─────────────────────────────────────────┐
│        Application Layer               │  ← Application/
│   • FreeRTOS 任务 • 状态机 • 业务逻辑  │     app_xxx.c
├─────────────────────────────────────────┤
│        Service Layer                   │  ← RecStore/ LittleFs/ FATFS/
│   • 记录存储 • 文件系统 • 协议解析     │     可跨项目复用
├─────────────────────────────────────────┤
│        BSP Driver Layer                │  ← Drivers/BSP/
│   • 器件驱动 • 传感器 • 显示 • 通信    │     依赖 HAL，不依赖 App
├─────────────────────────────────────────┤
│        HAL Layer                       │  ← Drivers/STM32F4xx_HAL_Driver/
│   • ST HAL 库 • CubeMX 生成代码       │     Core/
├─────────────────────────────────────────┤
│        Hardware                        │
└─────────────────────────────────────────┘
```

### 层间依赖铁律

- **只允许上层调用下层**，禁止下层 `#include` 上层头文件
- **下层通知上层**只能通过回调函数或 FreeRTOS Queue，不能直接调用
- **同层模块间**尽量不互相调用；确需通信时通过 Queue 或共享服务层接口
- BSP 驱动只依赖 HAL，不依赖 Application 或 Service

## 解耦三维度

### 1. 纵向分层 — 隔离硬件依赖

| 原则 | 做法 |
|------|------|
| 依赖方向 | 上层依赖下层，下层不知道上层存在 |
| 单一职责 | 每层只负责对应抽象级别 |
| 接口稳定 | 层间接口保持稳定，内部实现可变 |

**反面案例** — Application 直接操作寄存器：
```c
/* 禁止：应用层直接碰硬件 */
void app_sensor_task(void const *argument)
{
    SPI1->CR1 |= SPI_CR1_SPE;          /* 直接操作寄存器 */
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_SET); /* 直接调 HAL */
}
```

**正确做法** — 通过 BSP 驱动间接操作：
```c
/* Application 只调用 BSP 接口 */
void app_sensor_task(void const *argument)
{
    drv_sht3x_init();
    for (;;)
    {
        drv_sht3x_read(&fTemp, &fHum);
        osDelay(1000);
    }
}
```

### 2. 横向解耦 — 模块间松耦合

通过**函数指针**和**回调**实现模块间解耦，但要分级使用，不是所有模块都需要。

#### 分级策略（关键，防止过度设计）

| 级别 | 适用场景 | 方式 | 示例 |
|------|---------|------|------|
| **L0 直接调用** | 单一实现、不会替换 | 直接 `#include` + 调用 | LCD 驱动、固定传感器 |
| **L1 回调通知** | 下层需要通知上层 | 注册回调函数 | UART 接收完成通知 App |
| **L2 接口抽象** | 同类器件可替换、需要可测试性 | 函数指针结构体 | 通信接口（UART/蓝牙可切换） |

**L0：绝大多数 BSP 驱动用这个就够了。** 不要为了"架构正确"给每个驱动都套函数指针。

**L1 回调通知模板：**
```c
/* drv_xxx.h */
typedef void (*drv_xxx_callback_t)(uint32_t u32Event, void *pData);

void drv_xxx_init(drv_xxx_callback_t pfCallback);
```

**L2 接口抽象模板（仅在确实需要替换实现时使用）：**
```c
/* 通信接口抽象 */
typedef struct
{
    int32_t (*init)(void *pCfg);
    int32_t (*send)(const uint8_t *pData, size_t u32Len, uint32_t u32Timeout);
    int32_t (*recv)(uint8_t *pBuf, size_t u32Len, uint32_t u32Timeout);
    void *pPrivate;  /* 实现侧私有数据 */
} CommInterface_ST;

/* 上层使用接口，不关心底层实现 */
int32_t protocol_send(CommInterface_ST *pComm, const void *pData, size_t u32Len)
{
    return pComm->send((const uint8_t *)pData, u32Len, 1000);
}
```

### 3. 时序解耦 — 异步通信

**本项目使用 FreeRTOS Queue 实现时序解耦，不需要额外的事件总线。**

模式：ISR/模块 → Queue → 任务处理

```c
/* 事件结构体（<=8B，栈安全） */
typedef struct
{
    XxxEvtType_E eType;    /* 事件类型（4B） */
    uint32_t     u32Data;  /* 附加数据（4B） */
} XxxEvt_ST;

/* ISR 投递事件 */
xQueueSendFromISR(s_xQueue, &stEvt, &xHigherPriorityTaskWoken);

/* 任务侧阻塞等待 */
xQueueReceive(s_xQueue, &stEvt, portMAX_DELAY);
```

> 详细的 ISR 规范见 `rules/interrupt-handling.md`

## BSP 驱动接口规范

### 基本结构（适用于大多数驱动）

```c
/* drv_xxx.h */
#ifndef __DRV_XXX_H__
#define __DRV_XXX_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/**
 * @brief  初始化 XXX 器件
 * @retval 0:成功, <0:错误码
 */
int32_t drv_xxx_init(void);

/**
 * @brief  读取数据
 * @param  [out] pData: 输出缓冲区
 * @retval 0:成功, <0:错误码
 */
int32_t drv_xxx_read(void *pData);

#ifdef __cplusplus
}
#endif

#endif /* __DRV_XXX_H__ */
```

### 需要回调的驱动（通信类、异步类）

```c
/* drv_xxx.h */
typedef void (*drv_xxx_rx_cb_t)(uint8_t *pData, uint16_t u16Len);

int32_t drv_xxx_init(drv_xxx_rx_cb_t pfRxCallback);
int32_t drv_xxx_send(const uint8_t *pData, uint16_t u16Len);
```

### 驱动设计检查清单

- [ ] 驱动只依赖 HAL 层，不 `#include` Application 头文件
- [ ] 通知上层用回调或 Queue，不直接调用上层函数
- [ ] 配置参数通过 `#define` 或 config 结构体传入，不硬编码魔术数字
- [ ] 返回值使用统一错误码（0 成功，负数失败）

## 过度设计的识别与避免

### 以下情况 **不要** 使用函数指针/接口抽象：

| 信号 | 说明 |
|------|------|
| 只有一个实现 | LCD 驱动只用一款屏，不需要抽象 |
| 不会被替换 | 板载固定传感器，不套接口 |
| 模块内部使用 | 内部辅助函数不需要导出为接口 |
| 为了"将来可能" | YAGNI — 现在不需要就不做 |

### 以下情况 **应该** 使用函数指针/接口抽象：

| 信号 | 说明 |
|------|------|
| 已有两个以上实现 | UART 和蓝牙都走通信接口 |
| 需要运行时切换 | 不同模式使用不同通信通道 |
| 下层通知上层 | ISR 需要告知 Task 数据就绪 |
| 测试需要 Mock | 需要脱离硬件进行单元测试 |

### 代码量检查

如果一个"接口抽象层"的胶水代码比实际业务逻辑还多，说明过度设计了。回退到直接调用。
