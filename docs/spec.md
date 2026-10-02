# Webulator 统一模拟器 API 规格 · v1 草案

> 日期：2026-09-30 · 状态：草案，待评审 · 名字已定：Webulator（web + emulator）
> 依据：`docs/findings/v86-vs-qemu.md`（S0：v86 / QEMU 对比）、`docs/findings/mac-snapshots.md`（S2：三个 Mac 模拟器的快照实测）、`docs/backends.md`
> 语言：目前为中文，稳定后补英文版。

## 0. 定位与原则

Webulator 把多个 WebAssembly 模拟器（v86、Mini vMac、Basilisk II、SheepShaver，以后还有其他）包成**一套严格一致的浏览器 API**。
第一个使用方是 OS Museum（一个在浏览器里展示历代操作系统的项目），但 API 本身不依赖博物馆。

**原则**

1. **能力严格一致，不做最小公约数。** 契约里的每一项能力，每个核心都必须完整实现，并通过同一套一致性测试。
   有核心做不到的能力，就不进当前版本的契约；等所有核心都能做到时，再一起加入下一版。没有"可选能力"。
2. **能力和硬件属性分开。** "能否存档""鼠标能否精确落点"是能力，必须一致。
   "是 68k 还是 x86""有几个鼠标键""有没有滚轮""支持哪些分辨率"是被模拟机器的属性，API 统一暴露，但取值可以不同。
3. **契约按 profile 保证。** profile = 核心 + 机型 + 系统镜像（含必要的镜像预处理，比如为 Win9x 装绝对鼠标驱动）。
   一致性测试逐个 profile 运行。
4. **尽量不改模拟器源码。** 优先用编译参数和宿主层实现契约（S2 已证明 Mac 三个核心都可以）。必须改源码时，只加一小叠补丁，锁定上游版本，并尽量提交回上游。
5. **快照是一等公民。** "一张截图瞬间变成活的机器"是这个库存在的首要理由。

## 1. 分层

```
┌─────────────────────────────────────────────────────────────┐
│ ⑤ Automation   waitForScreen / click / type / 录制与回放      │  纯 JS，基于 ② 实现，与核心无关
├─────────────────────────────────────────────────────────────┤
│ ② Machine API  主线程公开 API：生命周期、画面、输入、磁盘、快照、时钟 │  本文 §3
├─────────────────────────────────────────────────────────────┤
│ ③ Runtime      Worker 内：控制块、虚拟时钟、块设备、快照容器、安全点     │  本文 §4，所有核心共用
├─────────────────────────────────────────────────────────────┤
│ ① Core Adapter 每个核心一个：把模拟器接到 Runtime 的 Core ABI 上      │  本文 §5
├─────────────────────────────────────────────────────────────┤
│   Emulator     v86 / Mini vMac / Basilisk II / SheepShaver …（wasm）│
└─────────────────────────────────────────────────────────────┘
④ 数据格式：磁盘清单、快照容器、profile 描述（§6）
⑥ 一致性测试（§7）
```

每台机器独占一个 Worker。模拟器主循环会阻塞 Worker 线程，所以主线程与 Worker 之间的实时控制走 SharedArrayBuffer（§4.1）。
这要求页面处于 cross-origin isolated 状态（COOP: same-origin + COEP: require-corp）。v1 不提供非隔离环境的回退方案。

## 2. 契约 v1：必须实现的能力

| # | 能力 | 严格定义 | 实测状态 |
|---|---|---|---|
| C1 | 生命周期 | 在 Worker 中创建、冷启动、暂停、继续、销毁；不依赖 DOM | 四个核心均已跑通 |
| C2 | 画面 | 输出 RGBA8888 帧，附带脏矩形和帧序号；分辨率变化时发出事件；可随时读取当前帧 | 已跑通；v86 需主动拉帧，SVGA 仅有行级脏区 |
| C3 | 键盘 | 按物理键码（W3C `KeyboardEvent.code`）发送按下/抬起；`type(text)` 按美式布局转换 | v86 已有；Mac 核心需要 code→ADB 映射表 |
| C4 | 指针 | `moveTo(x, y)` 按客户机像素**精确落点**；另有相对移动和按键 | Mac 核心原生支持；v86 需客户机装 vmmouse 驱动（属于 profile 预处理） |
| C5 | 磁盘 | 宿主提供块设备，支持按需读取；写入进入覆盖层，可导出和导入；**取块不阻塞客户机**：数据未到时客户机照常运行（中断、光标都不停），到了再完成这次读写（T12） | Mac 核心已跑通（4 KB 脏块）；v86 靠鸭子类型接入，待实现 |
| C6 | 快照 | 运行中随时请求存档，在有限时间内于安全点完成；可恢复到**全新实例**；恢复后首帧与存档时逐像素一致；构建不匹配时拒绝恢复 | 三个 Mac 核心已通过（首帧 0 像素差）；v86 原生支持，待接入 |
| C7 | 虚拟时钟 | 客户机时间只在机器运行时前进；暂停、存档到恢复之间的空白对客户机不可见；可指定初始时刻 | 三个 Mac 核心已通过；**缺了它 Basilisk II 恢复后会慢到秒级**；v86 待接入 |
| C8 | 无头运行 | 所有能力都能在没有画布的情况下使用（自动化、测试、展品工厂） | 已跑通 |

**不在 v1 的能力**（原因：至少一个核心目前做不到，或尚未验证）

| 能力 | 卡在哪 | 目标版本 |
|---|---|---|
| 音频输出 | 各核心都有音频出口，但未验证统一 PCM 格式和时钟同步 | v1.1 |
| 运行中换盘（软盘、光盘插拔） | Mac 核心有接口，v86 需要走内部对象；均未验证 | v1.1 |
| 网络（以太网帧） | Mini vMac 没有以太网 | v2（或给 Mini vMac 增加 LocalTalk 桥接） |
| 剪贴板 | v86 需要客户机代理；Mini vMac 的 Emscripten 构建没有 | v2 |
| 速度控制（原速 / 最快） | 仅 Mini vMac 自带；其他核心需在宿主层用虚拟时钟调速，未验证 | v2 |
| 确定性执行（同样输入必然同样输出） | 各核心都依赖宿主时间，没有逐指令计时 | 研究项 |

## 3. Machine API（主线程）

TypeScript 描述。所有坐标都是客户机像素坐标。所有异步操作返回 Promise。

### 3.1 创建

```ts
import { Machine } from "@webulator/api";

const m = await Machine.create({
  profile: await loadProfile("https://…/profiles/sheepshaver-g3bw-macos904.json"), // §6.3
  memory: 64 << 20,                       // 客户机内存；缺省用 profile 的默认值
  disks: [{ id: "hd0", source: { manifest: "https://…/macos904.manifest.json" } }],
  snapshot: { url: "https://…/finder.webusnap" },  // 可选：从快照启动，而不是冷启动
  clock: { start: Date.parse("1999-10-17T09:00:00Z") }, // 可选：客户机时钟的初始时刻
  display: canvasElement,                 // 可选：交给库负责绘制；无头运行时不传
});
```

| 字段 | 说明 |
|---|---|
| `profile` | 必填，见 §6.3。决定核心、机型、ROM、默认内存、分辨率、硬件属性 |
| `memory` | 可选，必须是 profile 允许的取值之一 |
| `disks` | 至少一个。`source` 可以是磁盘清单（§6.1）、`{ url, size }`（整块 Range 读取）或 `{ buffer }` |
| `snapshot` | 可选。给了就从快照恢复，不再冷启动；此时 `disks` 只提供基础镜像，快照里的覆盖层叠加在上面 |
| `clock.start` | 可选。冷启动时客户机时钟的起点；从快照恢复时忽略，时钟从快照里记录的时刻继续 |
| `display` | 可选。传入 canvas 时由库负责绘制和缩放；不传则只能通过 `screen` 读取像素 |

`Machine.create` 在 Worker 已加载核心、并已开始冷启动或已完成恢复时返回。返回时机器处于 `running` 状态。

### 3.2 描述与状态

```ts
m.info = {
  core: "sheepshaver", coreVersion: "2.5+im.3f1c2a0", buildId: "sha256:…",
  arch: "ppc", machine: "Power Macintosh G3 (Blue & White)",
  hardware: { pointerButtons: 1, wheel: false, keyboard: "adb-extended" },
  screen: { width: 640, height: 480 },
};
m.state            // "running" | "paused" | "destroyed" | "crashed"
m.on("state", s => …)
```

### 3.3 生命周期

```ts
await m.pause();     // 客户机停止执行，虚拟时钟冻结
await m.resume();
await m.restart();   // 冷启动，保留磁盘覆盖层
await m.destroy();   // 结束 Worker，释放内存
```

核心崩溃（wasm trap、abort）时，状态变为 `crashed`，并发出 `error` 事件。库不会自动重启。

### 3.4 画面

```ts
m.screen.width, m.screen.height
m.on("resize", ({ width, height }) => …)
m.on("frame", ({ seq, dirty }) => …)     // dirty: Rect[]；没有精确脏区的核心报整屏
const img = await m.screen.read(rect?)   // → ImageData（RGBA，拷贝）
m.screen.attach(canvas) / m.screen.detach()
```

- 帧只在内容变化时产生，至多每个显示刷新周期一次。
- `read()` 返回的是最近一帧，保证与客户机帧缓冲一致；它不会触发客户机重绘。

### 3.5 输入

```ts
m.input.key("KeyA", true); m.input.key("KeyA", false);   // KeyboardEvent.code
await m.input.type("Hello, world\n");                     // 美式布局；需要 Shift 的字符自动加 Shift
m.input.pointer.moveTo(320, 240);                         // 精确落点
m.input.pointer.move(5, -3);                              // 相对移动（游戏、指针锁定）
m.input.pointer.button(0, true); m.input.pointer.button(0, false);
m.input.pointer.wheel(0, -120);                           // 仅当 info.hardware.wheel 为 true
```

- 输入事件按顺序进入 Worker 的输入环形缓冲（§4.1），不会合并或丢失；缓冲满时 `key` 和 `button` 会等待，`moveTo` 只保留最新位置。
- 超出硬件属性的输入（比如在单键 Mac 上按右键）抛出 `UnsupportedInputError`，不做静默映射。

### 3.6 磁盘

```ts
const d = m.disks.get("hd0");
d.size; d.readOnly;
const overlay = await d.exportOverlay();   // → Blob（§6.1 的覆盖层格式）
// 导入：在 Machine.create 的 disks[i].overlay 里传入
```

### 3.7 快照

```ts
const snap = await m.saveState({ thumbnail: true });   // → Blob（§6.2 容器）
await m.restoreState(snap);                            // 在内部换一个新 Worker 恢复；m 的身份不变
```

- `saveState()` 在下一个安全点完成（§4.4）。当前实测：请求到拿到快照 17–318 ms。超过 `timeout`（默认 2 s）仍未到达安全点时抛出 `SnapshotTimeoutError`。
- 快照包含：客户机的全部状态、全部磁盘覆盖层、虚拟时钟、最后一帧。
- 快照绑定 `buildId`。恢复时核心构建不同则抛出 `BuildMismatchError`，不尝试兼容。
- 恢复后的第一帧必须与快照里记录的最后一帧逐像素一致（一致性测试 T2）。

### 3.8 时钟

```ts
m.clock.now()          // 客户机当前时刻（epoch ms）
m.clock.elapsed()      // 客户机已运行时长，不含暂停
```

客户机看到的时间 = 起始时刻 + 运行时长。暂停、存档到恢复之间的空白都不计入。

### 3.9 错误

| 错误 | 何时 |
|---|---|
| `ProfileError` | profile 无效，或参数超出 profile 允许的范围 |
| `AssetError` | ROM、磁盘或快照下载失败，或校验和不符 |
| `BuildMismatchError` | 快照的 `buildId` 与当前核心不同 |
| `SnapshotTimeoutError` | 在限定时间内没有到达安全点 |
| `UnsupportedInputError` | 输入超出机器的硬件属性 |
| `CoreCrashedError` | 核心 wasm trap 或 abort；机器进入 `crashed` |

## 4. Runtime（Worker 内，所有核心共用）

Runtime 就是 `prototype/web/emu-worker.mjs` 的正式版本。

### 4.1 控制块（SharedArrayBuffer）

模拟器主循环阻塞 Worker，`postMessage` 在运行中收不到，所以主线程到 Worker 的所有实时控制都走共享内存。

```
Int32Array control[ … ]
  [0]  flags        bit0 请求暂停 · bit1 请求存档 · bit2 请求销毁
  [1]  wake         Atomics.notify 用来唤醒空闲等待
  [2]  inputHead    环形缓冲写指针（主线程写）
  [3]  inputTail    环形缓冲读指针（Worker 写）
  [4]  pointerX     最新绝对坐标（合并写，不进环）
  [5]  pointerY
  [6]  pointerSeq   每次写坐标时加一
  [16..]  input ring: 256 条 × 4 个 int32 = { type, a, b, c }
          type: 1 key(code 编号, down) · 2 button(index, down) · 3 move(dx, dy) · 4 wheel(dx, dy)
```

S2 原型用的是单槽邮箱，连续两次输入之间如果核心没来得及读，前一次就会丢。正式版必须用环形缓冲。

Worker 到主线程方向（帧、日志、快照结果）用 `postMessage`；这个方向在阻塞时也能发出。

### 4.2 虚拟时钟

在加载任何核心代码**之前**，Runtime 替换 Worker 全局的 `Date.now` 和 `performance.now`：

```
guestNow = realNow + offset
```

- 暂停：记下暂停时的 `realNow`；继续时把暂停时长加进 `offset`。
- 存档：记录 `guestNow`（两个时钟各记一个）。
- 恢复：设置 `offset = 存档时的 guestNow − 当前 realNow`。

依据：Emscripten 的所有时间导入（`emscripten_date_now`、`emscripten_get_now`、`clock_time_get`）最终都调用这两个函数；v86 的 `microtick` 在模块加载时绑定 `performance.now`，所以替换必须在加载之前。
S2 实测：Basilisk II 不加虚拟时钟时，快照放一分钟再恢复，响应会慢到 5–8 秒。

Runtime 自己的计时和统计只用替换前保存的真实时钟。

### 4.3 块设备

```ts
interface BlockDevice {
  size: number;
  readOnly: boolean;
  read(offset: number, length: number, dst: Uint8Array): void;   // 同步
  write(offset: number, src: Uint8Array): void;                  // 同步，进覆盖层
  exportOverlay(): { chunkSize: number; chunks: number[]; data: Uint8Array };
}
```

- **读写本身是同步的，但取块不阻塞**（2026-10-02 修订，见 §11）：核心先问 `ready(offset, length)`；数据未到时
  Runtime 交给后台取块 Worker 去取，核心向客户机报"忙"，客户机继续运行，稍后再问。取块 Worker 把数据写进
  SharedArrayBuffer 并唤醒核心线程（核心线程可能从不回到事件循环）。没有先问就直接读的核心退回同步 XHR。
- 基础镜像按 256 KB 块、内容寻址存放（§6.1），可跨镜像去重，可长期缓存。
- 覆盖层按 4 KB 记录写过的块（S2 实测：Mac OS 9 从开机到桌面写了 46 块，约 184 KB）。

### 4.4 快照：安全点与容器

- **安全点**：核心每次轮询输入时（每个客户机 tick 至少一次），Runtime 检查"请求存档"标志；满足该核心的安全条件时，就在这里存档。
- **安全条件**由适配器声明（§5）。例如 Asyncify 类核心要求调用栈上的 CPU 主循环只出现一次，并且不在指定的旁路上（S2 的嵌套保护）。不满足时推迟到下一个安全点。
- 快照内容由适配器产出（不透明字节），Runtime 负责加上磁盘覆盖层、时钟、最后一帧，封装成 §6.2 的容器。

## 5. Core ABI（适配器接口）

每个核心实现一个适配器，运行在 Worker 里：

```ts
interface CoreAdapter {
  readonly manifest: {
    id: string;                 // "v86" | "minivmac" | "basilisk2" | "sheepshaver"
    version: string;            // 上游版本 + 补丁标识
    buildId: string;            // wasm 文件的 sha256
    license: string;            // "BSD-2-Clause" | "GPL-2.0-only"
    arch: "x86" | "m68k" | "ppc";
  };
  boot(env: CoreEnv, config: CoreConfig): void;          // 冷启动；可能永不返回（阻塞式主循环）
  restore(env: CoreEnv, config: CoreConfig, state: Uint8Array): void;
  saveState(): Uint8Array;        // 只会在安全点、且 isSafePoint() 为真时被调用
  isSafePoint(): boolean;
}

interface CoreEnv {
  disks: BlockDevice[];
  files: Record<string, Uint8Array>;  // ROM、BIOS、prefs 等
  input: InputQueue;                   // 读取 §4.1 的环形缓冲
  video: { open(w: number, h: number): void; frame(rgba: Uint8Array, dirty?: Rect[]): void };
  pollHook(): void;                    // 适配器必须在每个客户机 tick 至少调用一次；存档、暂停、销毁都在这里处理
}
```

### 5.1 两类核心的做法

| | Asyncify 类（Mini vMac、Basilisk II、SheepShaver） | 原生快照类（v86） |
|---|---|---|
| 主循环 | 阻塞，永不返回 | 由 v86 自己调度，可在事件循环里运行 |
| 存档 | 在输入轮询处 `Asyncify.handleSleep` 展开调用栈，复制整块线性内存、栈指针、Asyncify 数据指针和回卷入口 | 调用 `save_state()` |
| 恢复 | 新实例 `noInitialRun`，写回内存和栈指针，重建回卷表，`asyncify_start_rewind` 后回卷到原位 | `restore_state()` 或 `initial_state` |
| 编译 | `-sASYNCIFY -sASYNCIFY_IMPORTS=[EM_ASM 导入] -sASYNCIFY_ONLY=[主循环到输入轮询的路径]`，外加一段 `--pre-js` | 上游 npm 包，不需重新编译 |
| 安全条件 | 调用栈上 CPU 主循环只出现一次，且不在空闲等待等旁路上 | 始终为真 |
| 代价（实测） | Mini vMac、Basilisk II 无；SheepShaver 约 15% | 无 |

`ASYNCIFY_ONLY` 的名单写在各适配器的构建配置里（S2 实测名单见 `prototype/build/macemu/only*.json` 和 `docs/findings/mac-snapshots.md`）。

### 5.2 各核心的适配要点

- **v86**：用鸭子类型对象接入块设备（需要 `get`/`set`/`load`/`get_state`/`set_state` 等）；画面需要 Runtime 定时调用 `screen_fill_buffer()`，否则垂直回扫位不翻转；绝对鼠标每次还要附带一个非零的相对位移。
- **Mini vMac**：只能用 -O0 编译（-O1 及以上客户机会崩溃，原因未查）；没有以太网。
- **Basilisk II**：`INITIAL_MEMORY` 按客户机内存缩小（S2 用 80 MB，恢复时间减半）；需要 gmp/mpfr。
- **SheepShaver**：输入轮询位于 PPC 解释器主循环内部，Asyncify 带来约 15% 的速度损失。如需消除，可打一个补丁，让主循环每执行一段指令就返回外层一次。

## 6. 数据格式

### 6.1 磁盘清单与覆盖层

```jsonc
// *.manifest.json
{ "format": "webulator-disk/1", "name": "Mac OS 9.0.4 HD", "size": 209715200,
  "chunkSize": 262144, "hash": "sha256",
  "chunks": ["a1b2…", "", "c3d4…"],          // "" 表示全零块，不存储
  "baseUrl": "https://…/chunks/" }            // 块地址 = baseUrl + hash
```

覆盖层：`{ format: "webulator-overlay/1", chunkSize: 4096, chunks: [块号…] }`，后接各块数据。

### 6.2 快照容器

```
"WEBUSNAP" | u32 版本 = 1 | u32 头长度 | JSON 头 | 各段数据
头: { core, coreVersion, buildId, profile, createdAt,
      clock: { guestDate, guestPerf },
      screen: { width, height },
      sections: [ { name: "core" | "disk:<id>" | "frame", codec: "raw" | "gzip" | "zstd", length } ] }
```

- `core`：适配器的不透明快照。Asyncify 类核心就是整块线性内存加回卷信息，大部分是零，压缩后很小。
- `frame`：最后一帧，用于首帧校验，也可以直接当静态截图显示。
- 传输压缩：浏览器端基线是 gzip（`DecompressionStream` 原生支持）；zstd 小 15–35%，但需要额外的 wasm 解码器。

实测体积（原始 / gzip / zstd）：

| profile | 原始 | gzip | zstd |
|---|---|---|---|
| Mini vMac · System 6.0.8 | 48 MB | 0.56 MB | 0.33 MB |
| Basilisk II · System 7.5.3（32 MB 客户机内存） | 80 MB | 2.0 MB | 1.65 MB |
| SheepShaver · System 7.5.3 PPC（64 MB） | 128 MB | 6.4 MB | 4.8 MB |
| SheepShaver · Mac OS 9.0.4（64 MB） | 128 MB | 11.4 MB | 8.4 MB |
| v86 · Windows 98（128 MB） | 约 37 MB（S0 实测） | 未测 | 约 13 MB（v86 官方演示的存档文件） |

### 6.3 Profile

```jsonc
{
  "id": "sheepshaver-g3bw-macos904",
  "core": { "id": "sheepshaver", "buildId": "sha256:…" },
  "machine": { "name": "Power Macintosh G3 (Blue & White)", "rom": { "url": "…", "sha256": "…" },
               "memory": { "default": 67108864, "allowed": [67108864, 134217728] },
               "screen": { "width": 640, "height": 480 },
               "hardware": { "pointerButtons": 1, "wheel": false, "keyboard": "adb-extended" } },
  "disks": [ { "id": "hd0", "manifest": "…/macos904.manifest.json" } ],
  "coreConfig": { "prefs": "…" },               // 交给适配器的原样配置
  "conformance": {                               // 一致性测试要用的钩子
    "ready": { "screen": "…/finder-ready.png", "region": [0, 0, 640, 20] },
    "alive": [ { "click": [20, 8] }, { "expectChange": [0, 20, 220, 250] } ],
    "cursor": { "read": "memory", "y": "0x830", "x": "0x832" },  // 客户机光标位置从哪里读回（Mac 低内存 Mouse，Point 先 v 后 h）
    "textEcho": { "open": ["…"], "readBack": "screen-ocr" }
  }
}
```

## 7. 一致性测试

每个 profile 都必须全部通过。测试运行在 Playwright 驱动的无头 Chromium 里（S0、S2 已有这套设施）。

| # | 测试 | 通过标准 |
|---|---|---|
| T1 | 冷启动 | 在 profile 规定的时间内到达 `ready` 画面 |
| T2 | 快照往返 | 存档 → 销毁 → 新 Worker 恢复；首帧与存档帧 0 像素差；连续 5 次 |
| T3 | 恢复后可用 | 执行 profile 的 `alive` 脚本，每一步都在 1 s 内看到预期的画面变化 |
| T4 | 时间空白 | 存档后等 60 s 再恢复，T3 中的响应时间不超过冷启动时的 2 倍 |
| T5 | 暂停冻结时钟 | 暂停 10 s 再继续，客户机时钟前进小于 0.1 s |
| T6 | 精确落点 | `moveTo` 到 20 个随机点，从 `cursor` 钩子读回，误差为 0 |
| T7 | 键盘 | `type` 全部可打印 ASCII 字符，经 `textEcho` 读回后完全一致 |
| T8 | 磁盘覆盖层 | 在客户机里写文件 → 存档 → 恢复，文件仍在；导出覆盖层后重新导入，文件仍在 |
| T9 | 忙碌时存档 | 客户机高负载（profile 提供的负载脚本）时请求存档，2 s 内完成 |
| T10 | 输入不丢 | 连续发送 200 个按键事件（间隔 1 ms），客户机全部收到且顺序正确 |
| T11 | 无头 | 不传 canvas，T1–T3 仍然通过 |
| T12 | 取块不阻塞 | 每次取块延迟 400 ms、缓存为空，指针持续移动并执行 profile 的读盘操作，帧间隔小于 200 ms（§12） |
| P1 | 性能记录（不判定通过） | 记录快照版构建相对原版构建的启动时间比，写进报告 |

## 8. 打包与许可证

| 包 | 内容 | 许可证 |
|---|---|---|
| `@webulator/api` | Machine API、Runtime、容器格式 | MIT |
| `@webulator/automation` | 自动化层 | MIT |
| `@webulator/conformance` | 一致性测试与测试运行器 | MIT |
| `@webulator/core-v86` | v86 适配器（依赖上游 npm 包） | BSD-2-Clause |
| `@webulator/core-minivmac` | 适配器 + wasm + 构建脚本 + 源码指引 | GPL-2.0-only |
| `@webulator/core-basilisk2` | 同上 | GPL-2.0-only |
| `@webulator/core-sheepshaver` | 同上 | GPL-2.0-only |

ROM、BIOS 和系统镜像一律不随包分发，由 profile 以 URL 和校验和引用。

## 9. 当前实现与契约的差距

| | v86 | Mini vMac | Basilisk II | SheepShaver |
|---|---|---|---|---|
| C1 生命周期 | 待接入 | ✅ | ✅ | ✅ |
| C2 画面 | 待接入（需主动拉帧） | ✅ 有脏矩形 | ✅ 仅整帧 | ✅ 仅整帧 |
| C3 键盘 | 已有 API | code→ADB 映射表待写 | 同左 | 同左 |
| C4 精确落点 | 需客户机装驱动；未测 | 原生支持；T6 未测 | 同左 | 同左 |
| C5 磁盘 | 待接入 | ✅（整盘在内存，按需读取待做） | 同左 | 同左 |
| C6 快照 | 原生支持，待接入 Runtime | ✅ T2 通过 | ✅ | ✅ |
| C7 虚拟时钟 | 待接入 | ✅ | ✅ | ✅ |
| C8 无头 | ✅ | ✅ | ✅ | ✅ |
| 输入环形缓冲 | 待做 | 原型是单槽邮箱，待改 | 同左 | 同左 |

## 10. 待定问题

1. **v86 与控制块**：v86 不会阻塞 Worker，本可以直接用 `postMessage`。v1 为了一致，仍走同一个控制块；这会给 v86 多一点延迟，要实测。
2. **快照体积**：Mac OS 9 的快照 gzip 后 11 MB。是否需要"内存页相对基础快照做差量"（同一系统的所有展品共享一个开机快照）？估计能降到 1–2 MB，但格式会复杂一些。建议 v1.1。
3. **Mini vMac 编译优化**：-O1 以上让客户机崩溃。不影响正确性，但它是唯一只能跑 -O0 的核心。
4. **SheepShaver 的 15%**：接受，还是打补丁消除？建议先接受，等有真实展品后再判断。

## 11. v1 实现后的修订（2026-09-30）

实现和一致性测试中确定的补充，已按此实现：

1. **`screen.seq`**：当前帧的序号；0 表示这一帧来自快照文件，核心还没有出过帧。测试"恢复后首帧一致"时用它区分。
2. **输入节奏由适配器负责（C3/C4、T10）**：Basilisk II / SheepShaver 的 ADB 缓冲只有 16 个事件，Windows 98 的键盘缓冲也会溢出。
   适配器按 `eventIntervalMs`（默认 8 ms 客户机时间）逐个送出按键和鼠标按键事件，队列保序，不丢事件。指针移动不受限、总是合并为最新位置。
3. **v86 的绝对指针**：位置经 VMware 后门送出，但客户机驱动只在收到 PS/2 鼠标中断时读取，所以每次移动都先发一个相应的相对位移。
4. **客户机关机**：核心主循环因关机结束时，机器保持 `running`（没有新状态），Worker 改由定时器继续响应写入层导出等请求。
   Emscripten 退出路径抛出的 `ExitStatus` 不算崩溃。已知问题：SheepShaver 在 Mac OS 9 关机时核心自身越界访问，报告为 `crashed`。
5. **profile 可以声明客户机镜像期待的硬件**：`coreConfig.offlineNic`（v86）提供一块网卡，ARP/DHCP 在 Worker 内应答，不放任何流量出去。
   这是机器属性，不是网络能力；网络仍不在 v1 契约里。
6. **诊断接口（非规格）**：`disks.get(id).access()` 返回基础镜像的取块统计和首次访问顺序，可用来生成磁盘清单的 `prefetch`。
7. **一致性测试的调整**：
   - "画面稳定"按像素判断（变化少于 50 像素），因为部分核心每次都报整屏脏区，且文本插入符会闪烁；
   - T5 只统计暂停生效之后的时钟变化，暂停延迟单独记录；
   - T6 以光标形状对应的固定偏移为准，要求 20 个点全部一致；
   - T8 在核心支持时先正常关机；
   - T7 只比较 1.5 秒内保持不变的像素。

## 12. 取块不阻塞客户机（2026-10-02）

问题：Runtime 原先在核心线程里用同步 XHR 取块。取块期间整个客户机停住，光标也由客户机绘制，所以鼠标跟着冻结。
磁盘放到 R2 之后每块约 0.4–0.5 s，冷缓存下操作时明显卡顿。真机不是这样：磁盘忙时 CPU 照常运行、照常处理中断。

做法：

1. **Runtime**：`BlockDevice.ready(offset, length)` 检查涉及的块（被覆盖层整块覆盖的部分不需要基础数据），缺块时交给后台取块
   Worker（8 个 1 MB 槽位）。完成后写入共享内存、`Atomics.notify` 唤醒空闲等待；核心线程在 `poll()` 和 `ready()` 里收取。
2. **v86**：它的 IDE 本来就是异步的（回调里才发完成中断）。适配器把读写排进一个保序队列，数据到了才回调；
   等待中的写入自带一份数据副本（v86 会复用缓冲区）。存档请求等队列清空再做（客户机期间照常运行）。
3. **Basilisk II、SheepShaver**（同一份 `disk.cpp`）：`DiskPrime` 先调用 `Sys_ready`，数据未到时返回 1（忙），
   ROM 里驱动的 Prime 桩改成"返回 1 就重试"。两次重试之间 CPU 照常执行和响应中断。补丁见 `prototype/build/macemu/patches/`。
4. **Mini vMac**：`Sony_Prime` 同样先问，未到返回 1；重试循环追加在替换版 `.Sony` 驱动之后，原 Prime 入口改为跳过去，
   驱动本体的偏移都不变。补丁见 `prototype/build/minivmac/patches/`。
5. **快照**：Mac 核心不需要特殊处理，等待中的请求只是客户机里的重试循环，恢复后自然重新发起；v86 等队列清空后再存档。
6. **测试钩子（非契约）**：`Machine.create({ debug: { diskLatencyMs } })` 给每次取块加延迟，T12 用它。

新增一致性测试 T12：取块延迟 400 ms、缓存为空时，指针持续移动期间帧间隔必须小于 200 ms。补丁前三个 Mac 核心为 416–836 ms。
