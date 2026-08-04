---
trigger: glob
globs: **/*.{c,h}
paths:
  - "**/*.{c,h}"
description: 嵌入式 C 代码规范：变量/函数命名（匈牙利前缀）、头文件守卫模板、中文 Doxygen 注释、AStyle 格式化、FreeRTOS 栈管理
---

# 📐 嵌入式代码规范

## 命名规范

### 头文件守卫

```c
#ifndef __FILE_H__
#define __FILE_H__

/* 外部 C 兼容 */
#ifdef __cplusplus
extern "C" {
#endif

/* 接口内容 */

#ifdef __cplusplus
}
#endif

#endif
```

- **原则**：仅包含对外接口（声明、宏、类型）；严禁在头文件中定义变量。
- **关联**：每个 `.c` 须有对应同名 `.h`；禁止循环依赖与冗余包含。

### 变量命名

```c
/* 类型前缀 */
uint8_t  u8Var;      int8_t   s8Var;
uint16_t u16Var;     int16_t  s16Var;
uint32_t u32Var;     int32_t  s32Var;
float    fVar;       char     cVar;
bool     bVar;       void*    pVar;
uint8_t  au8Arr[10]; /* 数组前缀 'a' */

/* 作用域前缀 */
static uint16_t s_u16Static;   /* 静态变量 */
volatile uint32_t g_u32Global; /* 全局变量 */
const uint8_t c_u8Const = 0x55;/* 常量 */
extern uint32_t e_u32Extern;   /* 外部变量 */
```

### 函数与类型命名

```c
/* 分层命名规范 */
void drv_uart_init(void);  /* 驱动层: drv_ */
void bsp_led_toggle(void); /* BSP层: bsp_ */
void app_task_main(void);  /* 应用层: app_ */

/* 中断处理 (ISR_外设_Handler/Callback) */
void ISR_UART1_Handler(void);

/* 类型定义 */
typedef struct { u16 u16Id; } Msg_ST; /* 结构体后缀 _ST */
typedef enum { STATE_IDLE } State_E;  /* 枚举后缀 _E */

/* 宏定义 (全大写下划线) */
#define MAX_SIZE 256
```

## 📝 注释规范

- **语言**：必须使用**中文**。
- **函数注释**：采用 Doxygen 格式（@brief, @param, @retval, @note）。
- **行内注释**：`/* 注释内容 */`（前后保留一个空格）。

```c
/**
 * @brief  模块初始化
 * @param  [in] pConfig: 配置指针
 * @retval 0:成功, -1:参数错误
 */
s32 module_init(const Msg_ST *pMsg);

/* 示例 */
RCC->D1CFGR = 0x00; /* 寄存器清零 */
```

## 🛠️ 格式化与审查

### AStyle 配置

```bash
astyle --style=allman --indent=spaces=4 --indent-switches --indent-preproc-block --pad-oper --pad-header --unpad-paren --align-pointer=name --align-reference=name --convert-tabs --lineend=linux --max-code-length=100 --break-after-logical
```

- **核心效果**：Allman 风格（大括号换行），4 空格缩进，操作符后留空。

### 审查要点

- [ ] 无魔术数字，使用宏或枚举。
- [ ] 无硬编码延时，使用系统 tick。
- [ ] 临界区保护完整，资源申请/释放配对。
- [ ] 内存访问边界与返回值检查。


### FreeRTOS 栈管理规范

1. **禁止栈分配大对象**：结构体 >100B 或数组 >256B 必须定义为 `static` 或使用堆。
2. **阈值控制**：单个函数栈消耗须小于 256 字节。
3. **线程安全**：使用静态变量前必须确保无竞态或重入风险。