/**
 * @file    test_util.h
 * @brief   极简断言宏，避免为单元测试引入外部依赖
 */

#ifndef __TEST_UTIL_H__
#define __TEST_UTIL_H__

#include <stdio.h>
#include <stdint.h>

extern uint32_t g_u32Checks;
extern uint32_t g_u32Fails;

#define CHECK(cond)                                                            \
    do {                                                                       \
        g_u32Checks++;                                                         \
        if (!(cond)) {                                                         \
            g_u32Fails++;                                                      \
            printf("    FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                      \
    } while (0)

#define CHECK_EQ(a, b)                                                         \
    do {                                                                       \
        long _a = (long)(a);                                                   \
        long _b = (long)(b);                                                   \
        g_u32Checks++;                                                         \
        if (_a != _b) {                                                        \
            g_u32Fails++;                                                      \
            printf("    FAIL %s:%d  %s = %ld, expected %s = %ld\n",            \
                   __FILE__, __LINE__, #a, _a, #b, _b);                        \
        }                                                                      \
    } while (0)

#define CASE(name)   printf("  - %s\n", (name))

void test_frame(void);
void test_framer(void);
void test_link(void);

#endif /* __TEST_UTIL_H__ */
