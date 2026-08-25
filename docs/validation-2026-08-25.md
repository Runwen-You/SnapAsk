# SnapAsk 本机验证记录（2026-08-25）

## 结论

当前开发机已经取得唯一快照复用、显式发送边界、Credential Manager、线程池重任务、
4K 核心渲染/画布交互、100 次产品 UI/本地网络流和最终便携候选包的本机证据。该证据
不构成 M6 平台发布签字。

## 已核对的实现证据

- `EditorWindow::currentRenderedSnapshot()` 按 session revision 缓存唯一不可变快照；保存、
  复制、贴图、待发送预览和 AI 会话都引用该入口。
- 快照工作输入在 GUI 线程冻结，裁剪、标注/马赛克、PNG 和 SHA-256 在 `QThreadPool`；
  GDI 捕获、请求 Base64/JSON 和大型预览缩放也在 `QThreadPool`。
- 携带用户截图和问题的问答请求只从 `AiNetworkClient::sendExplicit()` 的显式发送路径
  启动；测试在发送前逐步显示/编辑并断言本地服务为 0 请求。设置探测是另一条显式路径，
  其图片测试只使用内置固定图。
- API Key 通过 `credentialRef` 进入 Windows Credential Manager；普通 JSON、导出和日志
  不保存 Key。普通自定义头只接受有限公开元数据白名单，opaque token 被 UI 和仓储拒绝；
  旧普通配置只迁移非敏感字段，明文凭据和未来版本被拒绝。
- 会话预算核算主图、已发送 PNG 与可重建画布/预览缓存，压力时可释放解码图和缩略图。
- 生产代码已经移除捕获阶段的独立 PNG 编码器；裁剪只返回像素，规范 PNG 只由
  `SnapshotRenderer` 生成。

## 已保存的本机测量

| 证据 | 结果 |
|---|---|
| `out/build/m6-release/performance-final.txt` | 4K PNG p50 86.915 ms、p95 142.205 ms、max 154.037 ms |
| 同一性能输出 | 拖动首次背景 25.556 ms；后续帧 p95 1.422 ms、max 1.454 ms |
| 同一性能输出 | 3 个性能用例通过；Qt Test 合计 5 passed、0 failed，3.754 s |
| `out/build/m6-release/product-stress-final.txt` | 100 次产品 UI/本地网络流通过，3 passed、0 failed，7.344 s |
| 同一压力输出 | 工作集 18.4 → peak 32.8 → end 32.8 MiB；early median 28.9、late median 32.5 MiB |
| 最终完整 CTest | Release `/W4 /WX` 构建成功；30/30 通过，0 失败，27.36 s |
| 最终便携 ZIP | 26.549 MiB；SHA-256 `3358033451c3eff60dd763f9deed7a66ab3e9bc29e28f1aa0953ed5865bae716` |

产品压力流使用 47 × 31 合成图、本地假 HTTP/SSE 服务和真实临时 Credential Manager
条目；它不包含真实 GDI 捕获、真实 AI 或 4K 会话。性能方法、原始样本和解释见
`docs/performance-results.md`。

最终候选位于 `out/releases/v0.2.0/SnapAsk-0.2.0-win-x64.zip`。stage 为
30 个文件、63.334 MiB（包含 app-local VC 运行库），ZIP 为 30 个同顶层条目；全部 30 个
PE 均为 x64，必需 Qt/Windows 平台/TLS/CRT 文件齐全，调试文件和用户配置扫描为 0，
ZIP 哈希与 `.sha256` 一致。已从该候选执行真实首启、第二实例交接和设置页视觉检查；
双击启动只保留一个设置窗口，毛玻璃页面切换无残影。另已在隔离目录完成
全新解压、覆盖解压和删除验证；这些仍不是干净 Windows 虚拟机验证。

## 未验证，不得外推

- 干净 Windows 10/11 虚拟机；
- 真实单/双/三显示器、混合 DPI、负坐标和横竖屏；
- RDP、HDR/SDR、休眠唤醒、锁屏、Explorer 重启和显示器热插拔；
- 冷启动、快捷键到真实 GDI 冻结首帧、空闲 CPU/内存和真实 4K 活跃会话内存；
- 在干净系统上执行最终候选包的全新解压、覆盖升级和移除；若以后提供安装器，还需验证
  安装、升级和卸载。

这些项目必须按 `docs/release-checklist.md` 在最终候选构建上执行并留存证据。
