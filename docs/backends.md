> Imported from the OS Museum project (2026-09). Paths such as `spikes/…`, `vendor/…` and `images/…` refer to that project; the reusable code now lives in `prototype/` here.

# 模拟器后端：能否非侵入式接入

> 2026-09-27 · 基于源码阅读，未构建运行。源码在 `vendor/`。
> v86 @ 5f9a90f（= npm 0.5.462）；infinite-mac + macemu（分支 infinite-mac-kanjitalk755）+ minivmac。

## 结论

| | v86 | Mini vMac | Basilisk II | SheepShaver |
|---|---|---|---|---|
| 覆盖 | Win 1.0–XP | System 1–6 | System 7–8.1 | 7.5.2–9.0.4 |
| 接入方式 | 上游 npm + 适配层 | Infinite Mac 的 wasm + 自写 `workerApi` | 同左 | 同左 |
| 无头 / Worker | ✅ 官方示例 | ✅ 本就是 Worker | ✅ | ✅ |
| RGBA 帧 | 内部：自己调 `vga.screen_fill_buffer()` | ✅ `blit`，带脏矩形 | ✅ `blit`，整帧 + 去重 | ✅ 同左 |
| 键盘 | ✅ 公开 API | ✅ SAB 输入槽 | ✅ | ✅ |
| 绝对鼠标 | `bus.send("mouse-absolute")`，客户机需 vmmouse 驱动 | ✅ 直接写位置 | ✅ `ADBMouseMoved` | ✅ |
| 自定义磁盘 | ✅ 鸭子类型对象 | ✅ `workerApi.disks` | ✅ | ✅ |
| 暂停冻结时钟 | 替换 `performance.now`/`Date.now`（Worker 内） | 替换 wasm 时间导入（Infinite Mac 已这么做） | 同左 | 同左 |
| **快照恢复到新实例** | ✅ 已有 | ❌ 需改 | ❌ 需改 | ❌ 需改（最难） |
| 许可证 | BSD-2 | GPL-2 | GPL-2 | GPL-2 |

**唯一必须动模拟器的地方：Mac 三个核心的快照恢复。**
- 原因：三者都是阻塞式主循环、单线程、无 Asyncify。线性内存可以随时在 `workerApi` 回调里复制出来，但 WASM 调用栈不在线性内存里，新实例无法从那里继续。
- 方案 A（只改编译参数）：`-s ASYNCIFY` + 窄 `ASYNCIFY_ONLY`，在回调处 unwind → 转储 → 新实例 rewind。不改源码，但要测解释器热循环被插桩后的性能损失。
- 方案 B（小源码补丁）：导出 `resume()`，跳过初始化直接重入 CPU 循环。估计 Mini vMac 30–60 行，Basilisk II 50–100 行，SheepShaver 80–150 行（嵌套 `execute_depth` 与 68k/PPC 模式切换）。
- 除内存外还要保存宿主侧状态：分块磁盘的已加载/已写块、音视频"已打开"状态、Emscripten FS。
- 原始体积：Basilisk II / SheepShaver 288 MB（`INITIAL_MEMORY` 固定，大部分为零），Mini vMac 48 MB。

## 实测更新（S2，2026-09-27）
Mini vMac 已验证方案 A 可行，**不改源码**：Asyncify 只插桩 5 个外层循环函数 + 9 行 `--pre-js`。
新页面里恢复约 40 ms 出第一帧，与存档时逐像素一致；恢复后菜单和 Calculator 正常；启动速度无可测损失；
快照 48 MB，gzip 后 0.56 MB。详见 `spikes/s2-mac-snapshot/README.md`。
遗留：-O1 及以上优化会让客户机崩溃（与 Asyncify 无关，Infinite Mac 也只发布 -O0）。

Basilisk II（Quadra 650 / System 7.5.3）同样验证通过，**不改源码**：Asyncify 只插 3 个函数，速度与原版相同，wasm +2%。
`INITIAL_MEMORY` 改为 80 MB 后，快照 gzip 2.0 MB，新页面约 160 ms 出第一帧。
两个新发现：① CPU 循环会嵌套，宿主层检查调用栈，嵌套时推迟存档；② **必须用虚拟时钟**，否则隔一段时间再恢复，
Basilisk 的指令配额校准会被时间差带偏，响应慢到秒级。虚拟时钟在 Worker 里包一层 `Date.now`/`performance.now` 即可。

SheepShaver（PM 9500 ROM / System 7.5.3 PPC）同样验证通过，**不改源码**：插 4 个函数，wasm +0.5%，快照 gzip 6.4 MB（64 MB 客户机内存），
新页面约 270 ms 出第一帧。代价：它的输入轮询在 PPC 解释器主循环函数内部，该函数必须插桩，**启动慢约 15%**。
如需消除，可打一个小补丁让主循环每 N 条指令返回外层。Mac OS 9.0.4（New World ROM）也已实测通过：快照 gzip 11.4 MB / zstd 8.4 MB，新页面约 0.5 秒出第一帧（Worker 内恢复 58–81 ms），恢复后操作与冷启动一致。

## v86 可选的上游补丁（非必须）
- 自定义屏幕适配器选项（~10 行）
- 运行时换盘接受自定义磁盘对象（~5 行）
- 可暂停虚拟时钟（~30–60 行）
- 列级脏矩形（~50 行）

## 注意事项
- v86 无头时必须自己周期调用 `screen_fill_buffer`，否则垂直回扫位不翻转，等待 vsync 的软件可能卡住。
- v86 绝对鼠标需同时发一个非零 `mouse-delta` 以触发 PS/2 中断。
- Mac 时钟：暂停后不补 tick，但每秒从宿主重读日期，Time Manager 到期任务会立即触发，所以必须用虚拟时钟。
- Mac wasm 必须配套它自己生成的 `.js`（`EM_ASM` 与构建绑定），我们只替换 `globalThis.workerApi`。接口无版本，需锁定 commit。
- GPL 核心单独发包并附源码与补丁；统一 API 层可用宽松许可证。
