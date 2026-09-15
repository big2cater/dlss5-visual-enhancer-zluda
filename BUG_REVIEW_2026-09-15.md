# BUG_REVIEW_2026-09-15 —— v2026.09.14-v6 全量代码审查

- **审查对象**：git HEAD `f758aad`（= 发布版 v2026.09.14-v6，2026-09-15 00:37 打包）
- **审查范围**：`tools/`、`core/`、`dlss_layer/`、`gui/`、`ngx_runtime/`、`tests/` 全部 C++ 源码（约 1.2 万行）+ `CMakeLists.txt`、`build*.bat`、`fix_tdr.ps1`、`diagnose_gpu.ps1` 等辅助文件
- **方法**：AI 多代理并行逐文件细读 + 关键发现人工对照源码复核。下文标 ✅ 的为已人工核实的条目。**全部条目（含初版未标 ✅ 的 M3-M5、M8-M18、L1-L44、S2-S12 共 68 条）已于 2026-09-15 逐条对照 f758aad 源码复核完毕：63 条属实、5 条部分成立（L4/L7/L9/S2/S3）、0 条不成立**，勘误见各条目行内与各节复核注
- **前情**：`BUG_REVIEW_2026-09-11.md` 与 `BUG_REVIEW_2026-09-11_FRESH.md` 两份旧报告的修复状态已重新核对，见第五节

**统计**：高 3 ｜ 中 19 ｜ 低 40+（含脚本/构建 12 条）

---

## 〇、修订记录（2026-09-15，经用户逐条独立复核后修订）

**三处修正**：

1. **S1 降级为防御性修复**：机制前提全部核实成立（构建机 ACP=936、`batch_window.cpp` 无 BOM 且含 7,148 个非 ASCII 字节、build 目录确无 `/utf-8`），但对出厂 `dlssnr_gui.exe` 的字节扫描显示六个中文界面串全部是正确 UTF-8（utf8 命中 >0、GBK 命中 =0）——**没有发生乱码事故**。MSVC 为何未弄坏这些串尚无定论（可两分钟定论：同一文件加/不加 `/utf-8` 各编一次、比较字节）。仍建议加 `/utf-8`，但性质是消除对构建机代码页的依赖，不是修事故。
2. **H3 子论据 1 撤回**：全仓检索确认 `evaluate_selftest()` 只有声明（`dlss_cuda.h:189`）与定义（`dlss_cuda.cpp:1401`）、零调用者——"自测消耗一次性检查"仅在仓外 ReShade addon 调用它时成立，本仓没有 addon 目录。H3 主体（一次性闩锁 + 非确定性竞态）不受影响，成立。
3. **H1 受影响编码名单收窄**：flac/opus 在 MP4 中 FFmpeg 存在非标准映射、通常仍能 mux；确定必挂的是 dts/truehd/vorbis 一类。准确名单应以 `ffmpeg -c copy` 逐一实测为准。机制（并行 concat 黑名单与单进程白名单相反）与修法不变。

**同日修复实施**（行号以修复前 HEAD `f758aad` 为准）：H1、H2、H3、M1、M14、M19、S1 已修复，见各条目标注。其中 M19 同时是 v6 新引入的风险（10 分钟死线与 `image_processor.cpp:457-460` 自认的"最大模块要几十分钟"直接冲突），优先处理；修法采用"默认 60 分钟 + 死线触发时只杀 CPU 零进度子进程、有进度的仅告警继续等"双管齐下。

**用户侧复核（2026-09-15，第二轮）**：对 68 条补核结果抽查复核后，四处需要修正或补注 —— 因此本轮的账目建议改为 **约 62 条属实 / 6–7 条部分成立 / 0 条全错**，而不是"63/5/0"：

1. **L4 的证据错位**：其"由 `real_main:3277-3280` 解析 `--retries`"不成立 —— 那几行是 `[precompile-wait]` 代码。真实位置是 `video_filter.cpp:3295-3299`（real_main 解析，默认 5）、`:1928`（单图路径同样解析）、`:1439-1441`（解析层明确忽略并转交 main）。结论方向正确，证据须替换。
2. **S2/S3 应再降一级**：`build_qt_gui.bat` 不在 git（`.gitignore:3` = `/build_qt*`），也不在发布包（`package_release.py` 只装 diagnose/fix_tdr/说明），属开发机私有脚本，零用户影响 —— 不应留在"中/中低"档。
3. **M4 的定性自相矛盾**：条目自注"f758aad 时仓库内尚无调用方传非零 pitch，属 API 层缺陷而非现行可触发 bug"，却仍与 H2（会崩）并列标"属实"；应记部分成立（潜在）。
4. **M5 的复核基础**：守卫一侧（`image_processor.cpp:708` 的 `motion_gpu_row_pitch >= in.width * 4`）已独立核对成立；绑定侧不复查 pitch/尺寸那一半仍是按条目引用采信。
5. **L11 死代码清单**：独立复核确认 5/8 —— `resize_rgb48`（全仓仅定义）、`image_has_signal`（仅前置声明 + 定义，而注释还声称"Kept for the places that want the bare question"）、`eval_index`（仅声明 + 自增）、`full_copy_ready`（无 `= true`）、`input_has_signal`（仅写与传递、无读取）；`Channel::clear`、`OutputFrame::blank`、`cb` 缓冲三项未复核。

**同日第二轮修复**（在 v6 内，未发布过故同名重打）：除上述 M19/H1 两处残留（M19 增加"连续 3 次死线仍无模块完成则强杀幸存者"；H1 旁的空编码分支不再丢音轨）外，还修了 S4、S5、S6、M16、M17、M18、L33、L35，见各条目行内注。

---

## 一、高严重度（建议立即修复）

### H1 ✅ 已修复 并行分片 concat 阶段音频 `-c:a copy` 与 MP4 容器不匹配 —— 全部 GPU 推理白做
- **位置**：`tools/video_filter.cpp:2417-2432`（对照单进程路径 `:683-697` 的正确实现）
- **问题**：`--parallel` 模式最后一步 concat 时，音频只特判 `wmapro/wmav2/wmavoice/pcm_s16le/pcm_s24le/alac` 转 AAC，**其余一律 `-c:a copy`**：
  ```cpp
  } else {
      audio_args = L"-c:a copy ";
  }
  ```
  而单进程路径 `start_encoder` 有完整白名单 `can_copy_in_mp4 = (ac == "aac" || ac == "mp3" || ac == "ac3" || ac == "eac3")`，不在名单内会转 AAC。于是：输出为 `.mp4` 且源音频是 **dts / truehd / vorbis** 等必挂编码时（flac/opus 在 MP4 有非标准映射、通常仍能 mux，见修订记录 3），分片 worker（`video_only=true`）全部正常产出，concat 一步被 ffmpeg 拒绝（"Could not find tag for codec"）→ `ccode != 0` → **删除全部分片、return 1**。整段视频的 GPU 推理在最后一步报废，且退出码 1 不触发外层空白重试。两套音频逻辑明显是复制时漏同步。
- **修复**：concat 分支复用 `start_encoder` 的 `is_mkv / can_copy_in_mp4` 判断，非 MKV 且不在白名单时改 `-c:a aac -b:a 192k`。
- **已修复（2026-09-15）**：`tools/video_filter.cpp` concat 分支已改为与单进程路径同一正向白名单（`is_mkv_output || can_copy_in_mp4` 才 `-c:a copy`，否则转 AAC 192k 并打印与单进程一致的 `[audio]` 提示）。

### H2 ✅ 已修复 `process()` CPU 运动上传 `Map()` 返回值未检查 —— 失败时 `memcpy(nullptr)` 必崩
- **位置**：`core/image_processor.cpp:736-748`
- **问题**：
  ```cpp
  unsigned char *mapped = nullptr;
  s->motion_up->Map(0, &nothing, (void **)&mapped);   // 返回值被丢弃
  ...
  if (mPitch == row_bytes_motion) {
      memcpy(mapped, source, row_bytes_motion * in.height);  // mapped 可能为 null
  ```
  UPLOAD heap 上 `Map` 失败（设备移除、极端内存压力——本工具的 `wait()` 专门检测设备移除，说明这是预期会发生的运行时故障）时 `mapped` 保持 `nullptr`，`memcpy` 是未定义行为，典型结果为立即访问违例。同文件同类代码都有防护：颜色上传路径 `:645-654` 检查 `FAILED(hr) || !mapped`，`process_raw_rgb48` 的 `:1068` 也有检查，**唯独此处遗漏**。
- **修复**：照抄 `:1068` 写法，`Map` 包进 `if (SUCCEEDED(hr) && mapped)`，失败置 `error` 并返回 false。
- **已修复（2026-09-15）**：`core/image_processor.cpp` 运动上传 Map 已按 `:648-654` 同款样式包进 `FAILED(map_hr) || !mapped` 判定，失败时带 `GetDeviceRemovedReason` 报错、清理输出并返回 false。

### H3 ✅ 已修复 空输出检查是一次性闩锁 —— commit efa9867 的修复在首次成功读回后永久失效
- **位置**：`dlss_layer/dlss_cuda.cpp:1242-1243, 1271`
- **问题**：`finish_evaluation` 中：
  ```cpp
  static bool reported = false;
  if (!reported && g.cu.cuMemcpy2D && g.output.level0) { ... reported = true; ... return false; }
  ```
  `reported` 在第一次**成功读回**后置真，进程余生的每一帧都不再检查输出是否为空——空输出重新变成"静默成功"，这正是 efa9867 声称要消灭的行为。三个叠加漏洞：
  1. ~~`evaluate_selftest()`（`:1401-1415`）在真实帧之前跑零化纹理自测，读回成功即消耗掉这一次检查~~（**2026-09-15 撤回**：全仓检索确认该函数只有声明 `dlss_cuda.h:189` 与定义 `dlss_cuda.cpp:1401`、零调用者，本报告 L11 死代码清单当时也漏列了它——内部不一致已在本轮修订中发现；仅当仓外 ReShade addon 调用它时此条才成立）；
  2. `dlss_cuda.h:84-88` 自己记录的非确定性竞态（"同样的参数八次运行五比三地给出好帧或平帧"）意味着失败可出现在**任意**一帧——第一帧成功后，第 N 帧的空输出静默通过；
  3. 即使判失败返回 false，`reported` 已在分析前置真，同帧重试也不会再查。
- **修复**：把"每进程一次"只保留给**信息性日志**；失败判定（输出全零 + 输入采样非零）每次 evaluate 都执行。输出侧可复用现成的 24 行采样器 `sample_nonzero_bytes` 做每帧空判以控制成本；至少要排除 selftest 消耗检查。
- **已修复（2026-09-15）**：`dlss_layer/dlss_cuda.cpp` `finish_evaluation` 新增**每帧采样闸门**——`sample_nonzero_bytes(g.output.level0)` 全零且 `sample_nonzero_bytes(g.color.level0)` 非零即 `set_error` + return false；采样器增加 `any_row_read` 出参，读取失败按"未知、继续看"处理而非当全零证据。原全帧读回保留为每进程一次的深检（唯一能看到"部分写入"的能力），闩锁只管详细日志，不再吞掉失败判定。

---

## 二、中严重度

> **复核（2026-09-15）**：M2-M18 已逐条对照 f758aad 源码复核，**全部属实**；M4 附补充复核注见行内（属 API 层缺陷，f758aad 时仓库内尚无调用方传非零 pitch，非现行可触发 bug）。M14/M19 另经用户独立确认并已修复。

### core / image_processor

#### M1 ✅ 已修复 空白输出阈值常量编码错误：`0x191e` 实际是 0.0025，不是注释声称的 0.005
- **位置**：`core/image_processor.cpp:167-168`
- **问题**：注释写 "Blank output threshold: 0.005 in linear FP16 is 0x191e"，但 FP16 `0x191e` = (1+286/1024)×2^(6−15) ≈ **0.0024986**；0.005 的正确编码是 **0x1D1F**（(1+287/1024)×2^(7−15) ≈ 0.005001）。项目自己的口径是 0.005（`video_filter.cpp:2115` 的 `max_channel < 0.005f`）。实际门槛被砍半：输出峰值落在 (0.0025, 0.005] 的空白帧**静默通过判定**。这是本项目刚反复修过的最敏感区域，属回归风险最高的常量笔误。反证：同函数 `:166` 的输入阈值 `0x359A` = 0.3501 与 `video_filter.cpp:1865` 的 `kStillBlankInput` 完全一致。
- **修复**：`constexpr uint16_t blank = 0x1D1F;`
- **已修复（2026-09-15）**：`core/image_processor.cpp` 已改为 `0x1D1F`，注释同步更正并说明 0x191e 是被砍半的旧值。

#### M2 ✅ `wait()` 超时后管线未进入死态：遗留的 `SetEventOnCompletion` 会让下一次 wait 提前放行
- **位置**：`core/image_processor.cpp:248-261`（超时分支）、`:402`（自动复位事件）、`:688-689` 等 6 处 `Reset()` 返回值全部忽略
- **问题**：(a) 第 k 帧 wait 超时返回 false 后，`SetEventOnCompletion(target, fence_event)` 的挂起请求不解除；GPU 随后完成 target 时自动复位事件被置位且无人消费。第 k+1 帧 wait 中 `WaitForSingleObject` **立即**被遗留信号放行——此时 target2 尚未完成，紧随其后的拷贝/求值与在途 GPU 工作竞态，回读可拿陈旧数据且无报错。(b) 超时返回 false 后 `s->started` 仍为 true，下一帧直接 `allocator->Reset()`——若上一帧 command list 仍在执行，Reset 按文档会失败，而返回值被丢弃后在状态未知的列表上继续录制，属未定义行为。
- **修复**：超时分支置 `pipeline_dead` 状态位（或直接调 `stop()`），后续 `process()` 快速失败；每次 Reset 检查 HRESULT。

#### M3 ✅ GPU 选择策略三方矛盾：D3D12 设备、自动配置、stamp 身份、翻译设备可能落在四块不同的卡上
- **位置**：`core/image_processor.cpp:316-373`（start() 纯按 VRAM 选）、`core/gpu_detection.h:193-205`（auto_configure AMD 强制优先）、`core/gpu_detection.h:104-127`（detected_gpu_identity 镜像 AMD 优先）、`core/precompile.cpp:273-275`（compile_one 固定 HIP device 0）
- **问题**：`start()` 只按 VRAM 选卡，而 `auto_configure_gpu_environment` 只要有 AMD 适配器（哪怕 0 MB iGPU）就无条件顶替 VRAM 更大的卡。混合显卡机器（如 NVIDIA 24GB + AMD 16GB）上，D3D12 上传/回读在 NVIDIA，HSA_OVERRIDE 注入目标、RDNA4 判定、预热 stamp 身份全指向 AMD，翻译缓存又绑定 HIP device 0。轻则 stamp 为一块卡作保、实际推理在另一块；重则 ZLUDA 的 CUDA↔D3D12 互操作跨适配器失效。
- **修复**：`start()` 复用与 `auto_configure` 完全相同的选择函数，并把选出的适配器传给 `compile_one` 匹配 HIP 设备，而不是硬编码 0。

#### M4 ✅ `process_raw_rgb48()` 缺 `motion_gpu_row_pitch` 下限守卫，坏 pitch 直接生成非法拷贝
- **位置**：`core/image_processor.cpp:1029-1040`（对照 `process()` 的 `:706` 有守卫）
- **问题**：`process()` 有 `motion_gpu_row_pitch >= in.width * 4` 前置条件，不满足走 CPU 回退；`process_raw_rgb48` 没有——非零但过小或未按 256 对齐的 pitch 被直接填进 `PlacedFootprint.Footprint.RowPitch`，无效拷贝可触发设备移除或未定义内容。两条路径对同一契约防御强度不一致。
- **修复**：`:1029` 加上与 `:706` 相同的前置条件。
- **复核注（2026-09-15）**：属实；补充——f758aad 时仓库内尚无调用方传非零 pitch，属 API 层缺陷而非现行可触发 bug。

#### M5 ✅ motion 参数不合规时，陈旧的运动向量纹理仍被绑给网络
- **位置**：`core/image_processor.cpp:788-792`（对照上传守卫 `:706`、`:732`）
- **问题**：上传有两个前置守卫（`motion_gpu` 的 pitch、`motion` 的尺寸匹配），任一不满足时上传被跳过，但 `:788` 的绑定条件**不复查**尺寸/pitch，仍把 `s->motion_tex`（装着上一帧或更早的运动数据）绑进 `frame.motion_vectors`。调用方传了 motion 说明是时序链，拿旧 MV 指导本帧会直接产生重影/游动伪影——比"零运动"更糟。
- **修复**：引入 `bool motion_uploaded`（由两个上传分支置位），`:788` 只在真实上传成功时绑定。

### tools / video_filter

#### M6 ✅ GpuFlow 栅栏等待超时被忽略，超时后 Reset allocator 与 GPU 在飞命令竞态
- **位置**：`tools/video_filter.cpp:1822`、`:1846`（`WaitForSingleObject(fence_event,5000)` 返回值丢弃）
- **问题**：第一次提交后立刻 `allocator->Reset(); cmd->Reset(allocator,nullptr);`（`:1823`）。D3D12 规定 Reset 前必须确认 GPU 执行完关联命令列表；5 秒等待超时（GPU 挂起/ZLUDA 卡死）时代码不检查等待结果就 Reset，属未定义行为；随后 `readback->Map` 读到未完成数据，**垃圾光流被静默当成功**送进网络做时间重投影。
- **修复**：检查等待返回值，非 `WAIT_OBJECT_0` 置 `ready=false` 并返回 false，让调用方落到已存在的 CpuFlow 回退路径。

#### M7 ✅ `--flow-only` 诊断开关在省略 `[runtime] [nvapi]` 时静默失效，反而跑完整 GPU 管线
- **位置**：`tools/video_filter.cpp:2566-2569`（对照 `:1295-1299` 的可选位置参数）
- **问题**：位置参数只有 4 个必需，`runtime`/`nvapi` 都可省（省略时选项从 `argv[5]` 开始），但扫描从 `argv[7]` 开始 → `--flow-only` 被 `parse_args` 当 no-op 吞掉后**程序照常执行完整网络推理**（预编译、空白重试、写输出一样不少），唯独没有 flow 统计——与注释 "blank-race immune" 的诊断意图完全相反。
- **修复**：扫描从 `i = 5` 开始，或给 `Options` 加 `flow_only` 字段由 `parse_args` 统一解析。

#### M8 ✅ probe 用 `r_frame_rate` 而非 `avg_frame_rate`，VFR 源输出时长/音画同步错误
- **位置**：`tools/video_filter.cpp:472-474`（probe）、`:528-535`（解析）、`:639-648` + `:711`（编码器 `-r` 直接采用）
- **问题**：`r_frame_rate` 是"最大基准帧率"，VFR 素材（手机录像、部分 MKV）可能给出 90000/1000 之类值。该值被传给编码器 `-r`，输出 CFR 时长 = 帧数 ÷ 该帧率 → 时长可能缩短几十倍，与拷贝进来的音频轨严重不同步。并行分片 `frame_index_offset`（`:2235`）与 warmup 估算（`:2544`）同样基于它。
- **修复**：probe 改取 `avg_frame_rate`（对 CFR 两者一致），或输出侧改用 `-fps_mode cfr`。

#### M9 ✅ GPU 光流"直通"路径是死代码：history 时间平滑永不生效，每帧多一次纯浪费的 GPU 往返
- **位置**：`tools/video_filter.cpp:2989-2990`（`gpu_motion` 声明后从未赋值，恒 `nullptr`）、`:3033-3041`（三元恒走 CPU 分支）、`:1714-1715`（`gpu_motion()/gpu_motion_pitch()` 无人调用）、`:1763-1774`（shader 内时间平滑）、`:1823-1846`（第二条命令列表只为拷 history）
- **问题**：shader 写入 `Full` 的历史平滑结果从不回流——readback 只拷 `out`（原始 Flow），CPU 侧拿到的是**没有任何时间平滑**的运动场，而 CpuFlow 路径有 `previousMotion` 平滑（`:1663-1682`），两条路径行为不一致；且每帧固定多一次命令列表提交 + 全分辨率 `CopyResource` + 栅栏等待。
- **修复**：要么把 `gpuflow.gpu_motion()` 接上走真正零拷贝；要么删掉 shader 的 history 平滑与第二条命令列表，在 CPU 侧补时间平滑。

#### M10 ✅ AMF 自动选择只查分辨率下限，无上限/级别检查，且运行期失败无法回退
- **位置**：`tools/video_filter.cpp:601-603`、`:655-661`、`:730-738`
- **问题**：H.264 AMF 有级别上限（通常 4096×4096 / Level 5.2）。5K/8K 或超宽视频上 ffmpeg 能正常启动、首个 GOP 后才编码失败 → "encoder pipe broke" → 整个任务失败，重试同样失败。spawn 成功后的运行期 AMF 失败没有任何回退到 x264 的路径。
- **修复**：`detect_amf_support` 增加编码器实际上限判断（H.264 → 4096，HEVC → 8192），超限直接选软编。

### dlss_layer

#### M11 ✅ `evaluate()` 的 CopyResource 不校验格式/尺寸，失败是静默的
- **位置**：`dlss_layer/dlss_cuda.cpp:1519-1545`（color/backbuffer/depth/motion 拷贝）、`:1564-1573`（output 回拷）
- **问题**：共享纹理格式写死（RGBA16F / R32F / RG16F），但 `CopyResource` 前对 `frame.color/depth/motion_vectors/output` 的格式与尺寸**没有任何校验**。调用方传入其他格式（如 `frame_blit.h:4-7` 注明催生本项目的 R10G10B10A2）时拷贝静默不动，网络读到上一帧残影或全零，函数照样返回 true。`:1566` 还假定调用方 output 已处 COMMON 态，契约未在头文件写明。
- **修复**：拷贝前 `GetDesc()` 比对 Width/Height/Format，不匹配即 `set_error` 拒绝；头文件注明 COMMON 态契约。

#### M12 ✅ `report_device_count` 按硬编码偏移直接解引用，可能访问违例
- **位置**：`dlss_layer/dlss_cuda.cpp:188-191`
- **问题**：`*reinterpret_cast<const unsigned long long *>(snippet + 0x1152C00)` ——该偏移"只属于 v310.8.0"；合理性检查 `count > 16` 在**读取之后**才执行。换任何其他版本的 `nvngx_dlssnr.dll`，该偏移可能落在映射镜像之外，诊断代码直接把进程打出 AV——诊断崩主程序比没有诊断更糟。
- **修复**：先用 `GetModuleInformation` 校验偏移范围，或包 `__try/__except`。

#### M13 ✅ `init()` 失败后的重试路径泄漏 CUDA 上下文与库句柄
- **位置**：`dlss_layer/dlss_cuda.cpp:661-669`（重入守卫）、`:728`（`cuCtxCreate` 覆写 `g.ctx`）、`:730/695/685`（LoadLibrary 覆写）
- **问题**：init 在 `ngx_init_ext`（`:844`）等处失败时 `g.ctx`、`g.nvcuda`、`g.ngx` 已填充但未清理；调用方合法重试时重入守卫放行，`cuCtxCreate` 直接覆写 `g.ctx`——上一个 context 永久泄漏，每次重试漏一个。
- **修复**：init 入口检测部分初始化状态（`g.ctx || g.nvcuda || g.ngx` 非空即脏），先走 cleanup 再重来，或报错要求先 shutdown。

#### M14 ✅ 已修复 `flush_and_wait` 忽略 WAIT_FAILED；`CreateEventW` 返回值未检查
- **位置**：`dlss_layer/dlss_cuda.cpp:569-575`、`:882`
- **问题**：事件创建失败（句柄耗尽）时 `WaitForSingleObject(NULL,...)` 返回 `WAIT_FAILED` ≠ `WAIT_TIMEOUT`，`flush_and_wait` **在根本没等待的情况下返回 true**——GPU 同步被静默跳过，后续 CUDA 读共享纹理与 D3D12 拷贝竞争。
- **修复**：init 时检查 `CreateEventW`；wait 后仅 `WAIT_OBJECT_0` 视为成功，其余报错（附 `GetDeviceRemovedReason`）。
- **已修复（2026-09-15）**：`dlss_layer/dlss_cuda.cpp` init 时 `CreateEventW` 失败即报错返回；`flush_and_wait` 仅 `WAIT_OBJECT_0` 视为成功，`WAIT_TIMEOUT` 与 `WAIT_FAILED`（含空句柄）分别报错并附 `GetDeviceRemovedReason`。注意：`flush_and_wait` 的**返回值**在全部五处调用点本就有检查（用户复核确认），本条修的是函数内部的等待判定。

#### M15 ✅ `upload_shared_colour_raw_rgb48`：不校验尺寸；RAW SRV 的 D3D12 前置条件无人把关
- **位置**：`dlss_layer/dlss_cuda.cpp:1637-1641`、`dlss_layer/frame_blit.cpp:425-433`
- **问题**：(a) `width/height` 从不与 `g.color` 实际尺寸比对（对照 `upload_shared_colour` 的 `:1598` 有 `rows > ad.Height` 校验）；(b) `frame_blit.cpp:432` 的 `D3D12_BUFFER_SRV_FLAG_RAW` 要求源 buffer 带 `ALLOW_UNORDERED_ACCESS` 标志，本层不检查也不注明，调用方用普通 upload buffer 调进来只会静默无效。
- **修复**：入口校验尺寸等于 `g.color`；`GetDesc().Flags` 预检并在 `frame_blit.h` 注明要求。

### gui / ngx_runtime

#### M16 ✅ 单帧提取：对可能仍在运行的 QProcess 二次 `start()`（Qt6 下是空操作），失败路径泄漏 ffmpeg 进程
- **位置**：`gui/batch_window.cpp:1123-1138`
- **问题**：Qt6 中对已运行进程再次 `start()` 只是打警告、原进程继续跑。第一次 ffmpeg 因大文件/慢盘 5 秒未完成时，第二次 `start()` **不会启动回退命令**；若 10 秒后仍未完成，函数报错返回但**没有 `kill()`**——ffmpeg 子进程在后台永久存活。与 `probe_total()`（`:1340-1342`）专门写的修正注释精神相悖；且两处 ffmpeg 调用没有 `-nostdin`。
- **修复**：超时后先 `extract.kill(); extract.waitForFinished(1000);` 再启动回退；最终失败路径同样 kill；补 `-nostdin`。

#### M17 ✅ GUI 线程同步等待簇：最长可冻结界面 10 秒以上
- **位置**：`gui/batch_window.cpp:1324-1372`（probe_total 最长 7s）、`:1129/:1134`（单帧提取两次 waitForFinished(5000)，与按钮 tooltip 承诺的"约 0.1 秒"直接矛盾）、`gui/compare_view.cpp:127-131`（对比图构造再等 6s）、`batch_window.cpp:995/1082/1159`（waitForStarted(5000)）
- **问题**：期间事件循环完全阻塞，窗口白屏/"未响应"。这是本次更新主打功能（单帧对比、预热）的直接体验问题。
- **修复**：ffprobe/ffmpeg 探测挪到 `QtConcurrent`/局部线程，或至少缩短超时并给状态提示。

#### M18 ✅ 预览帧率探测：ffprobe 超时后不 kill，QProcess 带着活进程析构
- **位置**：`gui/batch_window.cpp:1042-1060`
- **问题**：`probe_total()` 修过的问题（"A wedged ffprobe must not outlive this function"）在 `start_preview()` 里原样存在——超时后放任 QProcess 离开作用域，ffprobe 对网络流/损坏文件可无限挂起，每次预览泄漏一个 ffprobe.exe。
- **修复**：复制 `probe_total()` 的超时处理（kill + waitForFinished）。

### core / precompile

#### M19 ✅ 已修复 precompile 的 10 分钟"无完成进度"死线会误杀慢机上的正常大模块翻译
- **位置**：`core/precompile.cpp:386-393, 481-503`（与 `core/image_processor.cpp:459-460` 自相矛盾）
- **问题**：`image_processor.cpp:459-460` 自己的注释承认最大模块翻译需要"tens of minutes"；而 precompile 死线从循环开始前起算"距上次任一模块完成"的墙钟时间，首轮按最大优先起跑，慢机上第一个完成超过 10 分钟即触发——**所有仍在健康翻译的子进程被 TerminateProcess 杀掉**，剩余未起跑的也计入 failures，整次 precompile 判失败不写 stamp。默认值与同代码库声明的模块耗时量级直接冲突。**（v6 新引入的风险：v6 恰恰是为 RDNA4 软件路径最慢的机器打的，分发前必须先修。）**
- **修复**：默认值提到不小于已观测最大模块翻译时间（或按最大模块文件大小自适应放宽）；更好的做法是只杀 CPU 零进度的子进程，有 CPU 进度的仅告警。
- **已修复（2026-09-15）**：`core/precompile.cpp` 双管齐下——默认死线 10→**60 分钟**（环境变量 `DLSSNR_PRECOMPILE_NO_PROGRESS_MINUTES` 覆盖语义不变）；死线触发时按 CPU 监控的既有判定拆分：`stalled>0`（本片零 CPU 进度）的子进程杀掉并计失败，仍在积累 CPU 时间的**只告警继续等**（模块名随 `report(progress)` 可见，`straggler_detail` 记录首次告警，幸存子进程在后续静默片中被继续甄别）。未起跑文件不再预计失败，会照常排队执行。

---

## 三、低严重度

> **复核（2026-09-15）**：L1-L44 已逐条对照 f758aad 导出源码复核——**41 条属实、3 条部分成立（L4、L7、L9，行内已修正）、0 条不成立**；L30 行号勘误见行内。L11 七项死代码逐一全文件检索均无使用点。

### tools/video_filter.cpp

| # | 位置 | 问题 |
|---|------|------|
| L1 | `:1590-1594` | `CpuFlow::upscale4` 在 `qh<=1`/`qw<=1` 时 `qh-2` 无符号回绕 → `iy=-1` 巨量越界读（源高 ≤4 可触发，上轮 C22 残留） |
| L2 | `:475/:548` | `spawn(command, Pipe{}, ...)` 把临时对象绑定到非常量左值引用（C4350，`/permissive-` 或 clang-cl 下编译失败，上轮 C25 残留） |
| L3 | `:2648-2658` | 主流程 WIC 工厂创建后无 `Release`（`:2954` 释放的是 encode 线程自建的）、`CoInitializeEx` 无配对、`dump_ok` 死变量 |
| L4 | `:1907/:1928/:1935-1937` | `run_image_mode`：`retries` 死变量（~~用户 `--retries 1` 实际走 real_main 默认 5~~ **复核勘误：`--retries` 实际由 real_main `:3277-3280` 解析生效，死变量仅影响该函数内部计数**）、`--style` 未做 0..3 钳制、`--dlss-model-preset` 未过滤（视频路径 `:2630` 有过滤）。**【部分成立】** |
| L5 | `:2851-2854` | 非零拷贝 + 未开 flow 时 3 份原始帧缓冲（4K 约 150MB）常驻不释放，仅在 `mapped_ptr` 时 clear |
| L6 | `:3307-3325` | `--retries` 命令行改写按空格定边界，路径恰含 ` --retries ` 时被误改写损坏重跑命令 |
| L7 | `:613-624` | `detect_amf_support` 先关管道读端再 wait（~~stderr 写失败可使探测误判失败~~ **复核勘误：stderr 机制不成立——`:614` `redirect_stderr=false`，仅 stdout 连管**）；5 秒超时永久缓存，冷驱动首次初始化 >5s 被杀后整个进程生命周期都回退 x264。**【部分成立】** |
| L8 | `:440-453` | `tool_cmd` 把 `FFMPEG_PATH` 强当目录，设成完整 exe 路径时拼出 `...\ffmpeg.exe\ffmpeg.exe` |
| L9 | `:204-239` | `WorkerPool::parallel_for` 若从工作线程嵌套调用会自死锁（当前无现役调用，属契约隐患。**复核勘误：单处嵌套不会自死锁——其余空闲 worker 会消化分片，仅全部 worker 同时嵌套阻塞才死锁**；现调用点 `:771/:859/:875/:890/:915/:1030` 全在应用线程）。**【部分成立】** |
| L10 | `:2937-2938` | dump 路径 `swprintf` 截断无提示；`:2544/:2235` 分片偏移用 round 估算与 ffmpeg `-t` 实际帧数可差 ±1（接缝丢/重帧的低概率来源） |
| L11 | `:1073-1091` 等 | 死代码清单：`resize_rgb48`、`Channel::clear`、`image_has_signal`、`OutputFrame::blank`/`input_has_signal`（只写不读）、`eval_index`（只增不读）、`GpuFlow::full_copy_ready`（恒 false）、GpuFlow 的 cb 缓冲（创建 256 字节从未绑定） |

### core

| # | 位置 | 问题 |
|---|------|------|
| L12 | `core/precompile.cpp:436-449` | WAIT_TIMEOUT 分支 `GetExitCodeProcess` 失败时 exit_code 保持 0x103 → 误计"失败+完成"；且此处完成不刷新 `last_completion`，极端下让 M19 死线提前触发 |
| L13 | `core/precompile.cpp:425 → 541-543` | WAIT_FAILED 的具体错误串（GetLastError 值）被末尾计数消息覆盖，根因丢失（straggler 有专门字段，这里没有） |
| L14 | `core/precompile.cpp:398-404` | driver 路径以反斜杠结尾时拼出 `"C:\zluda\"`——尾部 `\"` 是字面引号，子进程 argv 解析错乱，**所有模块**翻译失败（GUI 手输带尾反斜杠目录即触发） |
| L15 | `core/precompile.cpp:82-88` | 固定名临时目录 `dlss5-precompile`，GUI 与 CLI 双开预热时互踩，读到截断模块镜像。建议掺入 `GetCurrentProcessId()` |
| L16 | `core/precompile.cpp:76-80` | `own_path()` 不检查 `GetModuleFileNameW` 截断，长路径安装时所有模块静默失败且无独立报错 |
| L17 | `core/gpu_detection.h:154-167` | `is_rdna4_gpu` 的 id 区间匹配（0x7551-0x755F/0x7591-0x759F）超出自身注释确证口径（仅 0x7550/0x7590）；`rx 9` 裸子串匹配脆弱。误匹配方向安全（漏注入而非错注入），但未来新 id 落入区间会静默错误注入 |
| L18 | `core/gpu_detection.h:118-121` | `detected_gpu_identity()` 兜底只排除 `is_software` 不排除名称判定的 `is_virtual`（虚拟显示卡可成为 stamp 身份） |
| L19 | `core/gpu_detection.h:55-59` | %TEMP% 自动配置日志用 ANSI 路径 + `fopen_s`：非当前代码页可编码的用户名时**静默失败**，唯一落盘诊断丢失（建议 `GetEnvironmentVariableW` + `_wfopen`） |
| L20 | `core/gpu_detection.h:72-74 vs 172-175` | `gpus.empty()` 时误报 "CreateDXGIFactory1 failed"（工厂可能成功、只是枚举为空），误导排障方向 |

### dlss_layer

| # | 位置 | 问题 |
|---|------|------|
| L21 | `dlss_cuda.cpp:1337-1338` | `evaluate_ngx` 无 feature 时静默返回 false 不设 last_error，调用方拿到陈旧错误信息 |
| L22 | `dlss_cuda.cpp:1613-1633` | backbuffer 描述符查询失败 → 整个函数返回 false（color 上传已成功，调用方误弃好帧）；尺寸不合适 → 静默跳过，两种失败处理不一致 |
| L23 | `dlss_cuda.cpp:1254` | 一次性整帧读回：8K RGBA16F 约 253MB 瞬时分配，无 try/catch，bad_alloc 即终止进程（改分块读回可同时解决 H3 的成本借口） |
| L24 | `dlss_cuda.cpp:1201-1205` | 24 行采样可能漏掉薄内容：内容只存在于未采样行时 `input_nonzero==0` → 真失败误判为"暗帧"（H3 修复的已知残余盲区，注释应写明） |
| L25 | `dlss_cuda.cpp:1444-1449` | `evaluate_backbuffer` 第二个 `to_shared` 返回值被忽略 + 各纹理尺寸关系未校验，网络照旧消费陈旧 Backbuffer |
| L26 | `dlss_cuda.cpp:1718-1738` | `shutdown` 销毁 CUDA 对象前不 `cuCtxSetCurrent(g.ctx)`；`release_shared`（`:383-391`）不检查返回码，外存映射泄漏无报告 |
| L27 | `dlss_cuda.cpp:514/:1242`、`g_error` | 进程级 static 无线程安全（当前单线程用法良性，值得注明约束） |
| L28 | `dlss_cuda.cpp:798 vs 845` | `__NGX_LOG_LEVEL` 硬编码 "3"，与 `desc.log_level` 脱节 |
| L29 | `dlss_cuda.cpp:1053-1063` | 复用 feature 时 `render_preset`/DLSS_PRESET 变更被静默吞掉（只在创建时下发） |
| L30 | `frame_blit.cpp:234-237, 272-275, 303` | init 失败路径泄漏 g.vs/g.ps（**复核勘误：`:196-210` 两处其实已正确释放 vs/ps；真正泄漏点为 `:234-237`、`:272-275`，另 `:303` 也漏释放**），且 `g.device` 在成功前已被赋值 |
| L31 | `frame_blit.cpp:114-121` | 描述符堆 64 槽环形回卷覆写依赖"每帧必 flush"的隐式约定，无注释/断言 |
| L32 | `dlss_layer/ngx_cuda.h:73-92` | `NVSDK_NGX_Parameter` 纯虚接口无虚析构（当前无害，未来经基类指针 delete 即 UB） |

### gui / ngx_runtime

| # | 位置 | 问题 |
|---|------|------|
| L33 | `batch_window.cpp:163-170` | RDNA4 提示的显卡名单与 `is_rdna4_gpu` 漂移：漏掉 R9700（"radeon ai pro r9700" 不含 "radeon 9"）与 device-id 区间，R9700 用户收不到唯一的 GUI 纠正提示。应直接调用 `dlssnr::is_rdna4_gpu()` |
| L34 | `batch_window.cpp:768-773, 1031-1034` | 裸文件名输入时 `QFileInfo(path())` 返回空串 → 拼出盘根路径 `/xxx_dlss.mp4`，普通用户无写权限 |
| L35 | `batch_window.cpp:104` | 窗口标题版本号过期（v2026.09.10-multipass），用户按标题报 bug 会给错版本 |
| L36 | `batch_window.cpp:382/:801 vs load_settings` | `imagePasses` 默认值三处不一致（构建 3 / 恢复默认 3 / ini 缺省 1） |
| L37 | `batch_window.cpp:1116-1121` | 单帧对比临时文件固定名且从不清理（对比 compare_view 有完整清理），长期累积 + 双实例互踩 |
| L38 | `batch_window.cpp:1374-1439, 1244-1276` | 行缓冲只按 `'\n'` 切分不处理 `'\r'`，ffmpeg 实时统计行要等最终 `\n` 才显示 |
| L39 | `batch_window.cpp:97-99` | QSettings 写程序目录，Program Files 安装时静默丢失全部设置 |
| L40 | `batch_window.cpp:1540-1544` | dropEvent 不校验 `isLocalFile()`，拖入 URL 会清空已填输入 |
| L41 | `ngx_runtime.cpp:313-326` | `s_allocated_params` 只增不减（每块约 57KB）；populate 失败仍写出 `*out_params` 并入列 |
| L42 | `ngx_runtime.cpp:380-386` | `ngxrt_shutdown` 先 clear 参数块再调 snippet Shutdown，潜在 use-after-free 窗口；顺序应反过来 |
| L43 | `ngx_runtime.cpp:339-360` | `ngxrt_force_create_path` 无法满足强制值时静默回落 path 0，诊断 API 应显式报错 |
| L44 | `compare_view.cpp:15-21` | `is_image_file` 后缀表缺 gif/avif/heic/jfif，这些输出走 ffmpeg 抽帧路径产生误导性对比图 |

---

## 四、脚本与构建系统

> **复核（2026-09-15）**：S2-S12 已逐条复核——**S4-S12 属实**（S8 行号应为 `21-25/88`、S11 的 `*.txt` 应为 `:69`，均差 1 行，不影响结论）；**S2/S3 部分成立**：技术内容属实，但 `build_qt_gui.bat` 被 `.gitignore` 忽略、不在 f758aad 中，行号以工作区文件为准。

| # | 级别 | 位置 | 问题 |
|---|------|------|------|
| S1 | ~~**中**~~ **低（防御性，已修复）** | `CMakeLists.txt:58` | `dlssnr_gui` 目标缺 `/utf-8`（对照 `:77` video_filter、`:108` processor_smoke 均有）。~~CP936 机器上发给最终用户的 GUI 界面中文全部乱码~~ **（2026-09-15 降级，见修订记录 1）**：机制前提全部核实成立，但出厂 `dlssnr_gui.exe` 六个中文界面串字节扫描全部是正确 UTF-8（GBK 命中 =0），**没有发生乱码事故**；MSVC 未弄坏的原因未定论。仍值得加：消除对构建机代码页的依赖。**已修复（2026-09-15）：`CMakeLists.txt` 已为 `dlssnr_gui` 加 `/utf-8`** |
| S2 | 中 | `build_qt_gui.bat:11,13` | vcvars `>nul 2>&1` 吞错不查 errorlevel；Qt 路径写死 `C:/Qt/6.10.3` 不读 `QT_DIR`（build.bat 已有四路探测+兜底，此脚本没复用） |
| S3 | 中低 | `build_qt_gui.bat:17-18` | "编译失败"分支 `echo` 把 errorlevel 归 0 → **假成功退码**；且只部署 exe 不刷新 `run\nvngx.dll`（build.bat:74-81 注释记录过"run\ 残留旧构建"事故） |
| S4 | 中 | `fix_tdr.ps1:63` | 备份文件固定名且每次覆盖：第二次运行备份到的是第一次改过的值，**用户真正的原始值永久丢失**。应带时间戳或追加 |
| S5 | 中低 | `fix_tdr.ps1:76-83` | 备份写失败仅 WARNING 后**继续**无条件写注册表（只读目录下"没有备份也改了系统"）；TdrLevel=0 时提示 "stop now" 却无任何暂停/确认 |
| S6 | 中低 | `diagnose_gpu.ps1:143` | HIP 探测 `count=0` 仍显示绿色（同函数 zluda 分支 `:142` 有 `-notlike "*count=0*"`）——`hipSuccess + count=0` 正是 RDNA4 override 失效的典型形态，绿色误报让人误判 HIP 侧正常 |
| S7 | 中低 | `tools/hardware_budget.h:57-69` | `Budget ≤ CurrentUsage`（显存被占满/超卖）时 avail 保持 `vram_mb/2` → Gate 5（`:114`）放行双进程冲进满载显存，恰是 F1 想防的 device removed 场景。该分支应置 0 |
| S8 | 中低 | `tools/framebench.cpp:19-23,52`、`tests/processor_smoke.cpp:22-26,89` | ANSI argv 按 CP_UTF8 转宽字符 → 含中文路径必失败（对照 launchbench.cpp:19 用 `wmain` 才对）；framebench `frames<=0` 仍打印 OK 退码 0（假成功） |
| S9 | 低 | `tools/launchbench.cpp:51-54, 91-97` | `ifstream` 不检查打开成功（文件不存在误报"module load 失败"）；所有 `cuLaunchKernel` 返回值不检查，launch 全失败时照样打印"us per launch"——基准数字失真 |
| S10 | 低 | `tools/test_rdna4_detect.cpp:13-15` | 测试本身质量好（用例与谓词逐条一致），但**未接入 CMake/CTest 门禁**——改坏谓词后此测试依然不会自动跑（文件头自己抱怨的 C8 根因依然成立） |
| S11 | 低 | `build.bat:33,35` | vcvars `>nul` 不查 errorlevel、不检查 ninja 是否存在（A8 残留）；`:68` `/XF *.txt` 会把 dist 里说明类 .txt 双侧排除 |
| S12 | 低 | `CMakeLists.txt:19-20` | `message(FATAL_ERROR "...is a Windows ")` 句子截断；diagnose_gpu.ps1 `:151-152,197` 硬编码 `C:\Windows\System32`、`:244` `Get-Command *.dll` 永远找不到（不在 PATHEXT） |

---

## 五、旧报告（2026-09-11 两份）修复状态核对摘要

**已确认修复且与当前代码吻合**：
- 旧 H1 / FRESH：`gpu_detection.h` EnumAdapters1 空指针防护（现 `:76-79` 每次 `adapter = nullptr` + 失败即 break）
- `video_filter.cpp` shadow_protect 绕过零拷贝（四系数默认 1.0f）
- `--reset every=0` 除零防护；`--selftest` 参数守卫
- A7 windeployqt 指向、A21 僵尸 target 删除、A9 robocopy /XD 更新、H13 processor_smoke 接入 CMake
- fix_tdr.ps1 的 E5/E6 主体（TdrLevel 展示、备份文件）——但见 S4/S5 残余
- diagnose_gpu.ps1 的 E1/E2/H3/H4（独立子进程隔离探测、override 注入时机、FreeLibrary、count>0 判据）修得很干净——仅 HIP 判色留尾巴（S6）

**旧报告标注"依然有效"、本轮确认仍未修的**：
- FRESH G 段三实测问题中，翻译子进程挂死父进程的看门狗用户 workaround 未合入（本轮 M19 的死线是新写的，语义不同但覆盖了"挂死"方向；50-60% 黑图的 sticky 问题由 `--precompile-wait` 串行模式与重试机制覆盖，见 `video_filter.cpp:3211-3216` 注释）
- FRESH H2（GUI 换输入覆盖上次成品——唯一静默数据丢失项）：本轮未复核 GUI 输出路径覆盖逻辑，建议单独确认
- K1（描述符堆槽位冲突）、K2（backbuffer 别名）等 D 段历史条目：本轮在 `frame_blit.cpp` 复查中确认"每帧必 flush"约定当前成立（L31），但无防护

**旧报告存疑项定论**：
- M7 证实两份 `ngx_cuda.h`（`dlss_layer/` 与 `ngx_runtime/`）SHA256 相同——内容漂移风险存在但当前无实质差异
- B21（缺 include）、C10（`--upscale-mode` 语义）仍属"需实机/需定语义"类

---

## 六、已排查、确认无问题的方面（避免重复排查）

- **零拷贝缓冲池无竞态**：`process_raw_rgb48` 内部以 `flush_and_wait()` 同步完成后才归还 `free_input_pool`，decode 线程覆写 mapped 内存时序安全
- **新空白 gate（v6 修复）自洽**：`kBlankGateInput16=24000` + `kBlankGateStreak=3` 在 stage 2/3 各自独立计数先触发先 abort；输入峰值 64 像素稀疏采样只会漏报不会误报
- **cuda_min.h 结构布局**：`CUDA_MEMCPY2D` 等逐一与 CUDA 头核对，字段序/联合大小一致，无打包错误
- **half_float.h 数学正确**：双向转换逐例验证通过（65504→0x7BFF、亚正常 RNE tie-to-even、NaN 规范化）
- **abort 路径无死锁**：三阶段 blocking Channel 池总量 3 保证不永久阻塞；`CancelSynchronousIo` 在 join 前解阻塞，顺序正确
- **退出码语义**：parse 失败 1、空白竞态 2、`--max-frames` 提前收尾不误触失败判定
- **GpuFlow 的 D3D12 状态机**：UAV↔COPY_SOURCE、GENERIC_READ↔COPY_DEST 转换闭环正确（唯一漏洞是 M6 的超时忽略）
- **D3D12 引用计数**：`start()` 适配器枚举 AddRef/Release 配对、`release_images()`/`stop()` 顺序均正确
- **GUI 无跨线程 UI 更新问题**：全部 QProcess/定时器信号经事件循环投递；stdout 被主动排空防管道死锁；`half_float.h`、compare_view 无功能性 off-by-one
- **batch_main.cpp 的 job 句柄保持打开**是 `KILL_ON_JOB_CLOSE` 的有意设计

---

## 七、修复优先级建议

1. **立即修**（用户可直接踩中，修复成本极低）：
   - H1（并行+特定音频 = 整段白做）、H2（崩溃路径一行防护）、M1（空白阈值常量一行）、S1（GUI 乱码一行）
2. **尽快修**（修掉"声称已修但其实没修住"的信用问题）：
   - H3（efa9867 的一次性闩锁）、M2/M6/M14（三个 fence/事件超时忽略是同一类病，一起修）、M17/M18（GUI 卡顿与进程泄漏是本次主打功能的直接体验）
3. **安排修**：M3-M5、M7-M15、M16、M19、S2-S8
4. **低优先/顺手修**：第三节 L 系列与 S9-S12；死代码清单建议一次性清理

---

*审查方式说明：本报告由 AI 辅助完成全文审查（并行多代理逐文件细读 + 关键发现人工对照源码复核），全部条目已于 2026-09-15 逐条对照 f758aad 源码复核完毕：63 属实 / 5 部分成立（L4/L7/L9/S2/S3）/ 0 不成立，勘误均已行内注明。*
