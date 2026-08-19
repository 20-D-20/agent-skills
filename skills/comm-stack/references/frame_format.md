# 线上帧格式

两端必须完全一致。任一常量不同都会导致**全帧丢弃且无任何提示** —— 排查时先对
`COMM_PAYLOAD_LEN_MAX`、帧头帧尾、CRC 多项式这三样。

## 布局

```
偏移   0      1     2     3      4     5     6      7      8 .. 7+N   8+N  9+N  10+N
     +------+-----+-----+-----+-------+-----+------+------+---------+-----+-----+------+
     | 0xAA | Dst | Src | Cmd | Flags | Seq | LenH | LenL | Data[N] | CRC | CRC | 0x55 |
     +------+-----+-----+-----+-------+-----+------+------+---------+-----+-----+------+
                                                                       L     H
```

| 字段 | 字节 | 说明 |
|---|---|---|
| Header | 1 | 固定 `0xAA` |
| Dst | 1 | 目标地址；`0xFF` 为广播 |
| Src | 1 | 源地址 |
| Cmd | 1 | 命令码，0x00–0xFF 全段可用 |
| Flags | 1 | bit0 = `COMM_FRAME_FLAG_RESPONSE`，其余保留必须为 0 |
| Seq | 1 | 请求序号，由发起方分配；应答原样回填 |
| Len | 2 | payload 字节数，**大端**，0 – `COMM_PAYLOAD_LEN_MAX`(1024) |
| Data | N | payload |
| CRC | 2 | CRC-16/MODBUS，**小端**（低字节在前） |
| Trailer | 1 | 固定 `0x55` |

总长度 = `N + 11`（`COMM_FRAME_OVERHEAD`）。

## CRC

- 算法：CRC-16/MODBUS，初值 `0xFFFF`，反射多项式 `0xA001`
- 覆盖范围：偏移 `1` 到 `7+N`（Dst 起，Data 末尾止），**不含**帧头、CRC 自身、帧尾
- 标准向量：`"123456789"` → `0x4B37`（`test_frame.c` 已锁死）

## Flags.RESPONSE 位

这一位不是可选装饰，它是框架正确性的一部分。

- `0`（请求帧）：进入去重缓存判定与命令表分发
- `1`（应答帧）：**只**用于匹配本机待确认的请求；匹配不上就静默丢弃，
  绝不进入命令分发，也绝不产生任何回帧

没有这一位，请求与应答在线上无法区分。当一端收到一个自己无法消费的应答时，
会把它当成新请求再回一个应答，对端同样处理 …… 形成无限乒乓，业务 handler 被反复执行。
框架 v1 正是靠"两端永远成对使用 needAck"这个隐含约定侥幸避开的，一旦某条命令
两端的 `NeedAck` 配置不一致就会当场爆炸。回归用例见 `test_link.c` 的
「应答帧绝不会被误当成新请求」。

## 应答 payload 约定

所有应答帧的 payload 第 0 字节固定为协议状态码：

| 状态码 | 值 | 含义 |
|---|---|---|
| `COMM_STATUS_OK` | 0x00 | 成功 |
| `COMM_STATUS_BUSY` | 0x01 | 设备忙 |
| `COMM_STATUS_INVALID_CMD` | 0x02 | 命令表未命中 |
| `COMM_STATUS_INVALID_PARAM` | 0x03 | payload 长度不在表项区间，或 handler 返回参数错 |
| `COMM_STATUS_HW_ERROR` | 0x04 | 硬件故障 |
| `COMM_STATUS_NOT_READY` | 0x05 | 数据未就绪 |
| `COMM_STATUS_DENIED` | 0x06 | 明确拒绝 |

status 之后的字节是 handler 写入的附加数据，会通过 `CommTxDone_F` 的
`pu8AckData` / `u16AckLen` 原样交给发起方。

## 序号与去重

- Seq 为 1 字节，由 `CommLink_Post` 递增分配，256 后回绕
- ACK 匹配条件：`Flags.RESPONSE` 置位 **且** `(SrcAddr, Cmd, Seq)` 与某个
  `WAIT_ACK` 槽完全一致
- 去重缓存按 `(SrcAddr, Cmd, Seq, payload 长度, payload 的 CRC16 摘要)` 判定。
  存摘要而非整个请求体，是为了让 1024 字节配额的实例也能开去重
- 高吞吐大包场景（如固件传输）建议把 `u8DupCount` 设为 0 关闭去重，
  由上层的块号自己保证幂等
