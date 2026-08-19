# 与框架 v1 的差异 / 移植要点

v1 指 MH3500C 工程里 `Application/comm/` 那套（`comm_frame` / `comm_mailbox` /
`comm_port` / `comm_router` / `comm_sender` / `protocol_dispatch`）。

## 模块对照

| v1 | v2 | 说明 |
|---|---|---|
| `comm_frame` | `comm_frame` | 保留，长度字段 1B→2B，新增 Flags 字节 |
| `comm_mailbox` | **消失** | 被 `comm_framer` 的无锁环形缓冲取代 |
| `comm_port` | **消失** | 降级为 config 里的 `pfSendRaw` 函数指针 + 使用方 5 行 wrapper |
| `comm_router` | 并入 `comm_link` | 它每一行都在操作实例状态，拆开反而要互相暴露内部结构 |
| `comm_sender` | 并入 `comm_link` | 同上 |
| `protocol_dispatch` | `comm_dispatch` | 保留注册槽模型，签名加 `pLink`，长度字段升 `uint16_t` |
| — | `comm_framer` | 新增：字节流解帧状态机 |

## v2 修掉的 v1 缺陷

1. **一个 idle 包必须恰好是一帧**
   v1 的 `CommFrame_Parse` 要求 `len == 9 + DataLen` 且 `buf[0] == 0xAA`，配合
   `HAL_UARTEx_ReceiveToIdle_DMA` 的整包投递，导致：两帧粘在一个 idle 包 → 两帧
   一起丢；一帧被拆成两个 idle 包 → 两段都丢；帧前一个噪声字节 → 整包丢。
   v1 里本该救场的 `CommFrame_FindHeader()` 从未被任何地方调用。
   v2 由 `comm_framer` 的字节流状态机彻底解决，并在坏帧后 resync 而非丢弃整段缓冲。

2. **ACK payload 被丢弃**
   v1 的 `CommSender_TryConsumeAck` 读完 `pu8Data[0]` 就扔掉整个 ACK，
   `OnSuccess(cmd, seq)` 带不回附加数据。v2 的 `CommTxDone_F` 完整带回。

3. **完成回调是全局弱符号**
   v1 一条链路只有一组 `OnSuccess/OnReject/OnFail`，多业务只能 `switch(cmd)`
   互相耦合。v2 改为 per-request 的 `pfDone + pvCtx`，业务之间零耦合。

4. **请求与应答在线上无法区分**
   见 `frame_format.md` 的 Flags 一节。v1 靠"两端永远成对使用 needAck"侥幸避开。

5. **ISR 暂存结构是单例**
   v1 的 `comm_mailbox_post_rx_from_isr` 用一个 `static CommMailboxRxMsg_ST`
   做暂存，两个 UART 的 ISR 存在抢占关系时会互相踩。v2 每实例一个无锁环。

6. **单实例 RAM 偏高**
   v1 约 4.3 KB/实例，其中独立 TX 队列白吃 ~1KB，且一帧 payload 被拷贝 4 次。
   v2 干掉 TX 队列，Post 直接组帧入槽，拷贝降到 2 次。

## 移植一条 v1 链路到 v2

1. 命令表：`ProtocolCmdEntry_ST` → `CommCmdEntry_ST`，长度字段 `uint8_t` → `uint16_t`，
   handler 签名首参加 `CommLink_ST *pstLink`
2. `CommSender_Post(dst, cmd, data, len, needAck)` → 填 `CommTxReq_ST` 后
   `CommLink_Post(pLink, &req)`，并把原来 `CommSender_OnSuccess/OnFail` 里按 cmd
   分支的逻辑拆成各请求自己的 `pfDone`
3. `PROTO_ADDR_SELF` 宏 → `cfg.u8SelfAddr`（每实例独立，不再是全局宏）
4. `comm_mailbox_post_rx_from_isr(port, buf, len)` → `CommLink_FeedFromISR(pLink, buf, len)`
5. `CommSender_Tick()` + `comm_mailbox_wait_rx()` 循环 → 一句 `CommLink_Tick(pLink, now)`
6. `Protocol_ArmResponse / CancelResponse / OnTxComplete` → `cfg.pfRespArm /
   pfRespCancel / pfRespTxDone`，并在 `HAL_UART_TxCpltCallback` 里转发
   `CommLink_OnTxComplete(pLink)`
7. **线上格式不兼容**：帧开销 9 → 11，长度字段 1B → 2B，多了 Flags 字节。
   两端必须同时升级，不能一端 v1 一端 v2

## 平台适配点

| 项 | 默认 | 需要改的场景 |
|---|---|---|
| `COMM_MEM_BARRIER()` | GCC/Clang 编译器屏障 | Keil AC5 定义为 `__schedule_barrier()`；多核或带写合并缓冲的平台定义为真正的 `__DMB()` |
| `DEBUG_COMM_LINK` | 0 | 需要 RTT 日志时在工程 Define 里置 1（依赖 `SEGGER_RTT.h`） |
| `COMM_PAYLOAD_LEN_MAX` | 1024 | 只在两端同时改；小 RAM 芯片应改 `cfg.u16MaxPayloadLen` 而不是这个 |
| `COMM_CMD_TABLE_SLOT_MAX` | 4 | 一条链路上超过 4 个独立业务模块时调大 |

## 大 payload 的时序代价

115200 波特下发满 1034 字节约 **90ms**，这期间链路被占死。若同一条链路上还有
周期帧（心跳、周期数据），会直接顶掉关键 ACK 的时间窗。大包链路要么提高波特率，
要么不要在上面跑周期帧。
