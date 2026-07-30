---
trigger: glob
globs: **/*.{c,h}
description: 中断处理规范、ISR 核心设计铁律（禁止阻塞/非法 API 调用）、FromISR 标准模板
---

# 中断处理规范

> 编辑 C/H 文件涉及中断处理（ISR/IRQHandler/FromISR）时自动生效

## 核心设计思想

ISR 运行在特权模式，占用硬件栈（MSP），打断一切低优先级任务。在 ISR 中执行耗时操作会导致死锁、栈溢出、实时性丧失。

**铁律：ISR 只做两件事 —— 读寄存器 + 投事件到队列，一切复杂逻辑交给 Task。**

这是本项目所有中断代码的设计出发点。Queue 值拷贝天然隔离 ISR 与 Task 的数据生命周期，枚举驱动的事件结构体让 Task 侧 switch-case 分发清晰可控。

## ISR 禁止事项（黑名单）

| 禁止项 | 原因 |
|--------|------|
| `printf`/`sprintf`/`log_*` | 依赖锁和堆，随机卡死 |
| `malloc`/`free`/`pvPortMalloc`/`vPortFree` | 堆操作非中断安全 |
| `xQueueSend`/`xSemaphoreTake`/`xSemaphoreGive` | 非 FromISR 版本会触发 configASSERT |
| `vTaskDelay`/`vTaskDelayUntil` | ISR 中不可阻塞 |
| `taskENTER_CRITICAL`/`taskEXIT_CRITICAL` | ISR 中必须用 `_FROM_ISR` 版本 |
| Mutex（`xSemaphoreTake` 互斥量） | FreeRTOS 不提供 Mutex 的 FromISR 版本 |
| 大对象栈分配（>64B） | ISR 栈有限，溢出导致 HardFault |

## FromISR 标准模板

```c
/**
 * @brief  XXX 中断服务函数
 * @note   仅投递事件到队列，禁止阻塞操作
 */
void ISR_XXX_Handler(void)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    /* 构造事件（<=16B，栈安全） */
    XxxEvt_ST stEvt;
    stEvt.eType = XXX_EVT_YYY;
    stEvt.u32Data = /* 从外设寄存器读取 */;

    /* 投递到队列 */
    if (xQueueSendFromISR(s_xXxxQueue, &stEvt, &xHigherPriorityTaskWoken) != pdTRUE)
    {
        s_u32XxxQueueFullCnt++;  /* static volatile 计数器，禁止打日志 */
    }

    /* 清除中断标志 */
    __HAL_XXX_CLEAR_FLAG(&hxxx, XXX_FLAG);

    /* 必须在 ISR 末尾调用 */
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}
```

## portYIELD_FROM_ISR 铁律

- 每个调用了 FromISR API 的 ISR **末尾必须**调用 `portYIELD_FROM_ISR(xHigherPriorityTaskWoken)`
- 多次 FromISR 调用传**同一个** `xHigherPriorityTaskWoken` 指针
- `xHigherPriorityTaskWoken` 必须初始化为 `pdFALSE`
- 遗漏此调用会导致高优先级任务无法及时抢占，产生不确定延迟

## 中断优先级规则

本项目配置：`configPRIO_BITS = 4`（16 级），`configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY = 5`

- 需要调用 FreeRTOS API → preemption priority **必须 >= 5**（数值越小优先级越高）
- 不调用 FreeRTOS API → 可用 0~4（不受 RTOS 管理）
- 推荐值：通信外设 6~7，DMA/ADC/Timer 5~6，紧急安全信号 1~3

## 统一 Queue 事件模式

### 事件结构体模板（目标 <=8B）

```c
/* 事件类型枚举 */
typedef enum
{
    XXX_EVT_NONE = 0,
    XXX_EVT_RX_COMPLETE,
    XXX_EVT_TX_COMPLETE,
    XXX_EVT_ERROR,
    XXX_EVT_COUNT
} XxxEvtType_E;

/* 事件结构体（8B） */
typedef struct
{
    XxxEvtType_E eType;    /* 事件类型（4B） */
    uint32_t     u32Data;  /* 附加数据（4B） */
} XxxEvt_ST;
```

### Queue 容量建议

| 事件频率 | 建议深度 | 示例 |
|----------|---------|------|
| 低频（<10Hz） | 4 ~ 8 | 按键、状态变更 |
| 中频（10-100Hz） | 8 ~ 16 | UART 帧完成 |
| 高频（>100Hz） | 16 ~ 32 | ADC 采样完成 |

### Queue Full 处理

- **禁止**在 ISR 中打日志
- 使用 `static volatile uint32_t` 计数器记录丢弃次数
- 在 Task 侧定期检查计数器并输出日志

### 临界区 API 选择

| 上下文 | 进入 | 退出 |
|--------|------|------|
| Task | `taskENTER_CRITICAL()` | `taskEXIT_CRITICAL()` |
| ISR | `taskENTER_CRITICAL_FROM_ISR()` | `taskEXIT_CRITICAL_FROM_ISR(ux)` |

大多数 ISR 不需要临界区——`xQueueSendFromISR` 本身已是中断安全的。仅在 ISR 需要原子访问共享变量时使用。
