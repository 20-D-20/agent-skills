---
name: comm-stack
description: 把一套可开多实例的 MCU 串口通信协议栈（字节流解帧 + ACK 重传 + 命令表注册，核心不依赖 FreeRTOS）接入到目标工程。当用户要在 STM32/FreeRTOS 工程里新增串口通信链路、搭建主从协议、移植旧的 comm 框架、或需要多串口多业务的通信框架时使用。
---

# comm-stack 部署 Skill

一套自带源码的 MCU 串口通信协议栈。**一个实例 = 一条完整独立的链路**，
开 N 个串口就定义 N 个实例，实例之间零共享。

## 这套框架是什么

```
[ISR] CommLink_FeedFromISR ──► 无锁 SPSC 环 ──┐
                                              │
[task] CommLink_Tick(pLink, nowMs) ─┬─ 解帧（吃粘包/拆包/噪声，坏帧后 resync）
                                    ├─ 地址过滤 → 应答匹配 / 去重 / 命令表分发 → 回帧
                                    ├─ 发出待发槽
                                    └─ ACK 超时重传，重试耗尽回调
[任意 task] CommLink_Post(pLink, &req) ──► 锁内组帧入槽
```

四个源文件，`src/` 下直接拷走：

| 文件 | 职责 | 是否有状态 |
|---|---|---|
| `comm_frame.{c,h}` | 线上格式：组帧 / 解帧 / CRC | 无 |
| `comm_framer.{c,h}` | 环形缓冲 + 字节流解帧状态机 | 每实例 |
| `comm_dispatch.{c,h}` | 命令表注册与查找 | 每实例 |
| `comm_link.{c,h}` | 实例、发送槽、重传、去重、回帧 | 每实例 |

核心不依赖 FreeRTOS：时间由 `CommLink_Tick` 的入参传入，互斥由 config 注入，
ISR 路径是无锁环。因此可以直接 `gcc` 编进 PC 测试程序（`test/build.sh`）。

## 部署流程

### 第 1 步：问清参数

每条链路都要问到这几项，缺一不可：

| 参数 | 说明 | 典型值 |
|---|---|---|
| 链路数量与用途 | 每个串口一条 | 采样枪 / 手操器 / 传感器模块 |
| 短名 | 变量与宏前缀 | `gun` / `disp` |
| UART 句柄与外设 | | `huart3` / `USART3` |
| 本机地址 | 每实例独立，与对端的 Dst 对应 | `0x01U` |
| payload 配额 | 本实例最大 payload，直接决定 RAM | 常规链路 64；大包链路 1024 |
| 并发待确认请求数 | 发送槽数 | 2 ~ 4 |
| 是否多 task 会 Post | 决定要不要注入锁 | |
| 是否需要"ACK 发完再动作" | 如升级后复位，决定要不要 `pfRespTxDone` | |

**协议上限 ≠ 实例配额**：`COMM_PAYLOAD_LEN_MAX`（1024）是线上格式上限，两端必须一致；
`cfg.u16MaxPayloadLen` 是该实例实际分配与接受的上限。支持 1024 是能力，不是每个
实例都要付 1KB×槽数的税。入站帧超实例配额会被丢弃并计入 `u32RxOversize`。

### 第 2 步：拷源码

把 `src/` 下 8 个文件拷进目标工程（建议 `Application/comm/` 或 `Middleware/comm/`），
在 Keil 里手工加入工程并添加头文件路径。**本 skill 不自动改 `.uvprojx`** ——
各工程的 XML 结构不一，脚本改坏了很难排查，且 Keil 会重写格式导致 diff 爆炸。

### 第 3 步：按模板生成每条链路的代码

按 `templates/` 逐个替换占位符生成：

| 模板 | 生成什么 | 放哪 |
|---|---|---|
| `link_instance.c.tmpl` | 静态缓冲 + config + `CommLink_Init` + 任务骨架 | `app_<链路>.c` |
| `port_wrapper.c.tmpl` | `pfSendRaw` 的 HAL 实现 | 同上 |
| `cmd_table.c.tmpl` | 业务命令表 + handler 骨架 + Post 示例 | 同上 |
| `isr_hook.c.tmpl` | UART IDLE+DMA → `CommLink_FeedFromISR` | `usart.c` 的 USER CODE 区 |

占位符：`{{LINK}}` `{{LINK_UPPER}}` `{{SELF_ADDR}}` `{{MAX_PAYLOAD}}` `{{RING_SIZE}}`
`{{SLOT_COUNT}}` `{{RESP_EXTRA}}` `{{HUART}}` `{{USARTX}}`。**生成后全文搜一遍
`{{`，确保一个占位符都没剩下。**

### 第 4 步：跑接线自检清单

见下节，逐条过。

### 第 5 步：编译

Keil 命令行编译目标工程，确认 0 Error 0 Warning。

## 接线自检清单

这套框架接错的地方**全部是静默失败，没有一条会编译报错**。逐条核对：

| # | 检查项 | 验证方式 | 错了会怎样 |
|---|---|---|---|
| 1 | ISR 里调了 `CommLink_FeedFromISR`，且**端口↔实例对应正确** | `grep -n "CommLink_FeedFromISR" usart.c`，核对每个 `USARTx` 分支喂的是不是自己那条链路 | 该链路永远收不到帧，`stats.stFramer.u32RxFrames` 恒为 0 |
| 2 | task 循环里调了 `CommLink_Tick(pLink, osKernelSysTick())` | `grep -rn "CommLink_Tick"` | 帧永远发不出去，超时永不触发 |
| 3 | `CommLink_RegisterCmdTable` 在**收帧循环之前**调用且检查了返回值 | 看 `app_<链路>.c` 里 `for(;;)` 之前 | 所有业务命令被回 `INVALID_CMD`，`u32RxUnknownCmd` 猛涨 |
| 4 | 多 task 会 `CommLink_Post` 时，`pfLock/pfUnlock` 已注入 | `grep -rn "CommLink_Post"` 数一下调用它的 task | 静默数据竞争，两个 task 抢到同一个发送槽 |
| 5 | `u16MaxPayloadLen <= COMM_PAYLOAD_LEN_MAX`，且各缓冲容量 = 配额 + `COMM_FRAME_OVERHEAD` | `CommLink_Init` 会返回 `COMM_ERR_PARAM`，**所以必须检查它的返回值** | Init 失败但没人管，整条链路死掉 |
| 6 | 环大小是 2 的幂 | 同上，`CommFramer_Init` 会拒绝 | 同上 |
| 7 | 两端 `COMM_PAYLOAD_LEN_MAX` / 帧头帧尾 / CRC 多项式完全一致 | 对比两个工程的 `comm_frame.h` | 全帧丢弃，无任何提示 |
| 8 | 每实例 `u8SelfAddr` 不同，且与对端发来的 `Dst` 匹配 | 查 `u32RxNotForMe` 是否在涨 | 帧被当作非本机帧丢弃 |
| 9 | 用了 `pfRespTxDone` 时，`HAL_UART_TxCpltCallback` 里转发了 `CommLink_OnTxComplete` | `grep -n "CommLink_OnTxComplete"` | 升级复位一类流程永远不触发 |
| 10 | Keil AC5 工程重定义了 `COMM_MEM_BARRIER` | `grep -rn "COMM_MEM_BARRIER"` | 编译器可能重排环指针的读写 |

联调时第一件事是打印 `CommLink_GetStats(pLink)`，绝大多数接线错误都能从计数器上一眼看出来。

## handler 的铁律

handler 同步执行在拥有该链路的 task 上下文，**必须快返回**。

- 耗时动作（擦 Flash、读慢传感器、等按键）一律改成"投递到自己的队列 + 立即返回
  `COMM_OK` / `COMM_ERR_BUSY`"，结果通过另一条命令上报
- 长度区间由框架在调用前校验，handler 里不用再判
- 写应答附加数据前确认长度 `<= CommLink_RespPayloadCap(pstLink)`
- 返回负错误码时，框架自动转成协议 status 回给对端，附加数据被丢弃

## 常见需求怎么做

**fire-and-forget（周期数据，不要 ACK）**
命令表里该项 `u8NeedAck = 0`；发起方 `CommTxReq_ST.u8NeedAck = 0`。
**两端必须一致** —— 一端 F&F 另一端要 ACK，会产生无主应答（`u32RxOrphanResp` 上涨）。

**拿到 ACK 里的附加数据**
`CommTxReq_ST.pfDone` 的 `pu8AckData` / `u16AckLen`。仅回调期间有效，要留请自行拷贝。

**ACK 发完再复位（Bootloader 场景）**
`cfg.pfRespArm` 里置武装标志，`cfg.pfRespCancel` 里撤销，`cfg.pfRespTxDone` 里执行复位。
`pfRespTxDone` 运行在中断上下文，必须满足 ISR 约束。

**高吞吐大包（固件传输）**
把 `u8DupCount` 设为 0 关闭去重（1 字节 seq 会快速回绕），由上层块号自己保证幂等。
另外注意 115200 下发满 1034 字节约 90ms，这条链路上别再跑周期帧。

**一条链路上多个独立业务**
各自定义 `static const CommCmdEntry_ST` 表并各自 `CommLink_RegisterCmdTable`，
上限 `COMM_CMD_TABLE_SLOT_MAX`（默认 4）。发送侧靠 per-request 的 `pfDone + pvCtx`
天然隔离，业务之间不需要知道对方存在。

## 验证框架本身

改动 `src/` 下任何文件后都要跑：

```bash
cd skills/comm-stack/test && ./build.sh
```

316 项断言，覆盖粘包 / 拆包 / 逐字节喂 / 前导噪声 / 坏帧后 resync / 环满 /
请求-ACK / 重传 / 超时 / 拒绝 / 去重开关 / 广播 / 地址过滤 / 多业务 / 槽满 /
配额越界 / 误码 / 链路拆包 / 应答不被误当请求。

## 参考

- `references/frame_format.md` —— 字节级线上格式、状态码、Flags 位的存在理由
- `references/porting_notes.md` —— 与旧框架 v1 的差异表、逐项移植步骤、平台适配点
