---
trigger: glob
globs: **/*.{c,h}
paths:
  - "**/*.{c,h}"
description: 嵌入式分层架构设计原则、模块解耦策略、跨层接口函数契约（参数/const/统一错误码 ErrCode_E/入参校验）、接口设计模板
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

> 详细的 ISR 规范见 `rules/mcu-interrupt-handling.md`

## 跨层接口的函数契约

> 本节只约束**跨层调用的对外接口**（在 `.h` 中声明的函数）。契约 = 调用方看得见并可依赖的全部承诺：参数、`const`、返回值、错误码。
> 函数内部写法（长度、单一职责、命名、提前返回）见 `rules/mcu-code-style.md`。
> 栈空间、中断安全、可重入约束见 `rules/mcu-code-style.md` 与 `rules/mcu-interrupt-handling.md`。

### 1. 依赖显式化

函数需要的信息**必须通过参数传入**，禁止通过全局变量偷渡。

**反面案例** — 依赖藏在全局变量里，签名看不出它要什么：
```c
uint8_t  g_u8RxBuf[256];
uint16_t g_u16RxLen;

/* 禁止：与两个全局变量绑死，换一路串口就没法复用 */
void Protocol_Parse(void)
{
    /* 偷偷读 g_u8RxBuf / g_u16RxLen */
}
```

正确签名把依赖摆出来：`int32_t protocol_parse(const uint8_t *pBuf, uint16_t u16Len);`

### 2. 参数设计

| 参数个数 | 规则 |
|---------|------|
| ≤4 | 直接列参数，无需理由 |
| 5 | 仅当参数属**同一语义组**（同一件事的不同侧面）时允许，否则打包 |
| >5 | 禁止，必须打包为 `XxxCfg_ST` 传指针 |

- `drv_i2c_write(u8Addr, u8Reg, pData, u16Len)` 属同一语义组，不拆
- 打包收益：新增字段只改结构体，函数签名与所有调用方代码不动
- 简单类型（`uint8_t` / `int32_t` / `float`）传值；结构体、数组、需修改原始数据传指针（MCU 栈空间宝贵，避免整块拷贝）
- **只读的指针参数必须加 `const`** — 这是对调用方的承诺："我不改你的数据"

### 3. 统一错误码

全工程共用一套错误码，**禁止模块自定义**。定义放在不依赖任何层的公共头文件（建议 `Common/Inc/err_code.h`）：

```c
typedef enum
{
    ERR_OK      =  0,   /* 成功 */
    ERR_PARAM   = -1,   /* 参数非法（含空指针） */
    ERR_TIMEOUT = -2,   /* 超时 */
    ERR_IO      = -3,   /* 硬件通信失败（I2C NACK / SPI 无响应） */
    ERR_CRC     = -4,   /* 校验失败 */
    ERR_BUSY    = -5,   /* 资源被占用 */
    ERR_NOMEM   = -6,   /* 内存 / 队列不足 */
    ERR_STATE   = -7,   /* 当前状态不允许该操作 */
} ErrCode_E;
```

- 该头文件**只放类型定义**：禁止 `#include` 其它项目头文件、禁止声明函数，防止长成上帝头文件
- 需要新的错误语义时扩这个枚举，不要在模块里另起一套
- **返回类型统一用 `int32_t` 而非 `ErrCode_E`**：部分函数需要「负数 = 错误码，非负 = 有效结果（如读到的字节数）」

### 4. 返回值的有无

**必须有返回值**（失败是常态，上层需据此重试 / 降级 / 告警）：

| 场景 | 典型失败 |
|------|---------|
| 硬件操作 | I2C NACK、Flash 擦写超时、ADC 未就绪 |
| 数据解析与协议处理 | CRC 错、帧长异常、命令字不支持 |
| 资源获取 | 内存分配、队列创建、互斥锁获取 |

**允许 `void`**：必定成功的简单操作（拉 GPIO、喂狗）／FreeRTOS 任务函数（见 `rules/mcu-freertos-task.md`）／ISR handler／HAL 与框架规定的回调签名／纯通知触发类（软复位）。

判据：**这个函数可能失败吗？失败了调用方需要知道吗？** 两问皆否才用 `void`。

### 5. 错误码 + 指针出参

需要同时返回「状态」和「数据」时，返回值只表状态，数据走出参：

```c
/* 禁止：返回值被数据占满，没地方报错 */
uint16_t drv_adc_read(uint8_t u8Ch);

/* 正确 */
int32_t drv_adc_read(uint8_t u8Ch, uint16_t *pu16Value);
```

### 6. 入参校验的边界

```c
int32_t drv_xxx_read(XxxData_ST *pData)
{
    if (NULL == pData)
    {
        return ERR_PARAM;   /* 静默返回，日志由调用方在检查返回值处打 */
    }
    /* ... */
}
```

- **对外接口（在 `.h` 中声明）必须校验**指针入参与取值范围
- **模块内部 `static` 函数可省略校验**，避免每层重复检查同一个指针
- **校验失败静默返回错误码，被调函数内部不打日志**：调用方才知道自己身处 ISR 还是任务、这次失败是否致命。被调方内部打日志既可能违反 `rules/mcu-debug-macro.md` 的 ISR 禁令，也会在轮询场景刷屏。日志打在调用方检查返回值处，见 `rules/mcu-easylogger.md`

### 7. 副作用显式化

函数除返回值外还修改全局状态（错误计数器、状态机、共享缓冲区）时，**必须让调用方看得见**：

- 优先在函数名中体现；若函数名需要用"和"字才能说清，说明该拆（见 `rules/mcu-code-style.md` 单一职责）
- 无法避免时，在 Doxygen 注释中用 `@note` 写明修改了什么
- 隐式修改全局变量在 RTOS / 中断环境下会直接变成竞态

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

### 接口设计检查清单

- [ ] 驱动只依赖 HAL 层，不 `#include` Application 头文件
- [ ] 通知上层用回调或 Queue，不直接调用上层函数
- [ ] 配置参数通过 `#define` 或 config 结构体传入，不硬编码魔术数字
- [ ] 函数依赖全部通过参数显式传入，未通过全局变量偷渡
- [ ] 参数个数 ≤4；为 5 个时属同一语义组；只读指针参数已加 `const`
- [ ] 对外接口返回值使用 `ErrCode_E`（`ERR_OK` 为 0，失败为负），返回类型为 `int32_t`
- [ ] 对外接口已校验指针入参与取值范围，失败返回 `ERR_PARAM` 且不在内部打日志
- [ ] 可能失败的操作有返回值；调用方检查了返回值
- [ ] 需要同时返回状态和数据时，用「错误码返回 + 指针出参」
- [ ] 修改全局状态的函数，已在函数名或 `@note` 中明示

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
