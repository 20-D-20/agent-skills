/**
 * @file    comm_frame.h
 * @brief   通信帧线上格式：组帧、解帧、CRC 校验（纯函数，无状态，无平台依赖）
 *
 * 线上格式（大端长度字段，CRC 小端）：
 *
 *   偏移   0      1     2     3      4     5     6      7      8 .. 7+N   8+N  9+N  10+N
 *        +------+-----+-----+-----+-------+-----+------+------+---------+-----+-----+------+
 *        | 0xAA | Dst | Src | Cmd | Flags | Seq | LenH | LenL | Data[N] | CRC | CRC | 0x55 |
 *        +------+-----+-----+-----+-------+-----+------+------+---------+-----+-----+------+
 *                                                                          L     H
 *
 *   总长度 = N + COMM_FRAME_OVERHEAD (11)
 *   CRC-16/MODBUS 覆盖 Dst .. Data 末尾（即偏移 1 .. 7+N），低字节在前
 *
 *   Flags.bit0 (COMM_FRAME_FLAG_RESPONSE)：
 *       0 = 请求帧，交给命令表分发
 *       1 = 应答帧，只用于匹配本机待确认的请求，永不进入命令分发
 *   没有这一位，一个应答会被对端误当成新请求再回一个应答，形成乒乓 ——
 *   这是本框架 v1 遗留的真实缺陷，单元测试 test_link 会复现它
 *
 * @note  本文件必须在通信双方保持完全一致，任一常量不同都会导致全帧丢弃且无任何提示
 */

#ifndef __COMM_FRAME_H__
#define __COMM_FRAME_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

/* ============================================================================
 * 线上格式常量 —— 移植时可改，但两端必须同步
 * ============================================================================ */

#ifndef COMM_FRAME_HEADER
#define COMM_FRAME_HEADER       0xAAU   /* 帧头标识 */
#endif

#ifndef COMM_FRAME_TRAILER
#define COMM_FRAME_TRAILER      0x55U   /* 帧尾标识 */
#endif

#ifndef COMM_PAYLOAD_LEN_MAX
#define COMM_PAYLOAD_LEN_MAX    1024U   /* 协议允许的最大 payload，两端必须一致 */
#endif

/* 帧固定开销：Header + Dst + Src + Cmd + Flags + Seq + Len(2) + CRC(2) + Trailer */
#define COMM_FRAME_OVERHEAD     11U
/* 帧头部字节数（Header 之后到 Data 之前）：Dst Src Cmd Flags Seq LenH LenL */
#define COMM_FRAME_HEADER_BYTES 7U

#define COMM_FRAME_MIN_LEN      COMM_FRAME_OVERHEAD
#define COMM_FRAME_LEN_MAX      (COMM_PAYLOAD_LEN_MAX + COMM_FRAME_OVERHEAD)

/* Flags 位定义 */
#define COMM_FRAME_FLAG_RESPONSE    0x01U   /* 置位表示这是一个应答帧 */

/* ============================================================================
 * 错误码 —— 数值与框架 v1 保持一致，仅追加新码
 * ============================================================================ */

typedef enum
{
    COMM_OK                =   0,   /* 成功 */
    COMM_ERR_PARAM         =  -1,   /* 参数错误 */
    COMM_ERR_FORMAT        =  -2,   /* 帧格式错误 */
    COMM_ERR_CRC           =  -3,   /* CRC 校验错误 */
    COMM_ERR_TIMEOUT       =  -4,   /* 通讯超时 */
    COMM_ERR_BUSY          =  -5,   /* 设备忙 */
    COMM_ERR_NACK          =  -6,   /* 对端返回 NACK */
    COMM_ERR_BUFFER_FULL   =  -7,   /* 缓冲区不足 */
    COMM_ERR_HARDWARE      =  -8,   /* 硬件故障 */
    COMM_ERR_NOT_SUPPORTED =  -9,   /* 不支持的命令 */
    COMM_ERR_NOT_READY     = -10,   /* 数据未就绪 */
    COMM_ERR_OVERSIZE      = -11    /* 超出本实例配额 */
} CommError_E;

/* ============================================================================
 * 协议状态码（应答 payload 的第 0 字节）
 * ============================================================================ */

typedef enum
{
    COMM_STATUS_OK            = 0x00U,
    COMM_STATUS_BUSY          = 0x01U,
    COMM_STATUS_INVALID_CMD   = 0x02U,
    COMM_STATUS_INVALID_PARAM = 0x03U,
    COMM_STATUS_HW_ERROR      = 0x04U,
    COMM_STATUS_NOT_READY     = 0x05U,
    COMM_STATUS_DENIED        = 0x06U
} CommStatus_E;

/* 广播地址：目标地址为该值的帧被视为广播帧 */
#ifndef COMM_ADDR_BROADCAST
#define COMM_ADDR_BROADCAST     0xFFU
#endif

/* ============================================================================
 * 解析结果
 * ============================================================================ */

/**
 * @brief  帧解析结果（pu8Data 指向调用方提供的缓冲区内部，不拥有内存）
 */
typedef struct
{
    uint8_t   u8DstAddr;    /* 目标地址 */
    uint8_t   u8SrcAddr;    /* 源地址 */
    uint8_t   u8Cmd;        /* 命令码 */
    uint8_t   u8Flags;      /* 标志位，见 COMM_FRAME_FLAG_* */
    uint8_t   u8Seq;        /* 请求序号 */
    uint16_t  u16DataLen;   /* 数据段长度 */
    uint8_t  *pu8Data;      /* 数据段指针，u16DataLen 为 0 时为 NULL */
} CommFrameInfo_ST;

/* ============================================================================
 * 接口
 * ============================================================================ */

/**
 * @brief  计算 CRC-16/MODBUS（初值 0xFFFF，多项式 0xA001 反射）
 * @param  [in] pu8Data 待校验数据指针
 * @param  [in] u16Len  待校验字节数
 * @return 16 位校验值；参数非法时返回 0
 */
uint16_t CommFrame_CalcCRC16(const uint8_t *pu8Data, uint16_t u16Len);

/**
 * @brief  校验末尾携带 2 字节 CRC（小端）的数据块
 * @param  [in] pu8Data 含数据与末尾 CRC 的缓冲区
 * @param  [in] u16Len  总长度（含 2 字节 CRC）
 * @retval 1: 校验通过, 0: 校验失败或参数非法
 */
uint8_t CommFrame_VerifyCRC16(const uint8_t *pu8Data, uint16_t u16Len);

/**
 * @brief  组装一个完整帧
 * @param  [in]  u8DstAddr    目标地址
 * @param  [in]  u8SrcAddr    源地址
 * @param  [in]  u8Cmd        命令码
 * @param  [in]  u8Flags      标志位，见 COMM_FRAME_FLAG_*
 * @param  [in]  u8Seq        请求序号
 * @param  [in]  pu8Data      数据段指针（u16DataLen 为 0 时可为 NULL）
 * @param  [in]  u16DataLen   数据段长度，须 <= COMM_PAYLOAD_LEN_MAX
 * @param  [out] pu8OutBuffer 输出缓冲区，容量须 >= u16DataLen + COMM_FRAME_OVERHEAD
 * @param  [in]  u16OutCap    输出缓冲区容量
 * @param  [out] pu16OutLen   输出完整帧长度
 * @retval COMM_OK: 成功, COMM_ERR_PARAM: 参数非法,
 *         COMM_ERR_OVERSIZE: 超过协议上限, COMM_ERR_BUFFER_FULL: 输出缓冲区不足
 */
int32_t CommFrame_Build(uint8_t u8DstAddr, uint8_t u8SrcAddr,
                        uint8_t u8Cmd, uint8_t u8Flags, uint8_t u8Seq,
                        const uint8_t *pu8Data, uint16_t u16DataLen,
                        uint8_t *pu8OutBuffer, uint16_t u16OutCap,
                        uint16_t *pu16OutLen);

/**
 * @brief  解析一个完整帧（要求 u16Len 恰好等于该帧的总长度）
 * @param  [in]  pu8Buffer  帧缓冲区
 * @param  [in]  u16Len     缓冲区中的字节数
 * @param  [out] pstOut     输出帧信息，pu8Data 指向 pu8Buffer 内部
 * @retval COMM_OK: 成功, COMM_ERR_PARAM: 参数非法,
 *         COMM_ERR_FORMAT: 帧头/帧尾/长度不符, COMM_ERR_CRC: CRC 错误
 * @note   本函数供测试与"整包即整帧"的简单场景使用；实际接收路径应使用
 *         comm_framer 的字节流状态机，它能处理粘包、拆包与前导噪声
 */
int32_t CommFrame_Parse(uint8_t *pu8Buffer, uint16_t u16Len, CommFrameInfo_ST *pstOut);

#ifdef __cplusplus
}
#endif

#endif /* __COMM_FRAME_H__ */
