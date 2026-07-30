---
trigger: glob
description: Qt/C++ 代码风格规范：命名约定、类结构模板、注释规范、clang-format 配置
globs: **/*.{cpp,h,hpp,ui}
---

# 📐 Qt/C++ 代码规范

## 命名规范

### 总览

| 元素 | 风格 | 示例 |
| --- | --- | --- |
| 类名 | 大驼峰 (PascalCase) | `SerialManager`, `DataParser` |
| 函数/方法 | 小驼峰 (camelCase) | `openPort()`, `parseData()` |
| 成员变量 | `m_` 前缀 + 小驼峰 | `m_serialPort`, `m_dataBuffer` |
| 局部变量 | 小驼峰 | `baudRate`, `portName` |
| 常量/枚举值 | 大驼峰 或 全大写下划线 | `DefaultTimeout`, `MAX_RETRY` |
| 信号 (signal) | 小驼峰，无动词前缀 | `dataReceived()`, `connectionChanged()` |
| 槽 (slot) | `on` + 对象 + 事件 或 小驼峰动词 | `onBtnClicked()`, `handleData()` |
| 命名空间 | 小写 | `namespace app {}` |
| 宏定义 | 全大写下划线 | `#define APP_VERSION "1.0"` |
| UI 控件 | 类型缩写 + 功能描述 (camelCase) | `btn_send`, `label_status`, `edit_portName` |

### UI 控件命名前缀

> **在 Qt Designer 中命名控件时统一使用以下前缀，便于代码中快速识别控件类型。**

| 前缀 | 控件类型 | 示例 |
| --- | --- | --- |
| `btn_` | QPushButton | `btn_connect`, `btn_send` |
| `label_` | QLabel | `label_status`, `label_serialData` |
| `edit_` | QLineEdit | `edit_portName`, `edit_command` |
| `combo_` | QComboBox | `combo_baudRate`, `combo_parity` |
| `check_` | QCheckBox | `check_autoSend`, `check_hex` |
| `radio_` | QRadioButton | `radio_ascii`, `radio_hex` |
| `spin_` | QSpinBox | `spin_timeout`, `spin_interval` |
| `text_` | QTextEdit / QPlainTextEdit | `text_log`, `text_receive` |
| `table_` | QTableWidget / QTableView | `table_data`, `table_config` |
| `tree_` | QTreeWidget / QTreeView | `tree_project`, `tree_menu` |
| `list_` | QListWidget / QListView | `list_ports`, `list_history` |
| `group_` | QGroupBox | `group_serial`, `group_display` |
| `tab_` | QTabWidget | `tab_main`, `tab_settings` |
| `slider_` | QSlider | `slider_speed`, `slider_zoom` |
| `progress_` | QProgressBar | `progress_transfer` |
| `action_` | QAction | `action_open`, `action_save` |
| `menubar_` | QMenuBar | `menubar_main` |
| `toolbar_` | QToolBar | `toolbar_main` |
| `statusbar_` | QStatusBar | `statusbar_main` |
| `dock_` | QDockWidget | `dock_console` |
| `stacked_` | QStackedWidget | `stacked_pages` |

## 类结构模板

### 标准 Qt Widget 类

```cpp
#ifndef CLASSNAME_H
#define CLASSNAME_H

#include <QWidget>  /* Qt 头文件在前 */

QT_BEGIN_NAMESPACE
namespace Ui { class ClassName; }
QT_END_NAMESPACE

/**
 * @brief  一句话说明类的职责
 * @note   补充说明（依赖、线程安全性等）
 */
class ClassName : public QWidget
{
    Q_OBJECT

public:
    explicit ClassName(QWidget *parent = nullptr);
    ~ClassName() override;

    /* --- 公有接口 --- */

signals:
    /* --- 信号声明 --- */

private slots:
    /* --- 私有槽函数 --- */

private:
    /* --- 初始化辅助函数 --- */
    void setupConnections();

    /* --- 成员变量 --- */
    Ui::ClassName *ui;
};

#endif // CLASSNAME_H
```

### 类声明排列顺序

```text
public → signals → public slots → protected → private slots → private
```

**每个区域内部**按以下顺序排列：

1. 类型定义 (typedef / using / enum)
2. 构造 / 析构
3. 公有接口方法
4. 成员变量

### 头文件包含顺序

```cpp
/* 1. 对应的头文件 */
#include "mywidget.h"

/* 2. 项目内头文件 */
#include "serialmanager.h"
#include "dataparser.h"

/* 3. Qt 头文件 */
#include <QWidget>
#include <QSerialPort>
#include <QTimer>

/* 4. 标准库头文件 */
#include <vector>
#include <memory>
```

## 📝 注释规范

- **语言**：注释以**中文**为主，Qt/C++、signal/slot、thread、event loop 等技术术语保留英文。
- **调试输出**：`qCDebug()`、`qCWarning()` 等日志打印必须使用**英文**，避免嵌入式终端或非 UTF-8 环境乱码；日志分类规则见下方「🪵 日志与调试」。
- **类注释**：头文件中的主要类必须使用 Doxygen 格式，说明类职责、页面/模块定位、关键依赖和线程约束。
- **对外接口注释**：public/protected 接口、跨模块调用接口、信号 signal 必须使用 Doxygen 格式说明用途、参数、返回值和调用约束。
- **槽函数注释**：复杂 slot、跨页面导航 slot、处理硬件/串口/定时器事件的 slot 必须说明触发来源和副作用；简单按钮响应可不单独注释。
- **关键逻辑注释**：页面跳转、状态机、异步回调、资源生命周期、线程边界、数据解析、错误恢复等关键逻辑必须用中文说明设计意图。
- **避免无效注释**：简单赋值、普通 `if/return`、变量名已自解释的代码不需要注释，禁止写“设置变量”“返回结果”这类重复代码含义的注释。
- **行内注释**：使用 `/* */` 风格,只用于解释不直观的原因、约束或设计意图。
- **TODO/FIXME 标记**：统一写作 `// TODO(作者): 待办描述` 或 `// FIXME(作者): 问题描述`，必须说明后续动作，禁止只写 `TODO`。

```cpp
/**
 * @brief  打开串口连接
 * @param  portName 串口设备名，如 "/dev/ttyUSB0"
 * @param  baudRate 波特率，默认 115200
 * @return true: 打开成功, false: 打开失败
 * @note   调用前确保串口未被其他进程占用
 */
bool SerialManager::openPort(const QString &portName, int baudRate)
{
    m_serial->setPortName(portName);  // 设置设备名
    m_serial->setBaudRate(baudRate);  // 设置波特率
    return m_serial->open(QIODevice::ReadWrite);
}
```

### 注释覆盖要求

新增或修改 Qt/C++ 代码时，按以下级别补齐注释：

| 位置 | 要求 |
| --- | --- |
| `.h` 类声明 | 主要业务类、页面类、共享控件类必须 Doxygen 注释，说明职责和依赖 |
| `.h` public/protected 接口 | 跨模块调用接口必须说明用途、参数、返回值、调用前提 |
| signal | 必须说明信号触发时机、携带数据含义、典型接收方 |
| slot | 复杂 slot 必须说明触发来源和副作用；简单 UI 点击响应可不注释 |
| enum / struct | 业务含义不直观时必须说明用途；字段含义不明显时逐字段注释 |
| 页面导航逻辑 | 必须说明跳转来源、目标页面、返回路径或特殊状态处理 |
| 线程 / 定时器 / 异步回调 | 必须说明运行线程、触发周期或回调来源，以及资源生命周期约束 |
| 数据解析 / 协议处理 | 必须说明数据格式、字段含义、边界检查和错误处理 |

## 信号槽连接规范

### 必须使用新式语法（编译期类型检查）

```cpp
/* ✅ 正确：新式 connect，编译期检查 */
connect(m_serial, &QSerialPort::readyRead,
        this, &MainWindow::readSerialData);

/* ❌ 禁止：旧式 SIGNAL/SLOT 宏，运行时才能发现错误 */
connect(m_serial, SIGNAL(readyRead()),
        this, SLOT(readSerialData()));
```

### 信号槽连接集中管理

将所有 `connect` 调用集中到 `setupConnections()` 私有方法中，保持构造函数简洁：

```cpp
MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    initSerialPort();
    setupConnections();  /* 所有信号槽连接在此 */ 
}

void MainWindow::setupConnections()
{
    connect(m_serial, &QSerialPort::readyRead,
            this, &MainWindow::readSerialData);
    connect(m_timer, &QTimer::timeout,
            this, &MainWindow::handleTimeout);
    connect(ui->btn_send, &QPushButton::clicked,
            this, &MainWindow::onBtnSendClicked);
}
```

## 🪵 日志与调试

统一使用 **`QLoggingCategory`** 管理日志分类，禁止在业务代码中直接调用裸的
`qDebug()` / `qWarning()` / `qInfo()` / `qCritical()`。分类声明建议集中在一对
专门的头文件/源文件中（如 `core/logcategories.h` + `core/logcategories.cpp`），
头文件里用 `Q_DECLARE_LOGGING_CATEGORY` 声明，源文件里用 `Q_LOGGING_CATEGORY`
定义并指定默认级别。选用 `QLoggingCategory` 而非手写 `#define`/`while(false)`
开关宏，是因为它同样有编译期格式检查、关闭时开销可忽略，但额外支持**不重新
编译**、通过 `QT_LOGGING_RULES` 环境变量或 `QLoggingCategory::setFilterRules()`
在运行时按分类开关——对已部署设备的现场排障很关键。

- **必须使用分类宏**：
  - `qCDebug(category)`：常规调试流信息。
  - `qCInfo(category)`：关键路径或业务状态变化（如：页面跳转、成功连接）。
  - `qCWarning(category)`：预期内的异常或警告（如：配置缺失、重试逻辑触发）。
  - `qCCritical(category)`：严重错误，可能导致功能失效（如：硬件初始化失败）。
  - 禁止裸调用 `qDebug()` / `qWarning()` / `qInfo()` / `qCritical()`（无 category 参数）。

- **选用/新建 category 的规则**：
  1. 写日志前先读集中声明分类的头文件（如 `core/logcategories.h`），确认是否
     已有匹配的 category，**不要凭记忆猜测**——该文件本身就是当前项目分类清单的
     唯一权威来源，不需要在规范文档里另外维护一份副本。
  2. 粒度是**业务域/子系统**，不是按文件、也不是整个项目共用一个。归类依据
     **日志内容所属的业务域**，而非日志所在文件的物理目录——例如某个 UI 层文件
     里如果打的是"触发下层某条业务链路"的日志，应归入该业务链路自己的
     category，而不是为这个 UI 文件单独开一个。这样现场用 `QT_LOGGING_RULES`
     按域开关时，同一条业务链路的日志才能一起打开/关闭，不用管它散落在哪几个
     文件里。
  3. 确实没有匹配的 category 时才新建：
     - 变量名 `log` + 业务域 PascalCase（如 `logCod`），字符串名建议用
       项目短名前缀 + 业务域小写（如 `"<project>.cod"`）。
     - 在分类头文件追加一行 `Q_DECLARE_LOGGING_CATEGORY(logXxx)`，在对应源文件
       追加一行 `Q_LOGGING_CATEGORY(logXxx, "<project>.xxx", <默认级别>)`。
     - 默认级别按项目所处阶段决定：仍在开发阶段建议 `QtDebugMsg`（现场能直接看到
       调试信息）；已上生产、需要默认安静则用 `QtWarningMsg`。整体调整时只改这
       一处即可，不需要逐处改调用代码。

- **旧代码迁移策略**：项目中可能存在尚未接入分类、仍是裸 `qDebug`/`qWarning`
  的历史代码。**只要求新增或修改的日志行使用分类宏**；因其他原因经过这些文件
  时，不要顺手批量迁移未触及的旧日志调用，避免把无关 diff 搞大。整体迁移需
  单独安排任务。

- **格式要求**：
  - 必须添加模块前缀（使用方括号），如 `[Comm]`, `[UI]`, `[Calib]`。即使 Qt 默认
    日志格式会自动附加 category 名（`%{if-category}%{category}: %{endif}%{message}`），
    仍要保留手写前缀——现场如果自定义了 `QT_MESSAGE_PATTERN`（例如只保留时间戳），
    手写前缀是唯一还能定位来源的信息。
  - 建议包含关键变量名，格式为 `[Module] Key info: value`。
    - 示例：`qCDebug(logComm) << "[Comm] Port opened:" << portName << "at baud rate:" << baudRate;`

- **语言规范**：**代码中的调试信息打印（qCDebug, qCWarning 等）统一使用英文**。
  - 禁止在日志输出中使用中文，以防在嵌入式终端、串口日志或非 UTF-8 环境下出现乱码。
  - 区分“面向用户的信息”与“面向开发者的信息”（Log 打印，必须英文）；UI 文案的内容与呈现
    遵循 [`user-facing-content.md`](./user-facing-content.md)，不得将底层错误信息直接显示给用户。

- **性能规范**：禁止在频繁触发的事件（如 `paintEvent`）或高频循环中进行无条件的
  `qCDebug` 打印，防止日志溢出影响性能。

## 🛠️ 格式化

### clang-format 配置

```yaml
# .clang-format
BasedOnStyle: WebKit
Language: Cpp
Standard: c++17

# 缩进
IndentWidth: 4
TabWidth: 4
UseTab: Never
AccessModifierOffset: -4
IndentCaseLabels: false
NamespaceIndentation: None

# 大括号
BreakBeforeBraces: Allman

# 列宽
ColumnLimit: 100

# 对齐
AlignConsecutiveAssignments: false
AlignConsecutiveDeclarations: false
AlignTrailingComments: true
PointerAlignment: Left

# 头文件排序
SortIncludes: true
IncludeBlocks: Preserve

# 其他
AllowShortFunctionsOnASingleLine: Inline
AllowShortIfStatementsOnASingleLine: Never
SpaceAfterCStyleCast: false
```

### 审查要点

- [ ] 无魔术数字：超时值、缓冲区大小等使用 `constexpr` 或 `#define`
- [ ] 信号槽全部使用新式 `&Class::method` 语法
- [ ] `new` 出来的 QObject 子类都传入了 `parent`，由 Qt 对象树管理生命周期
- [ ] 没有在析构函数中 `delete` 已有 parent 的子对象（Qt 对象树会自动释放）
- [ ] 头文件包含顺序正确，无冗余包含
- [ ] 调试信息（qCDebug/qCWarning 等）已全部使用英文，无中文硬编码
- [ ] 新增/修改的日志均使用 `qCDebug`/`qCWarning`/`qCInfo`/`qCCritical` + 对应 category，无裸调用
- [ ] UI 控件命名使用了标准前缀
