# SnapAsk

SnapAsk 0.3.0 是面向 Windows 10/11 的截图问答工具。它把 Snipaste 式截图、非破坏性标注、贴图和流式 AI 回答放在同一内存会话中，并以一次规范化快照渲染作为保存、复制、贴图和发送的唯一像素来源。

## 已实现能力

- Per-Monitor V2、混合缩放和负坐标多显示器截图，支持框选、窗口吸附及选区旁的极简浮动工具条。
- 矩形、箭头、文字和已栅格化马赛克；完整撤销、重做和清除恢复。
- 编辑器贴合截图选区显示，使用纯图标工具条；回答卡仅在点击“提问”后出现。
- 保存、剪贴板、贴图和 AI 请求共用 `SnapshotRenderer` 生成的同一个不可变快照。
- 只有用户点击发送或按 `Ctrl+Enter` 后才冻结并上传快照；停止会真正中止连接，迟到数据不会更新界面。
- OpenAI Responses 与兼容 Chat Completions；多服务档案、模型列表、文本/固定测试图探测，默认服务和模型只在设置中选择。
- API Key 只进入 Windows Credential Manager，不写入 JSON、导出文件或日志；自定义端点首次发送前单独确认。
- 回答与截图版本永久绑定；回答框内选中文字后显示右上角复制图标。
- 截图与回答卡四向自动布局及联动移动/置顶/隐藏/关闭；隐藏回答卡不参与拖动，显示后按帧合并移动事件。
- 截图工具条、编辑器、回答卡、设置和服务弹窗共用 Liquid Glass 设计系统；局部图像背景按区域缓存并响应指针高光，Windows 11 同时使用官方 DWM 背景材质，Windows 10、RDP、高对比或不可用环境自动采用高可读降级样式。
- 设置页按“通用 / AI 服务 / 隐私与关于”侧边栏分区，并显示统一来源的应用版本号。

明确不包含 OCR、滚动长截图、录屏、云同步、RAG、电脑控制 Agent、插件系统或非 Windows 平台。完整边界见 `SnapAsk_MVP_实现规格.md`。

## 构建与测试

需要 Windows x64、MSVC 2022、CMake 3.25+、Ninja，以及 Qt 6.5+ 的 Core、Gui、Widgets、Network、Svg 和 Test 模块。仓库预设默认使用 `.deps/Qt/6.8.3/msvc2022_64`。

```powershell
cmake --preset msvc-release -DSNAPASK_WARNINGS_AS_ERRORS=ON
cmake --build --preset release
ctest --preset release --output-on-failure
```

测试不依赖真实 AI 服务。网络集成使用本地假 HTTP/SSE 服务；凭据测试只创建随机临时 Windows 凭据并在结束时清理。Release 构建固定使用 `/O2 /Ob1`，避免 MSVC `/Ob2` 在 Qt 优化窗口析构路径中的不稳定行为。

## 便携发布

完成 Release 构建后运行：

```powershell
.\scripts\package-portable.ps1 -BuildDirectory .\out\build\m6-release
```

脚本只接受 x64 Release 可执行文件，通过 `windeployqt` 复制运行库，拒绝覆盖现有输出，并生成 ZIP 与 SHA-256。它会拒绝把服务配置、日志、转储、PDB 或用户凭据状态带入发布包。

## 隐私边界

截图和会话默认仅驻留内存，不自动落盘。连接测试使用程序内置的固定测试图，不接受当前截图或用户问题。Markdown 不执行 HTML/脚本、不加载远程图片；外部链接和自定义服务端点都需要确认。普通像素化马赛克不应视为强安全脱敏。
