/**
 * @file    comm_dispatch.h
 * @brief   命令表注册与查找（纯表查找，无状态机，无 IO）
 *
 * 设计约定：
 *  1. 框架内建命令只保留"两端行为完全一致"的那一条（GET_VERSION）
 *  2. 一切业务命令由业务模块自己定义 static const 表并注册，框架只存表指针，
 *     不做拷贝 —— 表本体常驻业务模块的 Flash
 *  3. 一条链路可注册多张表（多个独立业务各注册各的），互不干扰
 */

#ifndef __COMM_DISPATCH_H__
#define __COMM_DISPATCH_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "comm_frame.h"
#include <stdint.h>

/* 每条链路可注册的业务命令表张数上限 */
#ifndef COMM_CMD_TABLE_SLOT_MAX
#define COMM_CMD_TABLE_SLOT_MAX     4U
#endif

/* 框架内建命令码 */
#define COMM_CMD_GET_VERSION        0x02U

/* 协议版本号，随内建 GET_VERSION 应答返回 */
#ifndef COMM_PROTO_VERSION_MAJOR
#define COMM_PROTO_VERSION_MAJOR    2U
#endif
#ifndef COMM_PROTO_VERSION_MINOR
#define COMM_PROTO_VERSION_MINOR    0U
#endif

struct CommLink_ST;

/**
 * @brief  业务命令处理函数原型
 * @param  [in]  pstLink       所属链路实例（handler 可用它反向发帧或读取实例上下文）
 * @param  [in]  pstReq        入站请求帧（已完成 CRC 与长度区间校验）
 * @param  [out] pu8Payload    应答 payload 缓冲区，可写容量见 CommDispatch_PayloadCap()
 * @param  [out] pu16PayloadLen 应答 payload 实际长度，无附加数据时置 0
 * @retval COMM_OK: 成功, <0: CommError_E，由框架转换为协议 status 回给对端
 * @note   必须快速返回。本框架的 handler 一律同步执行在拥有该链路的 task 上下文，
 *         耗时动作（擦 Flash、慢传感器）应投递到自己的队列后立即返回 BUSY/OK
 */
typedef int32_t (*CommHandler_F)(struct CommLink_ST *pstLink,
                                 const CommFrameInfo_ST *pstReq,
                                 uint8_t *pu8Payload,
                                 uint16_t *pu16PayloadLen);

/**
 * @brief  命令表项
 * @note   u8NeedAck 为 0 时该命令为 fire-and-forget，框架不生成任何响应帧
 */
typedef struct
{
    uint8_t       u8Cmd;      /* 命令码 */
    uint16_t      u16MinLen;  /* 允许的 payload 最小长度 */
    uint16_t      u16MaxLen;  /* 允许的 payload 最大长度 */
    uint8_t       u8NeedAck;  /* 1: 需要回复 ACK, 0: 不回复 */
    CommHandler_F pfHandler;  /* 处理函数 */
} CommCmdEntry_ST;

/**
 * @brief  已注册命令表的集合（内嵌在 CommLink_ST 中，不单独实例化）
 */
typedef struct
{
    const CommCmdEntry_ST *apstTable[COMM_CMD_TABLE_SLOT_MAX];
    uint8_t                au8Count[COMM_CMD_TABLE_SLOT_MAX];
    uint8_t                u8SlotUsed;
} CommDispatch_ST;

/**
 * @brief  注册一张业务命令表
 * @param  [in] pstD    分发器
 * @param  [in] pstTable 命令表指针（须常驻内存，框架只保存指针）
 * @param  [in] u8Count  表项数量
 * @retval COMM_OK: 成功, COMM_ERR_PARAM: 参数非法, COMM_ERR_BUFFER_FULL: 槽位已满
 * @note   必须在收帧循环启动之前完成注册；注册期无并发，故内部不加锁
 */
int32_t CommDispatch_Register(CommDispatch_ST *pstD,
                              const CommCmdEntry_ST *pstTable, uint8_t u8Count);

/**
 * @brief  查找命令码对应的表项
 * @param  [in] pstD  分发器
 * @param  [in] u8Cmd 命令码
 * @return 命中的表项指针；未命中返回 NULL
 * @note   查找顺序为内建表优先，业务表按注册先后；命令码重复时先注册者生效
 */
const CommCmdEntry_ST *CommDispatch_Find(const CommDispatch_ST *pstD, uint8_t u8Cmd);

/**
 * @brief  将 CommError_E 转换为协议状态码 CommStatus_E
 * @param  [in] i32Ret 底层错误码
 * @return 对应的协议状态码
 */
uint8_t CommDispatch_ErrorToStatus(int32_t i32Ret);

#ifdef __cplusplus
}
#endif

#endif /* __COMM_DISPATCH_H__ */
