/**
 * @file    comm_link.h
 * @brief   通信链路实例：一个实例 = 一条完整独立的协议栈
 *
 * 一条链路拥有自己的解帧器、seq 计数器、发送槽、去重缓存与命令表集合，
 * 实例之间零共享。开 N 个串口就定义 N 个实例。
 *
 * 三条铁律：
 *  1. 所有缓冲区由调用方以 static 数组提供，框架零动态分配
 *  2. 框架核心不依赖 FreeRTOS：时间由 CommLink_Tick() 的入参传入，
 *     互斥由 config 注入，ISR 路径是无锁环形缓冲
 *  3. 除 CommLink_FeedFromISR() 外，所有接口都必须在拥有该链路的 task 上下文调用；
 *     CommLink_Post() 是唯一例外，注入 pfLock/pfUnlock 后可跨 task 调用
 *
 * 典型 task 骨架：
 * @code
 *   CommLink_Init(&s_stLink, &s_stLinkCfg);
 *   CommLink_RegisterCmdTable(&s_stLink, s_astAppCmdTable, APP_CMD_COUNT);
 *   for (;;) {
 *       CommLink_Tick(&s_stLink, osKernelSysTick());
 *       osDelay(5);
 *   }
 * @endcode
 */

#ifndef __COMM_LINK_H__
#define __COMM_LINK_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "comm_frame.h"
#include "comm_framer.h"
#include "comm_dispatch.h"
#include <stdint.h>

typedef struct CommLink_ST CommLink_ST;

/* ============================================================================
 * 发送结果与回调
 * ============================================================================ */

typedef enum
{
    COMM_TX_OK = 0,      /* 收到对端 STATUS_OK */
    COMM_TX_REJECTED,    /* 收到对端非 OK 状态，不再重传 */
    COMM_TX_TIMEOUT,     /* 重试耗尽仍未收到 ACK */
    COMM_TX_SEND_FAIL    /* 底层 pfSendRaw 反复失败，放弃 */
} CommTxResult_E;

/**
 * @brief  单次发送的完成回调
 * @param  [in] pstLink   所属链路
 * @param  [in] pvCtx     Post 时传入的用户上下文
 * @param  [in] eResult   发送结果
 * @param  [in] u8Status  对端返回的协议状态码（仅 OK/REJECTED 有效，否则为 0xFF）
 * @param  [in] pu8AckData ACK payload 中 status 之后的附加数据（可为 NULL）
 * @param  [in] u16AckLen  附加数据长度
 * @note   在拥有该链路的 task 上下文同步调用，不得阻塞。pu8AckData 仅在回调
 *         期间有效，需要留存请自行拷贝
 */
typedef void (*CommTxDone_F)(CommLink_ST *pstLink, void *pvCtx,
                             CommTxResult_E eResult, uint8_t u8Status,
                             const uint8_t *pu8AckData, uint16_t u16AckLen);

/**
 * @brief  一次发送请求
 */
typedef struct
{
    uint8_t        u8DstAddr;   /* 目标地址 */
    uint8_t        u8Cmd;       /* 命令码 */
    const uint8_t *pu8Data;     /* payload（u16DataLen 为 0 时可为 NULL） */
    uint16_t       u16DataLen;  /* payload 长度 */
    uint8_t        u8NeedAck;   /* 1: 等待 ACK 并按需重传, 0: 发出即忘 */
    CommTxDone_F   pfDone;      /* 完成回调，可为 NULL */
    void          *pvCtx;       /* 透传给回调的用户上下文 */
} CommTxReq_ST;

/* ============================================================================
 * 内部表项（由调用方以静态数组提供，不需要手工填内容）
 * ============================================================================ */

/** @brief 发送槽状态 */
typedef enum
{
    COMM_SLOT_FREE = 0,   /* 空闲 */
    COMM_SLOT_READY,      /* 已组帧，等待 Tick 发出 */
    COMM_SLOT_WAIT_ACK    /* 已发出，等待 ACK */
} CommSlotState_E;

/** @brief 发送槽 */
typedef struct
{
    uint8_t       u8State;      /* CommSlotState_E */
    uint8_t       u8DstAddr;
    uint8_t       u8Cmd;
    uint8_t       u8Seq;
    uint8_t       u8NeedAck;
    uint8_t       u8RetryCnt;
    uint8_t       u8Armed;      /* 1: u32DeadlineMs 已由 Tick 设定 */
    uint16_t      u16FrameLen;
    uint32_t      u32DeadlineMs;
    uint8_t      *pu8Frame;     /* 指向 cfg->pu8SlotFrameBuf 中本槽的那一段 */
    CommTxDone_F  pfDone;
    void         *pvCtx;
} CommTxSlot_ST;

/** @brief 去重缓存项 */
typedef struct
{
    uint8_t   u8Valid;
    uint8_t   u8SrcAddr;
    uint8_t   u8Cmd;
    uint8_t   u8Seq;
    uint16_t  u16ReqLen;
    uint16_t  u16ReqCrc;    /* 请求 payload 的 CRC16 摘要，避免缓存整个请求体 */
    uint32_t  u32StoredMs;
    uint16_t  u16RespLen;
    uint8_t  *pu8Resp;      /* 指向 cfg->pu8DupRespBuf 中本项的那一段 */
} CommDupEntry_ST;

/* ============================================================================
 * 统计
 * ============================================================================ */

typedef struct
{
    CommFramerStats_ST stFramer;          /* 接收解帧统计，由 GetStats 填充 */
    uint32_t u32RxNotForMe;               /* 目标地址非本机而丢弃 */
    uint32_t u32RxBroadcastDropped;       /* 广播帧被丢弃（未开启 u8AcceptBroadcast） */
    uint32_t u32RxUnknownCmd;             /* 命令表未命中 */
    uint32_t u32RxBadLen;                 /* payload 长度不在表项允许区间 */
    uint32_t u32DupHit;                   /* 去重缓存命中，直接重发历史 ACK */
    uint32_t u32AckConsumed;              /* 入站帧被消费为本机主动发帧的 ACK */
    uint32_t u32RxOrphanResp;             /* 无主应答帧（已超时放弃或不属于本机）被丢弃 */
    uint32_t u32TxPosted;                 /* Post 成功入槽 */
    uint32_t u32TxDropFull;               /* 发送槽满而丢弃 */
    uint32_t u32TxSendFail;               /* pfSendRaw 返回失败 */
    uint32_t u32TxRetry;                  /* ACK 超时触发的重传次数 */
    uint32_t u32TxTimeout;                /* 重试耗尽的请求数 */
    uint32_t u32TxRejected;               /* 收到非 OK 状态的请求数 */
} CommLinkStats_ST;

/* ============================================================================
 * 配置
 * ============================================================================ */

/**
 * @brief  链路配置
 * @note   所有缓冲区必须常驻内存（static 数组），框架只保存指针不做拷贝。
 *         推荐用 templates/link_instance.c.tmpl 生成，避免漏填字段。
 */
typedef struct
{
    /* --- 身份 --- */
    const char *pcName;             /* 日志前缀，如 "gun"；可为 NULL */
    uint8_t     u8SelfAddr;         /* 本实例的本机地址 */
    uint8_t     u8AcceptBroadcast;  /* 1: 处理广播帧但不回 ACK, 0: 丢弃广播帧 */
    uint16_t    u16MaxPayloadLen;   /* 本实例接受/发送的 payload 上限，<= COMM_PAYLOAD_LEN_MAX */

    /* --- 接收 --- */
    uint8_t  *pu8Ring;              /* ISR 字节环，大小必须是 2 的幂 */
    uint16_t  u16RingSize;
    uint8_t  *pu8RxFrame;           /* 组帧缓冲，容量 >= u16MaxPayloadLen + COMM_FRAME_OVERHEAD */
    uint16_t  u16RxFrameCap;

    /* --- 应答（handler 直接写在最终帧的 payload 位置，零拷贝） --- */
    uint8_t  *pu8RespFrame;         /* 应答组帧缓冲 */
    uint16_t  u16RespFrameCap;      /* >= 1(status) + 最大应答附加数据 + COMM_FRAME_OVERHEAD */

    /* --- 发送槽 --- */
    CommTxSlot_ST *pstSlots;        /* 发送槽数组 */
    uint8_t        u8SlotCount;     /* 槽数量，>= 1 */
    uint8_t       *pu8SlotFrameBuf; /* 槽帧缓冲，大小 = u8SlotCount * u16SlotFrameCap */
    uint16_t       u16SlotFrameCap; /* 单槽容量，>= u16MaxPayloadLen + COMM_FRAME_OVERHEAD */
    uint16_t       u16AckTimeoutMs; /* ACK 等待超时 */
    uint8_t        u8MaxRetry;      /* 最大重传次数（0 表示不重传） */

    /* --- 去重（u8DupCount 为 0 即完全关闭） --- */
    CommDupEntry_ST *pstDup;
    uint8_t          u8DupCount;
    uint8_t         *pu8DupRespBuf; /* 大小 = u8DupCount * u16DupRespCap */
    uint16_t         u16DupRespCap; /* 单项容量；应答帧超过它时不缓存 */
    uint32_t         u32DupTtlMs;   /* 缓存有效期 */

    /* --- 平台注入 --- */
    /**
     * @brief  底层发送，由使用方实现（通常是一句 HAL_UART_Transmit_DMA）
     * @retval COMM_OK: 已启动发送, COMM_ERR_BUSY: 通道忙（框架会在下次 Tick 重试）
     */
    int32_t (*pfSendRaw)(void *pvIo, const uint8_t *pu8Data, uint16_t u16Len);
    void    *pvIo;

    void  (*pfLock)(void *pvLockCtx);    /* 可为 NULL；仅当多 task 会调 Post 时必须提供 */
    void  (*pfUnlock)(void *pvLockCtx);
    void   *pvLockCtx;

    void  (*pfNotifyFromISR)(void *pvIo); /* 可为 NULL（走轮询）；ISR 收到数据后唤醒 task */

    /* --- 响应生命周期钩子（可全为 NULL） --- */
    void (*pfRespArm)(CommLink_ST *pstLink, uint8_t u8Cmd);    /* 应答即将送入发送通道 */
    void (*pfRespCancel)(CommLink_ST *pstLink, uint8_t u8Cmd); /* 应答送入发送通道失败 */
    void (*pfRespTxDone)(CommLink_ST *pstLink);                /* 发送通道传输完成 */
} CommLinkCfg_ST;

/* ============================================================================
 * 实例
 * ============================================================================ */

struct CommLink_ST
{
    CommLinkCfg_ST   stCfg;
    CommFramer_ST    stFramer;
    CommDispatch_ST  stDispatch;
    uint8_t          u8SeqCounter;
    uint8_t          u8DupNext;
    uint32_t         u32NowMs;
    void            *pvUser;      /* 使用方可自由存放业务上下文，框架不碰 */
    CommLinkStats_ST stStats;
};

/* ============================================================================
 * 接口
 * ============================================================================ */

/**
 * @brief  初始化链路实例
 * @param  [out] pstLink 链路实例（须为常驻的 static 变量）
 * @param  [in]  pstCfg  配置
 * @retval COMM_OK: 成功, COMM_ERR_PARAM: 参数非法或缓冲区容量不足
 */
int32_t CommLink_Init(CommLink_ST *pstLink, const CommLinkCfg_ST *pstCfg);

/**
 * @brief  注册一张业务命令表
 * @param  [in] pstLink  链路实例
 * @param  [in] pstTable 命令表（须常驻内存）
 * @param  [in] u8Count  表项数量
 * @retval COMM_OK / COMM_ERR_PARAM / COMM_ERR_BUFFER_FULL
 * @note   必须在首次调用 CommLink_Tick() 之前完成，否则相应命令会被回 INVALID_CMD
 */
int32_t CommLink_RegisterCmdTable(CommLink_ST *pstLink,
                                  const CommCmdEntry_ST *pstTable, uint8_t u8Count);

/**
 * @brief  向链路喂入串口收到的原始字节（ISR 上下文调用）
 * @param  [in] pstLink 链路实例
 * @param  [in] pu8Data 原始字节
 * @param  [in] u16Len  字节数
 * @note   ISR 安全。若配置了 pfNotifyFromISR，写入成功后会调用它唤醒 task
 */
void CommLink_FeedFromISR(CommLink_ST *pstLink, const uint8_t *pu8Data, uint16_t u16Len);

/**
 * @brief  通知链路：底层发送通道传输完成（通常由 UART TxCplt 回调转发）
 * @param  [in] pstLink 链路实例
 * @note   仅当配置了 pfRespTxDone 时才有意义；运行在中断上下文，实现须满足 ISR 约束
 */
void CommLink_OnTxComplete(CommLink_ST *pstLink);

/**
 * @brief  投递一帧待发消息
 * @param  [in] pstLink 链路实例
 * @param  [in] pstReq  发送请求
 * @retval COMM_OK: 已入槽, COMM_ERR_PARAM: 参数非法,
 *         COMM_ERR_OVERSIZE: payload 超本实例配额, COMM_ERR_BUFFER_FULL: 发送槽已满
 * @note   本函数只组帧入槽，实际发送发生在下一次 CommLink_Tick()。
 *         多 task 调用时必须已注入 pfLock/pfUnlock
 */
int32_t CommLink_Post(CommLink_ST *pstLink, const CommTxReq_ST *pstReq);

/**
 * @brief  链路主循环：排空接收、发出待发帧、处理 ACK 超时与重传
 * @param  [in] pstLink  链路实例
 * @param  [in] u32NowMs 当前毫秒时间戳（由调用方提供，如 osKernelSysTick()）
 * @note   必须周期性调用，否则帧发不出去、超时也不会触发
 */
void CommLink_Tick(CommLink_ST *pstLink, uint32_t u32NowMs);

/**
 * @brief  查询链路是否没有待发帧、也没有等待 ACK 的帧
 * @param  [in] pstLink 链路实例
 * @retval 1: 空闲, 0: 忙
 */
uint8_t CommLink_IsIdle(const CommLink_ST *pstLink);

/**
 * @brief  handler 可写入的应答附加数据容量（不含框架自动插入的 status 字节）
 * @param  [in] pstLink 链路实例
 * @return 可写字节数
 */
uint16_t CommLink_RespPayloadCap(const CommLink_ST *pstLink);

/**
 * @brief  获取链路统计（会顺带刷新内嵌的解帧器统计）
 * @param  [in] pstLink 链路实例
 * @return 统计结构体指针；参数非法时返回 NULL
 */
const CommLinkStats_ST *CommLink_GetStats(CommLink_ST *pstLink);

#ifdef __cplusplus
}
#endif

#endif /* __COMM_LINK_H__ */
