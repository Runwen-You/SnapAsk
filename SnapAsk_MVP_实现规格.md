# SnapAsk MVP 实现规格

> SnapAsk 是暂定项目代号，可在实现前或发布前更名。
>
> 本文档用于直接交给 Codex 实现。目标是完成一个可运行、可验证、可迭代的 Windows MVP，而不是只生成界面原型或工程脚手架。

## 1. 给 Codex 的执行要求

请在当前工作区实现本文档定义的 MVP，并遵守以下要求：

1. 使用 C++20、Qt 6 Widgets、CMake、MSVC x64，Windows 10/11 优先。
2. 不使用 Electron、Qt WebEngine、QML/Qt Quick 或内嵌浏览器。
3. 按 M0～M6 顺序实现；每个里程碑结束时都必须能编译、运行并通过相应测试。
4. 先实现纵向可用链路，再完善视觉；不要停留在静态 UI 或伪数据。
5. 保存、复制、贴图和 AI 发送必须共用唯一快照渲染管线。
6. API Key 只能保存到 Windows Credential Manager，不能写入普通配置、日志或崩溃信息。
7. 未经用户明确点击发送，不得上传截图、问题或剪贴板内容。
8. AI 失败不得影响截图、标注、复制、保存和贴图。
9. 不实现“明确非目标”中的功能，不自行扩大范围。
10. 每个里程碑完成后运行单元测试、相关集成测试和 Release 构建；修复失败后再进入下一阶段。

## 2. 产品定位

这是一个 Windows 优先、轻量、快速启动的 AI-first 截图问答工具。

它不是完整复刻 Snipaste，而是提供更短的视觉问答路径：

    全局快捷键
      → 框选屏幕内容
      → 标注或直接提问
      → 截图旁流式显示回答
      → 继续修改标注
      → 使用最新截图继续追问

产品主张：

> 截下来，圈出重点，问 AI；继续标记，再追问。

默认场景：

- 对截图中的知识内容进行简短问答。
- 简短解释代码片段、报错、界面和图表。
- 将截图置顶，边工作边参考 AI 回答。

## 3. 核心产品原则

### 3.1 所见即所得

用户看到的“当前截图”必须同时是：

- 保存到磁盘的截图。
- 复制到剪贴板的截图。
- 贴到桌面最上层的截图。
- 发送给 AI 的截图。

当前截图定义为：

    原始选区
      + 当前裁剪
      + 所有可见矩形、箭头和文字
      + 已栅格化的马赛克
      = RenderedSnapshot

所有出口必须消费同一个不可变 RenderedSnapshot，禁止分别绘制或重新拼装。

### 3.2 非破坏性标注

- 原始截图永不被标注直接修改。
- 矩形、箭头、文字和马赛克都保存为可编辑对象。
- 标注可选择、移动、缩放、修改和删除。
- 发送、保存或复制时才将原图和标注扁平化。
- 编辑控制点、选中边框、悬浮提示不得出现在最终快照中。

### 3.3 显式上传

- 截图完成、保存、复制、贴图和编辑都不能触发网络上传。
- 只有用户点击“发送”或按 Ctrl+Enter 才能上传。
- 发送区域必须显示实际图片预览、服务名、模型名和目标域名。
- 第三方兼容 API 的数据处理规则由对应服务商负责，首次使用自定义域名时必须提示。

### 3.4 会话与截图版本绑定

- 每次发送时冻结一个不可变截图版本，例如 v1、v2。
- 回答必须绑定其使用的截图版本。
- 用户在请求过程中继续绘制，也不能改变已经发送的版本。
- 旧回答不能随当前画布改变。
- 重试默认使用原请求快照；另提供“使用当前截图重新提问”。

## 4. MVP 功能范围

### 4.1 截图

必须支持：

- 可配置全局快捷键。
- 区域截图。
- 顶层窗口识别和窗口区域选择。
- 选区移动、八方向缩放和边界约束。
- 任意显示器上的截图。
- 混合 DPI、负坐标副屏、横竖屏和跨显示器矩形选区。
- Enter 确认，Esc 逐级取消。
- 取消截图不污染剪贴板、不写临时文件。

MVP 不要求识别窗口内部控件；UI Automation 控件级识别放到 P1。

### 4.2 标注

必须支持：

- 矩形。
- 箭头。
- 文字，支持中文输入法、换行和二次编辑。
- 马赛克区域或连续笔迹。
- 选择、移动、缩放、删除、调整样式。
- 标注颜色、线宽和字体记住上次选择。
- 清除全部标注。
- 恢复原始截图。

默认强调色使用高对比红色；矩形内部透明，箭头端点清晰，避免遮挡被解释内容。

### 4.3 撤销与重做

必须支持：

- Ctrl+Z 撤销。
- Ctrl+Y 和 Ctrl+Shift+Z 重做。
- 工具栏撤销、重做按钮自动置灰。
- 新建、删除、移动、缩放、样式修改、文字编辑、马赛克和清除全部均可撤销。
- 一次拖动只产生一条命令。
- 一笔连续马赛克只产生一条命令。
- 连续键盘微调可合并为合理数量的命令。
- 在撤销后产生新编辑时，标准地清除重做分支。

撤销只改变当前画布，不回写已经发送的截图版本或旧回答。

### 4.4 保存、复制、贴图与剪贴板

必须支持：

- 保存当前截图，默认 PNG。
- 复制当前截图到剪贴板。
- 从剪贴板图片创建截图会话或置顶贴图。
- 将当前截图贴到桌面最上层。
- 贴图可移动、缩放、关闭和切换置顶。
- 保存、复制、贴图和 AI 发送均使用相同 RenderedSnapshot。

保存无标注原图、透明度和点击穿透可放到 P1。

### 4.5 AI 问答

必须支持：

- 框选完成后立即可聚焦问题输入框。
- 当前截图和问题共同发送。
- 截图旁边流式显示回答。
- 停止生成、重新回答、复制回答。
- Markdown 基础排版。
- 代码块等宽字体、基础语法高亮和独立复制按钮。
- 围绕同一截图会话继续追问。
- 修改标注后显示“图片已修改，尚未发送”。
- 再次提问时发送最新 RenderedSnapshot 并创建新版本。
- 旧回答显示“基于 v1 / v2”等版本标记。
- 网络或 API 失败时保留截图、标注、问题和已收到的回答。

默认回答风格：

- 使用用户提问语言。
- 先给结论，再给 3～5 个重点。
- 知识问答保持简洁。
- 解释代码时说明用途、主要流程、关键变量和明显风险。
- 优先关注截图中的红框、箭头和文字标记。
- 内容不可辨认时明确说明，不猜测。

建议默认系统提示：

    你是一个截图知识问答助手。使用用户的语言回答。
    先给结论，再给 3～5 个必要要点，避免冗长背景。
    解释代码时说明用途、主要流程、关键变量和明显风险。
    图片更新时，重点关注用户新增的矩形、箭头、文字和马赛克区域。
    如果截图内容不完整或不可辨认，请明确指出，不要猜测。

### 4.6 截图旁回答卡片

截图窗口与回答卡片是两个独立窗口，但由同一会话控制器逻辑绑定。

必须支持：

- 回答卡片优先出现在截图右侧。
- 右侧空间不足时依次尝试左侧、下方和上方。
- 始终限制在当前显示器工作区内。
- 移动截图时回答卡片跟随。
- 截图和回答共同置顶、隐藏和关闭。
- 支持解除绑定后单独移动回答卡片。
- 长回答在卡片内部滚动，不能无限增高。
- 流式内容每约 33～60 ms 合批刷新，避免每个 token 重排整个文档。
- 回答卡片底部保留继续提问输入框。
- 回答卡片顶部显示当前服务、模型和截图版本。

### 4.7 iOS-inspired 玻璃 UI

视觉目标：

- 16～20 px 圆角。
- 半透明背景。
- 系统级背景材质或模糊。
- 细高光边框。
- 柔和阴影。
- 深浅色自适应。
- 蓝色为主要操作强调色。
- 动画约 150～200 ms。
- 代码块使用更实的半透明背景，保证可读性。

性能和兼容性约束：

- Windows 11 支持时使用官方 DWM 系统背景能力。
- Windows 10、远程桌面、高对比模式或 DWM 能力不可用时，退化为高可读性的半透明或实色面板。
- 不使用未公开的 SetWindowCompositionAttribute 技巧。
- 不使用 QGraphicsBlurEffect 模拟实时桌面模糊。
- 不进行持续抓屏或持续性高功耗动画。

### 4.8 多 API 友好配置

设置页采用“服务卡片 + 添加向导”，不能直接展示一整页复杂表单。

每个 AI 服务档案包含：

- 唯一 ID。
- 用户自定义名称。
- 协议类型。
- Base URL。
- Credential Manager 凭据引用。
- 默认模型。
- 可用模型列表。
- 连接和总请求超时。
- 能力标记：图片、流式输出、模型列表。
- 最后测试时间和简明状态。

MVP 明确支持两种协议：

1. OpenAI Responses API。
2. OpenAI-compatible Chat Completions。

用户操作：

- 添加、编辑、复制、删除服务档案。
- 设为默认服务。
- 自动尝试获取模型列表，失败时允许手动输入模型 ID。
- 分别执行“测试连接”和“测试图片理解”。
- 测试图片理解时使用内置无敏感测试图，并提示可能产生少量 API 费用。
- 回答卡片顶部快速切换服务和模型。
- 新配置无需重启即可使用。
- 导出普通配置时默认不包含 API Key。
- 删除服务时可同步删除 Windows Credential Manager 中的凭据。

高级选项默认折叠：

- 超时。
- 代理。
- 自定义请求头。

不得宣称兼容所有 OpenAI 风格接口。图片字段、SSE 事件、模型列表和错误格式必须由独立适配器隔离。

## 5. 状态流

### 5.1 截图会话

    Idle
      → Preparing
      → Capturing
      → Selecting
      → Editing
      → Pinned
      → Closing

补充规则：

- Esc 只退出当前层级，不一次关闭所有贴图。
- AI 失败后回到可编辑状态。
- 热键连按不能创建重叠的捕获流程。
- 关闭存在未保存修改或正在生成的会话时给出轻量确认。

### 5.2 AI 回合

    Draft
      → Encoding
      → Sending
      → Streaming
      → Completed

异常出口：

    Encoding / Sending / Streaming
      → Cancelled
      → Failed

每个回合都必须持有：

- request ID。
- snapshot ID。
- provider profile ID。
- model ID。
- 状态。

所有网络回调都要验证 request ID 和会话是否仍有效，关闭窗口后丢弃迟到事件。

## 6. 数据模型

建议核心模型：

    ScreenshotSession
    ├─ SourceImage
    ├─ AnnotationDocument
    ├─ QUndoStack
    ├─ currentRevision
    ├─ lastSavedHash
    ├─ lastSentHash
    ├─ SnapshotRevision[]
    ├─ ConversationTurn[]
    └─ WindowPlacement

    AnnotationDocument
    ├─ ordered Annotation[]
    ├─ selectedAnnotationIds
    └─ revision

    Annotation
    ├─ id
    ├─ type
    ├─ zOrder
    ├─ style
    └─ geometry

    SnapshotRevision
    ├─ snapshotId
    ├─ revisionNumber
    ├─ renderedHash
    ├─ PNG bytes 或可重建的不可变标注状态
    └─ createdAt

    ConversationTurn
    ├─ turnId
    ├─ snapshotId
    ├─ question
    ├─ providerProfileId
    ├─ modelId
    ├─ answerMarkdown
    ├─ status
    ├─ error
    └─ timestamps

    ProviderProfile
    ├─ id
    ├─ displayName
    ├─ protocol
    ├─ baseUrl
    ├─ modelId
    ├─ credentialRef
    ├─ timeout
    ├─ proxy
    └─ capabilityFlags

唯一快照对象建议：

    struct RenderedSnapshot {
        QImage image;
        QByteArray pngBytes;
        QByteArray sha256;
        QSize pixelSize;
        uint64_t revision;
    };

    RenderedSnapshot SnapshotRenderer::renderCurrent(
        const ScreenshotSession& session);

保存直接写入 pngBytes；复制、贴图和 AI 请求也直接消费同一实例或同一 sha256 对应的缓存结果。

## 7. 技术选型

- 语言：C++20。
- GUI：Qt 6 Widgets，采用稳定的 Qt 6 LTS 基线。
- 构建：CMake + Ninja + MSVC x64。
- 绘制：自定义 QWidget + QPainter + QImage。
- 撤销：QUndoStack + QUndoCommand。
- 网络：QNetworkAccessManager，异步读取并解析 SSE。
- 配置：版本化 JSON；窗口位置等轻量状态可用 QSettings。
- 密钥：Windows Credential Manager。
- 截图：MVP 使用 GDI BitBlt + CAPTUREBLT，通过接口隔离。
- Markdown：QTextDocument/QTextBrowser 加原生 CodeBlockWidget，不引入 WebEngine。
- 图片：内部统一 QImage::Format_ARGB32_Premultiplied，默认 PNG。
- 测试：Qt Test + 本地假 HTTP 服务 + Windows 手工矩阵。

Qt 目标模块：

    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    Qt6::Svg
    Qt6::Test

Windows 链接库：

    User32
    Gdi32
    Dwmapi
    Shcore
    Advapi32
    Ole32

## 8. 模块架构

    ┌──────────────────────────────────────────────┐
    │ UI：截图、画布、贴图、回答、设置、托盘       │
    └───────────────────┬──────────────────────────┘
                        │
    ┌───────────────────▼──────────────────────────┐
    │ 应用服务：会话、捕获、快照、保存、剪贴板     │
    └──────────┬───────────────────────┬───────────┘
               │                       │
    ┌──────────▼───────────┐  ┌────────▼───────────┐
    │ 核心领域模型          │  │ AI Provider 层      │
    │ 标注、版本、问答       │  │ 请求、SSE、错误映射 │
    └──────────┬───────────┘  └────────┬───────────┘
               │                       │
    ┌──────────▼───────────────────────▼───────────┐
    │ Windows 平台层：热键、截图、DPI、DWM、凭据   │
    └──────────────────────────────────────────────┘

边界要求：

- Windows 平台层不得持有业务会话。
- AI 层不得引用 QWidget。
- UI 不得直接读写 Credential Manager。
- Provider 的协议 JSON 不得泄漏到 UI。
- 核心标注模型不得持有 QWidget 或 QGraphicsItem 指针。

## 9. 关键 Windows API 与 Qt 实现点

### 9.1 DPI 与坐标

在 QApplication 创建前：

- 应用清单声明 PerMonitorV2。
- 可调用 SetProcessDpiAwarenessContext 进行运行时兼容检查。

唯一坐标原则：

> 原图、选区、标注和导出全部使用截图源的物理像素坐标；Qt 逻辑坐标只存在于 UI 输入层。

涉及：

- EnumDisplayMonitors。
- GetMonitorInfo。
- MonitorFromPoint。
- GetDpiForWindow。
- QueryDisplayConfig，可选，用于稳定显示器标识。

禁止直接混用 QScreen::geometry、Win32 窗口矩形和截图像素坐标。

### 9.2 全局快捷键

- RegisterHotKey / UnregisterHotKey。
- 使用 MOD_NOREPEAT。
- 通过 QAbstractNativeEventFilter 接收 WM_HOTKEY。
- 注册失败时在设置页显示冲突，不静默失败。
- MVP 不使用低级键盘钩子。

### 9.3 截图

MVP 后端：

- GetDC(nullptr)。
- CreateCompatibleDC。
- CreateDIBSection。
- BitBlt，使用 SRCCOPY | CAPTUREBLT。
- 虚拟桌面范围来自 SM_XVIRTUALSCREEN、SM_YVIRTUALSCREEN、SM_CXVIRTUALSCREEN、SM_CYVIRTUALSCREEN。

窗口识别：

- EnumWindows。
- IsWindowVisible。
- WindowFromPoint。
- GetAncestor(..., GA_ROOT)。
- DwmGetWindowAttribute(DWMWA_EXTENDED_FRAME_BOUNDS)。
- DwmGetWindowAttribute(DWMWA_CLOAKED)。

不要将 PrintWindow 作为主方案。捕获后端必须接口化，以便后续替换为 Windows.Graphics.Capture 或 Desktop Duplication。

### 9.4 画布与撤销

使用自定义 CanvasWidget：

- paintEvent 只负责显示。
- AnnotationDocument 是唯一标注状态。
- AnnotationRenderer 负责最终快照。
- 命中测试和控制点使用图像像素坐标。

QUndoCommand 至少包括：

- AddAnnotationCommand。
- RemoveAnnotationCommand。
- TransformAnnotationCommand。
- ChangeStyleCommand。
- EditTextCommand。
- AddMosaicStrokeCommand。
- ClearAnnotationsCommand。

命令通过 Annotation ID 操作文档，不保存容易悬空的裸指针。使用 mergeWith 合并连续微调。

QUndoStack 的单一 clean 状态不能同时表达“已保存”和“已发送”，因此必须分别维护 lastSavedHash 和 lastSentHash。

### 9.5 贴图和回答窗口

贴图窗口：

- Qt::FramelessWindowHint。
- Qt::Tool。
- Qt::WindowStaysOnTopHint。
- 必要时使用 SetWindowPos(HWND_TOPMOST / HWND_NOTOPMOST)。
- 截图窗口可使用 WS_EX_TOOLWINDOW。

截图和回答卡片保持独立 HWND，由 SessionWindowController 管理跟随位置、焦点、关闭和置顶。

可尝试使用 SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE) 防止递归截到自身，但截图前隐藏本应用窗口仍是主方案。

### 9.6 玻璃效果

运行时检测并使用：

- DwmSetWindowAttribute。
- DWMWA_WINDOW_CORNER_PREFERENCE。
- DWMWA_SYSTEMBACKDROP_TYPE。
- DwmExtendFrameIntoClientArea。
- 必要时 DwmEnableBlurBehindWindow。

Windows 11 的系统背景属性不可用时必须降级；不能依赖未公开 API。

### 9.7 凭据

- CredWriteW。
- CredReadW。
- CredDeleteW。
- 读取后调用 CredFree。
- 临时明文使用完后 SecureZeroMemory。

普通配置只保存类似：

    "credentialRef": "SnapAsk/provider/<uuid>"

### 9.8 网络与 SSE

QNetworkAccessManager 保持异步；大图缩放、PNG 编码、Base64 和哈希放入 QThreadPool。

SSE 解析必须处理：

- UTF-8 字符被网络分片截断。
- 一行分多次 readyRead。
- 一次 readyRead 包含多个事件。
- LF 和 CRLF。
- 多行 data 字段。
- DONE 事件。
- Responses 与 Chat Completions 的不同事件结构。
- 非法 JSON、连接中断和迟到数据。

先以 QByteArray 聚合完整事件，再进行 UTF-8 解码。取消请求使用 QNetworkReply::abort。

重定向默认禁用，或仅允许同源重定向；不得将 Authorization 自动发送到新主机。

## 10. AI Provider 设计

统一接口：

    ILlmProvider
    ├─ OpenAIResponsesProvider
    └─ OpenAIChatCompletionsProvider

统一事件：

    Started
    TextDelta
    UsageUpdated
    Completed
    Cancelled
    Failed

Provider 负责：

- 请求构造。
- 图片编码。
- 鉴权。
- SSE 解析。
- 错误映射。
- 取消。
- 文本连接测试。
- 图片理解测试。

会话上下文以本地规范化模型为真源，不能只依赖服务商的 response ID。这样切换 API 后仍可继续提问。

每轮首版发送：

- 当前最新 RenderedSnapshot。
- 当前问题。
- 有长度上限的近期文字问答上下文。

不重复发送所有历史截图。图片变化时，在提示中明确：

    图片已经更新，请重点关注用户新增的框选、箭头和文字标记。

OpenAI Responses 请求默认 store=false。兼容接口若不支持该字段，由适配器忽略。

## 11. 建议目录结构

    SnapAsk/
    ├─ CMakeLists.txt
    ├─ CMakePresets.json
    ├─ cmake/
    │  ├─ CompilerWarnings.cmake
    │  └─ Version.cmake
    ├─ src/
    │  ├─ app/
    │  │  ├─ main.cpp
    │  │  ├─ AppController.*
    │  │  ├─ AppStateMachine.*
    │  │  ├─ SingleInstance.*
    │  │  └─ TrayController.*
    │  ├─ domain/
    │  │  ├─ capture/
    │  │  ├─ annotation/
    │  │  │  └─ commands/
    │  │  └─ conversation/
    │  ├─ services/
    │  │  ├─ SnapshotRenderer.*
    │  │  ├─ CaptureService.*
    │  │  ├─ SessionService.*
    │  │  ├─ ClipboardService.*
    │  │  └─ SaveService.*
    │  ├─ ai/
    │  │  ├─ ILlmProvider.*
    │  │  ├─ AiRequest.*
    │  │  ├─ AiStreamEvent.*
    │  │  ├─ AiProfile.*
    │  │  ├─ AiProfileRepository.*
    │  │  ├─ SseDecoder.*
    │  │  ├─ OpenAIResponsesProvider.*
    │  │  ├─ ChatCompletionsProvider.*
    │  │  └─ AiErrorMapper.*
    │  ├─ platform/
    │  │  └─ windows/
    │  │     ├─ WindowsDpi.*
    │  │     ├─ GlobalHotkey.*
    │  │     ├─ ScreenCapture.*
    │  │     ├─ MonitorCoordinateMapper.*
    │  │     ├─ WindowPicker.*
    │  │     ├─ GlassBackdrop.*
    │  │     ├─ CredentialStore.*
    │  │     └─ WindowCaptureExclusion.*
    │  ├─ ui/
    │  │  ├─ capture/
    │  │  ├─ canvas/
    │  │  ├─ pin/
    │  │  ├─ answer/
    │  │  ├─ settings/
    │  │  ├─ tray/
    │  │  └─ common/
    │  └─ infrastructure/
    │     ├─ ConfigStore.*
    │     ├─ ConfigMigration.*
    │     ├─ RedactingLogger.*
    │     └─ AtomicFileWriter.*
    ├─ resources/
    │  ├─ icons/
    │  ├─ fonts/
    │  └─ app.qrc
    ├─ tests/
    │  ├─ unit/
    │  ├─ integration/
    │  ├─ golden/
    │  ├─ fixtures/
    │  └─ manual/
    │     └─ WindowsTestMatrix.md
    ├─ packaging/
    │  └─ windows/
    └─ docs/
       ├─ architecture.md
       ├─ privacy.md
       └─ release-checklist.md

CMake 目标建议：

    snapask_core
    snapask_ai
    snapask_platform_win
    snapask_ui
    snapask
    snapask_unit_tests
    snapask_integration_tests

## 12. 线程模型

主线程：

- Qt UI。
- QUndoStack。
- 全局热键和 native event。
- 会话状态机。
- QNetworkAccessManager 异步 I/O。

懒加载截图线程：

- GDI 或未来 WGC 抓取。
- HDC、HBITMAP 在创建它们的线程内销毁。

QThreadPool：

- 大图缩放。
- PNG 编码。
- Base64。
- SHA-256。

线程间只传递冻结的 QImage 或值对象，不传递 QPixmap。后台结果返回时必须验证 session ID、snapshot ID 和 request ID。

## 13. MVP 开发路线

### M0：应用外壳

实现：

- CMake 工程和测试目标。
- 托盘、退出和设置入口。
- 单实例。
- 日志脱敏框架。
- 主题令牌。
- PerMonitorV2 清单。

阶段完成条件：

- Release 构建可在干净 Windows 环境启动。
- 重复启动只激活现有实例。
- 单元测试框架可运行。

### M1：基础截图

实现：

- RegisterHotKey。
- GDI 虚拟桌面捕获。
- 多显示器选区、移动和八方向调整。
- 顶层窗口识别。
- 复制和保存 PNG。

阶段完成条件：

- 任意应用中按快捷键可完成框选并复制。
- 100%、125%、150%、200% 和混合缩放没有坐标偏移。
- 取消截图不产生临时文件。

### M2：非破坏性标注与唯一快照

实现：

- 矩形、箭头、文字和马赛克。
- 选择、移动、缩放、删除和样式调整。
- QUndoStack / QUndoCommand。
- SnapshotRenderer。
- 未保存、未发送状态。

阶段完成条件：

- 全部撤销回到原始状态，全部重做回到最终状态。
- 清除全部也可撤销。
- 保存、复制和模拟 AI 请求捕获到的 PNG 哈希一致。
- 马赛克已写入最终像素。

### M3：单 API 纵向问答链路

实现：

- 一个 OpenAI Responses 配置。
- Credential Manager。
- 当前 PNG + 问题请求。
- SSE 流式回答、停止、重试和错误提示。
- 基础回答卡片、Markdown 和代码块。
- 发送时冻结 SnapshotRevision。

阶段完成条件：

- 快捷键 → 框选 → 标注 → 输入问题 → 流式回答完整可用。
- 未点击发送时没有截图网络请求。
- 取消真正中止请求，迟到数据不更新 UI。
- 401、429、超时、断网和非法响应不丢会话。

### M4：友好多 API 配置

实现：

- 服务卡片和添加向导。
- 多档案增删改、复制和设默认。
- Responses 和兼容 Chat Completions 适配器。
- 自动模型列表和手动回退。
- 文本连接测试与图片理解测试。
- 回答卡片快速切换服务/模型。

阶段完成条件：

- 至少可保存三个服务档案并快速切换。
- 新配置无需重启。
- 密钥不出现在 JSON、日志和导出文件中。
- 删除档案可同步删除系统凭据。

### M5：持续标记追问、贴图联动和玻璃 UI

实现：

- 回答后重新编辑截图。
- 修改后显示未发送状态。
- v1、v2 与回答绑定。
- 截图和回答联动移动、置顶、隐藏和关闭。
- 自动布局。
- DWM 玻璃和降级样式。
- 流式 UI 合批更新。

阶段完成条件：

- v1 → A1 → 新标记 → v2 → A2 流程正确。
- A1 仍绑定 v1，A2 绑定 v2。
- 移动时无明显抖动。
- 长回答内部滚动。
- 深色、浅色和高对比模式均可读。

### M6：发布加固

实现：

- 显示器插拔、休眠唤醒、RDP 和 DPI 改变处理。
- 会话内存预算和缓存回收。
- 安装包或便携包。
- 首启隐私说明。
- 性能与稳定性测试。
- 干净 Windows 虚拟机验证。

阶段完成条件：

- 连续完成 100 次截图、编辑、提问和关闭，无崩溃、死锁或明显持续内存增长。
- 安装、升级和卸载验证完成。
- 所有 P0 验收项通过。

## 14. 测试要求

### 14.1 单元测试

必须覆盖：

- 逻辑坐标、物理像素和 DPI 转换。
- 负坐标、竖屏和混合缩放几何。
- 标注命中、边界、缩放和排序。
- 所有 QUndoCommand 的 undo / redo。
- 随机标注操作全部撤销和全部重做。
- SnapshotRenderer 金图测试。
- currentRevision、lastSavedHash、lastSentHash。
- SSE 半包、粘包、多行 data、UTF-8 边界、DONE 和非法 JSON。
- 两种 Provider 的请求和事件映射。
- URL 验证和配置迁移。
- 日志脱敏。
- Markdown 和代码块边界。

### 14.2 集成测试

使用本地假 HTTP 服务，不能依赖真实 API：

- 正常流式响应。
- 慢响应、超时、中途断开、401、429、500。
- 用户取消后服务继续发送，客户端不得更新已关闭会话。
- 切换档案后请求发送到正确端点。
- Credential Manager 的写入、读取和删除。
- 保存、剪贴板和被假服务捕获的 PNG 哈希一致。
- v1、v2 两轮问答与快照绑定。
- 第二实例与快捷键冲突。
- 用户点击发送之前服务端没有收到截图请求。

真实 API 只作为手动或显式启用的 smoke test，不能成为默认 CI。

### 14.3 Windows 手工测试矩阵

- Windows 10 和 Windows 11。
- 单屏、双屏、三屏。
- 100%、125%、150%、200% 及混合缩放。
- 副屏在主屏左侧、上方和负坐标区域。
- 横屏、竖屏、热插拔。
- 深色、浅色、高对比模式。
- 休眠唤醒、锁屏、RDP、Explorer 重启。
- 浏览器硬件加速窗口、视频和全屏应用。
- HDR 与 SDR 混用。
- 快捷键冲突。
- 断网、慢网、代理和证书错误。
- 不支持图片的模型和非标准兼容接口。
- 长代码、中文、Emoji 和长回答。
- 剪贴板粘贴到浏览器、Word、画图等常用程序。
- 隐私检查：未发送零上传、日志无敏感内容、默认不落盘。

UAC 安全桌面、DRM 保护内容、受保护窗口和部分独占全屏内容允许明确提示“不支持”，不得默认提权。

## 15. 性能目标

在 Release x64、无调试器环境测量，至少运行 30 次并记录 p50 / p95。

| 指标 | 目标 |
|---|---:|
| 冷启动到托盘可用 | p95 ≤ 700 ms |
| 常驻后快捷键到冻结首帧，单 4K | p95 ≤ 150 ms |
| 双 4K 快捷键到冻结首帧 | p95 ≤ 250 ms |
| 框选完成到问题输入可用 | ≤ 200 ms |
| 标注和拖动交互 | p95 ≤ 16.7 ms；最差 ≤ 33 ms |
| 空闲工作集 | 目标 ≤ 60 MB；硬上限 80 MB |
| 单 4K 活跃会话工作集 | 目标 ≤ 180 MB |
| 空闲 CPU | 平均 ≤ 0.2% |
| 4K 当前快照 PNG 生成 | ≤ 300 ms，不能长时间阻塞 UI |
| 流式文本刷新 | 每 33～60 ms 合批 |
| x64 部署包 | 建议 ≤ 70 MB，不含 VC 运行库 |

性能目标是验收基准，不允许为了达标牺牲像素正确性、隐私或稳定性。若参考机器无法达到，必须提供基准数据和瓶颈分析，而不是静默降低质量。

内存原则：

- 原始截图只保留一个主 QImage。
- 标注保存为对象，不复制整张位图。
- 选择确认后尽快释放完整虚拟桌面图。
- 已发送旧版本优先保存压缩 PNG 或可重建标注状态。
- 设置会话内存上限，超限时提示并回收可重建缓存。

## 16. P0 验收标准

1. 双显示器混合缩放下，选区与最终像素一致，无偏移、黑边或错位。
2. 每种标注均可撤销和重做；一次拖动只撤销一次；清除全部也可恢复。
3. 保存图、剪贴板图、贴图和实际 API 请求图来自同一快照；测试时 SHA-256 一致。
4. v1 提问后修改标注并发送 v2，A1 仍绑定 v1，A2 绑定 v2。
5. 回答在截图旁流式出现；移动截图时卡片跟随；所有布局不越出工作区。
6. 至少可创建并持久化三个 API 档案，快速切换；密钥不出现在普通配置和日志中。
7. 断网、超时、无效密钥、限流、模型不支持图片和接口不兼容时，给出可理解错误且不丢会话。
8. 用户明确发送前没有包含截图的网络请求。
9. 马赛克已栅格化到实际上传 PNG，不上传原图或可恢复图层。
10. AI 完全不可用时，截图、标注、撤销、保存、复制和贴图仍正常。
11. Windows 玻璃能力不可用时，界面自动降级且文字清晰可读。
12. 连续执行 100 次核心流程无崩溃、死锁或明显持续内存增长。

## 17. P1 候选功能

只有 P0 验收通过后再考虑：

- OCR 和“只发送文字”。
- 解释代码、解释报错、总结、翻译等快捷提示。
- 只问标记区域，同时发送全图与局部裁剪。
- 可选本地历史、搜索和导出。
- 对比两个截图版本。
- 拖入本地图片和多图问答。
- 保存无标注原图。
- 贴图透明度、点击穿透和隐藏全部。
- 更多厂商原生适配与本地模型。
- UI Automation 控件级边界识别。
- Windows Graphics Capture 后端。

## 18. 明确非目标

MVP 不实现：

- 完整复刻 Snipaste。
- 滚动长截图。
- 录屏或 GIF。
- Photoshop 式图层和复杂绘图。
- 云账号、云同步和团队共享。
- 知识库、RAG。
- 自动操作电脑的 Agent。
- macOS 或 Linux。
- 插件系统。
- 产品自营模型额度和计费。
- 宣称兼容所有 OpenAI 风格接口。

## 19. 隐私与安全要求

- 默认会话和截图只驻留内存，不自动写临时目录。
- API Key 不出现在 JSON、日志、崩溃报告、错误提示或导出文件中。
- 日志不得记录 Authorization、Base64 图片、问题正文、完整回答或请求体。
- 非 localhost 的公网 API 默认必须使用 HTTPS。
- 不允许关闭 TLS 证书校验。
- 重定向仅允许同源，否则停止并提示。
- Markdown 禁止脚本、HTML 执行、远程图片和自动资源加载。
- 打开回答中的外部链接前确认。
- 软件不向模型提供电脑控制工具。
- 错误信息必须脱敏。
- 连接测试不得使用用户当前截图。

注意：普通像素化马赛克不应宣称是强安全脱敏。后续可增加不可逆纯色遮挡工具；MVP 发送前仍必须确保马赛克已扁平化到输出像素。

## 20. 已知系统限制

- GDI 可能无法捕获 DRM、UAC 安全桌面、受保护窗口和部分独占全屏内容。
- HDR 场景可能存在色彩映射差异。
- 某些非标准兼容 API 可能不支持图片、SSE 或模型列表。
- Windows 10 不保证具备 Windows 11 的系统背景材质。
- 这些限制必须给出明确用户提示，不得通过默认提权或关闭安全机制绕过。

## 21. 官方技术参考

- OpenAI Responses API：支持文本、图片输入和多轮上下文  
  https://developers.openai.com/api/reference/cli/resources/responses/methods/create
- OpenAI 关于任务提示的建议：明确目标、约束、成功标准与输出格式  
  https://developers.openai.com/api/docs/guides/latest-model
- Microsoft RegisterHotKey  
  https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-registerhotkey
- Microsoft Per-Monitor V2 与应用清单  
  https://learn.microsoft.com/en-us/windows/win32/sbscs/application-manifests
- Microsoft DWM 窗口属性  
  https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/ne-dwmapi-dwmwindowattribute
- Microsoft BitBlt  
  https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-bitblt
- Microsoft Credential Manager：CredWriteW / CredReadW  
  https://learn.microsoft.com/en-us/windows/win32/api/wincred/nf-wincred-credwritew  
  https://learn.microsoft.com/en-us/windows/win32/api/wincred/nf-wincred-credreadw
- Qt QUndoStack  
  https://doc.qt.io/qt-6/qundostack.html
- Qt QNetworkAccessManager  
  https://doc.qt.io/qt-6/qnetworkaccessmanager.html
- Qt QAbstractNativeEventFilter  
  https://doc.qt.io/qt-6/qabstractnativeeventfilter.html

## 22. 最终完成定义

只有同时满足以下条件，MVP 才算完成：

- 核心链路可以真实运行，不依赖伪数据。
- P0 功能全部通过验收。
- 单元测试和集成测试全部通过。
- Windows 手工矩阵不存在阻断发布的问题。
- Release 构建可以在干净 Windows 环境运行。
- 隐私、安全和密钥要求全部满足。
- 性能指标已测量并记录。
- 安装、升级和卸载已验证。
- 代码结构允许后续增加 OCR、WGC 和更多 Provider，而无需重写核心会话、快照和标注模型。
