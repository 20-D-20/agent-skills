/**
 * @file    test_framer.c
 * @brief   comm_framer 单元测试：粘包、拆包、前导噪声、坏帧后的 resync
 *
 * 其中"坏帧后能捞回紧跟的好帧"是本模块存在的核心理由，也是框架 v1 的丢帧根因。
 */

#include "comm_framer.h"
#include "test_util.h"
#include <string.h>

#define TEST_RING_SIZE      512U
#define TEST_MAX_PAYLOAD    64U
#define TEST_FRAME_CAP      (TEST_MAX_PAYLOAD + COMM_FRAME_OVERHEAD)

static uint8_t       s_au8Ring[TEST_RING_SIZE];
static uint8_t       s_au8Frame[TEST_FRAME_CAP];
static CommFramer_ST s_stF;
static uint8_t       s_au8Scratch[512];

/**
 * @brief  重新初始化被测解帧器
 */
static void FramerReset(void)
{
    CommFramerCfg_ST stCfg;

    stCfg.pu8Ring          = s_au8Ring;
    stCfg.u16RingSize      = TEST_RING_SIZE;
    stCfg.pu8Frame         = s_au8Frame;
    stCfg.u16FrameCap      = TEST_FRAME_CAP;
    stCfg.u16MaxPayloadLen = TEST_MAX_PAYLOAD;

    CHECK_EQ(CommFramer_Init(&s_stF, &stCfg), COMM_OK);
}

/**
 * @brief  在缓冲区里造一帧
 * @param  [out] pu8Out  输出缓冲
 * @param  [in]  u8Cmd   命令码
 * @param  [in]  u8Seq   序号
 * @param  [in]  u16Len  payload 长度
 * @return 帧总长度
 */
static uint16_t MakeFrame(uint8_t *pu8Out, uint8_t u8Cmd, uint8_t u8Seq, uint16_t u16Len)
{
    uint8_t  au8Payload[TEST_MAX_PAYLOAD];
    uint16_t u16Total = 0U;
    uint16_t i;

    for (i = 0U; i < u16Len; i++)
    {
        au8Payload[i] = (uint8_t)(u8Seq + i);
    }

    CHECK_EQ(CommFrame_Build(0x02U, 0x01U, u8Cmd, 0U, u8Seq,
                             au8Payload, u16Len,
                             pu8Out, TEST_FRAME_CAP, &u16Total), COMM_OK);
    return u16Total;
}

/**
 * @brief  期望取出一帧并校验 cmd/seq
 * @param  [in] u8Cmd 期望命令码
 * @param  [in] u8Seq 期望序号
 */
static void ExpectFrame(uint8_t u8Cmd, uint8_t u8Seq)
{
    CommFrameInfo_ST stInfo;

    memset(&stInfo, 0, sizeof(stInfo));
    CHECK_EQ(CommFramer_Next(&s_stF, &stInfo), COMM_OK);
    CHECK_EQ(stInfo.u8Cmd, u8Cmd);
    CHECK_EQ(stInfo.u8Seq, u8Seq);
}

/**
 * @brief  期望当前取不出帧
 */
static void ExpectEmpty(void)
{
    CommFrameInfo_ST stInfo;
    CHECK_EQ(CommFramer_Next(&s_stF, &stInfo), COMM_ERR_NOT_READY);
}

/**
 * @brief  comm_framer 测试入口
 */
void test_framer(void)
{
    uint16_t u16LenA;
    uint16_t u16LenB;
    uint16_t u16LenC;
    uint16_t i;

    CASE("单帧：一次喂入完整帧");
    FramerReset();
    u16LenA = MakeFrame(s_au8Scratch, 0x10U, 0x01U, 4U);
    CommFramer_FeedFromISR(&s_stF, s_au8Scratch, u16LenA);
    ExpectFrame(0x10U, 0x01U);
    ExpectEmpty();

    CASE("粘包：三帧连在一个数据块里");
    FramerReset();
    u16LenA = MakeFrame(&s_au8Scratch[0], 0x11U, 0x01U, 0U);
    u16LenB = MakeFrame(&s_au8Scratch[u16LenA], 0x12U, 0x02U, 8U);
    u16LenC = MakeFrame(&s_au8Scratch[u16LenA + u16LenB], 0x13U, 0x03U, 3U);
    CommFramer_FeedFromISR(&s_stF, s_au8Scratch,
                           (uint16_t)(u16LenA + u16LenB + u16LenC));
    ExpectFrame(0x11U, 0x01U);
    ExpectFrame(0x12U, 0x02U);
    ExpectFrame(0x13U, 0x03U);
    ExpectEmpty();
    CHECK_EQ(CommFramer_GetStats(&s_stF)->u32RxFrames, 3U);

    CASE("拆包：逐字节喂入，最后一字节到位才成帧");
    FramerReset();
    u16LenA = MakeFrame(s_au8Scratch, 0x14U, 0x04U, 10U);
    for (i = 0U; i + 1U < u16LenA; i++)
    {
        CommFramer_FeedFromISR(&s_stF, &s_au8Scratch[i], 1U);
        ExpectEmpty();
    }
    CommFramer_FeedFromISR(&s_stF, &s_au8Scratch[u16LenA - 1U], 1U);
    ExpectFrame(0x14U, 0x04U);

    CASE("拆包：一帧被切成两段分两次喂入");
    FramerReset();
    u16LenA = MakeFrame(s_au8Scratch, 0x15U, 0x05U, 20U);
    CommFramer_FeedFromISR(&s_stF, s_au8Scratch, 7U);
    ExpectEmpty();
    CommFramer_FeedFromISR(&s_stF, &s_au8Scratch[7], (uint16_t)(u16LenA - 7U));
    ExpectFrame(0x15U, 0x05U);

    CASE("前导噪声：帧前的垃圾字节被跳过并计数");
    FramerReset();
    {
        uint8_t au8Noise[5] = { 0x00U, 0xFFU, 0x12U, 0x55U, 0x34U };
        CommFramer_FeedFromISR(&s_stF, au8Noise, sizeof(au8Noise));
        u16LenA = MakeFrame(s_au8Scratch, 0x16U, 0x06U, 2U);
        CommFramer_FeedFromISR(&s_stF, s_au8Scratch, u16LenA);
        ExpectFrame(0x16U, 0x06U);
        CHECK_EQ(CommFramer_GetStats(&s_stF)->u32RxSkipped, 5U);
    }

    CASE("resync 关键用例：CRC 坏帧之后紧跟的好帧必须能被捞回");
    FramerReset();
    u16LenA = MakeFrame(&s_au8Scratch[0], 0x17U, 0x07U, 6U);
    s_au8Scratch[8] ^= 0x01U;      /* 破坏第一帧的 payload */
    u16LenB = MakeFrame(&s_au8Scratch[u16LenA], 0x18U, 0x08U, 6U);
    CommFramer_FeedFromISR(&s_stF, s_au8Scratch, (uint16_t)(u16LenA + u16LenB));
    ExpectFrame(0x18U, 0x08U);     /* 坏帧被跳过，好帧完整取出 */
    ExpectEmpty();
    CHECK(CommFramer_GetStats(&s_stF)->u32RxCrcErr >= 1U);
    CHECK(CommFramer_GetStats(&s_stF)->u32RxResync >= 1U);

    CASE("resync：坏帧尾之后紧跟的好帧同样能捞回");
    FramerReset();
    u16LenA = MakeFrame(&s_au8Scratch[0], 0x19U, 0x09U, 4U);
    u16LenB = MakeFrame(&s_au8Scratch[u16LenA], 0x1AU, 0x0AU, 4U);
    /* 改帧尾会同时破坏帧尾检查（CRC 不覆盖帧尾） */
    s_au8Scratch[u16LenA - 1U] = 0x00U;
    CommFramer_FeedFromISR(&s_stF, s_au8Scratch, (uint16_t)(u16LenA + u16LenB));
    ExpectFrame(0x1AU, 0x0AU);
    CHECK_EQ(CommFramer_GetStats(&s_stF)->u32RxBadTail, 1U);

    CASE("长度字段超本实例配额：判废并 resync，后续好帧不受影响");
    FramerReset();
    {
        uint8_t au8Bogus[COMM_FRAME_OVERHEAD];
        memset(au8Bogus, 0, sizeof(au8Bogus));
        au8Bogus[0] = (uint8_t)COMM_FRAME_HEADER;
        au8Bogus[6] = 0x03U;   /* LenH：声称 payload 长度 0x03E8 = 1000 > 64 */
        au8Bogus[7] = 0xE8U;
        CommFramer_FeedFromISR(&s_stF, au8Bogus, sizeof(au8Bogus));
        u16LenA = MakeFrame(s_au8Scratch, 0x1BU, 0x0BU, 5U);
        CommFramer_FeedFromISR(&s_stF, s_au8Scratch, u16LenA);
        ExpectFrame(0x1BU, 0x0BU);
        CHECK_EQ(CommFramer_GetStats(&s_stF)->u32RxOversize, 1U);
    }

    CASE("payload 里含 0xAA 时，resync 不会把它误认成帧头而卡死");
    FramerReset();
    {
        uint8_t au8Payload[8];
        uint16_t u16Total = 0U;

        memset(au8Payload, 0xAAU, sizeof(au8Payload));
        CHECK_EQ(CommFrame_Build(0x02U, 0x01U, 0x1CU, 0U, 0x0CU,
                                 au8Payload, sizeof(au8Payload),
                                 s_au8Scratch, TEST_FRAME_CAP, &u16Total), COMM_OK);
        CommFramer_FeedFromISR(&s_stF, s_au8Scratch, u16Total);
        ExpectFrame(0x1CU, 0x0CU);
    }

    CASE("环形缓冲写满：溢出字节被丢弃并计数");
    FramerReset();
    {
        uint8_t au8Bulk[TEST_RING_SIZE];
        uint16_t u16Written;

        memset(au8Bulk, 0x00U, sizeof(au8Bulk));
        /* 环留一格区分空满，因此最多能写入 TEST_RING_SIZE-1 字节 */
        u16Written = CommFramer_FeedFromISR(&s_stF, au8Bulk, TEST_RING_SIZE);
        CHECK_EQ(u16Written, TEST_RING_SIZE - 1U);
        CHECK_EQ(CommFramer_GetStats(&s_stF)->u32RxDropRing, 1U);
    }

    CASE("Init 拒绝非法配置");
    {
        CommFramerCfg_ST stBad;
        CommFramer_ST    stTmp;

        stBad.pu8Ring          = s_au8Ring;
        stBad.u16RingSize      = 100U;          /* 不是 2 的幂 */
        stBad.pu8Frame         = s_au8Frame;
        stBad.u16FrameCap      = TEST_FRAME_CAP;
        stBad.u16MaxPayloadLen = TEST_MAX_PAYLOAD;
        CHECK_EQ(CommFramer_Init(&stTmp, &stBad), COMM_ERR_PARAM);

        stBad.u16RingSize = TEST_RING_SIZE;
        stBad.u16FrameCap = 16U;                /* 组帧缓冲容量不足 */
        CHECK_EQ(CommFramer_Init(&stTmp, &stBad), COMM_ERR_PARAM);

        stBad.u16FrameCap      = TEST_FRAME_CAP;
        stBad.u16MaxPayloadLen = COMM_PAYLOAD_LEN_MAX + 1U;
        CHECK_EQ(CommFramer_Init(&stTmp, &stBad), COMM_ERR_PARAM);
    }
}
