/**
 * @file    test_frame.c
 * @brief   comm_frame 单元测试：CRC、组帧、解帧边界
 */

#include "comm_frame.h"
#include "test_util.h"
#include <string.h>

static uint8_t s_au8Buf[COMM_FRAME_LEN_MAX + 32U];
static uint8_t s_au8Payload[COMM_PAYLOAD_LEN_MAX];

/**
 * @brief  组帧后立刻解帧，验证各字段与 payload 原样还原
 * @param  [in] u16Len 本轮使用的 payload 长度
 */
static void RoundTrip(uint16_t u16Len)
{
    CommFrameInfo_ST stInfo;
    uint16_t u16Total = 0U;
    uint16_t i;

    for (i = 0U; i < u16Len; i++)
    {
        s_au8Payload[i] = (uint8_t)(i * 7U + 3U);
    }

    CHECK_EQ(CommFrame_Build(0x02U, 0x01U, 0x55U, 0U, 0x7BU,
                             s_au8Payload, u16Len,
                             s_au8Buf, sizeof(s_au8Buf), &u16Total), COMM_OK);
    CHECK_EQ(u16Total, u16Len + COMM_FRAME_OVERHEAD);

    memset(&stInfo, 0, sizeof(stInfo));
    CHECK_EQ(CommFrame_Parse(s_au8Buf, u16Total, &stInfo), COMM_OK);
    CHECK_EQ(stInfo.u8DstAddr, 0x02U);
    CHECK_EQ(stInfo.u8SrcAddr, 0x01U);
    CHECK_EQ(stInfo.u8Cmd, 0x55U);
    CHECK_EQ(stInfo.u8Seq, 0x7BU);
    CHECK_EQ(stInfo.u16DataLen, u16Len);
    if (u16Len > 0U)
    {
        CHECK(stInfo.pu8Data != NULL);
        CHECK_EQ(memcmp(stInfo.pu8Data, s_au8Payload, u16Len), 0);
    }
    else
    {
        CHECK(stInfo.pu8Data == NULL);
    }
}

/**
 * @brief  comm_frame 测试入口
 */
void test_frame(void)
{
    CommFrameInfo_ST stInfo;
    uint16_t u16Total = 0U;

    CASE("CRC-16/MODBUS 标准向量");
    {
        /* "123456789" 的 CRC-16/MODBUS 标准值为 0x4B37 */
        const uint8_t au8Vec[] = { '1','2','3','4','5','6','7','8','9' };
        CHECK_EQ(CommFrame_CalcCRC16(au8Vec, sizeof(au8Vec)), 0x4B37U);
    }

    CASE("组帧/解帧往返：0 / 1 / 255 / 256 / 1024 字节 payload");
    RoundTrip(0U);
    RoundTrip(1U);
    RoundTrip(255U);
    RoundTrip(256U);
    RoundTrip(COMM_PAYLOAD_LEN_MAX);

    CASE("payload 含 0xAA / 0x55 不影响解析");
    {
        const uint8_t au8Tricky[] = { 0xAAU, 0x55U, 0xAAU, 0xAAU, 0x55U };
        CHECK_EQ(CommFrame_Build(0x02U, 0x01U, 0x10U, 0U, 0x01U,
                                 au8Tricky, sizeof(au8Tricky),
                                 s_au8Buf, sizeof(s_au8Buf), &u16Total), COMM_OK);
        CHECK_EQ(CommFrame_Parse(s_au8Buf, u16Total, &stInfo), COMM_OK);
        CHECK_EQ(stInfo.u16DataLen, sizeof(au8Tricky));
        CHECK_EQ(memcmp(stInfo.pu8Data, au8Tricky, sizeof(au8Tricky)), 0);
    }

    CASE("Build 拒绝超协议上限与容量不足");
    CHECK_EQ(CommFrame_Build(1U, 2U, 3U, 0U, 4U, s_au8Payload,
                             COMM_PAYLOAD_LEN_MAX + 1U,
                             s_au8Buf, sizeof(s_au8Buf), &u16Total),
             COMM_ERR_OVERSIZE);
    CHECK_EQ(CommFrame_Build(1U, 2U, 3U, 0U, 4U, s_au8Payload, 100U,
                             s_au8Buf, 50U, &u16Total),
             COMM_ERR_BUFFER_FULL);
    CHECK_EQ(CommFrame_Build(1U, 2U, 3U, 0U, 4U, NULL, 5U,
                             s_au8Buf, sizeof(s_au8Buf), &u16Total),
             COMM_ERR_PARAM);

    CASE("Parse 拒绝坏帧头 / 坏帧尾 / 长度不符 / CRC 错");
    CHECK_EQ(CommFrame_Build(0x02U, 0x01U, 0x10U, 0U, 0x01U, s_au8Payload, 4U,
                             s_au8Buf, sizeof(s_au8Buf), &u16Total), COMM_OK);

    s_au8Buf[0] = 0x00U;
    CHECK_EQ(CommFrame_Parse(s_au8Buf, u16Total, &stInfo), COMM_ERR_FORMAT);
    s_au8Buf[0] = (uint8_t)COMM_FRAME_HEADER;

    s_au8Buf[u16Total - 1U] = 0x00U;
    CHECK_EQ(CommFrame_Parse(s_au8Buf, u16Total, &stInfo), COMM_ERR_FORMAT);
    s_au8Buf[u16Total - 1U] = (uint8_t)COMM_FRAME_TRAILER;

    CHECK_EQ(CommFrame_Parse(s_au8Buf, (uint16_t)(u16Total - 1U), &stInfo),
             COMM_ERR_FORMAT);

    s_au8Buf[8] ^= 0x01U;   /* 翻转 payload 里一个 bit */
    CHECK_EQ(CommFrame_Parse(s_au8Buf, u16Total, &stInfo), COMM_ERR_CRC);
    s_au8Buf[8] ^= 0x01U;

    CHECK_EQ(CommFrame_Parse(s_au8Buf, u16Total, &stInfo), COMM_OK);

    CASE("Parse 拒绝过短缓冲与空指针");
    CHECK_EQ(CommFrame_Parse(s_au8Buf, 3U, &stInfo), COMM_ERR_FORMAT);
    CHECK_EQ(CommFrame_Parse(NULL, u16Total, &stInfo), COMM_ERR_PARAM);
    CHECK_EQ(CommFrame_Parse(s_au8Buf, u16Total, NULL), COMM_ERR_PARAM);
}
