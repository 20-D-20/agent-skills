/**
 * @file    comm_link.c
 * @brief   通信链路实例实现
 *
 * 数据流：
 *
 *   [ISR] CommLink_FeedFromISR ──► 无锁环形缓冲 ──┐
 *                                                 │
 *   [task] CommLink_Tick ─┬─ RxPump ──► CommFramer_Next ──► 地址过滤
 *                         │                    ├─ Flags.RESPONSE ─► 匹配待确认请求，否则丢弃
 *                         │                    └─ 请求帧 ─┬─► 去重命中，重发历史应答
 *                         │                               └─► 命令表分发 → 就地组应答 → 发出
 *                         ├─ TxPump  ──► 发出 READY 槽（Post 时已组好帧）
 *                         └─ Timeout ──► WAIT_ACK 槽超时重传 / 重试耗尽回调
 *
 *   [任意 task] CommLink_Post ──► 锁内：占槽 + 分配 seq + 组帧 ──► 槽置 READY
 */

#include "comm_link.h"
#include <string.h>
#include <stddef.h>

#ifndef DEBUG_COMM_LINK
#define DEBUG_COMM_LINK     0
#endif

#if DEBUG_COMM_LINK
#include "SEGGER_RTT.h"
#define LINK_DEBUG(pL, fmt, ...)                                          \
    SEGGER_RTT_printf(0, "[%s] " fmt,                                     \
                      ((pL)->stCfg.pcName != NULL) ? (pL)->stCfg.pcName : "link", \
                      ##__VA_ARGS__)
#else
#define LINK_DEBUG(pL, fmt, ...)    do { (void)(pL); } while (0)
#endif

/* 应答帧内 status 字节的偏移：Header(1) + Dst Src Cmd Flags Seq LenH LenL(7) */
#define COMM_RESP_STATUS_OFFSET     (COMM_FRAME_HEADER_BYTES + 1U)
/* 应答帧内 handler 可写附加数据的起始偏移 */
#define COMM_RESP_EXTRA_OFFSET      (COMM_RESP_STATUS_OFFSET + 1U)

/* 无 ACK 语义时回调里携带的占位状态码 */
#define COMM_STATUS_NONE            0xFFU

/* ============================================================================
 * 内部工具
 * ============================================================================ */

/**
 * @brief  进入临界区（未注入锁时为空操作）
 * @param  [in] pstLink 链路实例
 */
static void CommLink_Lock(CommLink_ST *pstLink)
{
    if (pstLink->stCfg.pfLock != NULL)
    {
        pstLink->stCfg.pfLock(pstLink->stCfg.pvLockCtx);
    }
}

/**
 * @brief  退出临界区（未注入锁时为空操作）
 * @param  [in] pstLink 链路实例
 */
static void CommLink_Unlock(CommLink_ST *pstLink)
{
    if (pstLink->stCfg.pfUnlock != NULL)
    {
        pstLink->stCfg.pfUnlock(pstLink->stCfg.pvLockCtx);
    }
}

/**
 * @brief  判断时间是否已到（按 32 位无符号回绕安全的方式比较）
 * @param  [in] u32NowMs      当前时间
 * @param  [in] u32DeadlineMs 截止时间
 * @retval 1: 已到或已过, 0: 未到
 */
static uint8_t CommLink_Expired(uint32_t u32NowMs, uint32_t u32DeadlineMs)
{
    return ((int32_t)(u32NowMs - u32DeadlineMs) >= 0) ? 1U : 0U;
}

/**
 * @brief  通过底层通道发出一段原始字节，并按需触发响应生命周期钩子
 * @param  [in] pstLink  链路实例
 * @param  [in] pu8Data  数据
 * @param  [in] u16Len   长度
 * @param  [in] u8Cmd    对应命令码（供钩子使用）
 * @param  [in] u8IsResp 1: 这是入站请求的应答（触发 Arm/Cancel 钩子）, 0: 主动发帧
 * @retval COMM_OK 或底层错误码
 */
static int32_t CommLink_RawSend(CommLink_ST *pstLink, const uint8_t *pu8Data,
                                uint16_t u16Len, uint8_t u8Cmd, uint8_t u8IsResp)
{
    int32_t i32Ret;

    if (u8IsResp && pstLink->stCfg.pfRespArm != NULL)
    {
        pstLink->stCfg.pfRespArm(pstLink, u8Cmd);
    }

    i32Ret = pstLink->stCfg.pfSendRaw(pstLink->stCfg.pvIo, pu8Data, u16Len);

    if (i32Ret != COMM_OK)
    {
        pstLink->stStats.u32TxSendFail++;
        if (u8IsResp && pstLink->stCfg.pfRespCancel != NULL)
        {
            pstLink->stCfg.pfRespCancel(pstLink, u8Cmd);
        }
    }

    return i32Ret;
}

/* ============================================================================
 * 应答组帧（就地构建，零拷贝）
 *
 *  偏移: 0    1    2    3     4      5    6     7     8        9 ..
 *       AA   Dst  Src  Cmd  Flags  Seq  LenH  LenL  Status   handler 写入的附加数据
 *
 *  Flags 恒为 COMM_FRAME_FLAG_RESPONSE，对端据此判定这是应答而非新请求。
 * ============================================================================ */

/**
 * @brief  在应答缓冲里就地补齐帧头与尾部
 * @param  [in]  pstLink   链路实例
 * @param  [in]  pstReq    对应的请求帧
 * @param  [in]  u8Status  协议状态码
 * @param  [in]  u16Extra  handler 已写入 COMM_RESP_EXTRA_OFFSET 起的附加数据长度
 * @param  [out] pu16Total 输出应答帧总长度
 * @retval COMM_OK / COMM_ERR_BUFFER_FULL
 */
static int32_t CommLink_BuildResponse(CommLink_ST *pstLink, const CommFrameInfo_ST *pstReq,
                                      uint8_t u8Status, uint16_t u16Extra,
                                      uint16_t *pu16Total)
{
    uint8_t *pu8Buf = pstLink->stCfg.pu8RespFrame;
    uint16_t u16PayloadLen;
    uint16_t u16Total;
    uint16_t u16Crc;

    if (u16Extra > CommLink_RespPayloadCap(pstLink))
    {
        return COMM_ERR_BUFFER_FULL;
    }

    u16PayloadLen = (uint16_t)(u16Extra + 1U);           /* status 占 1 字节 */
    u16Total      = (uint16_t)(u16PayloadLen + COMM_FRAME_OVERHEAD);

    pu8Buf[0] = (uint8_t)COMM_FRAME_HEADER;
    pu8Buf[1] = pstReq->u8SrcAddr;                       /* 回给请求方 */
    pu8Buf[2] = pstLink->stCfg.u8SelfAddr;
    pu8Buf[3] = pstReq->u8Cmd;
    pu8Buf[4] = (uint8_t)COMM_FRAME_FLAG_RESPONSE;       /* 关键：标记为应答 */
    pu8Buf[5] = pstReq->u8Seq;
    pu8Buf[6] = (uint8_t)(u16PayloadLen >> 8);
    pu8Buf[7] = (uint8_t)(u16PayloadLen & 0xFFU);
    pu8Buf[COMM_RESP_STATUS_OFFSET] = u8Status;

    u16Crc = CommFrame_CalcCRC16(&pu8Buf[1], (uint16_t)(COMM_FRAME_HEADER_BYTES + u16PayloadLen));
    pu8Buf[COMM_RESP_EXTRA_OFFSET + u16Extra]      = (uint8_t)(u16Crc & 0xFFU);
    pu8Buf[COMM_RESP_EXTRA_OFFSET + u16Extra + 1U] = (uint8_t)((u16Crc >> 8) & 0xFFU);
    pu8Buf[COMM_RESP_EXTRA_OFFSET + u16Extra + 2U] = (uint8_t)COMM_FRAME_TRAILER;

    *pu16Total = u16Total;
    return COMM_OK;
}

/* ============================================================================
 * 去重缓存
 * ============================================================================ */

/**
 * @brief  计算请求 payload 的摘要
 * @param  [in] pstReq 请求帧
 * @return CRC16 摘要；无 payload 时返回 0
 */
static uint16_t CommLink_ReqDigest(const CommFrameInfo_ST *pstReq)
{
    if (pstReq->u16DataLen == 0U || pstReq->pu8Data == NULL)
    {
        return 0U;
    }
    return CommFrame_CalcCRC16(pstReq->pu8Data, pstReq->u16DataLen);
}

/**
 * @brief  在去重缓存里查找同一请求，命中则原样重发历史应答
 * @param  [in] pstLink 链路实例
 * @param  [in] pstReq  当前请求帧
 * @retval 1: 命中并已重发, 0: 未命中
 */
static uint8_t CommLink_DupReply(CommLink_ST *pstLink, const CommFrameInfo_ST *pstReq)
{
    uint16_t u16Digest;
    uint8_t  i;

    if (pstLink->stCfg.u8DupCount == 0U)
    {
        return 0U;
    }

    u16Digest = CommLink_ReqDigest(pstReq);

    for (i = 0U; i < pstLink->stCfg.u8DupCount; i++)
    {
        CommDupEntry_ST *pstEntry = &pstLink->stCfg.pstDup[i];

        if (pstEntry->u8Valid == 0U)
        {
            continue;
        }
        if (CommLink_Expired(pstLink->u32NowMs,
                             pstEntry->u32StoredMs + pstLink->stCfg.u32DupTtlMs))
        {
            pstEntry->u8Valid = 0U;
            continue;
        }
        if (pstEntry->u8SrcAddr == pstReq->u8SrcAddr &&
            pstEntry->u8Cmd     == pstReq->u8Cmd &&
            pstEntry->u8Seq     == pstReq->u8Seq &&
            pstEntry->u16ReqLen == pstReq->u16DataLen &&
            pstEntry->u16ReqCrc == u16Digest)
        {
            LINK_DEBUG(pstLink, "dup hit cmd=0x%02X seq=%u\r\n",
                       pstReq->u8Cmd, pstReq->u8Seq);
            pstLink->stStats.u32DupHit++;
            (void)CommLink_RawSend(pstLink, pstEntry->pu8Resp, pstEntry->u16RespLen,
                                   pstReq->u8Cmd, 1U);
            return 1U;
        }
    }

    return 0U;
}

/**
 * @brief  把请求与其应答存入去重缓存（应答超过单项容量时不缓存）
 * @param  [in] pstLink  链路实例
 * @param  [in] pstReq   请求帧
 * @param  [in] pu8Resp  应答帧数据
 * @param  [in] u16Len   应答帧长度
 */
static void CommLink_DupStore(CommLink_ST *pstLink, const CommFrameInfo_ST *pstReq,
                              const uint8_t *pu8Resp, uint16_t u16Len)
{
    CommDupEntry_ST *pstEntry;

    if (pstLink->stCfg.u8DupCount == 0U || u16Len == 0U ||
        u16Len > pstLink->stCfg.u16DupRespCap)
    {
        return;
    }

    pstEntry = &pstLink->stCfg.pstDup[pstLink->u8DupNext];
    pstLink->u8DupNext = (uint8_t)((pstLink->u8DupNext + 1U) % pstLink->stCfg.u8DupCount);

    pstEntry->u8Valid     = 1U;
    pstEntry->u8SrcAddr   = pstReq->u8SrcAddr;
    pstEntry->u8Cmd       = pstReq->u8Cmd;
    pstEntry->u8Seq       = pstReq->u8Seq;
    pstEntry->u16ReqLen   = pstReq->u16DataLen;
    pstEntry->u16ReqCrc   = CommLink_ReqDigest(pstReq);
    pstEntry->u32StoredMs = pstLink->u32NowMs;
    pstEntry->u16RespLen  = u16Len;
    memcpy(pstEntry->pu8Resp, pu8Resp, u16Len);
}

/* ============================================================================
 * 入站帧处理
 * ============================================================================ */

/**
 * @brief  尝试把入站帧消费为本机主动发帧的 ACK
 * @param  [in] pstLink 链路实例
 * @param  [in] pstFrm  入站帧
 * @retval 1: 已消费, 0: 不是任何待确认请求的 ACK
 */
static uint8_t CommLink_TryConsumeAck(CommLink_ST *pstLink, const CommFrameInfo_ST *pstFrm)
{
    uint8_t i;

    if (pstFrm->u16DataLen < 1U || pstFrm->pu8Data == NULL)
    {
        return 0U;
    }

    for (i = 0U; i < pstLink->stCfg.u8SlotCount; i++)
    {
        CommTxSlot_ST *pstSlot = &pstLink->stCfg.pstSlots[i];
        CommTxDone_F   pfDone;
        void          *pvCtx;
        uint8_t        u8Status;

        if (pstSlot->u8State != (uint8_t)COMM_SLOT_WAIT_ACK ||
            pstSlot->u8Seq     != pstFrm->u8Seq ||
            pstSlot->u8Cmd     != pstFrm->u8Cmd ||
            pstSlot->u8DstAddr != pstFrm->u8SrcAddr)
        {
            continue;
        }

        /* 先取出回调信息再释放槽位：释放后其他 task 可能立刻抢占该槽 */
        pfDone   = pstSlot->pfDone;
        pvCtx    = pstSlot->pvCtx;
        u8Status = pstFrm->pu8Data[0];

        pstSlot->pfDone  = NULL;
        pstSlot->pvCtx   = NULL;
        pstSlot->u8State = (uint8_t)COMM_SLOT_FREE;

        pstLink->stStats.u32AckConsumed++;
        if (u8Status != (uint8_t)COMM_STATUS_OK)
        {
            pstLink->stStats.u32TxRejected++;
        }

        LINK_DEBUG(pstLink, "ack cmd=0x%02X seq=%u status=0x%02X\r\n",
                   pstFrm->u8Cmd, pstFrm->u8Seq, u8Status);

        if (pfDone != NULL)
        {
            pfDone(pstLink, pvCtx,
                   (u8Status == (uint8_t)COMM_STATUS_OK) ? COMM_TX_OK : COMM_TX_REJECTED,
                   u8Status,
                   (pstFrm->u16DataLen > 1U) ? &pstFrm->pu8Data[1] : NULL,
                   (uint16_t)(pstFrm->u16DataLen - 1U));
        }
        return 1U;
    }

    return 0U;
}

/**
 * @brief  处理一个已解析的入站帧
 * @param  [in] pstLink 链路实例
 * @param  [in] pstFrm  入站帧
 */
static void CommLink_HandleFrame(CommLink_ST *pstLink, const CommFrameInfo_ST *pstFrm)
{
    const CommCmdEntry_ST *pstEntry;
    uint8_t   u8IsBroadcast;
    uint8_t   u8Status;
    uint16_t  u16Extra = 0U;
    uint16_t  u16RespLen = 0U;
    int32_t   i32Ret;

    u8IsBroadcast = (pstFrm->u8DstAddr == (uint8_t)COMM_ADDR_BROADCAST) ? 1U : 0U;

    /* --- 地址过滤 --- */
    if (u8IsBroadcast)
    {
        if (pstLink->stCfg.u8AcceptBroadcast == 0U)
        {
            pstLink->stStats.u32RxBroadcastDropped++;
            return;
        }
    }
    else if (pstFrm->u8DstAddr != pstLink->stCfg.u8SelfAddr)
    {
        pstLink->stStats.u32RxNotForMe++;
        return;
    }
    else
    {
        /* 本机单播帧，继续处理 */
    }

    /* --- 应答帧只用于匹配待确认请求，绝不进入命令分发 --- */
    if ((pstFrm->u8Flags & (uint8_t)COMM_FRAME_FLAG_RESPONSE) != 0U)
    {
        if (u8IsBroadcast || CommLink_TryConsumeAck(pstLink, pstFrm) == 0U)
        {
            /* 迟到的应答（本机已超时放弃）或不属于本机的应答，直接丢弃。
             * 若这里改为回一个错误帧，两端会互相把对方的应答当成新请求，
             * 形成无限乒乓 —— 这正是 Flags 位存在的理由。 */
            pstLink->stStats.u32RxOrphanResp++;
        }
        return;
    }

    /* --- 去重：ACK 丢失时对端会以相同 seq 重发，命中则只重发历史应答 --- */
    if (!u8IsBroadcast && CommLink_DupReply(pstLink, pstFrm))
    {
        return;
    }

    /* --- 命令表分发 --- */
    pstEntry = CommDispatch_Find(&pstLink->stDispatch, pstFrm->u8Cmd);
    if (pstEntry == NULL)
    {
        pstLink->stStats.u32RxUnknownCmd++;
        LINK_DEBUG(pstLink, "unknown cmd=0x%02X\r\n", pstFrm->u8Cmd);
        if (u8IsBroadcast)
        {
            return;
        }
        u8Status = (uint8_t)COMM_STATUS_INVALID_CMD;
    }
    else if (pstFrm->u16DataLen < pstEntry->u16MinLen ||
             pstFrm->u16DataLen > pstEntry->u16MaxLen)
    {
        pstLink->stStats.u32RxBadLen++;
        LINK_DEBUG(pstLink, "bad len cmd=0x%02X len=%u range=%u-%u\r\n",
                   pstFrm->u8Cmd, pstFrm->u16DataLen,
                   pstEntry->u16MinLen, pstEntry->u16MaxLen);
        if (u8IsBroadcast || pstEntry->u8NeedAck == 0U)
        {
            return;
        }
        u8Status = (uint8_t)COMM_STATUS_INVALID_PARAM;
    }
    else
    {
        i32Ret = pstEntry->pfHandler(pstLink, pstFrm,
                                     &pstLink->stCfg.pu8RespFrame[COMM_RESP_EXTRA_OFFSET],
                                     &u16Extra);
        if (u8IsBroadcast || pstEntry->u8NeedAck == 0U)
        {
            return;   /* fire-and-forget 与广播一律不回帧 */
        }
        if (i32Ret != COMM_OK)
        {
            u16Extra = 0U;
            u8Status = CommDispatch_ErrorToStatus(i32Ret);
        }
        else
        {
            u8Status = (uint8_t)COMM_STATUS_OK;
        }
    }

    /* --- 组应答并发出 --- */
    if (CommLink_BuildResponse(pstLink, pstFrm, u8Status, u16Extra, &u16RespLen) != COMM_OK)
    {
        LINK_DEBUG(pstLink, "resp too large cmd=0x%02X extra=%u\r\n",
                   pstFrm->u8Cmd, u16Extra);
        return;
    }

    CommLink_DupStore(pstLink, pstFrm, pstLink->stCfg.pu8RespFrame, u16RespLen);
    (void)CommLink_RawSend(pstLink, pstLink->stCfg.pu8RespFrame, u16RespLen,
                           pstFrm->u8Cmd, 1U);
}

/* ============================================================================
 * Tick 的三个阶段
 * ============================================================================ */

/**
 * @brief  排空接收：把环里所有完整帧解出来逐个处理
 * @param  [in] pstLink 链路实例
 */
static void CommLink_RxPump(CommLink_ST *pstLink)
{
    CommFrameInfo_ST stFrm;

    while (CommFramer_Next(&pstLink->stFramer, &stFrm) == COMM_OK)
    {
        CommLink_HandleFrame(pstLink, &stFrm);
    }
}

/**
 * @brief  发出所有 READY 槽
 * @param  [in] pstLink 链路实例
 */
static void CommLink_TxPump(CommLink_ST *pstLink)
{
    uint8_t i;

    for (i = 0U; i < pstLink->stCfg.u8SlotCount; i++)
    {
        CommTxSlot_ST *pstSlot = &pstLink->stCfg.pstSlots[i];
        CommTxDone_F   pfDone;
        void          *pvCtx;

        if (pstSlot->u8State != (uint8_t)COMM_SLOT_READY)
        {
            continue;
        }

        /* Post 无法取得时间戳，首次被 Tick 看到时才给它设定"必须发出去"的窗口 */
        if (pstSlot->u8Armed == 0U)
        {
            pstSlot->u32DeadlineMs = pstLink->u32NowMs + pstLink->stCfg.u16AckTimeoutMs;
            pstSlot->u8Armed = 1U;
        }

        if (CommLink_RawSend(pstLink, pstSlot->pu8Frame, pstSlot->u16FrameLen,
                             pstSlot->u8Cmd, 0U) == COMM_OK)
        {
            if (pstSlot->u8NeedAck != 0U)
            {
                pstSlot->u8State       = (uint8_t)COMM_SLOT_WAIT_ACK;
                pstSlot->u8RetryCnt    = 0U;
                pstSlot->u32DeadlineMs = pstLink->u32NowMs + pstLink->stCfg.u16AckTimeoutMs;
            }
            else
            {
                pfDone = pstSlot->pfDone;
                pvCtx  = pstSlot->pvCtx;
                pstSlot->pfDone  = NULL;
                pstSlot->pvCtx   = NULL;
                pstSlot->u8State = (uint8_t)COMM_SLOT_FREE;
                if (pfDone != NULL)
                {
                    pfDone(pstLink, pvCtx, COMM_TX_OK, COMM_STATUS_NONE, NULL, 0U);
                }
            }
            continue;
        }

        /* 发送通道忙：保持 READY 下次再试，直到窗口耗尽 */
        if (CommLink_Expired(pstLink->u32NowMs, pstSlot->u32DeadlineMs))
        {
            pfDone = pstSlot->pfDone;
            pvCtx  = pstSlot->pvCtx;
            pstSlot->pfDone  = NULL;
            pstSlot->pvCtx   = NULL;
            pstSlot->u8State = (uint8_t)COMM_SLOT_FREE;
            LINK_DEBUG(pstLink, "send give up cmd=0x%02X\r\n", pstSlot->u8Cmd);
            if (pfDone != NULL)
            {
                pfDone(pstLink, pvCtx, COMM_TX_SEND_FAIL, COMM_STATUS_NONE, NULL, 0U);
            }
        }
    }
}

/**
 * @brief  检查 WAIT_ACK 槽的超时，触发重传或宣告失败
 * @param  [in] pstLink 链路实例
 */
static void CommLink_CheckTimeouts(CommLink_ST *pstLink)
{
    uint8_t i;

    for (i = 0U; i < pstLink->stCfg.u8SlotCount; i++)
    {
        CommTxSlot_ST *pstSlot = &pstLink->stCfg.pstSlots[i];
        CommTxDone_F   pfDone;
        void          *pvCtx;

        if (pstSlot->u8State != (uint8_t)COMM_SLOT_WAIT_ACK ||
            CommLink_Expired(pstLink->u32NowMs, pstSlot->u32DeadlineMs) == 0U)
        {
            continue;
        }

        if (pstSlot->u8RetryCnt < pstLink->stCfg.u8MaxRetry)
        {
            pstSlot->u8RetryCnt++;
            pstLink->stStats.u32TxRetry++;
            pstSlot->u32DeadlineMs = pstLink->u32NowMs + pstLink->stCfg.u16AckTimeoutMs;
            LINK_DEBUG(pstLink, "retry cmd=0x%02X seq=%u n=%u\r\n",
                       pstSlot->u8Cmd, pstSlot->u8Seq, pstSlot->u8RetryCnt);
            (void)CommLink_RawSend(pstLink, pstSlot->pu8Frame, pstSlot->u16FrameLen,
                                   pstSlot->u8Cmd, 0U);
            continue;
        }

        pfDone = pstSlot->pfDone;
        pvCtx  = pstSlot->pvCtx;
        pstSlot->pfDone  = NULL;
        pstSlot->pvCtx   = NULL;
        pstSlot->u8State = (uint8_t)COMM_SLOT_FREE;

        pstLink->stStats.u32TxTimeout++;
        LINK_DEBUG(pstLink, "timeout cmd=0x%02X seq=%u\r\n", pstSlot->u8Cmd, pstSlot->u8Seq);

        if (pfDone != NULL)
        {
            pfDone(pstLink, pvCtx, COMM_TX_TIMEOUT, COMM_STATUS_NONE, NULL, 0U);
        }
    }
}

/* ============================================================================
 * 公开接口
 * ============================================================================ */

/**
 * @brief  初始化链路实例
 * @param  [out] pstLink 链路实例
 * @param  [in]  pstCfg  配置
 * @retval COMM_OK / COMM_ERR_PARAM
 */
int32_t CommLink_Init(CommLink_ST *pstLink, const CommLinkCfg_ST *pstCfg)
{
    CommFramerCfg_ST stFramerCfg;
    uint16_t u16MinFrameCap;
    uint8_t  i;
    int32_t  i32Ret;

    if (pstLink == NULL || pstCfg == NULL)
    {
        return COMM_ERR_PARAM;
    }

    if (pstCfg->u16MaxPayloadLen == 0U ||
        pstCfg->u16MaxPayloadLen > COMM_PAYLOAD_LEN_MAX)
    {
        return COMM_ERR_PARAM;
    }

    u16MinFrameCap = (uint16_t)(pstCfg->u16MaxPayloadLen + COMM_FRAME_OVERHEAD);

    if (pstCfg->pu8RespFrame == NULL ||
        pstCfg->u16RespFrameCap < (uint16_t)(COMM_FRAME_OVERHEAD + 1U))
    {
        return COMM_ERR_PARAM;
    }

    if (pstCfg->pstSlots == NULL || pstCfg->u8SlotCount == 0U ||
        pstCfg->pu8SlotFrameBuf == NULL || pstCfg->u16SlotFrameCap < u16MinFrameCap)
    {
        return COMM_ERR_PARAM;
    }

    if (pstCfg->pfSendRaw == NULL)
    {
        return COMM_ERR_PARAM;
    }

    /* 锁必须成对提供，只给一半是典型的接线错误 */
    if ((pstCfg->pfLock == NULL) != (pstCfg->pfUnlock == NULL))
    {
        return COMM_ERR_PARAM;
    }

    if (pstCfg->u8DupCount > 0U &&
        (pstCfg->pstDup == NULL || pstCfg->pu8DupRespBuf == NULL ||
         pstCfg->u16DupRespCap == 0U))
    {
        return COMM_ERR_PARAM;
    }

    memset(pstLink, 0, sizeof(*pstLink));
    pstLink->stCfg = *pstCfg;

    stFramerCfg.pu8Ring          = pstCfg->pu8Ring;
    stFramerCfg.u16RingSize      = pstCfg->u16RingSize;
    stFramerCfg.pu8Frame         = pstCfg->pu8RxFrame;
    stFramerCfg.u16FrameCap      = pstCfg->u16RxFrameCap;
    stFramerCfg.u16MaxPayloadLen = pstCfg->u16MaxPayloadLen;

    i32Ret = CommFramer_Init(&pstLink->stFramer, &stFramerCfg);
    if (i32Ret != COMM_OK)
    {
        return i32Ret;
    }

    /* 把整块槽帧缓冲切给各个槽 */
    for (i = 0U; i < pstCfg->u8SlotCount; i++)
    {
        memset(&pstLink->stCfg.pstSlots[i], 0, sizeof(CommTxSlot_ST));
        pstLink->stCfg.pstSlots[i].pu8Frame =
            &pstCfg->pu8SlotFrameBuf[(uint32_t)i * pstCfg->u16SlotFrameCap];
    }

    /* 把整块去重应答缓冲切给各个缓存项 */
    for (i = 0U; i < pstCfg->u8DupCount; i++)
    {
        memset(&pstLink->stCfg.pstDup[i], 0, sizeof(CommDupEntry_ST));
        pstLink->stCfg.pstDup[i].pu8Resp =
            &pstCfg->pu8DupRespBuf[(uint32_t)i * pstCfg->u16DupRespCap];
    }

    LINK_DEBUG(pstLink, "init addr=0x%02X payloadMax=%u slots=%u dup=%u\r\n",
               pstCfg->u8SelfAddr, pstCfg->u16MaxPayloadLen,
               pstCfg->u8SlotCount, pstCfg->u8DupCount);

    return COMM_OK;
}

/**
 * @brief  注册一张业务命令表
 * @param  [in] pstLink  链路实例
 * @param  [in] pstTable 命令表
 * @param  [in] u8Count  表项数量
 * @retval COMM_OK / COMM_ERR_PARAM / COMM_ERR_BUFFER_FULL
 */
int32_t CommLink_RegisterCmdTable(CommLink_ST *pstLink,
                                  const CommCmdEntry_ST *pstTable, uint8_t u8Count)
{
    if (pstLink == NULL)
    {
        return COMM_ERR_PARAM;
    }
    return CommDispatch_Register(&pstLink->stDispatch, pstTable, u8Count);
}

/**
 * @brief  向链路喂入原始字节（ISR 上下文）
 * @param  [in] pstLink 链路实例
 * @param  [in] pu8Data 原始字节
 * @param  [in] u16Len  字节数
 */
void CommLink_FeedFromISR(CommLink_ST *pstLink, const uint8_t *pu8Data, uint16_t u16Len)
{
    if (pstLink == NULL)
    {
        return;
    }

    if (CommFramer_FeedFromISR(&pstLink->stFramer, pu8Data, u16Len) > 0U)
    {
        if (pstLink->stCfg.pfNotifyFromISR != NULL)
        {
            pstLink->stCfg.pfNotifyFromISR(pstLink->stCfg.pvIo);
        }
    }
}

/**
 * @brief  通知链路发送通道传输完成
 * @param  [in] pstLink 链路实例
 */
void CommLink_OnTxComplete(CommLink_ST *pstLink)
{
    if (pstLink != NULL && pstLink->stCfg.pfRespTxDone != NULL)
    {
        pstLink->stCfg.pfRespTxDone(pstLink);
    }
}

/**
 * @brief  投递一帧待发消息
 * @param  [in] pstLink 链路实例
 * @param  [in] pstReq  发送请求
 * @retval COMM_OK / COMM_ERR_PARAM / COMM_ERR_OVERSIZE / COMM_ERR_BUFFER_FULL
 */
int32_t CommLink_Post(CommLink_ST *pstLink, const CommTxReq_ST *pstReq)
{
    CommTxSlot_ST *pstSlot = NULL;
    uint16_t u16FrameLen = 0U;
    uint8_t  u8Seq;
    uint8_t  i;
    int32_t  i32Ret;

    if (pstLink == NULL || pstReq == NULL)
    {
        return COMM_ERR_PARAM;
    }

    if (pstReq->u16DataLen > 0U && pstReq->pu8Data == NULL)
    {
        return COMM_ERR_PARAM;
    }

    if (pstReq->u16DataLen > pstLink->stCfg.u16MaxPayloadLen)
    {
        return COMM_ERR_OVERSIZE;
    }

    CommLink_Lock(pstLink);

    for (i = 0U; i < pstLink->stCfg.u8SlotCount; i++)
    {
        if (pstLink->stCfg.pstSlots[i].u8State == (uint8_t)COMM_SLOT_FREE)
        {
            pstSlot = &pstLink->stCfg.pstSlots[i];
            break;
        }
    }

    if (pstSlot == NULL)
    {
        pstLink->stStats.u32TxDropFull++;
        CommLink_Unlock(pstLink);
        return COMM_ERR_BUFFER_FULL;
    }

    u8Seq = pstLink->u8SeqCounter++;

    i32Ret = CommFrame_Build(pstReq->u8DstAddr, pstLink->stCfg.u8SelfAddr,
                             pstReq->u8Cmd, 0U, u8Seq,
                             pstReq->pu8Data, pstReq->u16DataLen,
                             pstSlot->pu8Frame, pstLink->stCfg.u16SlotFrameCap,
                             &u16FrameLen);
    if (i32Ret != COMM_OK)
    {
        CommLink_Unlock(pstLink);
        return i32Ret;
    }

    pstSlot->u8DstAddr    = pstReq->u8DstAddr;
    pstSlot->u8Cmd        = pstReq->u8Cmd;
    pstSlot->u8Seq        = u8Seq;
    pstSlot->u8NeedAck    = pstReq->u8NeedAck;
    pstSlot->u8RetryCnt   = 0U;
    pstSlot->u8Armed      = 0U;
    pstSlot->u16FrameLen  = u16FrameLen;
    pstSlot->u32DeadlineMs = 0U;
    pstSlot->pfDone       = pstReq->pfDone;
    pstSlot->pvCtx        = pstReq->pvCtx;
    pstSlot->u8State      = (uint8_t)COMM_SLOT_READY;   /* 最后置位，发布该槽 */

    pstLink->stStats.u32TxPosted++;

    CommLink_Unlock(pstLink);
    return COMM_OK;
}

/**
 * @brief  链路主循环
 * @param  [in] pstLink  链路实例
 * @param  [in] u32NowMs 当前毫秒时间戳
 */
void CommLink_Tick(CommLink_ST *pstLink, uint32_t u32NowMs)
{
    if (pstLink == NULL)
    {
        return;
    }

    pstLink->u32NowMs = u32NowMs;

    CommLink_RxPump(pstLink);
    CommLink_TxPump(pstLink);
    CommLink_CheckTimeouts(pstLink);
}

/**
 * @brief  查询链路是否空闲
 * @param  [in] pstLink 链路实例
 * @retval 1: 空闲, 0: 忙
 */
uint8_t CommLink_IsIdle(const CommLink_ST *pstLink)
{
    uint8_t i;

    if (pstLink == NULL)
    {
        return 0U;
    }

    for (i = 0U; i < pstLink->stCfg.u8SlotCount; i++)
    {
        if (pstLink->stCfg.pstSlots[i].u8State != (uint8_t)COMM_SLOT_FREE)
        {
            return 0U;
        }
    }
    return 1U;
}

/**
 * @brief  handler 可写入的应答附加数据容量
 * @param  [in] pstLink 链路实例
 * @return 可写字节数
 */
uint16_t CommLink_RespPayloadCap(const CommLink_ST *pstLink)
{
    if (pstLink == NULL ||
        pstLink->stCfg.u16RespFrameCap < (uint16_t)(COMM_FRAME_OVERHEAD + 1U))
    {
        return 0U;
    }
    return (uint16_t)(pstLink->stCfg.u16RespFrameCap - COMM_FRAME_OVERHEAD - 1U);
}

/**
 * @brief  获取链路统计
 * @param  [in] pstLink 链路实例
 * @return 统计结构体指针
 */
const CommLinkStats_ST *CommLink_GetStats(CommLink_ST *pstLink)
{
    if (pstLink == NULL)
    {
        return NULL;
    }
    pstLink->stStats.stFramer = *CommFramer_GetStats(&pstLink->stFramer);
    return &pstLink->stStats;
}
