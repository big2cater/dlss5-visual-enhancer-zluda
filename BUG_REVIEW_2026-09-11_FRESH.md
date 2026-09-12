# 独立代码审查报告（新一轮）

**审查对象**：`dlss5-image-enhancer-zluda` 当前工作树（HEAD = `d8aacf4`）
**审查方式**：静态通读全部源文件 + 对可执行部分做实测复现（编译、哈希比对、数值对照）
**与旧报告的关系**：本文件不重复 `BUG_REVIEW_2026-09-11.md`。**每一条都经过独立复核**；其中若干条是旧报告已修好的项（见文末「已确认修复」），也标注了旧报告中**经核查不成立**的条目。

## 图例

- 🔴 **致命**：导致挂死、静默产出错误结果、或发布包不可用
- 🟠 **严重**：崩溃/泄漏/功能性失效，但需要特定条件才触发
- 🟡 **中等**：正确性、健壮性、可维护性问题
- 🟢 **轻微**：卫生问题、文档不符

## 复核结论摘要

| 严重度 | 数量 | 主要影响面 |
|---|---|---|
| 🔴 致命 | 5 | 挂死在无人可救的等待；静默产出黑帧却退出码 0；并行模式帧数错误 |
| 🟠 严重 | 12 | 句柄/资源泄漏；管线竞态；参数被静默忽略；输出文件被覆盖 |
| 🟡 中等 | 12 | 错误处理、路径、数值精度、打包可移植性 |
| 🟢 轻微 | 6 | 文档与实现不符、构建图缺项 |

> **A–G 段为第一轮（后端 `video_filter` + 核心层 `core/` `dlss_layer/`）**；
> **H 段为第二轮（Qt GUI + 随包 PowerShell/Python 脚本）**。两轮结论均已独立复核后才写入。

---

# A. 致命问题

## A1 🔴 `core/image_processor.cpp:198-204` — D3D12 栅栏等待无限阻塞，且 HRESULT 全部未检查

```cpp
void wait() {
    queue->Signal(fence, ++fence_value);
    if (fence->GetCompletedValue() < fence_value) {
        fence->SetEventOnCompletion(fence_value, fence_event);
        WaitForSingleObject(fence_event, INFINITE);
    }
}
```

**问题**：`Signal`、`SetEventOnCompletion` 的返回值都被丢弃，等待是 `INFINITE`。

**实证对照**：同一仓库 `dlss_layer/dlss_cuda.cpp:558-577` 的 `flush_and_wait()` 做的是**同一件事但做对了**——它有 10 秒超时、检查 HRESULT、并在超时时调用 `GetDeviceRemovedReason()` 报告原因。两处实现不一致，说明正确写法在仓库内已存在，只是没有应用到这一处。

**后果**：当设备被移除（TDR / 驱动重置 / ZLUDA 卡住）时，fence 永远不会被置位，该线程**永久阻塞**。调用点是 `process()`（`:597, :623, :668, :772`）与 `process_raw_rgb48()`（`:910, :950, :1043`），也就是渲染/GUI 线程。没有取消路径：`stop()` 无法被触达。

**加剧因素**：`core/image_processor.cpp:328` 的 `s->fence_event = CreateEventW(...)` **未检查返回值**。若事件句柄为 `NULL`，`wait()` 退化为「完全不等待」——`process()` 会在 GPU 仍在写 `s->readback` 时去 `Map()` 并 `memcpy`，静默产出错误像素且不报任何错。

**修复**：照搬 `flush_and_wait()` 的超时+错误检查模式；校验 `fence_event` 非空；等待返回 `WAIT_FAILED`/`WAIT_TIMEOUT` 时报错退出而非继续。

## A2 🔴 `tools/video_filter.cpp:2054-2073` — 并行分片编排器无超时，任一 worker 挂死则永不返回

```cpp
HANDLE handles[2] = { pi0.hProcess, pi1.hProcess };
DWORD code0 = STILL_ACTIVE, code1 = STILL_ACTIVE;
while (true) {
    WaitForMultipleObjects(2, handles, FALSE, 500);
    GetExitCodeProcess(pi0.hProcess, &code0);
    ...
}
```

**问题**：循环只在**两个子进程都退出**、或**某个子进程以非零码退出**时才 `break`。`WaitForMultipleObjects` 的返回值（可能为 `WAIT_FAILED`）被丢弃，也没有任何墙钟截止时间。

**后果**：这是本文件自己反复注释预警的场景（`:1160-1165`、`:2709-2713` 都在讲 GPU 可能卡住、ffmpeg 子进程可能堵塞管道）。一旦 worker 挂死：
- 父进程与 GUI **永久卡住**；
- `*.part0.mp4` / `*.part1.mp4` 临时文件永不清理；
- 若 `WaitForMultipleObjects` 因句柄问题返回 `WAIT_FAILED`，同样死循环。

**修复**：检查等待返回值；加入基于总时长的硬性截止（如 `total_dur × 系数 + 余量`），超时后 `TerminateProcess` 双方并返回失败。

## A3 🔴 `tools/video_filter.cpp:2568-2573` + `:2774` — 视频中途出现黑帧只警告，最终**退出码 0**（静默产出坏视频）

```cpp
bool blank = false;
half_rgba_to_rgb48(frame->out, frame->model_output.data(), blank);
if (blank) {
    blank_count.fetch_add(1);
    fprintf(stderr, "[warn] frame %u output looks blank\n", frame->index);
}
```

对比同文件 `:2686-2692`——**只有 `index == 0` 的首帧**才会触发失败与重试。

**问题**：`blank_count` 只被打印（`:2771`），从不影响 `failed` / 返回值（`:2774`）。

**后果**：`docs/issue_blank_race_report.md:31` 已实测记录「黑图在进程内是 sticky 的」。因此任何发生在首帧之后的复位边界（`--reset always`、或 `--reset every=N` 的周期性复位、或中途触发切镜检测）都可能产出黑帧段落，而工具**返回 0、报告成功**。用户拿到一段黑屏视频却没有任何错误提示。

**修复**：把 `blank_count > 0`（或超过某比例阈值）视为可重试失败，置 `retryable = true` 并返回非零。

## A4 🔴 `tools/video_filter.cpp:2481-2484` — 并行模式下 `--max-frames` 未按分片重定基，实际输出约 2N 帧

```cpp
unsigned dec_index = 0;
while (!abort_pipeline.load()) {
    if (options.max_frames && (int)dec_index >= options.max_frames) {
        if (decoder.process) TerminateProcess(decoder.process, 0);
        break;
    }
```

**问题**：`options.max_frames` 是用户的**全局**帧预算，却被原样传给每个 worker（`build_worker_cmd`）。而 `dec_index` 是**解码**序号，包含该分片的预热帧。于是 worker 0 解码 `0..N-1`，worker 1 解码「预热帧 + 另外 N 帧」，两者拼起来约为 `2N` 帧。同时 `--frame-index-offset` 又按 N 施加在第二片上，进度与帧号进一步错位。

**修复**：给每个 worker 传按分片重定基后的预算（并显式扣除预热帧数），或让子进程用「已写出帧数」而非解码序号做判断。

## A5 🔴 `tools/video_filter.cpp:2722-2736` — 管道句柄在阻塞线程仍在使用时就被 `close()`

```cpp
if (abort_pipeline.load()) {
    if (decoder.process) TerminateProcess(decoder.process, 1);
    if (encoder.process) TerminateProcess(encoder.process, 1);
    decoder_stdout.close();
    encoder_stdin.close();
    if (decode_thread.native_handle()) CancelSynchronousIo((HANDLE)decode_thread.native_handle());
    if (encode_thread.native_handle()) CancelSynchronousIo((HANDLE)encode_thread.native_handle());
    ...
}
if (decode_thread.joinable()) decode_thread.join();
if (encode_thread.joinable()) encode_thread.join();
```

**问题**：`decoder_stdout.close()` / `encoder_stdin.close()` 在 `CancelSynchronousIo` **之前**执行（`:2725-2726` 早于 `:2727-2728`），而 `join()` 更晚（`:2735-2736`）。此时 decode 线程可能仍阻塞在 `ReadFile(decoder_stdout.read_end, ...)`（`:2494`），encode 线程可能仍阻塞在 `WriteFile(encoder_stdin.write_end, ...)`（`:2576`）。

**后果**：句柄值被关闭后立即可能被其他线程的 `CreatePipe`/`CreateProcess` 复用，`ReadFile` 就作用到了**无关的内核对象**上——间歇性的错误句柄 I/O、污染相邻管道、或把 `ERROR_INVALID_HANDLE` 误报成正常的短读。

**修复**：正确顺序是「先杀子进程 → `CancelSynchronousIo` → `join()` → 最后才 `close()` 句柄」。

---

# B. 严重问题

## B1 🟠 `tools/video_filter.cpp:456-462` — `probe_video` 的读取循环使 15 秒超时形同虚设

```cpp
while (ReadFile(pipe.read_end, buffer, sizeof buffer, &read, nullptr) && read) {
    text.append(buffer, read);
}
pipe.close();
DWORD code = 1;
if (!wait_exit(child.process, 15000, code) || code != 0) {
```

**问题**：`ReadFile` 循环只在管道返回 EOF（`read == 0`）时退出，而 EOF 要求所有写端句柄都关闭。超时检查在循环**之后**，所以对「ffprobe 挂住不退出、也不关闭写端」这一超时本来要防的场景，代码根本走不到那一行。

**后果**：网络路径、损坏容器、被占用的解码器上，ffprobe 挂住即导致 `video_filter` 永久卡在 `probe_video`。15 秒常量实际是死代码。而且超时后也没有 `TerminateProcess`，子进程继续存活。

**修复**：把读取放到独立线程 + `join(timeout)`，或改用 `PeekNamedPipe` 轮询配合截止时间；超时后 `TerminateProcess` 并关闭句柄。

## B2 🟠 `tools/video_filter.cpp:389-394` — `wait_exit` 无法区分「超时」与「真实退出码」，且不杀子进程

```cpp
bool wait_exit(HANDLE process, DWORD timeout_ms, DWORD &code) {
    if (WaitForSingleObject(process, timeout_ms) != WAIT_OBJECT_0) return false;
    code = 1;
    GetExitCodeProcess(process, &code);
    return true;
}
```

**问题**：
1. 超时时直接 `return false`，但 **`code` 保持调用方的初始值 1**（见 `:2746` `DWORD decoder_code = 1`）。调用方 `wait_exit(decoder.process, 15000, decoder_code);`（`:2747-2748`）**忽略了返回值**，于是超时被当成「子进程以退出码 1 退出」，输出 `[FAIL] ffmpeg decoder exited with code 1`——**子进程其实根本没退出**，这条信息是误导性的。
2. `code = 1` 在 `GetExitCodeProcess` **之前**赋值，掩盖该调用的失败。
3. 超时时不 `TerminateProcess`，挂住的子进程只靠进程退出时的 job object 兜底。

**修复**：返回一个三态结果（success / timeout / failed），超时时显式 `TerminateProcess`，并把 `GetExitCodeProcess` 的返回值纳入判断。

## B3 🟠 `tools/video_filter.cpp:2586-2597` — `--dump-frames` 在并行模式下两片互相覆盖，且 `frames_written` 不是计数

```cpp
if (!options.dump_dir.empty() && dump_ok) {
    wchar_t name[512];
    swprintf(name, 512, L"%s\\frame_%05u.png", options.dump_dir.c_str(), frame->index);
```

```cpp
unsigned display_idx = frame->index;
if (options.is_child_chunk) {
    display_idx = (frame->index - options.warmup_discard_frames) + options.frame_index_offset;
}
frames_written.store(display_idx + 1);
```

**问题**：
1. dump 文件名用 `frame->index`（**解码**序号），在并行分片下两个 worker 的序号都从 0 附近开始 → **互相覆盖**，用户看到约一半的帧。
2. `frames_written` 存的是「最后一个显示序号 + 1」，**不是已写帧数**。它在 `:2764-2770` 被当作帧数用于吞吐统计，只有在序号从 0 连续时才凑巧正确。
3. `frame->index - options.warmup_discard_frames` 是无符号运算：若某分片交付的解码帧数少于预热丢弃数，会发生**无符号下溢**，`display_idx` 变成接近 `UINT_MAX` 的巨值——该值会进入文件名与 `[%.5u]` 进度行，`[done]` 吞吐数字随之变成无意义值。

**修复**：用独立计数器统计已写帧数；dump 文件用 `display_idx`；减法前做边界检查并钳制到 0。

## B4 🟠 `dlss_layer/dlss_cuda.cpp:1500-1503` — 背缓冲上传复用了颜色阵列的几何尺寸，且失败被丢弃

```cpp
if (!cu_ok(g.cu.cuMemcpy2D(&copy), "cuMemcpy2D (colour upload)"))
    return false;

if (g.backbuffer.level0) {
    copy.dstArray = g.backbuffer.level0;
    cu_ok(g.cu.cuMemcpy2D(&copy), "cuMemcpy2D (backbuffer upload)");
}
return true;
```

**问题**：上面的边界校验（`:1485-1488`，`src_pitch < row_bytes || rows > ad.Height`）是**针对 `g.color` 的描述符**做的。但 `g.color` 与 `g.backbuffer` 的尺寸并不相同：

- `g.color` 按 `render_width × render_height` 建立（`:1067-1068`）
- `g.backbuffer` 按 `output_width × output_height` 建立（`:1069-1070`）

而放大是被支持的（`core/image_processor.cpp:817-818` 明确允许 `output_width != width`）。所以在放大路径下，第二次 `cuMemcpy2D` 会试图把「颜色阵列大小」的数据写入「尺寸不同」的背缓冲阵列。驱动会以 `CUDA_ERROR_INVALID_VALUE` 拒绝，但**该返回值被丢弃**（`:1502` 未用 `cu_ok` 的结果），函数仍然 `return true`。

**后果**：静默失败被报告为成功，并发出一次越界的拷贝请求。

**修复**：对 `g.backbuffer` 单独取描述符并按它校验；用 `cu_ok` 检查第二次拷贝的结果。

## B5 🟠 `core/image_processor.cpp:347-350` / `:358-365` — 错误返回路径漏调 `stop()`，遗留 D3D12 设备与命令对象

```cpp
if (nvidia_mode) {
    cuda_driver = in_system_directory(L"nvcuda.dll");
    if (cuda_driver.empty()) {
        error = "NVIDIA mode needs NVIDIA's own CUDA driver, and nvcuda.dll is not in the system directory.";
        return false;      // <-- 没有 stop()
    }
```

同类的还有 `:358-365`（CUDA 驱动改名检查失败）。

**问题**：`start()` 在进入这些分支之前，已经在 `:242` 设了 `s->attempted = true`，并且已经创建了 `s->device`、`s->queue`、`s->allocator`、`s->cmd`、`s->fence`、`s->fence_event`。同一函数内其它失败路径（`:253`、`:304`、`:324`、`:444`）都规规矩矩地调用了 `stop()`，**只有这两条漏了**。

**后果**：这些 COM 对象与事件句柄在 `Processor` 存活期间一直被持有。调用方若按「失败就重试」的常规写法反复调用 `start()`，每次都在真实地泄漏一套 D3D12 对象 + 一个句柄。（`~Processor()` 最终会 `stop()` 兜底，所以是「生命周期内泄漏」而非永久泄漏——但这正是 `attempted` 重试机制存在的目的场景。）

**修复**：这两条路径补上 `stop()`，或在函数内统一用 RAII/单一出口管理。

## B6 🟠 `core/image_processor.cpp:368-369` — 进程级 DLL 搜索路径被永久改写，从不还原

```cpp
if (slash != std::wstring::npos)
    SetDllDirectoryW(cuda_driver.substr(0, slash).c_str());
```

**问题**：`SetDllDirectoryW` 修改的是**进程全局**的 DLL 搜索顺序（它会从搜索路径中移除当前目录并插入指定目录）。而 `stop()`（`:1075-1087`）里**没有** `SetDllDirectoryW(nullptr)` 或还原原值。

**后果**：在本项目的 ReShade 插件形态下，宿主进程就是游戏本身——此后游戏（或任何其它插件）的每一次 `LoadLibrary` 都会优先搜索 ZLUDA 所在目录。这是一个 DLL 劫持面，也解释了「装了本软件后别的程序行为怪异」这类难以归因的现象。

**修复**：保存原值并在 `stop()` 中还原（至少调用 `SetDllDirectoryW(nullptr)`）。

## B7 🟠 `dlss_layer/dlss_cuda.cpp:661-668` — `init()` 忽略 `device`/`queue`，第二个 `Processor` 会静默复用第一个的设备

```cpp
bool init(const InitDesc &desc) {
    if (g.initialized) return true;
    if (!desc.device || !desc.queue) { ... }
    g.device = desc.device;
    g.queue = desc.queue;
```

**问题**：`init()` 在已初始化时**无条件**返回 `true`，完全不比较 `desc.device` / `desc.queue` 是否与 `g.device` / `g.queue` 一致。而 `dlss_cuda` 的状态是**单例全局**（`State g`，`:128`），`frame_blit` 另有自己的全局（`frame_blit.cpp:112`）。

**后果**：任何在第一个 `Processor` 仍存活时创建第二个 `Processor` 的场景（或设备被重建后指针恰好复用同一地址），都会拿到「设备 A 的资源 + 用设备 B 的队列发命令」的组合。D3D12 调试层会报错，实际可能表现为 device removed。

`dlss_cuda.h:150-152` 的接口注释只说了失败时返回 false 并设置 `last_error()`，**没有**说明重复调用会被静默忽略——调用方无法从接口上得知这个约束。

**修复**：在 `State` 中保存 device/queue，二者不同时返回 false（或先完整 `shutdown()` 再重建）。

## B8 🟠 `ngx_runtime/ngx_runtime.cpp:283-320` — `reset_all()` 使片段持有的参数对象失效（注释已自认会致「同一参数有时好有时坏」）

```cpp
void reset_all() { count_ = 0; }
```

`find()`（`:130-135`）只遍历前 `count_` 项。`reset_all()` 把 `count_` 归零后，**先前存在过的所有名字都变成「找不到」**，而片段在 `Init_Ext1`（`:303` / `:305`）拿到的那个 `NVSDK_NGX_Parameter*` 仍然指向这个被清空的块。

**问题**：该文件 `:62-70` 的追踪注释本身就记录了「Get 发生在求值期」。因此 `shutdown()` + `init()` 循环之后，片段在**求值期**的查询会静默回退到默认值。更直接的是 `populate` 每次 `create_feature` 都会被调用（`dlss_cuda.cpp:863-864`），于是第二次 `create_feature` 从一个空块开始。

**修复**：不要对可能已被引用的块调用 `reset_all()`；改为每次 init/populate 分配一个全新的参数对象，或只清「创建参数」子集而非全部。

---

# C. 中等问题

## C1 🟡 `tools/video_filter.cpp:1277-1280` vs `:1358` — `--retries` 有两处 `else if`，第一处是死代码

```cpp
} else if (arg == "--retries") {          // :1277
    if (!need("--retries")) return false;
```
```cpp
} else if (arg == "--precompile-wait" || arg == "--retries" || arg == "--retry-delay") {  // :1358
```
`arg == "--retries"` 永远先命中 `:1277`，`:1358` 中该分支不可达。两处对「是否消费值」的约定也不同，属于典型的复制粘贴残留。**修复**：删除 `:1277-1280`。

## C2 🟡 `tools/video_filter.cpp:1241, 1301, 1304, 1321, 1332` — 多个数值参数完全不校验

`--passes`、`--style`、`--preset`、`--crf`、`--max-frames` 都用裸 `atoi`/`atof`，无范围检查。其中 `--max-frames -1` 是**具体可复现的静默失败**：它让 `:2481` 的 `(int)dec_index >= options.max_frames` 在第一帧就成立 → 立即 `TerminateProcess` 解码器 → 写出**零帧**输出，并以 **退出码 0** 报告成功。

**修复**：对每个数值参数做范围校验并拒绝非法值。

## C3 🟡 `tools/video_filter.cpp:1225-1231` — `argc < 7` 使文档中的可选项默认值不可达

```cpp
if (argc < 7) { usage(); return 1; }
options.runtime = argc > 5 ? widen(argv[5]) : L"nvngx.dll";
options.nvapi   = argc > 6 ? widen(argv[6]) : L"nvapi64.dll";
```
既然 `argc < 7` 已返回，`argc > 5` 与 `argc > 6` 恒为真，两个默认值分支是死代码。而 usage（`:1192` 附近）与 README（`README.md:119`）都把 `[nvngx.dll] [nvapi64.dll]` 标为**可选**。`--image` 路径（`:1801`）用的是 `argc < 6`，两套阈值不一致。

**修复**：统一改为 `argc < 5` 并保留条件默认值。

## C4 🟡 `tools/video_filter.cpp:1838-1854` — 单图模式接受但静默丢弃 9 个参数

`run_image_mode` 里解析了 `--upscale-mode`、`--model-scale`、`--cut-threshold`、`--crf`、`--fps`、`--max-frames`、`--parallel`、`--ffmpeg-threads`、`--encoder`，但 `enhancer::Settings` 的 `output_width/output_height` 从未被设置（默认 0 = 保持原尺寸，见 `core/image_processor.h:43-45`）。因此 `--upscale-mode performance` 被接受、被忽略、无任何警告。

## C5 🟡 `core/precompile.cpp:246-278` — 停滞检测器会误杀，并把「子进程消失」计为成功

停滞判定依据是「`GetProcessTimes` 报告的 CPU 时间在若干个时间片内没有增长」。在内存压力下（而 `room_for_another` 正是按空闲物理内存来放行的），编译进程的 CPU 时间**合理地会停滞**（在换页），这会被误判为挂死并杀掉。每次误杀都会向用户报告「N of M modules failed」（`:308-312`）。

另外，`GetProcessTimes` 失败时走 `:267-275` 的 `++failures`，而**子进程已消失**的情况却计入成功。

**修复**：用 `GetExitCodeProcess` 返回 `STILL_ACTIVE` 来区分「进程仍在但无 CPU 进展」与「进程已退出」；仅在确认 kill 成功时计为失败。

## C6 🟡 `tools/analyze_vopd.py:25` — 硬编码 ROCm 7.1 绝对路径

```python
objdump = r'C:\Program Files\AMD\ROCm\7.1\bin\llvm-objdump.exe'
```
ROCm 7.2（或任何装在别处的版本）上直接报 not found 退出。**修复**：从 `HIP_PATH` 推导，或按版本号搜索。

## C7 🟡 `tools/package_release.py:15, 45, 49-50` — 硬编码绝对路径，且用于发布的脚本不可移植

```python
root_dir = r"d:\Downloads\dlss5-image-enhancer-zluda"
...
    r"D:\aiwork\DLSSNRFilter-ZLUDA",
...
    r"D:\Program Files\Microsoft Visual Studio\18\Community\VC\Redist\MSVC\14.50.35710\x64\Microsoft.VC145.CRT",
```
换机器、换路径、换 VS 小版本即失效。此外 `find_file` 在 `bin_dirs`（包含 `run/`）中**先于** MSVC 再分发目录搜索（`:108-109`），意味着若 `run/` 里存在同名的 CRT DLL，会被**优先打包进发布包**，而不是取官方的。

**修复**：路径改为相对脚本位置推导 / 从环境变量获取；CRT 搜索顺序改为官方再分发目录优先。

## C8 🟡 `core/gpu_detection.h:110-117` — RDNA 4 识别靠型号字符串，易漏判

```cpp
bool is_rdna4 = (name_lower.find(L"9070") != std::wstring::npos ||
                 name_lower.find(L"9060") != std::wstring::npos ||
                 name_lower.find(L"9080") != std::wstring::npos ||
                 name_lower.find(L"rx 9") != std::wstring::npos ||
                 name_lower.find(L"rdna 4") != std::wstring::npos ||
                 name_lower.find(L"rdna4") != std::wstring::npos ||
                 name_lower.find(L"navi 4") != std::wstring::npos ||
                 (best_gpu->device_id >= 0x7480 && best_gpu->device_id <= 0x749F));
```
`"rdna 4"` / `"navi 4"` 三个分支是**死代码**：DXGI 的 `Description` 是型号名（如 `"AMD Radeon RX 9070 XT"`），永远不含这些字样。而 `device_id` 范围 `0x7480..0x749F` 只覆盖 Navi 48，**漏掉 Navi 44 等**；靠 `"rx 9"` 前缀兜底，对 OEM 命名不规范的卡（不含 `RX` 字样）同样会漏。

**后果**：未被识别的 RDNA 4 卡不会注入 `HSA_OVERRIDE_GFX_VERSION`，于是 `hipGetDeviceCount` 返回 0、`cuInit` 失败 100 —— 正是 README FAQ Q2（`README.md:161-162`）描述的症状。

**修复**：按 PCI device ID 表判断，而不是型号字符串。

---

# D. 轻微问题

## D1 🟢 `build_smoke.bat:5` — 硬编码 VS 到 C 盘，本机直接不可用（**已实测复现**）

```bat
set VS=C:\Program Files\Microsoft Visual Studio\18\Community
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
```
本机 VS18 位于 `D:\Program Files\...`（`build.bat:15` 的首选分支正是 D 盘）。实测执行 `build_smoke.bat`：

```
The system cannot find the path specified.
'cl' is not recognized as an internal or external command,
operable program or batch file.
[exit code: 1]
```

注意 `build.bat` 写了完整的四路探测 + 兜底（`:14-26`），`build_smoke.bat` 却退化成单一路径且**没有错误检查**（`vcvars64.bat` 失败后继续执行 `cl`）。`build_qt_gui.bat:3` 把它进一步恶化为 `>nul 2>&1` 静默吞掉错误。

**修复**：把 `build.bat` 的探测逻辑抽成共享脚本，或在 `build_smoke.bat` 中补上同样的分支 + `if errorlevel 1 exit /b 1`。

## D2 🟢 `build.bat:53-55` — `robocopy /XD` 列表漏了 `video_filter_autogen`

```bat
robocopy build dist /MIR /NFL /NDL /NJH /NJS ^
    /XD CMakeFiles dlss5-image-enhancer_autogen nvngx_autogen dlssnr_gui_autogen .qt zluda ^
```
`CMakeLists.txt` 为 `video_filter` 显式设了 `AUTOMOC OFF`（`:78`），当前不会生成该目录——但这是**靠巧合而非靠列表**：一旦 `AUTOMOC` 设置变化，autogen 中间产物就会被镜像进 `dist\`（发布目录）。列表应随 target 更新。

## D3 🟢 `build_smoke.bat:10-16` — 缺少 `core` 的头文件搜索路径

```bat
   /I "%ROOT%dlss_layer" ^
```
`core/image_processor.cpp:14` 与 `core/precompile.cpp:2` 都 `#include "gpu_detection.h"`，该头文件在 `core\` 下。当前能编译通过**仅因为** MSVC 对引号形式 `#include` 会先搜索包含者自身所在的目录——属于隐式依赖，不是显式声明。CMake 的对应 target 就正确地写了 `core`（`CMakeLists.txt:74`）。

## D4 🟢 发布包 `-v2` 与 `multipass` 两个 ZIP **字节完全相同**（**已实测**）

```
multipass: 6589B1E799D5A32B5AF7301B43937C8D22814741EC420B7B18EE09E03D730787
v2       : 6589B1E799D5A32B5AF7301B43937C8D22814741EC420B7B18EE09E03D730787
identical: True
```
两个 36.7 MB 的包、两个解压目录、以及 git 提交 `d8aacf4`（"update release package name to v2"）声称是两次不同发布，实际内容一致。要么删掉重复产物，要么明确说明 v2 只是改名。

## D5 🟢 两个发布包都不含 `nvngx_dlssnr.dll`（**符合预期，非缺陷**）

已核实两个包均只有 26 个条目、无任何 `*dlssnr.dll`。这是**有意设计**（`发布包说明.txt:52-54`、`package_release.py:147-151` 还有强制校验）。此处列出仅为避免误判：用户须自行放入该 DLL，首次运行的失败很可能源于此，而非代码缺陷。

---

# E. 经核查**不成立**的候选项（避免误修）

这些是我或并行审查一度怀疑、但复核后确认**没有问题**的项，明确记录以免后续浪费精力：

| 位置 | 一度怀疑 | 复核结论 |
|---|---|---|
| `gui/half_float.h:42-66` | `half_to_float` 半精度次正规数指数公式差 1 | **正确**。用 IEEE-754 binary16 参考实现逐值对照 `0x0001/0x0002/0x0003/0x0200/0x03ff/0x0400/0x3c00`，结果与参考**完全一致** |
| `gui/half_float.h:14-40` | `float_to_half` 溢出/舍入有误 | ⚠️ **第一轮判断有误，已更正**。我最初用手写的参考实现对照，而那个参考实现**同样用的是截断**，属于循环论证。改用 `numpy.float16`（真正的 round-to-nearest-even）复核后确认**确有偏差**，详见 **H1** |
| `core/gpu_detection.h:45-62` | `EnumAdapters1` 循环有空指针解引用/死循环 | **已修复**。`:46` 每次迭代都 `adapter = nullptr`，`:48` 用 `FAILED(hr) \|\| !adapter` break。早期报告 H1 的问题当前代码中不存在 |
| `tools/analyze_vopd.py:138` | `count / grand_vopd_ops` 除零崩溃 | **不可达**。`vopd_opcodes` 为空时该循环体根本不执行；二者由同一条件填充 |
| `tools/video_filter.cpp:2522` | `--reset every=0` 导致 `%` 除零 | **已防护**。`:1250` 在解析时 `if (!options.reset_every) options.reset_every = 60;` |
| `tools/video_filter.cpp:783-788` | 默认 `shadow_protect` 使 composite 恒激活、绕过零拷贝 | **已修复**。四个系数默认均为 `1.0f`，`is_active()` 只在偏离时返回 true |
| `fix_tdr.ps1:39` | `-ExecutionPolicy` 参数被乱码污染成 `bураs` | **无问题**。按码点核验实为 `U+0042 U+0079 U+0070 U+0061 U+0073 U+0073` = `Bypass`，是终端渲染误导 |
| `README.md` 参数默认值 | 文档默认值与实现不符 | **一致**。`--crf 18`、`--flow 0`、`--reset auto`、`--cut-threshold 0.30`、`--yield-ms 1` 均与 `video_filter.cpp:1153-1177` 相符 |
| 全仓库文本文件 | 存在 UTF-8 乱码/零宽字符 | **干净**。对全部 `.ps1/.bat/.py/.cpp/.h/.txt/.md` 扫描 `U+0400-U+04FF`、`U+200B-U+200D`、`U+FEFF`，命中 0 处 |

**构建状态（实测）**：`cmake --build build_qt --config Release` 全部成功，22/22 个目标（`video_filter.exe`、`dlss5-image-enhancer.exe`、`dlssnr_gui.exe`、`nvngx.dll`）链接通过，**无编译错误或警告阻断**。`CMakeLists.txt:17` 设的 `CMAKE_CXX_STANDARD 17` 与源码相符（全仓库未使用任何 C++20 特性）。

---

# G. 关于仓库内那份未处理的问题报告

`docs/issue_blank_race_report.md` 记录的三个实测问题（全新进程 50-60% 产出全黑图；ZLUDA ComputeCache 不持久化；翻译子进程挂死且父进程无限等待）**在当前代码中仍未见对应修复**：

- 「父进程无限等待子进程」→ 本报告 **A2** 与 **C5** 仍成立；
- 「全黑图 sticky」→ 本报告 **A3** 指出中途黑帧依然以退出码 0 通过；
- 该文档 `:48-53` 列的本地 workaround（看门狗、并发上限 4）在 `core/precompile.cpp` 中均无对应实现。

建议把那份文档当作 A2/A3/C5 的**验收标准**来用。

---

---

# H. 第二轮：Qt GUI 与随包脚本

审查对象：`gui/`（`batch_window.cpp` / `main_window.cpp` / `main.cpp` / `batch_main.cpp` / `compare_view.cpp` / `half_float.h`）、`diagnose_gpu.ps1`、`fix_tdr.ps1`、`CMakeLists.txt`。
**注意**：`dlssnr_gui.exe` 是发布包中的**主交付物**（`README.md:110`、`package_release.py:62`），因此本段的 GUI 问题直接影响用户。

## H1 🟡 `gui/half_float.h:14-40` — `float_to_half` 用**截断**而非 round-to-nearest，系统性偏低 1 ULP

```cpp
if (exponent <= 0) {
    if (exponent < -10) return (uint16_t)sign;
    mantissa |= 0x800000u;
    const uint32_t shift = (uint32_t)(14 - exponent);
    return (uint16_t)(sign | (mantissa >> shift));   // 纯 >> ，无舍入
}
if (exponent >= 0x1F) return (uint16_t)(sign | 0x7C00u);
return (uint16_t)(sign | ((uint32_t)exponent << 10) | (mantissa >> 13));  // 同样无舍入
```

**实测**（转写该函数，与 `numpy.float16` 即真正的 IEEE-754 binary16 round-to-nearest-even 对照）：

```
       value     impl    numpy  verdict
     2.982e-08 0x000000 0x000001  *** MISMATCH ***
       3e-08   0x000000 0x000001  *** MISMATCH ***
       4e-08   0x000000 0x000001  *** MISMATCH ***
       5e-08   0x000000 0x000001  *** MISMATCH ***
      -4e-08   0x008000 0x008001  *** MISMATCH ***
     65520.0   0x007bff 0x007c00  *** MISMATCH ***
...
随机扫描 20 万个 float（指数 -45..20）：**mismatches = 18645 / 200000（约 9.3%）**
```

**两类错误**：
1. **无 round-to-nearest**：约 9.3% 的输入比正确值**小 1 ULP**（截断导致系统性偏负）。
2. **次正规数向上舍入丢失**：`[2.98e-8, 5.96e-8)` 区间内本应舍入到最小次正规数 `0x0001`，实际被截断成 `0x0000`。

**影响面（已核实调用点，非死代码）**：
- `tools/video_filter.cpp:708` — 构建 sRGB→linear 的 **65536 项 LUT**，视频路径每个像素都查它；
- `tools/video_filter.cpp:955-957` — 合成后的最终输出写回 half；
- `tools/video_filter.cpp:1521-1522`、`1598-1599` — **运动矢量量化**（`vx * 4.0f` 后截断），截断使光流产生偏向零的系统性亚像素偏差；
- `gui/main_window.cpp:71-74` — 单图输入转换。

**严重度说明（不夸大）**：半精度 1 ULP ≈ 满量程的 0.05%，远小于 8-bit 输出的 1/255（0.4%），因此**对 SDR 8-bit 成品的可见影响很小**。定级为「中等」是因为它是与 IEEE-754 的真实偏差、影响面广（含运动矢量路径），而不是因为它会让画面明显出错。

**附带发现（非缺陷）**：同文件 `half_to_float`（`:42-66`）经 `numpy.float16` 逐值复核（含全部次正规数）**完全 bit-exact**，无需改动。

**修复**：`(mantissa >> shift)` 改为带 round-to-nearest-even 的移位；或直接用 `_cvtss_sh`（MSVC 内建，一条指令）。

## H2 🟠 `gui/batch_window.cpp:186-188` + `:721` — `auto_output_` 一旦置 false 永不恢复，换输入会**覆盖上一次的输出文件**

```cpp
connect(output_, &QLineEdit::textChanged, this, [this] {
    if (!filling_) auto_output_ = false;      // 只要用户手改过一次输出框，就永久 false
});
```

```cpp
const QString derived = ... ;                 // 按新输入算出的正确输出路径
if (!auto_output_) return;                    // <-- 在写入 output_ 之前就 return 了
filling_ = true;
output_->setText(derived);
```

**并且它是持久化的**：`save_settings()` 存（`:643`），`load_settings()` 读（`:597`）。

**后果**：用户只要曾手动编辑过输出路径一次（很常见），此后**再选择任何新的输入视频，输出框都不会更新**。界面显示视频模式、看起来一切正常，运行时却把结果写到**上一个视频的输出路径**——直接覆盖上一次的成品。这是静默的数据丢失。

**修复**：在 `input_` 的 `textChanged` 处理里（`:183-185`）把 `auto_output_` 重新置 `true`；或仅当 `output_` 的当前值等于「上一个输入推导出的路径」时才沿用。

## H3 🟠 `diagnose_gpu.ps1:216` — override 测试用 `*SUCCESS*` 匹配，**ZLUDA 失败也报绿色 [OK]**（实测确认）

```powershell
@("12.0.1", "12.0.0", "11.0.0") | ForEach-Object {
    $output = & powershell.exe ... -IsolatedOnly
    if ($output -like "*SUCCESS*") {          # <-- 冒号后见实测
        Write-Host "    [OK] HSA_OVERRIDE_GFX_VERSION=$ver -> $output" -ForegroundColor Green
```

子进程输出格式是 `"$hipRes | ZLUDA: $zRes"`（`:104`），其中：
- `hipRes` 成功时含 `(SUCCESS)`（`:50`）；
- `zRes` 成功时是 `(CUDA_SUCCESS)`（`:76`）。

**实测 `-like` 语义**（`*SUCCESS*` 是子串匹配，**不需要**独立词）：

```
output: hipGetDeviceCount -> status=0 (SUCCESS), count=0 | ZLUDA: cuInit(0) -> status=100 (CUDA_ERROR_NO_DEVICE)
'*SUCCESS*'      matches: True    <-- [6] 用的是这个
'*CUDA_SUCCESS*' matches: False   <-- [5] 用的是这个（正确）
```

**后果**：只要 **HIP 初始化成功但一个设备都看不到**（`res==0, count=0`——正是该工具要诊断的场景），三个 override 版本**全部**打印绿色 `[OK]`，而 `cuInit` 明确失败了。同一文件 `[5]` 段（`:199`）用的是正确的 `*CUDA_SUCCESS*`，两处不一致本身就是证据。另外 `$LASTEXITCODE` 被丢弃。

**修复**：改为分别判定两半（`*CUDA_SUCCESS*`），或解析 `count=` 数值。

## H4 🟠 `diagnose_gpu.ps1:23` + `:37-87` — 声明了 `FreeLibrary` 却从不调用，导致 `[4]` 段测的可能是**已经加载的那份 DLL**

```csharp
[DllImport("kernel32.dll", SetLastError = true)]
public static extern bool FreeLibrary(IntPtr hModule);   // :23 声明后全文件无调用点

public static string TestHip(string path) {
    IntPtr h = LoadLibrary(path);                        // :38
    ...
    return ret;                                          // :61 任何路径都不 FreeLibrary
}
```

Windows 加载器按**模块基名**匹配已加载模块：脚本 `[3]` 段（`:141-145`）先加载 `System32\amdhip64_7.dll`，`[4]` 段（`:175`）再加载 `HIP_PATH\bin\amdhip64_7.dll`——**基名相同**，于是直接返回已加载的 System32 模块句柄，**根本不会去加载 HIP_PATH 那份**。`TestZluda`（`:64-87`）同理。

**后果**：`[4]` 段报告的"HIP_PATH 那份 DLL 看不到设备"实际测的是 System32 那份，该结论不成立，会把人引向错误的排查方向（这与用户 `cuInit failed: 100` 的排查直接相关）。

**修复**：每个候选 DLL 放到**独立子进程**里测（脚本已有 `-IsolatedOnly` 机制，可复用）；或至少调用 `FreeLibrary` 并在加载时用 `LoadLibraryEx` 配合 `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR`。

## H5 🟠 `gui/compare_view.cpp:121-131` — `QProcess` 在超时后**未 kill 就被析构**，ffmpeg 成为孤儿

```cpp
QProcess ffmpeg;
ffmpeg.start(QStringLiteral("ffmpeg"), {...});
if (!ffmpeg.waitForFinished(5000) || ffmpeg.exitCode() != 0) {
    QFile::remove(png);
    return {};                       // <-- ffmpeg 可能仍在运行，QProcess 随即析构
}
```

**问题**：`waitForFinished(5000)` 超时返回 `false` 后直接 `return`，既没有 `ffmpeg.kill()` 也没有 `waitForFinished`，`QProcess` 的析构函数在子进程仍存活时执行。Qt 会打印 `QProcess: Destroyed while process ... is still running`，而 ffmpeg 继续跑（在 4K 素材上提取单帧可能触发较长的解码）。

**并且没有任何 job object 兜底**：`gui/batch_main.cpp`（全部 26 行）**没有**调用 `kill_children_when_this_process_ends()`，而 `gui/main.cpp:45` 为一个同类的 GUI 调了。`dlssnr_gui.exe` 直接派生的 ffmpeg / ffprobe 因此不受任何作业对象约束。

**修复**：超时分支补 `ffmpeg.kill(); ffmpeg.waitForFinished(2000);`；并给 `batch_main.cpp` 加上与 `main.cpp:28-45` 相同的 job object。

## H6 🟡 `gui/batch_main.cpp`（全文）— `dlssnr_gui.exe` 无作业对象：**强杀/崩溃** GUI 会留下整个 `video_filter` 进程树

已核实两条事实：
- `tools/video_filter.cpp:95-104` 确实自建 job object（`KILL_ON_JOB_CLOSE`）并在 `:2192` 调用，因此**它的** ffmpeg / 分片 worker 子进程在它死时会被带走。所以「Stop 按钮留下孤儿 ffmpeg」这一说法**不成立**（见 E 段更正）。
- 但 `dlssnr_gui.exe` 自身**没有** job object。`closeEvent`（`:1319-1323`）与 `stop_run`（`:1117-1123`）都只 `process_.kill()` 直接子进程。

**后果**：正常点关闭/停止是安全的（`kill()` 让 `video_filter` 死掉，其 job 连带清理）。但若 GUI 被**任务管理器强杀、崩溃、或机器休眠导致其消失**，`video_filter.exe` 不在任何父级 job 里，会继续连同它的整个子进程树运行、占着 GPU 与输出文件。`main.cpp:28-45` 的注释正是为了这个场景写的。

**修复**：在 `batch_main.cpp` 里调用同一个 `kill_children_when_this_process_ends()`（把它从 `main.cpp` 的匿名 namespace 提出来共用）。

## H7 🟡 `gui/main_window.cpp:361-362` — 关闭窗口时 `std::_Exit(0)`，把「用户中止」报成**成功**

```cpp
if (!worker_thread_.wait(2000)) {
    std::_Exit(0);
}
```

**问题**：翻译/求值可能运行数十分钟（同文件 `:349-360` 的注释自认），此时关闭窗口会走这一支，进程以**退出码 0**结束——任何检查 `%ERRORLEVEL%` 的脚本（`build.bat`、`build_qt_gui.bat`、CI）都会把一次被中止的运行当成干净成功。`_Exit` 还会跳过静态析构与 `atexit`，未 flush 的 stderr 可能丢失。

**缓解**：`dlss5-image-enhancer.exe` **不在**发布包内（`package_release.py:61-70` 的 `required_binaries` 只有 `dlssnr_gui.exe` 与 `video_filter.exe`），所以影响的是开发者自测路径而非用户。

**修复**：改为 `std::_Exit(2)`；并优先 `worker_thread_.terminate()` + 有界等待，再落到硬退出。

## H8 🟡 `gui/batch_window.cpp:1153-1156` — `probe_total` 的 `value != rate` 启发式在「时长恰等于帧率」时失效，进度条恒定 0%

```cpp
} else {
    bool ok = false;
    const double value = text.toDouble(&ok);
    if (ok && value > 0 && value != rate) duration = value;
}
```
```cpp
if (rate > 0 && duration > 0) frames_total_ = (long)(rate * duration);
```

**具体反例**：一个**30 秒、30 fps** 的片段——`r_frame_rate` 为 `30/1`（先被解析，`rate = 30`），随后 `format=duration` 为 `30`，`value != rate` 为 **false**，`duration` 保持 0。于是 `:1159` 的守卫不成立，`frames_total_` 停在 `-1`，进度条永不前进，状态栏只显示无总数的 `处理中… 已用 N 秒 · 帧 M`（`:1277-1280`）——**正是 `:1139-1140` 注释声称已修好的症状**。

**修复**：改用 `-of csv=p=0` 按**位置**解析两个字段，而不是按数值猜。

## H9 🟡 `gui/batch_window.cpp:186-188` 之外的设置往返：`image` 存一个、恢复两个

`save_settings()` 把 `image` 存为 `image_mode_->isChecked()`（`:649`），`load_settings()` 却同时写入两个单选钮（`:595-596`）。`load_settings` 之后**不重新调用** `auto_fill_output`，因此上一次会话若停在 image 模式、输出名是 `x_dlss.mp4`，重启后会恢复成「image 模式 + .mp4 输出」的组合，而 `start_run` 会原样把它交给 `video_filter.exe --image ... x_dlss.mp4`（`:817`），随后在 `dump_png` 处失败。

**修复**：单选钮设置完成后，在 `filling_` 守卫内补一次 `auto_fill_output(false)`。

## H10 🟡 `gui/batch_window.cpp:1226` — `on_finished` 丢弃 `ExitStatus`，用户主动停止与真实崩溃无法区分

```cpp
void BatchWindow::on_finished(int code, QProcess::ExitStatus) {   // 形参未命名，被丢弃
    ...
    if (code != 0) status_->setText(tr("处理失败（exit=%1），见日志").arg(code));
```
`stop_run()` 的 `kill()` 同样以非零码进入这一支，用户点了「停止」却看到橙色的「处理失败」告警。**修复**：命名该形参，按 `status == QProcess::CrashExit` 分支，并在主动停止时抑制失败配色。

## H11 🟡 `gui/batch_window.cpp:1199-1208` — 解析子进程 stderr 的整数运算可能溢出

```cpp
const int done = translating.captured(1).toInt();
const int total = translating.captured(2).toInt();
if (total > 0) progress_->setValue(qMin(99, done * 99 / total));
```
`total > 0` 守卫了除零（这点没问题），但 `done * 99` 是 `int` 运算，而 `done` 来自对**子进程 stderr** 的未校验捕获（格式由 `tools/video_filter.cpp` 输出）。`done = 999999999` 时 `done * 99` 溢出（有符号溢出即 UB，实践中变负 → 进度条回跳）。**修复**：`(int)((long long)done * 99 / total)`，并把 `done` 钳到 `[0, total]`。

## H12 🟢 `gui/main_window.cpp:168-172` — `--selftest` 的最小合法形式被守卫拒绝（**索引本身是对的**）

```cpp
int run_self_test(const QStringList &arguments) {
    if (arguments.size() < 6) {
        fprintf(stderr, "usage: --selftest <in> <out.png> <network> <driver> [runtime] [nvapi]\n");
```
`main.cpp:74` 传的是 `arguments.mid(1)`，`--selftest` 落在索引 0。因此：
- `arguments[1]` = 输入图 ✔、`arguments[3]` = network ✔、`arguments[4]` = driver ✔、`arguments[5]` = runtime ✔、`arguments[6]` = nvapi ✔ —— **全部正确**。

真正的问题只是守卫：按 usage 写的 `--selftest <in> <out.png> <network> <driver>`（`mid(1)` 后 size == 5）会因为 `< 6` 直接打印 usage 并返回 2，即**文档标为可选的两个参数实际必须提供**。**修复**：`if (arguments.size() < 5)`。

## H13 🟢 `CMakeLists.txt` — `tests/processor_smoke.cpp` 未纳入任何 target

全文件无引用该源的 `add_executable`。它只由仓库根的 `build_smoke.bat` 旁路编译，因此 `cmake -B build` 全新配置**不会**产生 smoke test（而该脚本本机已确认不可用，见 D1）。`tools/hardware_budget.h` 同样不属于任何 target（靠引号包含相对解析）。**修复**：加一个 `add_executable(processor_smoke ...)` 或明确在文档中说明它只走旁路脚本。

---

# H-补. 第二轮**不成立**的候选项（含对第一轮我自己的更正）

| 位置 | 一度怀疑 | 复核结论 |
|---|---|---|
| `gui/main_window.cpp:174` / `main.cpp:74` | `--selftest` 参数错位：把 `out.png` 当输入、把 driver 当 network | **不成立**。`mid(1)` 把 `--selftest` 放到索引 0，各索引与 usage 一一对应（见 H12）。真实问题只是守卫过严 |
| `diagnose_gpu.ps1:99` / `:189` / `:211` | `Get-Location` 返回 `PathInfo` 对象，`Join-Path` 会拼出非法路径 | **不成立**。实测 `Join-Path (Get-Location) "run\nvcuda.dll"` → `D:\...\run\nvcuda.dll`，`PathInfo` 被正确字符串化 |
| `tools/video_filter.cpp` job object | `dlssnr_gui` 无 job → Stop 后留下孤儿 ffmpeg | **部分不成立**。`video_filter.cpp:95-104` 自建 job 且 `:2192` 调用，其 ffmpeg 子进程会被连带清理。**残余**问题见 H6（GUI 被强杀/崩溃时 `video_filter` 本身成孤儿） |
| `tools/analyze_vopd.py:138` | 除零崩溃 | **不可达**（空计数器时循环体不执行） |
| `fix_tdr.ps1:39` | `-ExecutionPolicy` 被西里尔同形字污染 | **不成立**。按码点核验为纯 ASCII `Bypass` |
| `tests/processor_smoke.cpp:43-45` | `CopyPixels` 的缓冲区大小传成了 2 倍 | **不成立**。`pixels` 是 `uint16_t` 向量，`size() * 2` 正是 64bpp 的**字节**数 |
| `gui/compare_view.cpp:29-37` | `image_rect` 用零高 pixmap 除零 | **不成立**。`isNull()` 已守卫；且经 `CompareDialog` 构造时两张图都为 null 会提前返回 |
| `gui/batch_window.cpp` 组合框索引 | `setCurrentIndex(-1)` 导致越界读 `QStringList` | **不成立**。所有使用点都有 `> 0 && < N` 或 `qMax(0, ...)` 守卫（`:827-828, 870, 878, 882, 891-894`）。残余问题仅是非法 ini 值会被原样写回而不被规范化 |
| `gui/main_window.cpp:596` vs `gui/batch_window.cpp:94` | 两套设置存储（注册表 vs ini）属缺陷 | **风格不一致，非缺陷**。功能各自正常，仅影响便携性 |
| `CMakeLists.txt:17` `CMAKE_CXX_STANDARD 17` + `std::_Exit` | 需要 C++20 才能编译 | **不成立**。`std::_Exit` 是 C++11；全仓库未使用任何 C++20 特性，17 足够 |

---

# 结论一：修复优先级建议

**第一批（会造成挂死或静默错误结果，建议立即处理）**

1. **A1** — `State::wait()` 的无限等待与未检查 HRESULT（照搬同仓库 `flush_and_wait()` 即可）
2. **A2** — 并行编排器补超时与兜底终止
3. **A3** — 中途黑帧必须以非零码报告
4. **A5** — 修正「杀子进程 → 取消 I/O → join → 关句柄」的顺序
5. **A4** — `--max-frames` 按分片重定基

**第二批（资源与状态正确性）**

6. **B5** — `start()` 两条错误路径补 `stop()`
7. **B8** — `reset_all()` 不得清空已被引用的参数块
8. **B4** — `g.backbuffer` 用自身描述符校验
9. **B7** — `init()` 比较 device / queue
10. **B6** — 还原 `SetDllDirectoryW`

**第三批（可用性）**

11. **B1 / B2** — 让超时机制真正生效；`wait_exit` 改三态
12. **B3** — dump 文件名与帧计数修正
13. **C1 / C2 / C3** — 参数解析的死代码与范围校验
14. **D1** — 修好 `build_smoke.bat`（本机已确定不可用）
15. **D4** — 清理重复的发布产物

**第四批（GUI 与随包脚本，第二轮新增）**

16. **H2** — `auto_output_` 永不恢复 → 换输入会**覆盖上一次成品**（本报告中唯一有静默数据丢失风险的一条，建议优先于其它 GUI 项）
17. **H3** — `diagnose_gpu.ps1` 的 override 测试对失败报绿色 `[OK]`（诊断工具给出错误结论）
18. **H4** — `diagnose_gpu.ps1` 从不 `FreeLibrary` → `[4]` 段测的可能是同一份 DLL
19. **H5 / H6** — `compare_view` 超时后不 kill；`batch_main.cpp` 补 job object
20. **H1** — `float_to_half` 改为 round-to-nearest（`_cvtss_sh` 一行即可）
21. **H8 / H9 / H10 / H11 / H12 / H13** — 进度解析、设置往返、退出状态区分、整数溢出、自测守卫、构建图

---

# 结论二：一句话总览

这份代码库的**工程完成度不低**：自建 job object、D3D12 栅栏超时、sRGB↔linear 双程转换、查找表、多进程分片都实现了，本轮实测 **22/22 个构建目标全部成功**。问题集中在**「错误路径与边界」**这一类：

- **最该先修的是 A1 / A2** —— 两处「无限等待」，其中 A1 会把渲染/GUI 线程锁死且没有任何取消路径；
- **A3 + H2 是两条静默数据损坏路径** —— 一个产出黑帧视频段却报成功，一个把结果写到上一次的输出路径上覆盖掉；
- **A5、H4、H3 属于「机制写对了但用错了」** —— 关闭句柄与取消 I/O 的顺序颠倒、遗漏 `FreeLibrary` 导致诊断结论失真、一处通配符用错让工具对失败报成功；
- `docs/issue_blank_race_report.md` 里那份实测报告（含用户已跑通的 workaround）至今**没有对应的修复落地**，建议直接当作 A2 / A3 / C5 的验收清单。


