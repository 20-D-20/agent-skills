/**
 * @file    comm_dispatch.c
 * @brief   命令表注册与查找实现
 */

#include "comm_dispatch.h"
#include <stddef.h>

/* ============================================================================
 * 内建命令
 * ============================================================================ */

/**
 * @brief  处理获取协议版本号命令 (0x02)
 * @param  [in]  pstLink        所属链路（未使用）
 * @param  [in]  pstReq         入站请求帧（未使用）
 * @param  [out] pu8Payload     输出版本字节 [0]=Major, [1]=Minor
 * @param  [out] pu16PayloadLen 输出长度，固定为 2
 * @retval COMM_OK: 成功, COMM_ERR_PARAM: 参数非法
 */
static int32_t CommDispatch_HandleGetVersion(struct CommLink_ST *pstLink,
                                             const CommFrameInfo_ST *pstReq,
                                             uint8_t *pu8Payload,
                                             uint16_t *pu16PayloadLen)
{
    (void)pstLink;
    (void)pstReq;

    if (pu8Payload == NULL || pu16PayloadLen == NULL)
    {
        return COMM_ERR_PARAM;
    }

    pu8Payload[0]   = (uint8_t)COMM_PROTO_VERSION_MAJOR;
    pu8Payload[1]   = (uint8_t)COMM_PROTO_VERSION_MINOR;
    *pu16PayloadLen = 2U;

    return COMM_OK;
}

/* 内建表：只放两端行为完全一致的命令。PING / GET_STATUS 各端语义不同，
 * 属于业务命令，由各自的 app 模块注册。 */
static const CommCmdEntry_ST s_astBuiltinTable[] =
{
    /* cmd                  minLen maxLen ACK handler                        */
    { COMM_CMD_GET_VERSION,   0U,    0U,   1U, CommDispatch_HandleGetVersion },
};

/* ============================================================================
 * 公开接口
 * ============================================================================ */

/**
 * @brief  注册一张业务命令表
 * @param  [in] pstD     分发器
 * @param  [in] pstTable 命令表指针
 * @param  [in] u8Count  表项数量
 * @retval COMM_OK / COMM_ERR_PARAM / COMM_ERR_BUFFER_FULL
 */
int32_t CommDispatch_Register(CommDispatch_ST *pstD,
                              const CommCmdEntry_ST *pstTable, uint8_t u8Count)
{
    if (pstD == NULL || pstTable == NULL || u8Count == 0U)
    {
        return COMM_ERR_PARAM;
    }

    if (pstD->u8SlotUsed >= (uint8_t)COMM_CMD_TABLE_SLOT_MAX)
    {
        return COMM_ERR_BUFFER_FULL;
    }

    pstD->apstTable[pstD->u8SlotUsed] = pstTable;
    pstD->au8Count[pstD->u8SlotUsed]  = u8Count;
    pstD->u8SlotUsed++;

    return COMM_OK;
}

/**
 * @brief  查找命令码对应的表项
 * @param  [in] pstD  分发器
 * @param  [in] u8Cmd 命令码
 * @return 命中的表项指针；未命中返回 NULL
 */
const CommCmdEntry_ST *CommDispatch_Find(const CommDispatch_ST *pstD, uint8_t u8Cmd)
{
    uint32_t i;
    uint8_t  u8Slot;

    for (i = 0U; i < (sizeof(s_astBuiltinTable) / sizeof(s_astBuiltinTable[0])); i++)
    {
        if (s_astBuiltinTable[i].u8Cmd == u8Cmd)
        {
            return &s_astBuiltinTable[i];
        }
    }

    if (pstD == NULL)
    {
        return NULL;
    }

    for (u8Slot = 0U; u8Slot < pstD->u8SlotUsed; u8Slot++)
    {
        const CommCmdEntry_ST *pstTable = pstD->apstTable[u8Slot];

        for (i = 0U; i < pstD->au8Count[u8Slot]; i++)
        {
            if (pstTable[i].u8Cmd == u8Cmd)
            {
                return &pstTable[i];
            }
        }
    }

    return NULL;
}

/**
 * @brief  将 CommError_E 转换为协议状态码
 * @param  [in] i32Ret 底层错误码
 * @return 对应的协议状态码
 */
uint8_t CommDispatch_ErrorToStatus(int32_t i32Ret)
{
    switch (i32Ret)
    {
        case COMM_ERR_PARAM:
        case COMM_ERR_OVERSIZE:
            return (uint8_t)COMM_STATUS_INVALID_PARAM;

        case COMM_ERR_BUSY:
            return (uint8_t)COMM_STATUS_BUSY;

        case COMM_ERR_NOT_READY:
        case COMM_ERR_TIMEOUT:
            return (uint8_t)COMM_STATUS_NOT_READY;

        case COMM_ERR_NACK:
            return (uint8_t)COMM_STATUS_DENIED;

        case COMM_ERR_NOT_SUPPORTED:
            return (uint8_t)COMM_STATUS_INVALID_CMD;

        case COMM_ERR_HARDWARE:
        default:
            return (uint8_t)COMM_STATUS_HW_ERROR;
    }
}
