---
trigger: always_on
---

# 架构文档自动维护

> 当新增/删除页面、新增共享控件、修改导航路由、新增功能模块时自动生效

## 触发条件

完成功能代码编写后，若本次改动涉及以下任一情况，**必须**读取 `project_doc/qt_architecture.md` 并检查是否需要更新：

| 变更类型 | 检查章节 |
|---------|---------|
| 新增/删除页面类 (ui/pages/) | 页面清单 + 导航路由 + 模块依赖 |
| 修改 PageId 枚举 (core/pageid.h) | 页面清单（枚举值列） |
| 新增/修改导航信号或 MainWindow::setupConnections() | 导航路由 |
| 新增共享控件 (ui/widgets/) | 共享控件 + 目录→层级映射 |
| 新增目录层级 (services/、models/、io/ 等) | 目录→层级映射 + 模块依赖 |
| 新增跨层功能（串口通信、数据库访问等） | 功能→模块映射 + 模块依赖 |
| 修改布局约束（分辨率、StatusBar 高度等） | 关键约束 |

## 各章节更新规则

### 目录→层级映射
- 新增目录时添加行，保持 Entry → Controller → Core → View → Resource 排列顺序
- 将来新增 services/、models/、io/ 层时插入到对应层级位置

### 页面清单
- 与 core/pageid.h 枚举保持一致
- 记录：PageId(值)、类名、文件、职责（一句话）、导航信号列表
- 新增页面时**同步检查** CMakeLists.txt 的 PROJECT_SOURCES 是否已添加

### 共享控件
- 记录：类名、文件、固定尺寸、职责

### 导航路由
- 与 MainWindow::setupConnections() 保持一致
- 格式：来源页面 | 信号名 | 目标 PageId
- navigateBack 的目标需明确标注（Home 还是 CalibrationMenu）

### 功能→模块映射
- 格式：`UI 信号 → Controller 路由 → 目标模块`
- 只记录跨层调用链，页面内部交互不记录

### 关键约束
- 仅记录跨模块的不变量（PageId 顺序、绝对定位尺寸、导航职责划分等）
- 新增约束时用一句话说明"什么"和"为什么"

### 模块依赖
- 用树形缩进表示 parent-child 关系
- 括号标注关键子控件类型

## 更新原则

- 只更新受影响的章节，不重写整个文档
- 保持表格格式一致
- 文档总量控制在 500 行以内，超出时合并或精简低价值条目