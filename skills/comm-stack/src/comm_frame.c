/**
 * @file    comm_frame.c
 * @brief   通信帧线上格式实现
 *
 * @note  CRC 默认使用逐位实现（无查表，省 512B Flash）。若目标平台对大 payload
 *        的 CRC 耗时敏感，可自行改为 256 项查表版本，两者结果完全一致。
 */

#include "comm_frame.h"
#include <string.h>

/* CRC-16/MODBUS 反射多项式 */
#define COMM_CRC16_POLY     0xA001U
#define COMM_CRC16_INIT     0xFFFFU

/**
 * @brief  计算 CRC-16/MODBUS
 * @param  [in] pu8Data 待校验数据指针
 * @param  [in] u16Len  待校验字节数
 * @return 16 位校验值；参数非法时返回 0
 */
uint16_t CommFrame_CalcCRC16(const uint8_t *pu8Data, uint16_t u16Len)
{
    uint16_t u16Crc = COMM_CRC16_INIT;
    uint16_t i;
    uint8_t  u8Bit;

    if (pu8Data == NULL || u16Len == 0U)
    {
        return 0U;
    }

    for (i = 0U; i < u16Len; i++)
    {
        u16Crc ^= (uint16_t)pu8Data[i];
        for (u8Bit = 0U; u8Bit < 8U; u8Bit++)
        {
            if ((u16Crc & 0x0001U) != 0U)
            {
                u16Crc = (uint16_t)((u16Crc >> 1) ^ COMM_CRC16_POLY);
            }
            else
            {
                u16Crc >>= 1;
            }
        }
    }

    return u16Crc;
}

/**
 * @brief  校验末尾携带 2 字节 CRC（小端）的数据块
 * @param  [in] pu8Data 含数据与末尾 CRC 的缓冲区
 * @param  [in] u16Len  总长度（含 2 字节 CRC）
 * @retval 1: 校验通过, 0: 校验失败或参数非法
 */
uint8_t CommFrame_VerifyCRC16(const uint8_t *pu8Data, uint16_t u16Len)
{
    uint16_t u16Calc;
    uint16_t u16Recv;

    if (pu8Data == NULL || u16Len < 3U)
    {
        return 0U;
    }

    u16Calc = CommFrame_CalcCRC16(pu8Data, (uint16_t)(u16Len - 2U));
    u16Recv = (uint16_t)pu8Data[u16Len - 2U] |
              (uint16_t)((uint16_t)pu8Data[u16Len - 1U] << 8);

    return (u16Calc == u16Recv) ? 1U : 0U;
}

/**
 * @brief  组装一个完整帧
 * @param  [in]  u8DstAddr    目标地址
 * @param  [in]  u8SrcAddr    源地址
 * @param  [in]  u8Cmd        命令码
 * @param  [in]  u8Flags      标志位，见 COMM_FRAME_FLAG_*
 * @param  [in]  u8Seq        请求序号
 * @param  [in]  pu8Data      数据段指针
 * @param  [in]  u16DataLen   数据段长度
 * @param  [out] pu8OutBuffer 输出缓冲区
 * @param  [in]  u16OutCap    输出缓冲区容量
 * @param  [out] pu16OutLen   输出完整帧长度
 * @retval COMM_OK / COMM_ERR_PARAM / COMM_ERR_OVERSIZE / COMM_ERR_BUFFER_FULL
 */
int32_t CommFrame_Build(uint8_t u8DstAddr, uint8_t u8SrcAddr,
                        uint8_t u8Cmd, uint8_t u8Flags, uint8_t u8Seq,
                        const uint8_t *pu8Data, uint16_t u16DataLen,
                        uint8_t *pu8OutBuffer, uint16_t u16OutCap,
                        uint16_t *pu16OutLen)
{
    uint16_t u16Idx = 0U;
    uint16_t u16Crc;

    if (pu8OutBuffer == NULL || pu16OutLen == NULL)
    {
        return COMM_ERR_PARAM;
    }

    if (u16DataLen > 0U && pu8Data == NULL)
    {
        return COMM_ERR_PARAM;
    }

    if (u16DataLen > COMM_PAYLOAD_LEN_MAX)
    {
        return COMM_ERR_OVERSIZE;
    }

    if (u16OutCap < (uint16_t)(u16DataLen + COMM_FRAME_OVERHEAD))
    {
        return COMM_ERR_BUFFER_FULL;
    }

    pu8OutBuffer[u16Idx++] = COMM_FRAME_HEADER;
    pu8OutBuffer[u16Idx++] = u8DstAddr;
    pu8OutBuffer[u16Idx++] = u8SrcAddr;
    pu8OutBuffer[u16Idx++] = u8Cmd;
    pu8OutBuffer[u16Idx++] = u8Flags;
    pu8OutBuffer[u16Idx++] = u8Seq;
    pu8OutBuffer[u16Idx++] = (uint8_t)(u16DataLen >> 8);   /* LenH，大端 */
    pu8OutBuffer[u16Idx++] = (uint8_t)(u16DataLen & 0xFFU); /* LenL */

    if (u16DataLen > 0U)
    {
        memcpy(&pu8OutBuffer[u16Idx], pu8Data, u16DataLen);
        u16Idx = (uint16_t)(u16Idx + u16DataLen);
    }

    /* CRC 覆盖 Dst .. Data 末尾 */
    u16Crc = CommFrame_CalcCRC16(&pu8OutBuffer[1], (uint16_t)(u16Idx - 1U));
    pu8OutBuffer[u16Idx++] = (uint8_t)(u16Crc & 0xFFU);      /* CRC 低字节在前 */
    pu8OutBuffer[u16Idx++] = (uint8_t)((u16Crc >> 8) & 0xFFU);
    pu8OutBuffer[u16Idx++] = COMM_FRAME_TRAILER;

    *pu16OutLen = u16Idx;
    return COMM_OK;
}

/**
 * @brief  解析一个完整帧
 * @param  [in]  pu8Buffer 帧缓冲区
 * @param  [in]  u16Len    缓冲区字节数，须恰好等于帧总长度
 * @param  [out] pstOut    输出帧信息
 * @retval COMM_OK / COMM_ERR_PARAM / COMM_ERR_FORMAT / COMM_ERR_CRC
 */
int32_t CommFrame_Parse(uint8_t *pu8Buffer, uint16_t u16Len, CommFrameInfo_ST *pstOut)
{
    uint16_t u16DataLen;

    if (pu8Buffer == NULL || pstOut == NULL)
    {
        return COMM_ERR_PARAM;
    }

    if (u16Len < COMM_FRAME_MIN_LEN)
    {
        return COMM_ERR_FORMAT;
    }

    if (pu8Buffer[0] != COMM_FRAME_HEADER ||
        pu8Buffer[u16Len - 1U] != COMM_FRAME_TRAILER)
    {
        return COMM_ERR_FORMAT;
    }

    u16DataLen = (uint16_t)(((uint16_t)pu8Buffer[6] << 8) | (uint16_t)pu8Buffer[7]);
    if (u16Len != (uint16_t)(u16DataLen + COMM_FRAME_OVERHEAD))
    {
        return COMM_ERR_FORMAT;
    }

    /* 校验区：Dst .. CRC 高字节，即偏移 1 .. u16Len-2 */
    if (CommFrame_VerifyCRC16(&pu8Buffer[1], (uint16_t)(u16Len - 2U)) == 0U)
    {
        return COMM_ERR_CRC;
    }

    pstOut->u8DstAddr  = pu8Buffer[1];
    pstOut->u8SrcAddr  = pu8Buffer[2];
    pstOut->u8Cmd      = pu8Buffer[3];
    pstOut->u8Flags    = pu8Buffer[4];
    pstOut->u8Seq      = pu8Buffer[5];
    pstOut->u16DataLen = u16DataLen;
    pstOut->pu8Data    = (u16DataLen > 0U) ? &pu8Buffer[COMM_FRAME_HEADER_BYTES + 1U] : NULL;

    return COMM_OK;
}
