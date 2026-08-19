/**
 * @file    comm_framer.h
 * @brief   接收侧字节流解帧器：无锁 SPSC 环形缓冲 + 解帧状态机
 *
 * 设计要点：
 *  1. ISR 只调用 CommFramer_FeedFromISR()，内部零 RTOS API、零阻塞、零日志
 *  2. task 侧循环调用 CommFramer_Next() 逐帧取出，直到返回 COMM_ERR_NOT_READY
 *  3. 任一阶段校验失败时，回退到该候选帧头之后一个字节重新找头（resync），
 *     而不是丢弃整段缓冲 —— 否则一个坏帧会连带吞掉粘在它后面的好帧
 *  4. 本文件不依赖 FreeRTOS，也不依赖任何硬件，可直接链进 PC 单元测试
 *
 * 内存序：环形缓冲是单生产者（ISR）/ 单消费者（task）模型。生产者先写数据
 * 再发布 head，消费者先读 head 再读数据。Cortex-M 单核下编译器屏障已足够；
 * 若移植到多核或带写合并缓冲的平台，请重定义 COMM_MEM_BARRIER。
 */

#ifndef __COMM_FRAMER_H__
#define __COMM_FRAMER_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "comm_frame.h"
#include <stdint.h>

/* ============================================================================
 * 内存屏障 —— 按平台重定义
 *   Keil AC5 : #define COMM_MEM_BARRIER() __schedule_barrier()
 *   多核平台 : #define COMM_MEM_BARRIER() __DMB()
 * ============================================================================ */
#ifndef COMM_MEM_BARRIER
#  if defined(__GNUC__) || defined(__clang__)
#    define COMM_MEM_BARRIER()   __asm__ __volatile__("" ::: "memory")
#  else
#    define COMM_MEM_BARRIER()   do { } while (0)
#  endif
#endif

/* ============================================================================
 * 接收侧统计
 * ============================================================================ */

typedef struct
{
    uint32_t u32RxFrames;    /* 成功解出的帧数 */
    uint32_t u32RxDropRing;  /* 环形缓冲满导致丢弃的字节数 */
    uint32_t u32RxSkipped;   /* 找帧头时跳过的垃圾字节数 */
    uint32_t u32RxCrcErr;    /* CRC 校验失败次数 */
    uint32_t u32RxBadTail;   /* 帧尾不符次数 */
    uint32_t u32RxOversize;  /* 长度字段超出本实例配额次数 */
    uint32_t u32RxResync;    /* 触发重新找头的总次数 */
} CommFramerStats_ST;

/* ============================================================================
 * 配置与实例
 * ============================================================================ */

/**
 * @brief  解帧器配置
 * @note   所有缓冲区由调用方提供，须常驻内存；框架只保存指针
 */
typedef struct
{
    uint8_t  *pu8Ring;         /* ISR 写入的字节环缓冲 */
    uint16_t  u16RingSize;     /* 环大小，必须是 2 的幂且 >= 16 */
    uint8_t  *pu8Frame;        /* 组帧输出缓冲 */
    uint16_t  u16FrameCap;     /* 组帧缓冲容量，须 >= u16MaxPayloadLen + COMM_FRAME_OVERHEAD */
    uint16_t  u16MaxPayloadLen;/* 本实例接受的最大 payload，须 <= COMM_PAYLOAD_LEN_MAX */
} CommFramerCfg_ST;

/**
 * @brief  解帧器实例
 */
typedef struct
{
    CommFramerCfg_ST   stCfg;
    volatile uint16_t  u16Head;   /* 生产者(ISR)写入位置 */
    volatile uint16_t  u16Tail;   /* 消费者(task)读取位置 */
    uint16_t           u16Mask;     /* u16RingSize - 1 */
    uint16_t           u16Have;     /* 组帧缓冲中已累积的字节数 */
    uint16_t           u16Consumed; /* 上一帧占用的前缀字节数，下次调用开头才丢弃
                                     * （延迟消费：立即前移会覆盖刚交给调用方的帧数据） */
    CommFramerStats_ST stStats;
} CommFramer_ST;

/* ============================================================================
 * 接口
 * ============================================================================ */

/**
 * @brief  初始化解帧器
 * @param  [out] pstF   解帧器实例
 * @param  [in]  pstCfg 配置（内容会被拷贝进实例，但缓冲区仍由调用方持有）
 * @retval COMM_OK: 成功, COMM_ERR_PARAM: 参数非法（含环大小不是 2 的幂、
 *         组帧缓冲容量不足、payload 配额超过协议上限）
 */
int32_t CommFramer_Init(CommFramer_ST *pstF, const CommFramerCfg_ST *pstCfg);

/**
 * @brief  向解帧器喂入原始字节（ISR 上下文调用）
 * @param  [in] pstF   解帧器实例
 * @param  [in] pu8Src 收到的原始字节
 * @param  [in] u16Len 字节数
 * @return 实际写入环形缓冲的字节数；小于 u16Len 表示环已满，差额被丢弃并计数
 * @note   ISR 安全：不调用任何 RTOS API，不阻塞，不打日志
 */
uint16_t CommFramer_FeedFromISR(CommFramer_ST *pstF, const uint8_t *pu8Src, uint16_t u16Len);

/**
 * @brief  取出下一个完整帧（task 上下文调用）
 * @param  [in]  pstF   解帧器实例
 * @param  [out] pstOut 输出帧信息，pu8Data 指向实例的组帧缓冲内部
 * @retval COMM_OK: 取出一帧, COMM_ERR_NOT_READY: 暂无完整帧, COMM_ERR_PARAM: 参数非法
 * @note   返回的 pu8Data 在下一次调用 CommFramer_Next() 前有效，之后会被覆盖
 */
int32_t CommFramer_Next(CommFramer_ST *pstF, CommFrameInfo_ST *pstOut);

/**
 * @brief  清空解帧器状态（丢弃环内与组帧缓冲内的全部数据）
 * @param  [in] pstF 解帧器实例
 * @note   仅供链路重置使用；不清零统计计数
 */
void CommFramer_Reset(CommFramer_ST *pstF);

/**
 * @brief  获取接收侧统计
 * @param  [in] pstF 解帧器实例
 * @return 统计结构体指针；参数非法时返回 NULL
 */
const CommFramerStats_ST *CommFramer_GetStats(const CommFramer_ST *pstF);

#ifdef __cplusplus
}
#endif

#endif /* __COMM_FRAMER_H__ */
