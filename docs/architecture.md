# SnapAsk MVP 架构（M0～M6）

## 不可破坏的边界

SnapAsk 的实现以四条约束为中心：所有像素消费者共享同一规范快照；截图和问题只有在
用户明确发送后才进入问答网络请求；API Key 只进入 Windows Credential Manager；截图、
编辑、保存、复制和贴图不依赖 AI 是否可用。

应用保持 Qt 6 Widgets、C++20、MSVC x64 和 Windows 原生平台接口，不包含 WebEngine、
QML、OCR、录屏、滚动长截图、云历史、RAG 或电脑控制 Agent。

## 组件职责

| 层 | 主要组件 | 职责 |
|---|---|---|
| 应用编排 | `AppController`、`AiSessionController`、`SingleInstance`、`TrayController` | 生命周期、托盘、截图任务、编辑器/回答卡和显式发送流程 |
| 领域 | `ScreenshotSession`、`AnnotationDocument`、标注命令、`ConversationSession` | 物理像素会话、非破坏标注、撤销历史、不可变问答版本绑定 |
| 服务 | `SnapshotRenderer`、`SaveService`、`ClipboardService`、`SessionMemoryBudget` | 唯一快照、保存/剪贴板、会话预算和可重建缓存回收 |
| AI | `AiNetworkClient`、两种 Provider、`SseDecoder`、`AiProfileRepository`、`ProviderProbeClient` | 请求映射、SSE、错误分类、多服务普通配置和显式探测 |
| Windows 平台 | GDI 捕获、显示器映射、窗口识别、全局快捷键、DPI、DWM、Credential Manager、生命周期监视 | 封装 Win32 能力和可读降级 |
| Widgets UI | 捕获层、画布、编辑器、回答卡、贴图窗、服务向导、隐私说明 | 用户交互；`QPixmap` 和活动 Widget 只停留在 GUI 线程 |

可执行文件嵌入 Per-Monitor V2、`amd64`、`asInvoker` manifest；程序不请求提权。

## 截图与物理像素坐标

`AppController::startCapture()` 先隐藏 SnapAsk 自身窗口并刷新 DWM，再把
`GdiScreenCapture::captureVirtualDesktop()` 提交给 `QThreadPool`。工作线程使用
`BitBlt + CAPTUREBLT` 获取虚拟桌面和显示器物理矩形，完成后把值类型 `CaptureFrame`
交回 GUI 线程。捕获失败会恢复隐藏窗口并保留应用。

原图、裁剪矩形、标注几何、保存和上传统一使用截图源物理像素；Qt 逻辑坐标只在输入
映射和窗口布局边界存在。`CaptureOverlay` 负责框选、八方向调整、窗口吸附和约束，确认
后只把选区会话交给编辑器，不持续抓屏。

## 标注、撤销与交互缓存

矩形、箭头、文字和马赛克作为对象保存在 `AnnotationDocument`，由 `QUndoStack` 和专用
命令实现增加、变换、样式、文本、清除、键盘微调及合并。马赛克在规范渲染时真正写入
最终像素。

`CanvasWidget` 按内容版本缓存静态展示图。拖动/缩放已有标注时只在交互开始构建一次
“不含被操作对象”的背景，之后每帧绘制轻量预览；内容提交、撤销或重做会使缓存失效。
这些展示图属于可重建缓存，可在内存预算压力下释放。

## 唯一冻结快照与缓存复用

```text
ScreenshotSession 当前版本
        │  GUI 线程冻结值对象（原图共享引用、裁剪、标注副本、revision）
        ▼
SnapshotRenderInput ──QThreadPool──► 裁剪 + 标注/马赛克扁平化
                                      + 无损 PNG + SHA-256
        │
        ▼
EditorWindow revision 缓存中的唯一 RenderedSnapshot
        ├── 保存
        ├── 系统剪贴板
        ├── 贴图窗
        ├── 本地待发送预览
        └── 显式 AI 发送 / ConversationSession 版本
```

`EditorWindow::currentRenderedSnapshot()` 是产品层唯一入口。它先在 GUI 线程调用
`freezeCurrent()` 得到不含活动 QObject/QPixmap 的值对象，再把 `renderFrozen()` 提交到
线程池；等待期间继续处理绘制、定时器和网络事件，但排除用户输入以保护原子边界。

结果必须同时匹配会话 UUID 和当前 revision 才能进入缓存。相同 revision 后续请求返回
同一个 `RenderedSnapshot` 实例，保存、复制、贴图、预览和 AI 不会各自重绘；内容提交
会清除该缓存。`RenderedSnapshot` 的 PNG、SHA-256、尺寸和 revision 作为不可变元数据，
解码后的 `QImage` 只是可从 PNG 重建的缓存。

集成测试核对同一 revision 的对象地址、PNG 字节和 SHA-256，并验证编辑后产生新实例、
新 revision 和新哈希；假服务实际捕获的上传 PNG 也与该实例完全一致。

## 显式发送与零预发送截图上传

打开回答卡、生成本地预览、绘制、撤销、保存、复制和贴图都不会启动截图问答网络请求。
大型待发送缩略图的平滑缩放也在 `QThreadPool` 完成；`QPixmap` 只在结果回到 GUI 线程后
创建。

只有回答卡的“发送”或 `Ctrl+Enter` 才调用 `ConversationSession::beginExplicitSend()`：

1. 确认服务档案、模型和自定义端点授权。
2. 取得当前 revision 的规范缓存；若已存在直接复用。
3. 创建不可变 `SnapshotRevision`，把回答永久绑定到该 PNG/SHA-256。
4. 通过 `credentialRef` 从 Windows Credential Manager 临时读取 API Key。
5. `AiNetworkClient::sendExplicit()` 校验输入，把 SHA 完整性检查、JSON 和 Base64 请求体
   构建提交到 `QThreadPool`。
6. 只有后台结果的 session/snapshot/request 标识与 SHA 全部匹配，才在 GUI 线程创建
   `QNetworkAccessManager` 并发出请求。

因此线程池中的 Base64/JSON 构建本身也位于显式发送边界之后。取消会终止尚未完成的
后台构建或活动 `QNetworkReply`，终态请求拒绝迟到事件。重试原问题默认复用旧
`SnapshotRevision`；“使用当前截图重新提问”是另一项显式发送，并创建新版本。

设置页的模型列表、文本连接和图片理解探测是单独的用户操作。图片理解探测只使用程序
内置固定测试图，不会取用当前截图、问题或剪贴板。集成测试在显示编辑器、打开回答卡、
生成预览、编辑问题和标注后仍断言本地服务收到 0 个截图请求，直到 `Ctrl+Enter` 才收到
唯一请求。

## AI 网络与会话

`OpenAIResponsesProvider` 和 `ChatCompletionsProvider` 只负责端点、请求体和事件映射；
`AiNetworkClient` 负责异步传输、连接/请求超时、同源重定向、代理、取消和终态隔离。
`SseDecoder` 支持分片、粘包、多行 data、CRLF、UTF-8 边界、DONE 和非法响应。

`ConversationSession` 保存压缩 PNG 绑定的版本和问答 turn。用户继续编辑不会修改在途或
历史版本；旧回答始终显示其 v1/v2 标记。流式文本在 UI 侧合批刷新，失败保留问题、已收
文本、截图会话和标注。

## 配置、凭据与日志

普通服务配置是版本化 JSON，当前 schema 为 1，只保存显示名、端点、协议、模型、超时、
能力、有限的公开元数据请求头和 `SnapAsk/provider/<uuid>` 引用。自定义头只允许
`X-Client`/`X-Client-Name: SnapAsk` 及白名单 `X-Region` 值；任意 opaque token、鉴权头和
其他值不会进入普通 JSON。无版本和明确 version 0 的旧普通配置可保守迁移：仅按既有
UUID 补出引用；明文/嵌套凭据字段、畸形版本和未来版本一律拒绝。普通导出不含 API Key。

`CredentialStore` 使用 `CredWriteW`、`CredReadW` 和 `CredDeleteW`。临时密钥使用后覆盖，
日志过滤 Authorization、Key/Token、Base64 图片、问题、回答和请求体；错误事件不带服务
原始响应正文。自定义公网端点要求 HTTPS，首次发送前按 origin 单独确认；重定向只允许
同源。

## 线程与对象边界

| 工作 | 执行位置 | 结果保护 |
|---|---|---|
| GDI 虚拟桌面捕获 | `QThreadPool` | capture job UUID；过期结果丢弃 |
| 裁剪、标注/马赛克、PNG、SHA-256 | `QThreadPool` | session UUID + revision |
| 请求 SHA 复核、Base64 和 JSON | `QThreadPool` | session + snapshot + request UUID 及 SHA |
| 大图待发送缩略图缩放 | `QThreadPool` | preview generation |
| Widget、`QPixmap`、窗口、网络对象 | GUI 线程 | QObject 父子关系、`QPointer` 和终态检查 |

线程池只接收拥有自身生命周期的 Qt 值类型；活动文档、Widget、`QPixmap`、凭据管理器句柄
和 `QNetworkReply` 不跨线程。

## 会话内存预算

`SessionMemoryBudget` 默认硬上限为 128 MiB，为规格中的单 4K 活跃会话 180 MB 上限预留
UI、网络和 Qt 运行时空间。它核算一个主原图、已发送压缩 PNG，以及画布展示图和待发送
预览等可重建缓存；压力下按最近最少使用顺序释放可重建项。发送后可丢弃快照解码图和
缩略图，只保留压缩 PNG、哈希和版本绑定。

## Windows 外观、生命周期与发布

设置、编辑器、回答卡和贴图使用官方 DWM backdrop 能力；不可用、RDP 或高对比状态走
高可读实色/半透明降级，不使用未公开 composition API。`SystemLifecycleMonitor` 对显示器、
DPI、会话和电源事件去抖；环境变化会取消在途捕获，并重新评估外观和联动窗口布局。

便携发布脚本只接受 x64 Release 构建，调用 `windeployqt`、加入 app-local VC143 CRT、
拒绝覆盖现有输出，并阻止配置、日志、转储和 PDB 进入 ZIP，最后生成 SHA-256。

## 验证边界

当前代码与本机自动化证据不等于完整平台认证。尚未验证的干净 Windows 10/11、真实多屏
混合 DPI、RDP/HDR、休眠与热插拔、安装升级卸载等项目列在
`docs/release-checklist.md`，不能由几何单测、offscreen UI 测试或本机便携包 smoke 外推。
