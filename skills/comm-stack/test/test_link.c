/**
 * @file    test_link.c
 * @brief   comm_link 单元测试：两个实例背靠背 loopback + 故障注入 + 假时钟
 *
 * 拓扑：NodeA.pfSendRaw ──► (故障注入) ──► NodeB.CommLink_FeedFromISR
 *       NodeB.pfSendRaw ──► (故障注入) ──► NodeA.CommLink_FeedFromISR
 *
 * 时间完全由 Pump() 里的假时钟推进，测 500ms 级超时不需要真的等待。
 */

#include "comm_link.h"
#include "test_util.h"
#include <string.h>

#define NODE_MAX_PAYLOAD    64U
#define NODE_FRAME_CAP      (NODE_MAX_PAYLOAD + COMM_FRAME_OVERHEAD)
#define NODE_SLOTS          2U
#define NODE_DUP            2U
#define NODE_DUP_RESP_CAP   32U
#define NODE_RESP_CAP       (32U + COMM_FRAME_OVERHEAD)
#define NODE_RING_SIZE      256U

#define ACK_TIMEOUT_MS      100U
#define MAX_RETRY           2U

/* 测试用命令码 */
#define CMD_ECHO            0x30U   /* 回显请求 payload */
#define CMD_BUSY            0x31U   /* 固定返回 COMM_ERR_BUSY */
#define CMD_COUNT           0x32U   /* 计数，用于去重验证 */
#define CMD_FF              0x33U   /* fire-and-forget，不回 ACK */
#define CMD_BIZ2            0x40U   /* 第二张业务表里的命令 */

/* ============================================================================
 * 节点（实例 + 全部静态缓冲 + 故障注入开关）
 * ============================================================================ */

typedef struct Node_ST Node_ST;

struct Node_ST
{
    CommLink_ST     stLink;
    uint8_t         au8Ring[NODE_RING_SIZE];
    uint8_t         au8RxFrame[NODE_FRAME_CAP];
    uint8_t         au8RespFrame[NODE_RESP_CAP];
    CommTxSlot_ST   astSlots[NODE_SLOTS];
    uint8_t         au8SlotBuf[NODE_SLOTS * NODE_FRAME_CAP];
    CommDupEntry_ST astDup[NODE_DUP];
    uint8_t         au8DupBuf[NODE_DUP * NODE_DUP_RESP_CAP];

    /* 故障注入 */
    Node_ST        *pstPeer;
    uint8_t         u8DropNext;     /* 一次性丢弃下一帧 */
    uint8_t         u8DropAll;      /* 持续丢弃所有出站帧 */
    uint8_t         u8CorruptNext;  /* 一次性翻转下一帧的一个 bit */
    uint8_t         u8SplitNext;    /* 一次性把下一帧切成两段分别喂入 */
    uint8_t         u8BusyNext;     /* 一次性让 pfSendRaw 返回 BUSY */
    uint32_t        u32SendCalls;   /* pfSendRaw 被调用次数 */
};

/* 业务侧计数器 */
static uint32_t s_u32EchoCalls;
static uint32_t s_u32CountCalls;
static uint32_t s_u32FfCalls;
static uint32_t s_u32Biz2Calls;

/**
 * @brief  故障注入的底层发送：把出站字节喂给对端的解帧器
 * @param  [in] pvIo   源节点
 * @param  [in] pu8    出站字节
 * @param  [in] u16Len 长度
 * @retval COMM_OK / COMM_ERR_BUSY
 */
static int32_t WireSend(void *pvIo, const uint8_t *pu8, uint16_t u16Len)
{
    Node_ST *pstSrc = (Node_ST *)pvIo;
    uint8_t  au8Copy[NODE_FRAME_CAP];

    if (pstSrc->u8BusyNext != 0U)
    {
        pstSrc->u8BusyNext = 0U;
        return COMM_ERR_BUSY;
    }

    pstSrc->u32SendCalls++;

    if (pstSrc->u8DropAll != 0U || pstSrc->u8DropNext != 0U)
    {
        pstSrc->u8DropNext = 0U;
        return COMM_OK;   /* 线上丢了，但本机认为发送成功 */
    }

    memcpy(au8Copy, pu8, u16Len);

    if (pstSrc->u8CorruptNext != 0U)
    {
        pstSrc->u8CorruptNext = 0U;
        au8Copy[u16Len / 2U] ^= 0x01U;
    }

    if (pstSrc->u8SplitNext != 0U)
    {
        pstSrc->u8SplitNext = 0U;
        CommLink_FeedFromISR(&pstSrc->pstPeer->stLink, au8Copy, 3U);
        CommLink_FeedFromISR(&pstSrc->pstPeer->stLink, &au8Copy[3],
                             (uint16_t)(u16Len - 3U));
        return COMM_OK;
    }

    CommLink_FeedFromISR(&pstSrc->pstPeer->stLink, au8Copy, u16Len);
    return COMM_OK;
}

/**
 * @brief  初始化一个节点
 * @param  [out] pstNode   节点
 * @param  [in]  u8Addr    本机地址
 * @param  [in]  pstPeer   对端节点
 * @param  [in]  u8DupCnt  去重缓存项数（0 表示关闭去重）
 * @param  [in]  u16MaxPl  本实例 payload 配额
 */
static void NodeInit(Node_ST *pstNode, uint8_t u8Addr, Node_ST *pstPeer,
                     uint8_t u8DupCnt, uint16_t u16MaxPl)
{
    CommLinkCfg_ST stCfg;

    memset(pstNode, 0, sizeof(*pstNode));
    pstNode->pstPeer = pstPeer;

    memset(&stCfg, 0, sizeof(stCfg));
    stCfg.pcName            = "node";
    stCfg.u8SelfAddr        = u8Addr;
    stCfg.u8AcceptBroadcast = 1U;
    stCfg.u16MaxPayloadLen  = u16MaxPl;

    stCfg.pu8Ring       = pstNode->au8Ring;
    stCfg.u16RingSize   = NODE_RING_SIZE;
    stCfg.pu8RxFrame    = pstNode->au8RxFrame;
    stCfg.u16RxFrameCap = NODE_FRAME_CAP;

    stCfg.pu8RespFrame    = pstNode->au8RespFrame;
    stCfg.u16RespFrameCap = NODE_RESP_CAP;

    stCfg.pstSlots        = pstNode->astSlots;
    stCfg.u8SlotCount     = NODE_SLOTS;
    stCfg.pu8SlotFrameBuf = pstNode->au8SlotBuf;
    stCfg.u16SlotFrameCap = NODE_FRAME_CAP;
    stCfg.u16AckTimeoutMs = ACK_TIMEOUT_MS;
    stCfg.u8MaxRetry      = MAX_RETRY;

    stCfg.pstDup        = (u8DupCnt > 0U) ? pstNode->astDup : NULL;
    stCfg.u8DupCount    = u8DupCnt;
    stCfg.pu8DupRespBuf = (u8DupCnt > 0U) ? pstNode->au8DupBuf : NULL;
    stCfg.u16DupRespCap = NODE_DUP_RESP_CAP;
    stCfg.u32DupTtlMs   = 5000U;

    stCfg.pfSendRaw = WireSend;
    stCfg.pvIo      = pstNode;

    CHECK_EQ(CommLink_Init(&pstNode->stLink, &stCfg), COMM_OK);
}

/* ============================================================================
 * 业务 handler
 * ============================================================================ */

/**
 * @brief  回显请求 payload
 * @param  [in]  pstLink  链路
 * @param  [in]  pstReq   请求帧
 * @param  [out] pu8Pl    应答附加数据缓冲
 * @param  [out] pu16Len  应答附加数据长度
 * @retval COMM_OK
 */
static int32_t HandleEcho(CommLink_ST *pstLink, const CommFrameInfo_ST *pstReq,
                          uint8_t *pu8Pl, uint16_t *pu16Len)
{
    (void)pstLink;
    s_u32EchoCalls++;
    if (pstReq->u16DataLen > 0U)
    {
        memcpy(pu8Pl, pstReq->pu8Data, pstReq->u16DataLen);
    }
    *pu16Len = pstReq->u16DataLen;
    return COMM_OK;
}

/**
 * @brief  固定返回忙，用于验证 REJECTED 语义
 */
static int32_t HandleBusy(CommLink_ST *pstLink, const CommFrameInfo_ST *pstReq,
                          uint8_t *pu8Pl, uint16_t *pu16Len)
{
    (void)pstLink; (void)pstReq; (void)pu8Pl;
    *pu16Len = 0U;
    return COMM_ERR_BUSY;
}

/**
 * @brief  计数，用于验证去重是否真的阻止了业务重复执行
 */
static int32_t HandleCount(CommLink_ST *pstLink, const CommFrameInfo_ST *pstReq,
                           uint8_t *pu8Pl, uint16_t *pu16Len)
{
    (void)pstLink; (void)pstReq; (void)pu8Pl;
    s_u32CountCalls++;
    *pu16Len = 0U;
    return COMM_OK;
}

/**
 * @brief  fire-and-forget 命令
 */
static int32_t HandleFf(CommLink_ST *pstLink, const CommFrameInfo_ST *pstReq,
                        uint8_t *pu8Pl, uint16_t *pu16Len)
{
    (void)pstLink; (void)pstReq; (void)pu8Pl;
    s_u32FfCalls++;
    *pu16Len = 0U;
    return COMM_OK;
}

/**
 * @brief  第二张业务表里的命令
 */
static int32_t HandleBiz2(CommLink_ST *pstLink, const CommFrameInfo_ST *pstReq,
                          uint8_t *pu8Pl, uint16_t *pu16Len)
{
    (void)pstLink; (void)pstReq;
    s_u32Biz2Calls++;
    pu8Pl[0] = 0xB2U;
    *pu16Len = 1U;
    return COMM_OK;
}

static const CommCmdEntry_ST s_astTable1[] =
{
    { CMD_ECHO,  0U, 16U, 1U, HandleEcho  },
    { CMD_BUSY,  0U,  0U, 1U, HandleBusy  },
    { CMD_COUNT, 0U,  4U, 1U, HandleCount },
    { CMD_FF,    0U, 16U, 0U, HandleFf    },
};

static const CommCmdEntry_ST s_astTable2[] =
{
    { CMD_BIZ2, 0U, 0U, 1U, HandleBiz2 },
};

/* ============================================================================
 * 完成回调记录器
 * ============================================================================ */

typedef struct
{
    uint32_t       u32Calls;
    CommTxResult_E eResult;
    uint8_t        u8Status;
    uint8_t        au8Ack[32];
    uint16_t       u16AckLen;
} DoneRec_ST;

/**
 * @brief  发送完成回调：把结果记进调用方给的记录器
 */
static void OnDone(CommLink_ST *pstLink, void *pvCtx, CommTxResult_E eResult,
                   uint8_t u8Status, const uint8_t *pu8Ack, uint16_t u16AckLen)
{
    DoneRec_ST *pstRec = (DoneRec_ST *)pvCtx;

    (void)pstLink;
    pstRec->u32Calls++;
    pstRec->eResult  = eResult;
    pstRec->u8Status = u8Status;
    pstRec->u16AckLen = u16AckLen;
    if (pu8Ack != NULL && u16AckLen > 0U && u16AckLen <= sizeof(pstRec->au8Ack))
    {
        memcpy(pstRec->au8Ack, pu8Ack, u16AckLen);
    }
}

/* ============================================================================
 * 假时钟驱动
 * ============================================================================ */

static Node_ST  s_stA;
static Node_ST  s_stB;
static uint32_t s_u32Now;

/**
 * @brief  推进假时钟并驱动两个节点的 Tick
 * @param  [in] u32Ms 推进的毫秒数
 */
static void Pump(uint32_t u32Ms)
{
    uint32_t u32End = s_u32Now + u32Ms;

    for (;;)
    {
        CommLink_Tick(&s_stA.stLink, s_u32Now);
        CommLink_Tick(&s_stB.stLink, s_u32Now);
        if ((int32_t)(s_u32Now - u32End) >= 0)
        {
            break;
        }
        s_u32Now += 5U;
    }
}

/**
 * @brief  重建 A/B 两个节点并清零业务计数
 * @param  [in] u8DupCntB B 侧去重项数
 */
static void ResetPair(uint8_t u8DupCntB)
{
    NodeInit(&s_stA, 0x01U, &s_stB, NODE_DUP, NODE_MAX_PAYLOAD);
    NodeInit(&s_stB, 0x02U, &s_stA, u8DupCntB, NODE_MAX_PAYLOAD);
    s_stA.pstPeer = &s_stB;
    s_stB.pstPeer = &s_stA;

    CHECK_EQ(CommLink_RegisterCmdTable(&s_stB.stLink, s_astTable1,
                                       (uint8_t)(sizeof(s_astTable1) /
                                                 sizeof(s_astTable1[0]))), COMM_OK);

    s_u32EchoCalls  = 0U;
    s_u32CountCalls = 0U;
    s_u32FfCalls    = 0U;
    s_u32Biz2Calls  = 0U;
    s_u32Now        = 1000U;
}

/**
 * @brief  构造一个发送请求
 */
static CommTxReq_ST MakeReq(uint8_t u8Dst, uint8_t u8Cmd, const uint8_t *pu8Data,
                            uint16_t u16Len, uint8_t u8NeedAck, DoneRec_ST *pstRec)
{
    CommTxReq_ST stReq;

    memset(&stReq, 0, sizeof(stReq));
    stReq.u8DstAddr  = u8Dst;
    stReq.u8Cmd      = u8Cmd;
    stReq.pu8Data    = pu8Data;
    stReq.u16DataLen = u16Len;
    stReq.u8NeedAck  = u8NeedAck;
    stReq.pfDone     = (pstRec != NULL) ? OnDone : NULL;
    stReq.pvCtx      = pstRec;
    return stReq;
}

/* ============================================================================
 * 测试用例
 * ============================================================================ */

/**
 * @brief  comm_link 测试入口
 */
void test_link(void)
{
    DoneRec_ST stRec;
    DoneRec_ST stRec2;
    CommTxReq_ST stReq;
    const uint8_t au8Payload[4] = { 0xDEU, 0xADU, 0xBEU, 0xEFU };

    CASE("正常往返：请求 → ACK → pfDone(OK)，且 ACK payload 原样带回");
    ResetPair(NODE_DUP);
    memset(&stRec, 0, sizeof(stRec));
    stReq = MakeReq(0x02U, CMD_ECHO, au8Payload, 4U, 1U, &stRec);
    CHECK_EQ(CommLink_Post(&s_stA.stLink, &stReq), COMM_OK);
    Pump(50U);
    CHECK_EQ(stRec.u32Calls, 1U);
    CHECK_EQ(stRec.eResult, COMM_TX_OK);
    CHECK_EQ(stRec.u8Status, COMM_STATUS_OK);
    CHECK_EQ(stRec.u16AckLen, 4U);
    CHECK_EQ(memcmp(stRec.au8Ack, au8Payload, 4U), 0);
    CHECK_EQ(s_u32EchoCalls, 1U);
    CHECK_EQ(CommLink_IsIdle(&s_stA.stLink), 1U);

    CASE("内建 GET_VERSION：无需注册即可应答，且版本字节能回到发起方");
    ResetPair(NODE_DUP);
    memset(&stRec, 0, sizeof(stRec));
    stReq = MakeReq(0x02U, COMM_CMD_GET_VERSION, NULL, 0U, 1U, &stRec);
    CHECK_EQ(CommLink_Post(&s_stA.stLink, &stReq), COMM_OK);
    Pump(50U);
    CHECK_EQ(stRec.u32Calls, 1U);
    CHECK_EQ(stRec.eResult, COMM_TX_OK);
    CHECK_EQ(stRec.u16AckLen, 2U);
    CHECK_EQ(stRec.au8Ack[0], COMM_PROTO_VERSION_MAJOR);
    CHECK_EQ(stRec.au8Ack[1], COMM_PROTO_VERSION_MINOR);

    CASE("ACK 丢失 → 超时重传 → 成功，且去重保证业务只执行一次");
    ResetPair(NODE_DUP);
    memset(&stRec, 0, sizeof(stRec));
    s_stB.u8DropNext = 1U;                 /* 丢掉 B 回的第一个 ACK */
    stReq = MakeReq(0x02U, CMD_COUNT, NULL, 0U, 1U, &stRec);
    CHECK_EQ(CommLink_Post(&s_stA.stLink, &stReq), COMM_OK);
    Pump(300U);
    CHECK_EQ(stRec.u32Calls, 1U);
    CHECK_EQ(stRec.eResult, COMM_TX_OK);
    CHECK_EQ(s_u32CountCalls, 1U);         /* 关键：重传没有让业务跑第二次 */
    CHECK(CommLink_GetStats(&s_stA.stLink)->u32TxRetry >= 1U);
    CHECK(CommLink_GetStats(&s_stB.stLink)->u32DupHit >= 1U);

    CASE("关闭去重后，同一请求的重传会让业务执行两次（验证开关确实生效）");
    ResetPair(0U);
    memset(&stRec, 0, sizeof(stRec));
    s_stB.u8DropNext = 1U;
    stReq = MakeReq(0x02U, CMD_COUNT, NULL, 0U, 1U, &stRec);
    CHECK_EQ(CommLink_Post(&s_stA.stLink, &stReq), COMM_OK);
    Pump(300U);
    CHECK_EQ(stRec.eResult, COMM_TX_OK);
    CHECK_EQ(s_u32CountCalls, 2U);
    CHECK_EQ(CommLink_GetStats(&s_stB.stLink)->u32DupHit, 0U);

    CASE("重试耗尽 → pfDone(TIMEOUT)");
    ResetPair(NODE_DUP);
    memset(&stRec, 0, sizeof(stRec));
    s_stB.u8DropAll = 1U;                  /* B 的所有应答都丢 */
    stReq = MakeReq(0x02U, CMD_ECHO, NULL, 0U, 1U, &stRec);
    CHECK_EQ(CommLink_Post(&s_stA.stLink, &stReq), COMM_OK);
    Pump(1000U);
    CHECK_EQ(stRec.u32Calls, 1U);
    CHECK_EQ(stRec.eResult, COMM_TX_TIMEOUT);
    CHECK_EQ(CommLink_GetStats(&s_stA.stLink)->u32TxRetry, MAX_RETRY);
    CHECK_EQ(CommLink_GetStats(&s_stA.stLink)->u32TxTimeout, 1U);
    CHECK_EQ(CommLink_IsIdle(&s_stA.stLink), 1U);

    CASE("对端返回非 OK → pfDone(REJECTED) 且不重传");
    ResetPair(NODE_DUP);
    memset(&stRec, 0, sizeof(stRec));
    stReq = MakeReq(0x02U, CMD_BUSY, NULL, 0U, 1U, &stRec);
    CHECK_EQ(CommLink_Post(&s_stA.stLink, &stReq), COMM_OK);
    Pump(300U);
    CHECK_EQ(stRec.u32Calls, 1U);
    CHECK_EQ(stRec.eResult, COMM_TX_REJECTED);
    CHECK_EQ(stRec.u8Status, COMM_STATUS_BUSY);
    CHECK_EQ(CommLink_GetStats(&s_stA.stLink)->u32TxRetry, 0U);

    CASE("未知命令 → 对端回 INVALID_CMD");
    ResetPair(NODE_DUP);
    memset(&stRec, 0, sizeof(stRec));
    stReq = MakeReq(0x02U, 0x7EU, NULL, 0U, 1U, &stRec);
    CHECK_EQ(CommLink_Post(&s_stA.stLink, &stReq), COMM_OK);
    Pump(50U);
    CHECK_EQ(stRec.eResult, COMM_TX_REJECTED);
    CHECK_EQ(stRec.u8Status, COMM_STATUS_INVALID_CMD);
    CHECK_EQ(CommLink_GetStats(&s_stB.stLink)->u32RxUnknownCmd, 1U);

    CASE("payload 长度不在表项区间 → 对端回 INVALID_PARAM");
    ResetPair(NODE_DUP);
    memset(&stRec, 0, sizeof(stRec));
    {
        const uint8_t au8Long[20] = { 0 };
        stReq = MakeReq(0x02U, CMD_COUNT, au8Long, 20U, 1U, &stRec);  /* 表项上限 4 */
        CHECK_EQ(CommLink_Post(&s_stA.stLink, &stReq), COMM_OK);
        Pump(50U);
        CHECK_EQ(stRec.u8Status, COMM_STATUS_INVALID_PARAM);
        CHECK_EQ(s_u32CountCalls, 0U);
        CHECK_EQ(CommLink_GetStats(&s_stB.stLink)->u32RxBadLen, 1U);
    }

    CASE("fire-and-forget：不回 ACK，发出即完成");
    ResetPair(NODE_DUP);
    memset(&stRec, 0, sizeof(stRec));
    s_stB.u32SendCalls = 0U;
    stReq = MakeReq(0x02U, CMD_FF, au8Payload, 4U, 0U, &stRec);
    CHECK_EQ(CommLink_Post(&s_stA.stLink, &stReq), COMM_OK);
    Pump(50U);
    CHECK_EQ(stRec.u32Calls, 1U);
    CHECK_EQ(stRec.eResult, COMM_TX_OK);
    CHECK_EQ(s_u32FfCalls, 1U);
    CHECK_EQ(s_stB.u32SendCalls, 0U);      /* B 一个字节都没回 */
    CHECK_EQ(CommLink_IsIdle(&s_stA.stLink), 1U);

    CASE("回归：应答帧绝不会被误当成新请求（Flags.RESPONSE 位的存在理由）");
    /* A 用 fire-and-forget 发一个对端表里 NeedAck=1 的命令：对端必然回一个应答，
     * 而 A 这边没有待确认请求能消费它。若线上没有"这是应答"的标记，A 会把该
     * 应答当成新请求再回一个应答，对端又把它当成新请求 …… 业务被反复执行。
     * 这是框架 v1 的真实缺陷，由本用例在 v2 上锁死。 */
    ResetPair(NODE_DUP);
    memset(&stRec, 0, sizeof(stRec));
    stReq = MakeReq(0x02U, CMD_ECHO, au8Payload, 4U, 0U, &stRec);
    CHECK_EQ(CommLink_Post(&s_stA.stLink, &stReq), COMM_OK);
    Pump(200U);
    CHECK_EQ(s_u32EchoCalls, 1U);
    CHECK_EQ(CommLink_GetStats(&s_stA.stLink)->u32RxOrphanResp, 1U);
    CHECK_EQ(CommLink_GetStats(&s_stB.stLink)->u32RxUnknownCmd, 0U);

    CASE("无主应答（本机已超时放弃或根本没发过）被静默丢弃，不产生任何回帧");
    ResetPair(NODE_DUP);
    {
        uint8_t  au8Resp[COMM_FRAME_LEN_MAX];
        uint8_t  au8Body[1] = { (uint8_t)COMM_STATUS_OK };
        uint16_t u16RespLen = 0U;

        /* 直接伪造一个发给 A 的应答帧：A 没有任何待确认请求能消费它 */
        CHECK_EQ(CommFrame_Build(0x01U, 0x02U, CMD_ECHO,
                                 COMM_FRAME_FLAG_RESPONSE, 0x99U,
                                 au8Body, 1U,
                                 au8Resp, sizeof(au8Resp), &u16RespLen), COMM_OK);

        s_stA.u32SendCalls = 0U;
        CommLink_FeedFromISR(&s_stA.stLink, au8Resp, u16RespLen);
        Pump(50U);

        CHECK_EQ(CommLink_GetStats(&s_stA.stLink)->u32RxOrphanResp, 1U);
        CHECK_EQ(s_stA.u32SendCalls, 0U);   /* 关键：一个字节都不回，乒乓无从发生 */
    }

    CASE("非本机地址的帧被丢弃并计数");
    ResetPair(NODE_DUP);
    memset(&stRec, 0, sizeof(stRec));
    stReq = MakeReq(0x09U, CMD_ECHO, NULL, 0U, 0U, &stRec);
    CHECK_EQ(CommLink_Post(&s_stA.stLink, &stReq), COMM_OK);
    Pump(50U);
    CHECK_EQ(s_u32EchoCalls, 0U);
    CHECK_EQ(CommLink_GetStats(&s_stB.stLink)->u32RxNotForMe, 1U);

    CASE("广播帧：被处理但不回 ACK");
    ResetPair(NODE_DUP);
    s_stB.u32SendCalls = 0U;
    stReq = MakeReq(COMM_ADDR_BROADCAST, CMD_COUNT, NULL, 0U, 0U, NULL);
    CHECK_EQ(CommLink_Post(&s_stA.stLink, &stReq), COMM_OK);
    Pump(50U);
    CHECK_EQ(s_u32CountCalls, 1U);
    CHECK_EQ(s_stB.u32SendCalls, 0U);

    CASE("多独立业务：两张命令表各自注册，回调按请求路由互不串台");
    ResetPair(NODE_DUP);
    CHECK_EQ(CommLink_RegisterCmdTable(&s_stB.stLink, s_astTable2, 1U), COMM_OK);
    memset(&stRec, 0, sizeof(stRec));
    memset(&stRec2, 0, sizeof(stRec2));
    stReq = MakeReq(0x02U, CMD_ECHO, au8Payload, 2U, 1U, &stRec);
    CHECK_EQ(CommLink_Post(&s_stA.stLink, &stReq), COMM_OK);
    stReq = MakeReq(0x02U, CMD_BIZ2, NULL, 0U, 1U, &stRec2);
    CHECK_EQ(CommLink_Post(&s_stA.stLink, &stReq), COMM_OK);
    Pump(100U);
    CHECK_EQ(stRec.u32Calls, 1U);
    CHECK_EQ(stRec.u16AckLen, 2U);
    CHECK_EQ(stRec.au8Ack[0], 0xDEU);
    CHECK_EQ(stRec2.u32Calls, 1U);
    CHECK_EQ(stRec2.u16AckLen, 1U);
    CHECK_EQ(stRec2.au8Ack[0], 0xB2U);
    CHECK_EQ(s_u32EchoCalls, 1U);
    CHECK_EQ(s_u32Biz2Calls, 1U);

    CASE("发送槽满 → Post 返回 BUFFER_FULL 并计数");
    ResetPair(NODE_DUP);
    stReq = MakeReq(0x02U, CMD_ECHO, NULL, 0U, 1U, NULL);
    CHECK_EQ(CommLink_Post(&s_stA.stLink, &stReq), COMM_OK);
    CHECK_EQ(CommLink_Post(&s_stA.stLink, &stReq), COMM_OK);
    CHECK_EQ(CommLink_Post(&s_stA.stLink, &stReq), COMM_ERR_BUFFER_FULL);
    CHECK_EQ(CommLink_GetStats(&s_stA.stLink)->u32TxDropFull, 1U);
    CHECK_EQ(CommLink_IsIdle(&s_stA.stLink), 0U);

    CASE("Post 的 payload 超本实例配额 → OVERSIZE");
    ResetPair(NODE_DUP);
    {
        static uint8_t au8Big[NODE_MAX_PAYLOAD + 1U];
        stReq = MakeReq(0x02U, CMD_ECHO, au8Big, NODE_MAX_PAYLOAD + 1U, 1U, NULL);
        CHECK_EQ(CommLink_Post(&s_stA.stLink, &stReq), COMM_ERR_OVERSIZE);
    }

    CASE("入站 payload 超对端配额 → 对端丢弃并计 RxOversize");
    {
        static Node_ST s_stSmall;
        NodeInit(&s_stA, 0x01U, &s_stSmall, NODE_DUP, NODE_MAX_PAYLOAD);
        NodeInit(&s_stSmall, 0x02U, &s_stA, NODE_DUP, 8U);   /* 配额只有 8 字节 */
        s_stA.pstPeer     = &s_stSmall;
        s_stSmall.pstPeer = &s_stA;
        CHECK_EQ(CommLink_RegisterCmdTable(&s_stSmall.stLink, s_astTable1,
                                           (uint8_t)(sizeof(s_astTable1) /
                                                     sizeof(s_astTable1[0]))), COMM_OK);
        s_u32EchoCalls = 0U;
        s_u32Now = 1000U;

        stReq = MakeReq(0x02U, CMD_ECHO, au8Payload, 4U, 0U, NULL);
        CHECK_EQ(CommLink_Post(&s_stA.stLink, &stReq), COMM_OK);
        /* 手工推进，Pump 绑定的是 s_stB */
        for (int i = 0; i < 10; i++)
        {
            CommLink_Tick(&s_stA.stLink, s_u32Now);
            CommLink_Tick(&s_stSmall.stLink, s_u32Now);
            s_u32Now += 5U;
        }
        CHECK_EQ(s_u32EchoCalls, 1U);   /* 4 字节在配额内，正常处理 */

        {
            static uint8_t au8Mid[16];
            stReq = MakeReq(0x02U, CMD_ECHO, au8Mid, 16U, 0U, NULL);
            CHECK_EQ(CommLink_Post(&s_stA.stLink, &stReq), COMM_OK);
            for (int i = 0; i < 10; i++)
            {
                CommLink_Tick(&s_stA.stLink, s_u32Now);
                CommLink_Tick(&s_stSmall.stLink, s_u32Now);
                s_u32Now += 5U;
            }
            CHECK_EQ(s_u32EchoCalls, 1U);   /* 16 > 8，被解帧器判废 */
            CHECK_EQ(CommLink_GetStats(&s_stSmall.stLink)->stFramer.u32RxOversize, 1U);
        }
    }

    CASE("线路误码：帧被翻转一个 bit → 丢弃，重传后成功");
    ResetPair(NODE_DUP);
    memset(&stRec, 0, sizeof(stRec));
    s_stA.u8CorruptNext = 1U;
    stReq = MakeReq(0x02U, CMD_ECHO, au8Payload, 4U, 1U, &stRec);
    CHECK_EQ(CommLink_Post(&s_stA.stLink, &stReq), COMM_OK);
    Pump(300U);
    CHECK_EQ(stRec.eResult, COMM_TX_OK);
    CHECK(CommLink_GetStats(&s_stB.stLink)->stFramer.u32RxCrcErr >= 1U);

    CASE("链路拆包：帧被切成两段送达，仍能正常处理");
    ResetPair(NODE_DUP);
    memset(&stRec, 0, sizeof(stRec));
    s_stA.u8SplitNext = 1U;
    stReq = MakeReq(0x02U, CMD_ECHO, au8Payload, 4U, 1U, &stRec);
    CHECK_EQ(CommLink_Post(&s_stA.stLink, &stReq), COMM_OK);
    Pump(100U);
    CHECK_EQ(stRec.eResult, COMM_TX_OK);
    CHECK_EQ(s_u32EchoCalls, 1U);

    CASE("发送通道瞬时忙：保持待发，下个 Tick 自动重试");
    ResetPair(NODE_DUP);
    memset(&stRec, 0, sizeof(stRec));
    s_stA.u8BusyNext = 1U;
    stReq = MakeReq(0x02U, CMD_ECHO, au8Payload, 4U, 1U, &stRec);
    CHECK_EQ(CommLink_Post(&s_stA.stLink, &stReq), COMM_OK);
    Pump(100U);
    CHECK_EQ(stRec.eResult, COMM_TX_OK);
    CHECK_EQ(s_u32EchoCalls, 1U);

    CASE("Init 拒绝非法配置");
    {
        CommLinkCfg_ST stCfg;
        CommLink_ST    stTmp;
        CommTxSlot_ST  astSlots[1];
        static uint8_t au8Ring[64];
        static uint8_t au8Rx[NODE_FRAME_CAP];
        static uint8_t au8Resp[NODE_RESP_CAP];
        static uint8_t au8Slot[NODE_FRAME_CAP];

        memset(&stCfg, 0, sizeof(stCfg));
        stCfg.u8SelfAddr        = 0x01U;
        stCfg.u16MaxPayloadLen  = NODE_MAX_PAYLOAD;
        stCfg.pu8Ring           = au8Ring;
        stCfg.u16RingSize       = 64U;
        stCfg.pu8RxFrame        = au8Rx;
        stCfg.u16RxFrameCap     = NODE_FRAME_CAP;
        stCfg.pu8RespFrame      = au8Resp;
        stCfg.u16RespFrameCap   = NODE_RESP_CAP;
        stCfg.pstSlots          = astSlots;
        stCfg.u8SlotCount       = 1U;
        stCfg.pu8SlotFrameBuf   = au8Slot;
        stCfg.u16SlotFrameCap   = NODE_FRAME_CAP;
        stCfg.pfSendRaw         = WireSend;
        stCfg.pvIo              = &s_stA;
        CHECK_EQ(CommLink_Init(&stTmp, &stCfg), COMM_OK);

        stCfg.pfSendRaw = NULL;                       /* 必填项缺失 */
        CHECK_EQ(CommLink_Init(&stTmp, &stCfg), COMM_ERR_PARAM);
        stCfg.pfSendRaw = WireSend;

        stCfg.u16MaxPayloadLen = COMM_PAYLOAD_LEN_MAX + 1U;
        CHECK_EQ(CommLink_Init(&stTmp, &stCfg), COMM_ERR_PARAM);
        stCfg.u16MaxPayloadLen = NODE_MAX_PAYLOAD;

        stCfg.u16SlotFrameCap = 16U;                  /* 槽容量不足 */
        CHECK_EQ(CommLink_Init(&stTmp, &stCfg), COMM_ERR_PARAM);
        stCfg.u16SlotFrameCap = NODE_FRAME_CAP;

        stCfg.pfLock = (void (*)(void *))0x1234U;     /* 锁只给了一半 */
        CHECK_EQ(CommLink_Init(&stTmp, &stCfg), COMM_ERR_PARAM);
        stCfg.pfLock = NULL;

        stCfg.u8DupCount = 2U;                        /* 开了去重但没给缓冲 */
        CHECK_EQ(CommLink_Init(&stTmp, &stCfg), COMM_ERR_PARAM);
    }
}
