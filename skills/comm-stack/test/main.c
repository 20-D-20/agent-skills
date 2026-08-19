/**
 * @file    main.c
 * @brief   comm-stack 单元测试入口
 */

#include "test_util.h"

uint32_t g_u32Checks;
uint32_t g_u32Fails;

/**
 * @brief  依次运行三个测试套件并汇总结果
 * @retval 0: 全部通过, 1: 存在失败用例
 */
int main(void)
{
    printf("comm_frame\n");
    test_frame();

    printf("comm_framer\n");
    test_framer();

    printf("comm_link\n");
    test_link();

    printf("\n%u checks, %u failures\n",
           (unsigned)g_u32Checks, (unsigned)g_u32Fails);

    if (g_u32Fails == 0U)
    {
        printf("PASS\n");
        return 0;
    }

    printf("FAIL\n");
    return 1;
}
