/**
 * @file    comm_framer.c
 * @brief   接收侧字节流解帧器实现
 *
 * 解帧流程（CommFramer_Next 一次调用内循环，直到取出一帧或环取空）：
 *
 *   1. 组帧缓冲为空时：从环里逐字节丢弃，直到读到帧头 0xAA（计 u32RxSkipped）
 *   2. 累积到 8 字节（Header + Dst Src Cmd Flags Seq LenH LenL），读出 payload 长度
 *      - 长度超本实例配额  → resync（丢掉当前帧头，从其后一个字节重新找头）
 *   3. 累积到 payload 长度 + COMM_FRAME_OVERHEAD 字节
 *   4. 校验 CRC 与帧尾，任一失败 → resync
 *   5. 成功则输出该帧，并把缓冲里剩下的字节前移，供下一次调用继续解析
 *
 * resync 是本模块存在的全部理由：不做 resync 的实现（丢弃整段缓冲）会让一个
 * 坏帧连带吞掉紧跟其后的好帧，这正是框架 v1 的丢帧根因。
 */

#include "comm_framer.h"
#include <string.h>

/* ============================================================================
 * 环形缓冲（无锁 SPSC）
 * ============================================================================ */

/**
 * @brief  查询环内可读字节数（消费者侧调用）
 * @param  [in] pstF 解帧器实例
 * @return 可读字节数
 */
static uint16_t CommFramer_RingCount(const CommFramer_ST *pstF)
{
    uint16_t u16Head = pstF->u16Head;   /* 先读 head */
    COMM_MEM_BARRIER();
    return (uint16_t)((u16Head - pstF->u16Tail) & pstF->u16Mask);
}

/**
 * @brief  从环内弹出一个字节（消费者侧调用，调用前须确认非空）
 * @param  [in] pstF 解帧器实例
 * @return 弹出的字节
 */
static uint8_t CommFramer_RingPop(CommFramer_ST *pstF)
{
    uint8_t u8Byte = pstF->stCfg.pu8Ring[pstF->u16Tail];
    COMM_MEM_BARRIER();                                    /* 先读数据，再发布 tail */
    pstF->u16Tail = (uint16_t)((pstF->u16Tail + 1U) & pstF->u16Mask);
    return u8Byte;
}

/* ============================================================================
 * 组帧缓冲辅助
 * ============================================================================ */

/**
 * @brief  丢弃组帧缓冲里的第一个字节，并在剩余字节中重新定位帧头
 * @param  [in] pstF 解帧器实例
 * @note   找不到帧头时清空缓冲；跳过的字节计入 u32RxSkipped
 */
static void CommFramer_Resync(CommFramer_ST *pstF)
{
    uint8_t *pu8Frame = pstF->stCfg.pu8Frame;
    uint16_t u16Idx;

    pstF->stStats.u32RxResync++;

    /* 从第 1 个字节起找下一个帧头（第 0 个是刚被判废的帧头，必须跳过） */
    for (u16Idx = 1U; u16Idx < pstF->u16Have; u16Idx++)
    {
        if (pu8Frame[u16Idx] == (uint8_t)COMM_FRAME_HEADER)
        {
            break;
        }
    }

    pstF->stStats.u32RxSkipped += u16Idx;

    if (u16Idx >= pstF->u16Have)
    {
        pstF->u16Have = 0U;
        return;
    }

    pstF->u16Have = (uint16_t)(pstF->u16Have - u16Idx);
    memmove(pu8Frame, &pu8Frame[u16Idx], pstF->u16Have);
}

/**
 * @brief  从环里补足字节到组帧缓冲，直到达到目标数量或环取空
 * @param  [in] pstF     解帧器实例
 * @param  [in] u16Want  期望累积到的字节总数
 * @retval 1: 已累积到 u16Want, 0: 环已取空仍不足
 */
static uint8_t CommFramer_Fill(CommFramer_ST *pstF, uint16_t u16Want)
{
    while (pstF->u16Have < u16Want)
    {
        if (CommFramer_RingCount(pstF) == 0U)
        {
            return 0U;
        }
        pstF->stCfg.pu8Frame[pstF->u16Have] = CommFramer_RingPop(pstF);
        pstF->u16Have++;
    }
    return 1U;
}

/* ============================================================================
 * 公开接口
 * ============================================================================ */

/**
 * @brief  初始化解帧器
 * @param  [out] pstF   解帧器实例
 * @param  [in]  pstCfg 配置
 * @retval COMM_OK: 成功, COMM_ERR_PARAM: 参数非法
 */
int32_t CommFramer_Init(CommFramer_ST *pstF, const CommFramerCfg_ST *pstCfg)
{
    if (pstF == NULL || pstCfg == NULL ||
        pstCfg->pu8Ring == NULL || pstCfg->pu8Frame == NULL)
    {
        return COMM_ERR_PARAM;
    }

    /* 环大小必须是 2 的幂（用掩码取模），且留一格区分空/满 */
    if (pstCfg->u16RingSize < 16U ||
        (pstCfg->u16RingSize & (uint16_t)(pstCfg->u16RingSize - 1U)) != 0U)
    {
        return COMM_ERR_PARAM;
    }

    if (pstCfg->u16MaxPayloadLen > COMM_PAYLOAD_LEN_MAX)
    {
        return COMM_ERR_PARAM;
    }

    if (pstCfg->u16FrameCap < (uint16_t)(pstCfg->u16MaxPayloadLen + COMM_FRAME_OVERHEAD))
    {
        return COMM_ERR_PARAM;
    }

    memset(pstF, 0, sizeof(*pstF));
    pstF->stCfg   = *pstCfg;
    pstF->u16Mask = (uint16_t)(pstCfg->u16RingSize - 1U);

    return COMM_OK;
}

/**
 * @brief  向解帧器喂入原始字节（ISR 上下文）
 * @param  [in] pstF   解帧器实例
 * @param  [in] pu8Src 原始字节
 * @param  [in] u16Len 字节数
 * @return 实际写入的字节数
 */
uint16_t CommFramer_FeedFromISR(CommFramer_ST *pstF, const uint8_t *pu8Src, uint16_t u16Len)
{
    uint16_t u16Head;
    uint16_t u16Tail;
    uint16_t u16Free;
    uint16_t u16Written;

    if (pstF == NULL || pu8Src == NULL || u16Len == 0U)
    {
        return 0U;
    }

    u16Head = pstF->u16Head;
    u16Tail = pstF->u16Tail;            /* 先读 tail */
    COMM_MEM_BARRIER();

    /* 留一格空位区分空与满 */
    u16Free = (uint16_t)((u16Tail - u16Head - 1U) & pstF->u16Mask);

    u16Written = (u16Len <= u16Free) ? u16Len : u16Free;

    for (uint16_t i = 0U; i < u16Written; i++)
    {
        pstF->stCfg.pu8Ring[u16Head] = pu8Src[i];
        u16Head = (uint16_t)((u16Head + 1U) & pstF->u16Mask);
    }

    COMM_MEM_BARRIER();                 /* 先写数据，再发布 head */
    pstF->u16Head = u16Head;

    if (u16Written < u16Len)
    {
        pstF->stStats.u32RxDropRing += (uint32_t)(u16Len - u16Written);
    }

    return u16Written;
}

/**
 * @brief  取出下一个完整帧（task 上下文）
 * @param  [in]  pstF   解帧器实例
 * @param  [out] pstOut 输出帧信息
 * @retval COMM_OK / COMM_ERR_NOT_READY / COMM_ERR_PARAM
 */
int32_t CommFramer_Next(CommFramer_ST *pstF, CommFrameInfo_ST *pstOut)
{
    uint8_t *pu8Frame;
    uint16_t u16DataLen;
    uint16_t u16Total;

    if (pstF == NULL || pstOut == NULL)
    {
        return COMM_ERR_PARAM;
    }

    pu8Frame = pstF->stCfg.pu8Frame;

    /* 延迟消费：丢弃上一次已交付给调用方的那一帧，把剩余字节前移 */
    if (pstF->u16Consumed > 0U)
    {
        pstF->u16Have = (uint16_t)(pstF->u16Have - pstF->u16Consumed);
        if (pstF->u16Have > 0U)
        {
            memmove(pu8Frame, &pu8Frame[pstF->u16Consumed], pstF->u16Have);
        }
        pstF->u16Consumed = 0U;
    }

    for (;;)
    {
        /* --- 阶段 1：定位帧头 --- */
        while (pstF->u16Have == 0U)
        {
            uint8_t u8Byte;

            if (CommFramer_RingCount(pstF) == 0U)
            {
                return COMM_ERR_NOT_READY;
            }

            u8Byte = CommFramer_RingPop(pstF);
            if (u8Byte == (uint8_t)COMM_FRAME_HEADER)
            {
                pu8Frame[0] = u8Byte;
                pstF->u16Have = 1U;
            }
            else
            {
                pstF->stStats.u32RxSkipped++;
            }
        }

        /* --- 阶段 2：补足帧头部，读出 payload 长度 --- */
        if (CommFramer_Fill(pstF, (uint16_t)(COMM_FRAME_HEADER_BYTES + 1U)) == 0U)
        {
            return COMM_ERR_NOT_READY;
        }

        u16DataLen = (uint16_t)(((uint16_t)pu8Frame[6] << 8) | (uint16_t)pu8Frame[7]);

        if (u16DataLen > pstF->stCfg.u16MaxPayloadLen)
        {
            pstF->stStats.u32RxOversize++;
            CommFramer_Resync(pstF);
            continue;
        }

        /* --- 阶段 3：补足整帧 --- */
        u16Total = (uint16_t)(u16DataLen + COMM_FRAME_OVERHEAD);
        if (CommFramer_Fill(pstF, u16Total) == 0U)
        {
            return COMM_ERR_NOT_READY;
        }

        /* --- 阶段 4：校验 CRC 与帧尾 --- */
        if (CommFrame_VerifyCRC16(&pu8Frame[1], (uint16_t)(u16Total - 2U)) == 0U)
        {
            pstF->stStats.u32RxCrcErr++;
            CommFramer_Resync(pstF);
            continue;
        }

        if (pu8Frame[u16Total - 1U] != (uint8_t)COMM_FRAME_TRAILER)
        {
            pstF->stStats.u32RxBadTail++;
            CommFramer_Resync(pstF);
            continue;
        }

        /* --- 阶段 5：输出并保留缓冲里剩下的字节 --- */
        pstOut->u8DstAddr  = pu8Frame[1];
        pstOut->u8SrcAddr  = pu8Frame[2];
        pstOut->u8Cmd      = pu8Frame[3];
        pstOut->u8Flags    = pu8Frame[4];
        pstOut->u8Seq      = pu8Frame[5];
        pstOut->u16DataLen = u16DataLen;
        pstOut->pu8Data    = (u16DataLen > 0U)
                             ? &pu8Frame[COMM_FRAME_HEADER_BYTES + 1U]
                             : NULL;

        /* 只记录本帧长度，实际前移推迟到下次调用开头 —— 立即前移会覆盖
         * 刚刚交给调用方的 pstOut->pu8Data。 */
        pstF->u16Consumed = u16Total;

        pstF->stStats.u32RxFrames++;
        return COMM_OK;
    }
}

/**
 * @brief  清空解帧器状态
 * @param  [in] pstF 解帧器实例
 */
void CommFramer_Reset(CommFramer_ST *pstF)
{
    if (pstF == NULL)
    {
        return;
    }
    pstF->u16Have     = 0U;
    pstF->u16Consumed = 0U;
    pstF->u16Tail     = pstF->u16Head;
}

/**
 * @brief  获取接收侧统计
 * @param  [in] pstF 解帧器实例
 * @return 统计结构体指针
 */
const CommFramerStats_ST *CommFramer_GetStats(const CommFramer_ST *pstF)
{
    return (pstF != NULL) ? &pstF->stStats : NULL;
}
