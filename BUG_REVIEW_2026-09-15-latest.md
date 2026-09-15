# 代码审查报告 — 最新版本（HEAD = 9d288f7，v6 之后 7 个修复 commit）

审查日期：2026-09-15
审查范围：`f758aad..HEAD`（即上次 v6 评审后的全部修复改动），重点核对新引入代码是否真正生效、是否引入新缺陷。
审查方式：静态逐行审查 + 与既有硬约束（音频逻辑一致性、Map() 校验、逐帧空检）交叉比对。

> 说明：v6 报告里已修复的 7 项（统一音频逻辑、Map() 校验、阈值 0x191E→0x1D1F、逐帧空检、flush_and_wait、预编译 60 分钟预算、/utf-8）本次复核**确认已正确落地**，不再重复列出。本报告只列**新发现的问题**。
>
> **二次复核（同日晚些时候）**：H1/H2/L1/L2 已逐条对照源码再次验证，结论均成立；验证过程中新增发现 **H3**（见下）。
>
> **修复状态（同日）**：**H1/H2/H3/L1/L2 已全部修复**并通过编译冒烟（`build_smoke.bat` + `video_filter.cpp` 单独编译均零错误零新警告）。H1 另做了 6 组解析断言（含 VFR `avg≠nominal`、旧 ffprobe 3 字段、`avg=0/0` 回退、CRLF、垃圾输入拒绝），全部通过。详见各节"修复"小节与文末"已落实的修复"。

---

## H1 — `probe_video` 的 avg_frame_rate 解析是死代码，VFR 漂移修复实际未生效

严重程度：**高**（本次 commit 声称修复的目标完全没达成，属"静默无效修复"）

位置：[probe_video](file:///d:/Downloads/dlss5-image-enhancer-zluda/tools/video_filter.cpp#L540-L556)

```cpp
if (sscanf(text.c_str(), "%u,%u,%63s,%63s\n%lf", &w, &h, rate, rate_avg, &dur) >= 4 && w && h) {
    ...
    if (rate_avg[0]) rate_of(rate_avg, params.fps);   // 永远不执行
    if (params.fps <= 0) rate_of(rate, params.fps);
} else if (sscanf(text.c_str(), "%u,%u,%63s,%63s", &w, &h, rate, rate_avg) == 4 && w && h) {
    ...
}
```

问题：`%63s` 转换符按 C 标准只匹配**非空白**序列，**逗号不是空白**。ffprobe 的 stream 行形如
`1920,1080,30000/1001,30000/1001`，因此第一个 `%63s` 会把 `30000/1001,30000/1001` **整段**吞进 `rate`，
随后格式串里的字面量 `,` 在输入处遇到的是换行符，匹配失败，`sscanf` 提前停止，返回 **3**。

后果链：
- 第 1 分支 `>= 4` 为假 → 跳过；
- 第 2 分支 `== 4` 同样为假 → 跳过；
- 落入第 3 分支（旧的两字段格式）`>= 3` 为真，`rate` 拿到贪婪捕获的 `"30000/1001,30000/1001"`，
  再经 `rate_of` 的 `sscanf(value,"%u/%u")` 恰好解析出 `30000/1001`。

所以：`rate_avg` **永远是空串**，`if (rate_avg[0])` 这条"优先用 avg_frame_rate"的分支从不执行。
最终 `params.fps` 始终取自 `r_frame_rate`（标称帧率），而 commit 注释明确说标称帧率正是 VFR 源
产生漂移的根因。**该修复对本应受益的 VFR 场景零效果，fps 行为与修复前完全一致。**

修复建议：`%63s` 无法按逗号分词。改用显式手工切分（按 `,` 拆 stream 行），或让 ffprobe 用
`-of default=noprint_wrappers=1:nokey=1` 让每个字段独立成行后再逐行读，避免依赖 `%s` 在逗号处截断。

---

## H2 — `GpuFlow::compute` 在 `sync()` 失败后缺少 pipeline 退役保护，下一帧会在 GPU 仍占用时 `Reset()` 分配器

严重程度：**中**（触发需 5s GPU 停顿，但一旦触发即进入 D3D12 未定义行为，且每帧重复）

位置：[GpuFlow::compute](file:///d:/Downloads/dlss5-image-enhancer-zluda/tools/video_filter.cpp#L1847-L1872)、调用点 [flow 主循环](file:///d:/Downloads/dlss5-image-enhancer-zluda/tools/video_filter.cpp#L3048-L3062)

新加的 `sync()` 把"超时即当成功"改成了 `if(!sync()) return false;`，这是正确的方向。但：

- `compute()` 返回 false 后，调用方仅把**本帧**回退到 CPU flow；`gpu_flow_ready` 仍为 true，
  下一帧仍会进入 `gpuflow.compute()`；
- 而 `sync()` 返回 false 意味着 GPU 在 5s 内没排空，此时第一条命令列表引用的
  `out`/`readback`/`prev`/`cur` 资源**可能仍在被 GPU 执行**；
- 下一帧进入 `compute()` 第一件事就是 `allocator->Reset(); cmd->Reset(...)`（[L1848](file:///d:/Downloads/dlss5-image-enhancer-zluda/tools/video_filter.cpp#L1848)、[L1829 的 Map 路径](file:///d:/Downloads/dlss5-image-enhancer-zluda/tools/video_filter.cpp#L1829)），
  对"其命令列表仍在执行中"的 allocator 调用 `Reset()` 是 D3D12 明确禁止的未定义行为。

对比：`image_processor.cpp` 里同类问题已经用 `pipeline_dead` 标志 + `begin_command_list()` 做了
"失败即退役、后续帧快速失败"的处理（[begin_command_list](file:///d:/Downloads/dlss5-image-enhancer-zluda/core/image_processor.cpp)）。
`GpuFlow` 是独立实现的第二套 D3D12 管线，**没有移植同样的退役逻辑**，属于同一类缺陷的遗漏点。

修复建议：给 `GpuFlow` 增加与 `Processor::State` 一致的 `pipeline_dead` 语义——`sync()` 返回 false
时置位，之后 `compute()` 开头直接 `return false`，不再触碰 allocator/cmd，让整段流程稳定停在 CPU flow 回退上。

---

## H3 —（二次复核新增）`process_raw_rgb48` 的 CPU-motion 上传分支绕过 `pipeline_dead` 门，直接 `allocator->Reset()` 且不检查返回值

严重程度：**中**（与 H2 同类：仅在管线已判死/超时后触发，但触发即 UB，且是视频路径的实际回退路径）

位置：[process_raw_rgb48 CPU-motion 分支](file:///d:/Downloads/dlss5-image-enhancer-zluda/core/image_processor.cpp#L1184-L1185)，对照正确写法 [begin_command_list](file:///d:/Downloads/dlss5-image-enhancer-zluda/core/image_processor.cpp#L250-L258)

```cpp
s->allocator->Reset();                       // L1184：无 FAILED 检查，无 pipeline_dead 门
s->cmd->Reset(s->allocator, nullptr);        // L1185
```

`image_processor.cpp` 里共有 6 处命令录制入口，其中 5 处（process() 两处、process_raw_rgb48 的 GPU-motion 分支 L1135、主录制 L1294 等）都走 `s->begin_command_list()`——它先查 `pipeline_dead`，`Reset` 失败时置位退役并返回 false。**唯独 CPU-motion 分支（L1184）直接裸调 `allocator->Reset()`**：

- 若上一帧 `wait()` 超时已置 `pipeline_dead`（[image_processor.cpp L273-L277](file:///d:/Downloads/dlss5-image-enhancer-zluda/core/image_processor.cpp#L273)），本帧带着 CPU 运动向量进来时，会在死管线上 Reset/录制，正是 `begin_command_list()` 注释里描述的"unknown state 上录制 = UB"；
- `Reset()` 的 HRESULT 被丢弃，即使运行时拒绝 Reset 也继续 `cmd->Reset(...)` + 录制。

该行由 commit `7e1b53e5`（2026-09-10）引入，早于本次审查的 7 个修复 commit，但 HEAD 上仍然存在。**注意它是视频路径的实际分支**：GPU flow 不可用时 CPU flow 生成的 motion 走的正是这里，所以"GPU 故障 → 回退 CPU motion → 撞死管线"是一条现实链路。

修复建议：把 L1184-L1185 换成 `if (!s->begin_command_list()) { error=...; out.pixels.clear(); return false; }`，与 L1135 的 GPU-motion 分支写法完全对齐。

---

## L1 — `finish_evaluation` 改为每帧采样，引入每帧堆分配与最多 24 次同步回读

严重程度：**低**（正确性无碍，热路径开销）

位置：[finish_evaluation](file:///d:/Downloads/dlss5-image-enhancer-zluda/dlss_layer/dlss_cuda.cpp#L1279-L1301)、[sample_nonzero_bytes](file:///d:/Downloads/dlss5-image-enhancer-zluda/dlss_layer/dlss_cuda.cpp#L1219-L1252)

逐帧空检是 v6 评审明确要求的（正确），但实现细节有两个可优化点：
1. `sample_nonzero_bytes` 每次调用 `std::vector<unsigned char> host(row_bytes, 0)` 做堆分配；
   现在每帧至少调用 1 次、输出判空时调用 2 次，即每帧 1–2 次堆分配。
2. 每帧最多 24 个 `cuMemcpy2D`（array→host，同步；输出判空时再补最多 24 个输入采样，最坏 48 个/帧）。
   在 ZLUDA 上这类回读不便宜，叠加在已经逐帧同步的评测路径上，是稳定的吞吐税。

修复建议（非必须）：把 `host` 缓冲提到 `g` 里按 `row_bytes` 缓存复用；采样行数按分辨率/帧预算
下调（如 8 行）。功能不变，去掉每帧分配。

---

## L2 — `process_raw_rgb48` 的 motion `Map()` 失败被静默吞掉，与 `process()` 的硬失败不一致

严重程度：**低**（行为不一致；设备移除时 raw 路径不报错，只是丢运动引导）

位置：[process_raw_rgb48 CPU-motion 分支](file:///d:/Downloads/dlss5-image-enhancer-zluda/core/image_processor.cpp#L1158-L1195)

`process()` 里 motion `Map()` 失败会 `error=...; return false`（[L792-L806](file:///d:/Downloads/dlss5-image-enhancer-zluda/core/image_processor.cpp#L792)），
而视频实际走的 `process_raw_rgb48()` 里，CPU-motion 的 `Map()` 用
`if (SUCCEEDED(...) && mapped) { ... motion_uploaded = true; }`——失败时既不报错也不置位，
仅静默地不绑定运动纹理。虽然 `motion_uploaded` 门控保证了"不会绑定陈旧向量"（这点是对的），
但设备移除这类硬故障在 raw 路径上被降级为"无声丢引导"，与 process() 的语义不一致，排障时更难定位。

修复建议：raw 路径的 motion `Map()` 失败也应像 process() 一样区分"设备移除"与"尺寸不符"，
至少对 `FAILED(hr)` 记录/返回错误，保持两条路径对 Map 失败的处理一致（呼应项目硬约束：
所有 Map() 调用必须 `FAILED(hr) || !mapped` 校验）。

---

## 已复核、确认无问题的改动（记录以免重复排查）

- **precompile.cpp 新 deadline 逻辑**（[L504-L576](file:///d:/Downloads/dlss5-image-enhancer-zluda/core/precompile.cpp#L504)）：erase 后 `--j` 处理正确；`killed`/`working` 命名循环与随后 kill 循环对 `stalled[j] > 0` 的判据一致；`deadline_firings` 在有模块完成时复位（[L590](file:///d:/Downloads/dlss5-image-enhancer-zluda/core/precompile.cpp#L590)），"3 次预算"语义符合注释。未见新缺陷。
- **两条音频路径一致性**（项目硬约束）：`start_encoder`（[L709-L714](file:///d:/Downloads/dlss5-image-enhancer-zluda/tools/video_filter.cpp#L709)）与并行 concat（[L2464-L2472](file:///d:/Downloads/dlss5-image-enhancer-zluda/tools/video_filter.cpp#L2464)）的 copy 白名单 `{aac,mp3,ac3,eac3}`、`is_mkv`、`ac.empty()` 三分支**完全一致**，v6 的分歧已消除。
- **create_feature 新 reuse 门**（[L1067-L1091](file:///d:/Downloads/dlss5-image-enhancer-zluda/dlss_layer/dlss_cuda.cpp#L1067)）：新增比较全部 `neural.*` create-time 参数，正确堵住了"改了 preset/style 却复用旧网络"的静默 bug。
- **`--flow-only` 改为解析字段**（[L1360](file:///d:/Downloads/dlss5-image-enhancer-zluda/tools/video_filter.cpp#L1360)）：修掉了从固定下标扫 argv 漏判 flag 的问题，正确。
- **upscale4 的 `qw<2||qh<2` 前置返回**（[L1599](file:///d:/Downloads/dlss5-image-enhancer-zluda/tools/video_filter.cpp#L1599)）：堵住了无符号 `qh-2` 回绕越界读，正确。
- **frame_blit `allocate_view_slots` 越界返回 0xFFFFFFFF + 各调用点检查**（[L120-L124](file:///d:/Downloads/dlss5-image-enhancer-zluda/dlss_layer/frame_blit.cpp#L120)）与 init 失败路径 `release_partial()`：泄漏修复正确。
- **processor_smoke/framebench 改 `wmain`**：修掉 CP936 机器上 GBK 路径被当 UTF-8 解码的问题，正确。
- **batch_window settings 迁移到 AppConfigLocation + 一次性 carry-over**、**probe 结果缓存 `probe_input_once`**、**拖放非本地 URL 拒绝**、**read_error 支持裸 `\r` 分行**：逻辑自洽，未见新缺陷。

---

## 已落实的修复（2026-09-15 同日）

- **H1 已修**：`probe_video` 弃用 `sscanf %s` 链，改为手工按逗号切分 stream 行（`tools/video_filter.cpp`，新解析块约 L540-L588）。`rate_avg` 现在能正确取出；旧 ffprobe 3 字段、`avg=0/0` 回退、CRLF、垃圾输入各行为与原意图一致。6 组断言全过，其中 VFR 样例（`r=30000/1001, avg=2997/125`）确认取 avg=23.976。
- **H2 已修**：`GpuFlow` 增加 `pipeline_dead` 成员与 `begin_command_list(initial)` 守卫；`sync()` 的三种失败（Signal/SetEventOnCompletion/等待超时）均置位退役；`compute()` 开头快速失败；两处 `allocator->Reset()` 全部换为 `begin_command_list()`；`init()` 成功路径清位（允许重建后复用）。
- **H3 已修**：`process_raw_rgb48` CPU-motion 分支（`core/image_processor.cpp` 约 L1197-L1204）改为 `s->begin_command_list()`，错误消息与 GPU-motion 分支（L1135）一致。
- **L1 已修**：`sample_nonzero_bytes` 的 staging 行缓冲提升为 `State::sample_host`（`dlss_layer/dlss_cuda.cpp`），按 `row_bytes` 增长复用，去掉每帧 1–2 次堆分配。24 行采样密度保持不变（刻意不降低，保住"条带状内容仍可见"的检测语义）。
- **L2 已修**：`process_raw_rgb48` CPU-motion 的 `Map()` 失败改为与 `process()` 相同的硬失败（含 `GetDeviceRemovedReason` 诊断写入 `error`），不再静默丢运动引导（`core/image_processor.cpp` 约 L1158-L1170）。

## 建议的下一步

1. ~~修 H1~~ ~~修 H2~~ ~~修 H3~~ ~~L1/L2 排期~~ —— 均已完成（见上节）。
2. 分发前按 v6 计划补测：VFR 源（端到端确认编码器 `-r` 取 avg 帧率、无漂移）+ dts/truehd/vorbis 并行音频转码。
3. H1 解析断言已固化：解析逻辑抽到 `tools/probe_parse.h`（`video_filter.cpp` 与测试共用一份实现），断言在 `tests/probe_parse.cpp`（7 组，含垃圾/空输入拒绝），由 `build_smoke.bat` 构建并自动运行。
4. 注意：本机 MSVC 实际可用（此前"无编译器"的判断有误）；`video_filter.cpp` 全量编译通过，仅存量 `C4267` 警告位于未触碰的 `CpuFlow` 层级初始化代码，与本次修复无关。
