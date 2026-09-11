# 代码审查报告

**审查日期**：2026-09-11
**审查范围**：`tools/video_filter.cpp`、`gui/batch_window.cpp` 等 Qt GUI、`core/`、`dlss_layer/`、`ngx_runtime/`、构建与打包配置
**审查方式**：四路并行静态代码审查 + 关键结论逐行人工复核

---

## 环境事实（已核实，作为后续判断的前提）

| 项 | 实际值 |
|---|---|
| Visual Studio | `D:\Program Files\Microsoft Visual Studio\18\Community`（**不在 C 盘**） |
| Qt | `C:\Qt\6.10.3\msvc2022_64`（**不是 6.11.2**） |
| C# GUI（`gui_app/`） | **已删除**，发布说明确认"原 WinForms 界面已正式弃用移除" |
| 主力 CLI | `tools/video_filter.cpp`，2847 行 |
| 主力 GUI | `gui/batch_window.cpp`，约 1290 行（新增 composite / 预览 / 定帧 / 并行等） |
| 交付物 | `dlssnr_gui.exe`（Qt 版）+ `video_filter.exe`（CLI） |

## 图例

- 🔴 **致命**：导致无法编译/无法运行/发版出空包，必须立即修
- 🟠 **严重**：崩溃、数据/显存/句柄泄漏、静默的错误结果、误导性失败信息
- 🟡 **一般**：功能不达预期、设置丢失、明显交互缺陷
- 🟢 **轻微**：死代码、不一致、边缘情况
- 🟣 **存疑**：无法仅凭读代码定论，需实机验证
- ✅ 已由本报告作者逐行人工复核确认

---

# A. 构建 / 打包 / CMake

| # | 级别 | 位置 | 问题 |
|---|---|---|---|
| A1 | 🔴 ✅ | `build.bat:6` | VS 路径写 `C:\Program Files\...`，本机在 `D:\Program Files\...` → vcvars64.bat 不存在，cl/cmake/ninja 全不在 PATH |
| A2 | 🔴 ✅ | `build.bat:5` | Qt 默认 `C:\Qt\6.11.2`，本机 `C:\Qt\6.10.3` → 不手动设置就 `exit /b 1` |
| A3 | 🔴 ✅ | `package_release.py:22-34` | 从 `run\` 取文件，但 `run\` 无任何脚本生成（`build.bat` 产出 `dist\`，CMake 无 install 规则），且 `.gitignore` 忽略 `/run` |
| A4 | 🔴 ✅ | `package_release.py:47-52, 84, 92` | 缺文件只 `Warning` 不中断，最后照样打印 `Successfully generated` / `Verification passed` → **打出空包还显示成功** |
| A5 | 🟠 ✅ | `package_release.py` | 缺 MSVC 运行库 `vcruntime140.dll` / `vcruntime140_1.dll` / `msvcp140.dll` → 干净机器 `0xc0000135` 起不来 |
| A6 | 🟠 ✅ | `package_release.py:29` | 只拷 `icuuc.dll`，缺 `icuin*.dll` / `icudt*.dll` → `Qt6Core.dll` 加载失败，`dlssnr_gui.exe` 根本起不来 |
| A7 | 🟠 ✅ | `build.bat:19` | windeployqt 打的是 `dlss5-image-enhancer.exe`，但交付物是 `dlssnr_gui.exe` |
| A8 | 🟠 ✅ | `build.bat:7-12` | 只校验 `QT_DIR`，不校验 VS 路径 → 失败信息误导排查方向 |
| A9 | 🟡 | `build.bat:34` | `/XD` 漏 `dlssnr_gui_autogen`（MOC 目录会被 `/MIR` 镜像进 dist）。佐证：`nvngx_autogen` 在列表里，但 `CMakeLists:93` 已关掉它的 AUTOMOC，说明这份列表没跟着 target 更新 |
| A10 | 🟡 | `build_smoke.bat:8` | `OUT=%ROOT%..\bin` → 产物落到 `d:\Downloads\bin`（工作区外），且那里没有 nvngx/nvcuda/nvapi，跑不起来 |
| A11 | 🟡 | `build_smoke.bat:5` | VS 路径同样是 C 盘错误 |
| A12 | 🟡 | `CMakeLists.txt` | 无任何 `install()` 规则 → `dist\`、`run\`、`CMakeLists` 三处各写一遍文件清单，加一个文件要改三处，无单一事实来源 |
| A13 | 🟡 | `package_release.py:7` | 命名丢掉 `ZLUDA` 关键词（历史包是 `DLSSNRFilter-ZLUDA-*`） |
| A14 | 🟡 | 三处 | 版本号不一致：窗口标题 `v2026.09.10` / 打包脚本 `v2026.09.11` / README 与发布说明 `2026-09-11` |
| A15 | 🟢 | `package_release.py:6` | `root_dir` 硬编码绝对路径，换机器即失效 |
| A16 | 🟢 | `package_release.py:59-63` | `shutil.copy2` 拷 plugins，若 `platforms/` 下出现子目录会抛 `IsADirectoryError` |
| A17 | 🟢 | `package_release.py` | 打包后不清理 staging 目录（靠 `.gitignore` 躲版本控制） |
| A18 | 🟢 | `build.bat:15` | 固定 `-G Ninja` 且不检查 ninja 是否存在（vcvars64 不会把它加进 PATH） |
| A19 | 🟢 | `CMakeLists.txt:45` | 注释仍写 "the same window as the C# DLSSNRFilter"，但 C# 版已删除 |
| A20 | 🟢 | `ngx_runtime/ngx_cuda.h` ≡ `dlss_layer/ngx_cuda.h` | 内容完全相同的两份文件，CMake 只用前者 → 后者是死副本，改错文件不会有任何报错 |
| A21 | 🟢 | `CMakeLists.txt:24-38` | 僵尸 target `dlss5-image-enhancer`：每次全量编译、永不进发布包、代码路径与 `dlssnr_gui` 分叉（进程内 vs 子进程），参数漂移无人知晓 |

---

# B. Qt GUI（`gui/batch_window.cpp` 等）

| # | 级别 | 位置 | 问题 |
|---|---|---|---|
| B1 | 🟠 ✅ | `:816-817` | `reset_modes[reset_->currentIndex()]` 无越界保护，ini 被改/旧版残留时 `currentIndex()` 为 -1 → 野读崩溃。同函数 `codec_`(:858)、`upscale_`(:870) 都做了边界检查，唯独漏这个 |
| B2 | 🟠 ✅ | `:652` / `:654` vs `load_settings` | 存了 `model`、`imagePasses`，`load_settings` 里**没读** → 重启后 DLSS 模型预设与单图处理次数静默回到默认 |
| B3 | 🟠 | `:993-997` / `:1063-1067` | 预览 / 定帧对比未重置 `chunk0_max_` / `chunk1_max_`（`start_run():911-916` 有重置）→ 跑过一次完整视频后再点预览，进度条开局就是 100% |
| B4 | 🟠 | `compare_view.cpp:114-133` | `frame_of` 在 GUI 线程同步 `waitForFinished(60000)`，比图模式下前后两个视频各一次 → 最多冻结 120 秒。同文件其他地方用的是 5s/2s 超时 |
| B5 | 🟠 | `:331` + `video_filter.cpp:735-740` | 默认 `shadow_protect_=50` 使 composite 恒激活 → 永久绕过零拷贝快路径（与发布说明"GPU 硬件格式转换"的宣传矛盾），且"恢复默认"也回不到中性值 |
| B6 | 🟡 | `:806` / `:813` / `:1112` vs `:774` | `validate()` 用 `.trimmed()`，`arguments()` / `probe_total()` 用原始值 → 粘贴带空格的路径能点开始但立刻失败，且 ffprobe 失败使进度条恒 0% |
| B7 | 🟡 | `:938` / `:1113` | `probe_total()` 在 `process_.start()` **之后**阻塞主线程 5 秒 → 预编译阶段的进度在这 5 秒内完全不显示 |
| B8 | 🟡 | `:184-186` | `auto_output_` 从不持久化 → 上次手填的输出路径，下次改输入就被静默覆盖成 `<输入名>_dlss.mp4` |
| B9 | 🟡 | `:728-752` | `reset_effects()` 漏重置 `crf_` / `parallel_` / `fps_` / `max_frames_` / `dump_` / `dump_dir_` / `audio_`（却重置了同组的 `codec_`） |
| B10 | 🟡 | `:1038` / `:1043` | 定帧临时文件写到程序目录（`frame_hold_out.png` / `frame_hold_in.png`）且从不清理 → 装在 Program Files 时不可写，功能直接不可用 |
| B11 | 🟡 | `:93-95` | `settings_path()` 把 ini 写在 exe 同目录 → 只读目录时设置静默全部丢失（老工具用的是 `QSettings("组织名","应用名")`，走注册表/AppData） |
| B12 | 🟡 | `:1246-1253` | `on_second_tick()` 每秒把"帧 N · 本帧 x ms · 平均 y ms"覆盖成"处理中… 已用 N 秒"，逐帧状态基本看不到 |
| B13 | 🟡 | `:135` | 只连了 `readyReadStandardError`，stdout 从不读 → 后端若往 stdout 大量输出，子进程会阻塞在写管道上 |
| B14 | 🟡 | `:754-763` | `apply_composite_preset()` 有未说明的副作用：除 4 个滑条外还强制 `local_tone_=0`、`style_=2`、`skin_structure_=10`、`auto_mask_=true`。除"原生电影"外其余预设的 tooltip 都没写，用户点了会丢掉已调好的参数 |
| B15 | 🟡 | `:879-881` | `style_` / `preset_` / `model_` 的 `currentIndex()` 也未钳制 → 可能发出 `--style -1` / `--preset -1` |
| B16 | 🟢 | `:959-962` | 输出路径无扩展名时预览名变成 `xxx_preview3s.`（结尾带点） |
| B17 | 🟢 | `:1093-1099` | `stop_run()` 只 kill 直接子进程；`video_filter.exe` 会派生 ffmpeg、分片子进程、重试子进程，仅靠 `video_filter.cpp:2107` 的 job object 兜底 |
| B18 | 🟢 | `:971` / `:1046` / `:1106` | 对 `ffprobe` / `ffmpeg` 只依赖 PATH、无存在性检查，失败要等 5 秒超时后才给提示 |
| B19 | 🟢 | `:774` / `:787` | `validate()` 只查存在性，不查是不是文件（填目录也能过，然后后端 LoadLibrary 失败） |
| B20 | 🟢 | `:1153-1160` | 并行分片进度推算硬编码二分（`mid = frames_total_/2`），而 `concurrency` 在 auto 模式下可能不是 2；`--parallel off` 时也按二分算。目前数学上巧合正确，逻辑脆弱 |
| B21 | ⚠️待验证 | `:355` / `:120-121` / `:604,839,991` | 疑似缺 `#include <QGridLayout>`、`<QGuiApplication>`、`<cmath>`（用了 `QGridLayout`、`QGuiApplication::primaryScreen()`、`std::round`/`std::abs`）。**若属实则编译不过，属致命**。建议先跑一次编译确认 |

## 老工具 `gui/main_window.cpp`

| # | 级别 | 位置 | 问题 |
|---|---|---|---|
| B22 | 🟠 | `:196-215` | 自测路径：`process()` 返回 false 但 `output` 非空时会把**全黑图写成 PNG 并返回 0**（与 D9 联动，属假成功） |
| B23 | 🟢 | `main_window.h:144` | `showing_output_` 只写不读，死状态 |
| B24 | 🟢 | `:169` | `run_self_test` 要求 `argc>=6`，与 usage 声明的 `[runtime] [nvapi]` 可选矛盾 |
| B25 | 🟢 | `:739` / `:423` | `set_busy(true)` 不禁用"Open image" → 处理中换图会让 `input_` 与 `on_finished` 显示的结果错位 |

---

# C. 后端 `tools/video_filter.cpp`（2847 行）

| # | 级别 | 位置 | 问题 |
|---|---|---|---|
| C1 | 🔴 | `:2633-2642`（关闭在 `:2638-2639`）对比阻塞点 `:2406` / `:2489` | abort 时先 `CloseHandle` 再 join 线程，而 decode 线程极可能仍阻塞在 `ReadFile`、encode 线程仍阻塞在 `WriteFile`（4K 单帧 49.7MB 远超管道缓冲，写阻塞是常态）。MSDN 明确禁止关闭有未决同步 I/O 的句柄；更危险的是 Windows 会**立即复用句柄数值**，任何一次 `CreatePipe` 都可能拿到刚释放的值。正确顺序：先杀子进程 / `CancelSynchronousIo` → join → close |
| C2 | 🟠 | `:2114` / `:2679` / `:2777-2820` | 退出码 2 语义冲突：`parse_args` 失败（参数拼错）也返回 2，外层无法区分"用法错"与"ZLUDA 空白竞态" → 会连续派生 5 个子进程，每个都再打印一遍 usage，并重复完整预编译 |
| C3 | 🟠 | `:2231` / `:2237` / `:2258-2262` | 早期返回路径泄漏 ffmpeg 子进程与管道：`:2231` 未关 `decoder_stdout` 且未终止已启动的 decoder；`:2237` 只关 `encoder_stdin`；`:2258-2262` 两个 Pipe 句柄均未关。根因：`Pipe`（`:105-129`）**没有析构函数**，`close()` 必须手工调用 |
| C4 | 🟠 | `:2172-2178` + `:477-500` | `[FAIL] decoder: %s` 打印的 error 恒为空（`start_decoder` 根本没有 error 参数）→ 用户只看到 `[FAIL] decoder: `，无法区分是 ffmpeg 不在 PATH 还是路径错误 |
| C5 | 🟠 | `:1892-1901` / `:1984` | 分片编排器 `concurrency` 形参**只用于打印**，实际永远只切 2 片（`split_sec = total_dur / 2.0`）；`WaitForMultipleObjects(..., INFINITE)` 无超时，任一 worker 挂死则父进程与 GUI 永久卡住 |
| C6 | 🟠 | `:2708-2756` vs `:2777-2820` | `--precompile-wait` 在 `run_main_once` **之前**执行，而重试是重跑整个 `real_main` → 默认 5 次重试会跑最多 6 次完整串行预热 |
| C7 | 🟠 | `:2807-2814` | 重试子进程 `bInheritHandles = FALSE` 且 `si` 未设 `STARTF_USESTDHANDLES`（`:2727-2732` 的预热子进程用的是 `can_inherit=TRUE`，两处不一致）→ **第 2 次及以后 GUI 完全收不到 stderr，日志空白、进度条停在 0%，看起来像卡死** |
| C8 | 🟠 | `:2385-2386` | `failed` / `retryable` 是普通 bool，被 decode 线程(:2420)、encode 线程(:2494)、主线程(:2587, :2599) 并发写（同文件 `reset_count`/`blank_count`/`elapsed_total` 都用了 `std::atomic`）→ 形式上是 data race，可能丢失失败标记、以退出码 0 报告一次实际失败的运行 |
| C9 | 🟡 | `:496-497` / `:609-613` | `--ffmpeg-threads` 位置错：解码器放在 `-i` **之后**（属输出组 rawvideo muxer，无编码器）、编码器夹在两个 `-i` **之间**（属第二个输入组）→ 都限制不到目标线程，`hardware_budget.h:127-129` 的限流意图完全落空 |
| C10 | 🟡 | `:2302-2303` + `:601-603` | `settings.output_width/height` 被设为 `model_w/model_h`（渲染分辨率），而 `perf_quality` 在 `:460` / `:792` 硬编码为 2 → `--upscale-mode quality/balanced/performance/ultra` 对网络零影响，实际放大由编码器的 `-vf scale=...:flags=lanczos` 完成，即普通重采样。**用户期望的 DLSS 超分并未发生，且无任何提示** |
| C11 | 🟡 | `:1761-1787` | 单图模式 13 个参数未实现 → `unknown option` → 触发 C2 重跑风暴：`--upscale-mode`、`--model-scale`、`--flow`、`--cut-threshold`、`--crf`、`--fps`、`--no-audio`、`--max-frames`、`--parallel`、`--yield-ms`、`--dump-frames`、`--ffmpeg-threads`、`--child-chunk`、`--frame-index-offset`。GUI 只在视频分支传这些，暂时没踩雷；CLI 用户一踩就炸 |
| C12 | 🟡 | `:2331` | 零拷贝拿到 `mapped_ptr` 后 `raw_bytes` 永不再用，却仍占 `3 × frame_bytes`（4K 约 150MB，8K 约 600MB）。应在确认后 `clear(); shrink_to_fit();` |
| C13 | 🟡 | `:1672` | `GpuFlow::compute` 不检查 `Map` 的 HRESULT，失败时 `p` 保持 `nullptr` 就 `memcpy` → 空指针写。同函数 `:1709` 的 `readback->Map` 反而检查了 `FAILED`，前后不一致 |
| C14 | 🟡 | `:2549-2565` / `:2547-2548` | GPU 光流失败（`generated==false`）时，`in_frame->motion` 保留上一次使用该池化缓冲时的**陈旧光流**并被送进网络做时间重投影，应在 else 分支显式清零。另外 `gpu_motion` / `gpu_motion_pitch` 声明后**从未赋值** → `GpuFlow::gpu_motion()`/`gpu_motion_pitch()` 是死代码，GPU 算出的 `full_out` 从未被使用，光流仍绕回 CPU 侧 |
| C15 | 🟡 | `:2126-2137` | `model_w/model_h` 未强制偶数（仅 `model_scale<1` 时 `& ~1u`），而 `output_w/output_h` 强制了 → `--model-scale 1.0`（默认）时奇数宽度直接进 `-s WxH` + `yuv420p`；且 `output_w` 改成偶数后与 `model_w` 不一致，编码器会插入一次 1 像素的意外缩放 |
| C16 | 🟡 | `:2655-2664` | `decoder_code` 取回后从不检查（只检查 `encoder_code`）。`--max-frames` 提前 `TerminateProcess` 让退出码变 0 掩盖问题，真正的解码错误（15s 超时后 code 保持 1）完全静默 |
| C17 | 🟡 | `:1945-1946` | 分片 `frame_index_offset` 用 `round(split_sec * fps)` 估算，与 ffmpeg `-t` 决定的实际帧数常差 ±1；warmup 丢弃帧数也用估算 → 拼接处可能**丢 1 帧或重复 1 帧**，进度计数与母带对不齐 |
| C18 | 🟡 | `:1754` / `:1768` | 单图模式 `--retries` 解析后**全文无引用**（死变量）→ 用户传 3 实际走外层 `real_main:2765` 的默认 5 |
| C19 | 🟢 | `:1177` vs `:1182-1183` | `if (argc < 7)` 使文档里"可选"的 `[runtime]` / `[nvapi]` 默认值不可达（`argc>5` / `argc>6` 两个分支是死代码） |
| C20 | 🟢 | `:1204-1210` / `:1271-1284` | `--cut-threshold`、`--crf`、`--max-frames`、`--encoder` 无任何范围校验：`--max-frames -1` 让 `:2393` 的 `(int)0 >= -1` 立即成立 → 0 帧输出；`--crf -5` 直接喂 ffmpeg |
| C21 | 🟢 | `:1214-1220` | `--upscale-mode` 传入非法值静默回退 1.0，不报错、不提示 |
| C22 | 🟢 | `:1461` / `:1465` | `CpuFlow::upscale4`：`if (iy >= (int)qh - 1) iy = qh - 2;` 当 `qh <= 1`（即 `model_h <= 4`）时 `iy = -1` → `f[(size_t)-1 * qw + ix]` 巨量越界读。`qw<=1` 同理（理论可触发） |
| C23 | 🟢 | `:2269` / `:2467` / `:2522` | `CoInitializeEx` 在主线程(:2269)调用后从不 `CoUninitialize`；encode 线程(:2467)单独初始化又在 `:2522` 反初始化，与主线程的 MTA 引用计数不匹配。另外 `wic`(:2265 创建) 被 encode 线程跨线程使用（WIC 非自由线程对象）——**存疑**，未确认实际负载下是否出问题 |
| C24 | 🟢 | `:1777` vs `:2247` | 单图模式 `--dlss-model-preset` 未过滤 `"default"`/`"默认"`（视频模式有过滤）→ 会原样写进 `DLSS_PRESET` 环境变量，两处行为不一致（`dlss_cuda.cpp:1096` 会 atoi 成 0，实际无害但语义不统一） |
| C25 | 🟢 | `:405` / `:453` | `spawn(command, Pipe{}, pipe, child, true)` 把临时对象绑定到**非 const 左值引用** `Pipe &feed`。这是 MSVC 的非标准扩展，在 `/permissive-` 或 clang-cl 下会编译失败 |
| C26 | 🟢 | `:967` / `:2527` / `:2655` | 死代码：`resize_rgb48()` 定义后从未调用、`eval_index` 只写不读、`decoder_code` 只写不读 |
| C27 | 🟢 | `:2027-2050` | concat 列表以 **UTF-8 无 BOM** 写出。Windows 版 ffmpeg 对 `-f concat` 路径编码的处理随版本而异，含中文等非 ASCII 路径时可能拼合失败。**存疑**，未实机验证 |
| C28 | 🟢 | `:1126` vs `:2128-2133` | `Options::model_scale` 的注释写 "compatibility; legacy GUI flag is ignored"，但代码**确实使用**了它（缩放 `model_w/h` 并触发解码端 `-vf scale`）→ 注释与实现矛盾，易误导维护 |
| C29 | 🟢 | `:2501` | `swprintf(name, 512, L"%s\\frame_%05u.png", ...)`：dump 目录带尾部分隔符会生成 `dir\\frame_00000.png`；另外并行分片下两个 worker 会写同一目录同名文件互相覆盖（`:1917-1942` 未改写 `--dump-frames`） |

---

# D. 核心层（`image_processor` / `precompile` / `dlss_cuda` / `ngx_runtime`）

| # | 级别 | 位置 | 问题 |
|---|---|---|---|
| D1 | 🟠 | `core/precompile.cpp:234-270` | 不处理 `WaitForMultipleObjects` 的 `WAIT_FAILED`。触发条件：`DLSSNR_PRECOMPILE_JOBS` 被设成 >64（`:146-149` 直接 `atoi` 赋给 `ceiling`，无上限校验）或任何让等待失败的情形。后果三重：① 所有仍在跑的子进程被遗弃（句柄从不 `CloseHandle`）② 紧接着 `:289` `DeleteFileW` 删掉所有 `module_*.bin`，子进程正在读的文件被抽走 → 返回 3 ③ `failures` 很可能仍是 0 → `:292-297` 返回 `true`，调用方 `image_processor.cpp:342-350` 当"非致命"只记一行 log，**整个预编译被静默跳过**，网络退化成运行时串行翻译（几十分钟） |
| D2 | 🟠 | `core/precompile.cpp:229` | `if (running.empty()) break;` 在 `CreateProcessW` 连续失败时触发（此时 `next < files.size()`），同样跳过后续 `CloseHandle` 与等待，直接落到 `:289` 删文件 → 句柄与进程双双泄漏 |
| D3 | 🟠 | `core/image_processor.cpp:223-227` vs `:232-267` | `start()` 失败后重试时 D3D12 对象全套泄漏：`if (s->attempted)` 只做 `dlss_cuda::shutdown()` + `release_images()`，而 `release_images()`(:190-199) 不碰 `device`/`queue`/`allocator`/`cmd`/`fence`/`fence_event`；这些成员随后被 `IID_PPV_ARGS(&s->device)`(:243) 等直接覆盖。触发：第一次 start 失败后用户再点一次 Enhance。每次泄漏一整套 D3D12 对象 + 一个事件句柄 |
| D4 | ⚠️**已更正** | `dlss_layer/frame_blit.cpp:179-181` | **【第四轮更正：此项现已不成立】** 当前代码是 `if (g.ready && g.device == device) return true; if (g.ready) shutdown();` —— device 身份**已校验**，且设备变化时会**自动 shutdown() 重建**，原"跨设备提交 PSO"问题已消除；`shutdown()` 虽仍无外部调用点，但由 `init()` 内部在 `:181` 调用已足够（详见 K0）。**残余边缘风险**：若新建的 device 恰好复用了旧 device 的指针地址，`g.device == device` 会误判为同一设备而跳过重建。 ~~原描述：init() 里 `if (g.ready) return true;` 不校验 device 身份；shutdown() 全仓库无调用点~~（grep 只找到 `dlss_cuda.cpp:1329`/`1512` 两处 `init`，`dlss_cuda.cpp:1586-1611` 的 `shutdown()` 也不调 `frame_blit::shutdown()`）。触发链：`Processor::stop()`(:1000-1012) `Release()` device → `start()` 新建 device → `frame_blit::init(newDevice)` 因 `g.ready==true` 直接返回 → `g.device`/root signature/PSO/两个 descriptor heap 全指向**旧设备**，却把 dlss_cuda 用**新设备**创建的 `g.cmd` 传进去（`:1517,:1522`）→ 跨设备提交命令，`E_INVALIDARG` / device removed。**GUI 的"空白结果重启会话"路径正好是这个序列** |
| D5 | 🟠 | `core/image_processor.cpp:397-400` / `:491-500` | `process()` 只查 `in.empty()`，不校验 `in.pixels.size()` 与 `width*height*4` 是否一致，直接 `memcpy(mapped, source, row_bytes * in.height)`（row_bytes = width*8）。motion(`:567-575`) 同理（`:560-561` 只比宽高）。当前 in-tree 调用方都恰好一致，属"随时会炸的缺失防御" |
| D6 | 🟠 | `core/image_processor.cpp:143` | 空白检测 `if (in.empty() \|\| out.empty() \|\| in.pixels.size() != out.pixels.size()) return false;` → 一旦输出尺寸与输入不同（即放大/缩小，`Settings::output_width/height` 非 0，见 `:403-404`），该条件恒成立，函数直接返回 false（="不像空白"）。于是 `process()` 末尾唯一一道防"ZLUDA 空 launch 返回全黑"的闸门(`:726-729`) 在**所有需要放大的路径上失效**，黑帧被当成功结果返回 |
| D7 | 🟠 | `core/image_processor.cpp:461` / `:655-657` + `dlss_cuda.cpp:1045,1157-1168` | 静图（非 is_video）`passes>1` 时：`feature.max_passes = 2` → 真的调了第二次 `CreateFeature`（多建一整个 DLSSNR feature，第二个 transformer 网络的全部 workspace，通常几百 MB），但循环里 `frame.pass_index = 0` **恒定** → `dlss_cuda.cpp:1238-1239` 永远选中 `features[0]`。同时 `s->intermediate`(:429-431，输出尺寸的 RGBA16F UAV) 也一并创建却从不使用。在显存吃紧的 ZLUDA/ROCm 环境下直接压缩可用显存，把 `CreateFeature` 推向 generic failure |
| D8 | 🟡 | `core/image_processor.cpp:1000-1001` | `stop()` 只在 `if (s->started)` 时调 `dlss_cuda::shutdown()`。若 `start()` 在 `attempted=true` 之后、`s->started=true`(:386) 之前失败（如 `dlss_cuda::init` 在 `ngx_populate`/`CreateFence` 失败，`:864-883`），此时 `nvapi64.dll`/`nvcuda.dll`/`nvngx.dll` 都已 LoadLibrary、CUDA context 已建、NGX Init_Ext 已成功 → 三个 `FreeLibrary` 和 `cuCtxDestroy` 全部被跳过，残留到进程结束 |
| D9 | 🟡 | `core/image_processor.cpp:670-672` / `:711-713` / `:726-729` | `out` 在 `:670-672` 先无条件 resize（value-init 为 0）。于是：readback 失败 → 返回 false 但 `out` 已是"尺寸正确、内容全 0"的**非空**图像；判为空白 → 返回 false 但 `out` 是真实全黑图。而 `:393-400` 的最前置早退完全不动 `out`，复用同一 `out` 会拿到上次成功的结果。调用方 `gui/main_window.cpp:196-215` 靠 `output.empty()` 判失败 → **把全黑图写成 PNG 并返回 0**（见 B22） |
| D10 | 🟡 | `dlss_layer/dlss_cuda.cpp:1540-1568` | `read_shared_output` 直接 `cuMemcpy2D`，缺 `cuCtxSetCurrent(g.ctx)`；同文件 `upload_shared_colour`(:1473-1474) 与 `evaluate_ngx`(:1236) 都显式调用。`debug_read_shared_colour`(:1570-1584) 同样缺失 |
| D11 | 🟡 | `dlss_layer/dlss_cuda.cpp:1046-1052` | `create_feature()` 重用判据不完整：① `passes` 从 2 降到 1 时 `wanted_passes=1`、`g.num_features=2 >= 1` → 命中重用，`features[1]` 及其 workspace **永不释放** ② `desc.feature` 与 `desc.neural.*` 完全不参与比较 ③ `g.current = desc` 只在 `:1170`（全部成功后）赋值，`:1067-1076` 的 `make_shared` 任一步失败会 return false，此时 features 已全部释放而 `g.current` 仍是旧值 → 后续 `evaluate()` 报 "evaluate called before create_feature"，`evaluate_ngx:1247` 又拿过期的 `g.current` 填帧参数 |
| D12 | 🟡 | `dlss_layer/dlss_cuda.cpp:23, 27, 128` | 整个 DLSS 层是全局变量单例，无线程安全：`g_error[512]`(:23) 由 `set_error` 写 / `last_error()` 读；`g_reshade_log`(:27) 由 `set_log_sink` 在 GUI 线程写、由 NGX 回调在**工作线程**读；`State g`(:128)。目前只有一个 worker 侥幸没冲突，出现第二个 `Processor` 实例会互相覆盖 |
| D13 | 🟡 | `gui/main_window.cpp:266-274` | 日志回调 `g_window`（`:266` 全局裸指针）：工作线程读 `g_window` 后 `QMetaObject::invokeMethod(g_window, ...)`，而主线程在构造末尾(:340-341)设置、析构开头(:344-346)清空，无任何同步 → 数据竞争；析构后置 nullptr 之后，工作线程若已在 `if (!g_window)` 判断之后被抢占，会对正在析构的窗口投递事件。注释(:264-265)声称"窗口比层活得久"，但 `:348-363` 的 `wait(2000)` / `std::_Exit(0)` 与之矛盾 |
| D14 | 🟡 | `ngx_runtime/ngx_runtime.cpp:231-260` | `ngxrt_load` 半成功（DLL 能加载但缺某个导出，如拿错版本的 `nvngx_dlssnr.dll`）时：`g_snippet` 已非 null，`:250` return 错误字符串后 `dlss_cuda::init`(:822) 返回 false；但重试时 `:231-232` `if (g_snippet) return nullptr`（成功），而 `s_create` 等仍是 null → 后续 `ngxrt_init` 返回 Fail，错误信息变成莫名其妙的 "NGX...failed: 0x..."，**真正的原因（缺导出）再也不会被报出来**。且 DLL 从不 `FreeLibrary` |
| D15 | 🟡 | `core/image_processor.cpp:736-997` | `process_raw_rgb48()` 到 `:994-996` 直接 `return true`，**没有任何空白检测**，也没有 `last_ms` 之外的失败信号。而 `process()` 是有 `looks_like_blank_result`(:726-729) 的。`process_raw_rgb48` 正是**视频主路径**（`tools/video_filter.cpp:2571`），而"空白竞态"恰恰是视频场景的已知问题（该文件 `:1823-1826` 注释）→ 核心层的安全网漏在了最需要它的路径上 |
| D16 | 🟡 | `gui/half_float.h:28` | `float_to_half` 里 `if (exponent >= 0x1F) return sign \| 0x7C00u;`：NaN 的指数字段也是 0xFF → 落到这一支，尾数被丢弃，输出 `0x7C00`（= +Inf）。而 `half_to_float`(:48-49) 保留了 `mantissa<<13`，NaN 进 NaN 出 → **两侧不对称**。后果：`linear_to_srgb(half_to_float(...))` 拿到 Inf 被 clamp 成 1.0（NaN 像素显示为纯白），绕过 clamp 的路径则是未定义值。附带：全程截断无 round-to-nearest，`exponent < -10` 一律归零（本应舍入到 2^-24） |
| D17 | 🟢 | `core/precompile.cpp:104-130` | `compile_one` 子进程里：`LoadLibraryW`(:105) 后无 `FreeLibrary`；`cuCtxCreate` 成功后 `return 5`/`return 6` 两条路径都不 `cuCtxDestroy`；`cuModuleLoadData` 成功也不 `cuModuleUnload`。子进程随即退出所以实际影响小，但 `:172-173` 注释提过"改成线程内调用"的方案，那时就是真泄漏 |
| D18 | 🟢 | `core/precompile.cpp:56-62` | 临时目录名固定 `GetTempPathW + L"dlss5-precompile"`，无 PID / 无唯一后缀 → 两个实例同时启动会共用同一批 `module_%03zu.bin` 文件名；A 在 `:289` 删文件 / `:290` 删目录时 B 的子进程可能正在读 |
| D19 | 🟢 | `core/precompile.cpp:192-200` | 排序比较器里 `GetFileAttributesExW` 每个比较调两次（磁盘 I/O），中途瞬时失败（杀软/lock）返回 0 会破坏严格弱序 → `std::sort` 的 UB 风险。另外 `[&modules, &files]` 捕获了 `modules` 却完全没用（死捕获） |
| D20 | 🟢 | `core/precompile.cpp:212-213` / `:50-54` | 子进程命令行手工拼接，对 `self`/`files[next]`/`driver` 只加外层引号，路径内含 `"` 会解析错；`own_path()` 用 `MAX_PATH` 固定缓冲，路径超长被静默截断 → `CreateProcessW` 失败并计入 `failures` |
| D21 | 🟢 | `core/image_processor.cpp:356` / `:307-308` | `SetEnvironmentVariableW(L"ZLUDA_NVAPI_GPU_ARCH", L"0x1B0")` 在 precompile(`:340-351`) **之后**才执行 → 预编译子进程拿不到这个变量；另外 `SetDllDirectoryW`(:307-308) 全仓库**从不还原**，进程级 DLL 搜索路径被永久改写 |
| D22 | 🟢 | `dlss_layer/dlss_cuda.cpp:1194` / `:514` | static 局部变量导致诊断只跑一次：`static bool reported = false;`（feature 因尺寸变化被重建后，再也不验证新输出纹理里有没有像素——而这条日志正是判断网络是否真跑了的关键手段）；`static int kind = -1;` 同理 |
| D23 | 🟢 | `dlss_layer/dlss_cuda.cpp:569-574` | `flush_and_wait` 10s `WAIT_TIMEOUT` 后直接 return false，没有等待/重置 `g.fence_event` → 下一次 `SetEventOnCompletion` 前若误用 `WaitForSingleObject` 可能立刻被上次遗留的信号唤醒 |
| D24 | 🟢 | `ngx_runtime/ngx_runtime.cpp:104` / `:158-162` | `*o = (unsigned)(e->kind == Kind::F64 ? (unsigned long long)e->v.f64 : e->v.u64);`：负 double 或 > 2^32 的正 double 转 `unsigned long long` 是 UB；`strncpy(e.name, n, 95)` 会把超过 95 字符的参数名截断，造成前缀相同的长名互相覆盖 |
| D25 | 🟣 | `dlss_layer/ngx_cuda.h:232` | **`DLSSNR.ControlMask` 定义了，但全仓库没有任何地方 `Set`**。`settings.auto_mask` → `feature.neural.use_auto_mask` → `nr_param::UseAutoMask`(`dlss_cuda.cpp:1012`) 这条链是通的，但真正的掩码资源从头到尾没给。若 snippet 在 `UseAutoMask=1` 时去 `Get("DLSSNR.ControlMask")` 拿不到，就会走默认分支——这与 `dlss_cuda.cpp:966-967` 注释担心的"absent value 与 rejected value 无法区分"完全一样，**可能是"同一参数有时好有时平"那个非确定性问题的来源**。需确认 snippet 是否真读这个键 |
| D26 | 🟣 | `dlss_layer/ngx_cuda.h:183` | `ngx_param::EnableOutputSubrects` 定义了却从不 Set——代码在 `:972-993` 设了一堆 `DLSSNR.*Subrect*`，却没有任何地方打开 subrect 开关 |
| D27 | 🟣 | `dlss_layer/dlss_cuda.cpp:1409` | `CopyResource(g.color.resource, frame.color)` 的源纹理 `frame.color` 在 `image_processor.cpp:529` 被转回 `COMMON` 态、从不转 `COPY_SOURCE`。D3D12 对默认堆无标志纹理有隐式提升，实践通常不报错，但 debug layer 会告警；ZLUDA/AMD 下是否有实际影响未知 |
| D28 | 🟣 | `dlss_layer/dlss_cuda.cpp:1593-1597` | `shutdown()` 调 `release_shared()`(:383-391) 时没有先 `cuCtxSetCurrent(g.ctx)`，依赖"上下文仍然是当前的"。若某次 evaluate 后上下文被换过，这些 destroy 会静默失败 |
| D29 | 🟣 | `core/image_processor.cpp:627` / `:761` | `process_raw_rgb48` **不创建 `s->upload`**。若同一 `Processor` 实例混用两个接口，`:761` 的重建条件不会触发（`s->width/height` 相同），`s->upload` 一直是 null → `process()` 的 `:475-502` 分支会在 `s->upload->Map` 空指针解引用。当前未找到混用的调用方，属潜在风险 |

---

# 已确认**不是** bug（避免重复排查）

- `arguments()` 与 `video_filter` 的参数名/顺序/取值**完全对齐**：无拼写错误、无漏传、无后端不识别的参数。下拉框索引与字符串表一一对应（`reset_modes[]`/`codecs[]`/`model_scales[]`/`upscale_modes[]`）。
- 单图模式公共尾部参数确实生效（`--no-auto-mask`/`--gamma`/`--dlss-model-preset`/`--style`/`--preset` 等被 `run_image_mode` 白名单接受）。
- `frame_re_` 正则与实际输出格式匹配（`:2512` 的 `[%.5u] %.0f ms (avg %.1f, reset=%d, blanks=%d)`）；precompile 正则与 `precompile.cpp:282` 也匹配。
- upscale 模式下输出缓冲**不会**越界：`settings.output_width/height = model_w/model_h`，`out_f->model_output`(`:2368`) 与 `half_rgba_to_rgb48`(`:916-920`) 写入量严格相等。
- `--gamma` 无重复应用：`half_to_rgb48_lut()`(:712-721) 只被 encode 线程调用，`dump_png`(:1056-1058) 走 `clamp_half_to_u16`，两者各应用一次 `to_sdr`。
- `CpuFlow::match` 的 `initQ`/`fE` 索引已按 1/8 网格 `cw8×ch8` 修正，与 `median3(fE, (ew+3)/4, (eh+3)/4)` 一致，无越界。
- `WorkerPool::parallel_for` 的 `remaining` 无竞争窗口（`store` 在临界区内、早于 `notify_all`）。
- `build_smoke.bat` 链接依赖够：`d3d12.lib`/`dxgi.lib`/`d3dcompiler.lib`/`wintrust.lib` 由各文件 `#pragma comment(lib)` 提供；`NOMINMAX` 缺失也不是问题（`image_processor.cpp:150` 用了 `(std::max)(...)` 括号形式）。
- AUTOMOC 配置正确（`CMakeLists.txt:19` 全局开启，video_filter / nvngx 显式关闭），`Q_OBJECT` 不会链接失败。
- 数值格式无区域问题：`QString::number(value, 'f', 2)` 强制 C 风格小数点。

---

# 需实机才能定论的 3 条

1. **B21**：`gui/batch_window.cpp` 是否真的缺 `#include <QGridLayout>` / `<QGuiApplication>` / `<cmath>`。若属实，`dlssnr_gui` 目标直接编译不过（致命）。**先跑一次 `cmake --build` 确认。**
2. **C10**：`--upscale-mode` 的正确语义。当前 DLSS 完全没参与超分（只是末尾 ffmpeg lanczos），而 GUI tooltip 与发布说明都宣称"DLSS 直接输出更高分辨率"。要么接真超分（改 `settings.output_width/height` 与 `perf_quality`，需实机验证），要么改文案避免误导。
3. **D25**：`DLSSNR.ControlMask` 是否被 snippet 真正读取。这决定了自动遮罩功能是否真的完整，以及是否是"时好时平"非确定性的根因。

---

---

# 第二轮补充审查（E / F / G 段）

审查对象：随包发布的诊断脚本、`tools/` 工具、仓库文档。
共新增 **24 条**（E1-E8、F1-F9、G1-G9，其中 G9 为排除项）。

## E. 诊断脚本（随包发布，用户会直接双击运行）—— 优先级最高

| # | 级别 | 位置 | 问题 |
|---|---|---|---|
| E1 | 🔴 | `diagnose_gpu.ps1:28` / `:115-140` | **`LoadLibrary` 同名冲突，导致 `[4]` 段的测试结果是假的。** Windows 加载器按模块**基名**匹配已加载模块：脚本在 `[3]` 先加载 `System32\amdhip64_7.dll`，随后 `[4]` 试图加载 `HIP_PATH\bin\amdhip64_7.dll` —— **两者基名完全相同**，Windows 直接返回已加载的 System32 模块句柄，**根本不会去加载 HIP_PATH 那份**。因此截图中"[4] HIP_PATH ... count=0"实际测的还是 System32 那份，**"两套 HIP 都看不到设备"这个结论不成立**。雪上加霜的是 `:12-13` 导入了 `FreeLibrary` 却**从未调用**，模块常驻进程，让后续所有测试都落在同一个已加载模块上。**修法**：每份 DLL 放到**独立子进程**里测，或先 `FreeLibrary` 再用 `LoadLibraryEx` 配合 `LOAD_LIBRARY_SEARCH_*`。 |
| E2 | 🔴 | `diagnose_gpu.ps1:164-175` | **`[6]` 段的 override 测试完全无效。** HIP 运行时在**首次调用时初始化并缓存设备列表**，而 `HSA_OVERRIDE_GFX_VERSION` 是在 `:168` 才用 `SetEnvironmentVariable(..., "Process")` 设置的 —— 此时第一次 `hipGetDeviceCount`（`:120`）早已完成初始化。三次 override（12.0.0 / 12.0.1 / 11.0.0）**没有任何一次真正生效**，截图里那三行 `count=0` 是毫无意义的输出。**修法**：每个 override 也必须起新进程。 |
| E3 | 🟠 | `diagnose_gpu.ps1:115-175` | 只测 System32 与 HIP_PATH 两个固定位置，**从不检测程序实际会加载哪一份**。真实 DLL 搜索顺序是「应用目录 → System32 → PATH」。若程序目录（如 `G:\dlssnr\`）或 PATH 中还有第三份 `amdhip64*.dll`，脚本完全测不到。建议加一步：列出程序目录下的 `amdhip64*.dll` + `where amdhip64_7.dll`。 |
| E4 | 🟢 | `diagnose_gpu.ps1:116` / `:169-170` | 硬编码 `C:\Windows\System32`，Windows 装在其他盘符时失效。 |
| E5 | 🟠 | `fix_tdr.ps1:15` / `:48` | 读了 `$currentTdrLevel`（`:15`）却**从不使用**（死变量），然后无条件写 `TdrLevel=3`。若用户原本设的是 `TdrLevel=0`（完全关闭 TDR 检测，重负载计算的常见做法），脚本会把其**回退**为开启检测+恢复，而脚本既不显示原值、不给提示、也不备份。 |
| E6 | 🟡 | `fix_tdr.ps1:45-59` | 修改 HKLM 系统级注册表，**不创建还原点、不导出备份、不告知如何还原**（连默认值是什么都没说）。这是随包发给所有用户的高风险操作。 |
| E7 | 🟡 | `fix_tdr.ps1:39` | 提权写死 `powershell`（Windows PowerShell 5.1）。只装 PowerShell 7 的机器上 `powershell` 不存在 → 提权失败且无提示。 |
| E8 | 🟢 | `fix_tdr.ps1:48` | `TdrLevel=3`（Recover）本就是 Windows **默认值**，这行实际没改变任何行为；真正起作用的是 `TdrDelay=10`。发布说明 `:36` 与脚本输出可能让用户误以为改了三项。 |

## F. `hardware_budget.h` / `analyze_vopd.py`

| # | 级别 | 位置 | 问题 |
|---|---|---|---|
| F1 | 🟠 | `tools/hardware_budget.h:54` | `info.avail_vram_mb = vram_mb;` 把**总显存**当作可用显存回退。若 `QueryVideoMemoryInfo`（`:60`）失败，可用显存被**严重高估** → Gate 5（4K 且可用 <12GB 才降级单进程，`:111`）不会触发 → 在显存实际已占满的机器上仍启用双进程 → **爆显存 / device removed**。回退值应当保守（如取一半或 0）。 |
| F2 | 🟡 | `tools/hardware_budget.h:52` | 按"独显显存最大"选适配器。核显+独显、或 N 卡 + A 卡混装时，可能选中并非实际执行 DLSS 的那张卡。（`GameViewer` 这类虚拟适配器会被 `:47` 的 `DXGI_ADAPTER_FLAG_SOFTWARE` 过滤，无碍） |
| F3 | 🟢 | `tools/hardware_budget.h:119` | `safe = 2` 硬编码，与 `video_filter.cpp` 永远只切 2 片（C5）互相印证：`concurrency` 形参是死参数，今天一致但脆弱。 |
| F4 | 🟡 | `hardware_budget.h:128` + C9 | **叠加失效**：辛苦算出的 `ffmpeg_threads_per_worker` 经 C9（`--ffmpeg-threads` 放在 `-i` 之后、根本不生效）后**完全落空** → 发布说明宣称的"严格限制后台编码线程"实际没有生效。 |
| F5 | 🟠 | `tools/analyze_vopd.py:25` | 硬编码 `C:\Program Files\AMD\ROCm\7.1\bin\llvm-objdump.exe`。ROCm **7.2** 的机器上直接 "not found" 退出；且硬编码 C 盘。应从 `HIP_PATH` 推导或搜索版本。 |
| F6 | 🟠 | `tools/analyze_vopd.py:138` | `count / grand_vopd_ops`：若 `grand_vopd_ops == 0`（RDNA2 / 无 VOPD 指令 / 缓存为空）→ **ZeroDivisionError 崩溃**。`:106` 有三元保护，`:138` 漏了。 |
| F7 | 🟡 | `tools/analyze_vopd.py:32` | `SELECT ... FROM modules` 无异常处理，ZLUDA 版本变更导致表/列名不符时直接 `sqlite3.OperationalError` 崩溃。 |
| F8 | 🟡 | `tools/analyze_vopd.py:49-57` | `subprocess.run` 抛异常时 `finally` 只删临时文件，异常继续向上传播，无友好提示。 |
| F9 | 🟣 | `tools/analyze_vopd.py:86-92` | 指令解析假设 `addr: mnemonic` 格式（`tokens[0].endswith(':')` 时取 `tokens[1]`）。llvm-objdump 不同版本输出格式不同（有的为 `addr: 十六进制字节 mnemonic`）；若是后者，`tokens[1]` 会取到字节串而非助记符 → 全部指令分类与 VOPD 统计失真。需实测确认。 |

## G. 仓库文档 vs 实现（含一份未修复的实测问题报告）

| # | 级别 | 位置 | 问题 |
|---|---|---|---|
| G1 | 🔴 | `docs/issue_blank_race_report.md` | **仓库里躺着一份尚未修复的严重问题实测报告**（RX 7900 XT / gfx1100 / ROCm 7.1），记录了三个问题：① **全新进程 50-60% 产出全黑图**（进程内 sticky，实测 4/6、3/6 黑，黑图 RGB≈1-3/255 且值恒定）；② **ZLUDA ComputeCache 不持久化**，每次重翻译 15 个模块 40-90s；③ **翻译子进程会挂死**（3.5s CPU / 7+ 分钟），而 precompile 无限等待 → 整个 GUI 卡死。**更关键的是**：该文档 `:48-53` 记录的用户本地 workaround **没有合并进主仓库** —— "Watchdog（零 CPU 进度 3 分钟则杀掉）" vs 主仓库 `precompile.cpp` 仍是无超时等待（对应 D1/D2、C5）；"Concurrency cap 默认上限 4（原为自适应最高 16）" vs 主仓库 `precompile.cpp:146-149` 仍是 `atoi` **无上限** —— 这正是 D1 的 `>64` 触发条件。**建议把这份文档当作 D1/D2/C5/D6/D15 修复的验收标准。** |
| G2 | 🟡 | `发布包说明.txt:8` | "之后会直接调用本机编译缓存（秒级启动）" —— 与 G1② 直接矛盾。 |
| G3 | 🟡 | `发布包说明.txt:49` | "严格限制后台编码线程" —— 与 C9 / F4（限流实际落空）矛盾。 |
| G4 | 🟡 | `发布包说明.txt:30-32` | "视频输入管道直接映射显存零拷贝…GPU 计算着色器（<0.05 ms）" —— 与 B5（默认 `shadow_protect=50` 使 composite 恒激活、零拷贝被绕过）矛盾。 |
| G5 | 🟡 | `发布包说明.txt:48` | "时序预热（Warmup Overlap 30帧）…彻底根除视频拼接闪烁与跳帧" —— 与 C17（`frame_index_offset` 用 round 估算，接缝会丢 1 帧/重复 1 帧）矛盾。 |
| G6 | 🟡 | `发布包说明.txt:35` | "D3D12 命令队列加入 10 秒超时安全检测与 GetDeviceRemovedReason，杜绝死锁" —— 与 D23（10s `WAIT_TIMEOUT` 后 `g.fence_event` 保持触发态未清理）矛盾。 |
| G7 | 🟡 | `发布包说明.txt:38-39` | "RDNA 4 提速 20~30 倍，单帧推理降至 40~60ms" —— 与 README Q2 及实际诊断（RX 9070 XT 出现 `cuInit failed: 100`）矛盾，至少在部分机器上不成立。 |
| G8 | 🟢 | `batch_main.cpp:16-17` vs `batch_window.cpp:93-95` | `batch_main.cpp` 设置了 `setOrganizationName`/`setApplicationName`，但 `settings_path()` 用的是显式 ini 路径（exe 同目录）而非 `QSettings` 的组织/应用默认位置 → 印证 B11：作者本意是写注册表/AppData，实际退化成写程序目录。 |
| G9 | ✅ | 已核实**无**问题 | ① `gui/batch_main.cpp:12-13` 有 HighDpi `PassThrough` 策略（DPI 缩放这条盲区排除）；② `gui/main.cpp:68` 的 `--compile-one` 要求 4 个参数，与 `precompile.cpp:212-213` 拼出的命令行一致；③ `main.cpp:74` 传给 `run_self_test` 的 `arguments.mid(1)` 索引正确（B24 只涉及 `size()<6` 与 usage 声明不一致）。 |

---

---

# 第三轮补充审查（H / I 段）

审查对象：`core/gpu_detection.h`（**新发现的文件**，不在最初目录快照中）、`README.md` 全文。
新增 **17 条**（H1-H9、I1-I7，其中 I7 为排除项）。

## H. `core/gpu_detection.h` —— 与你朋友的 `cuInit failed: 100` 直接相关

**这个文件的作用**：枚举 DXGI 显卡，识别到 RDNA 4 就自动注入 `HSA_OVERRIDE_GFX_VERSION=12.0.1`、`AMD_DIRECT_DISPATCH=0`，并为 AMD 卡设置 `ZLUDA_NVAPI_GPU_ARCH=0x1B0`。

**已核实的调用点（共 4 处，时序均正确，无问题）**：
`tools/video_filter.cpp:2827`（`main()` 第一条语句）、`gui/batch_main.cpp:10`（`main()` 起始）、`core/image_processor.cpp:236`（`start()` 内，早于 precompile `:340` 与 `dlss_cuda::init`）、`core/precompile.cpp:106`（`compile_one` 子进程内）。

| # | 级别 | 位置 | 问题 |
|---|---|---|---|
| H1 | 🔴 | `:44-58` | **`EnumAdapters1` 循环存在空指针解引用与死循环风险。** `adapter` 在 `:44` 声明一次后复用，循环条件只判断 `!= DXGI_ERROR_NOT_FOUND`。若 `EnumAdapters1` 返回其他错误（如 `DXGI_ERROR_NOT_CURRENTLY_AVAILABLE`——**驱动更新中 / 远程会话下正是这个值**，你朋友机器上就有 `GameViewer` 虚拟显示适配器），循环体仍会执行 → `:47 adapter->GetDesc1()` 与 `:57 adapter->Release()` 作用于空/野指针 → 崩溃；且因永远等不到 `NOT_FOUND` 会**无限循环**。规范写法是每次迭代先 `adapter = nullptr` 并在 `FAILED(hr)` 时 break。 |
| H2 | 🔴 | `:66` | **GPU 枚举失败时静默返回，不注入任何环境变量、不打印任何日志。** `if (gpus.empty()) return;` —— 一旦 `CreateDXGIFactory1` 失败或枚举异常，整个自动配置被跳过，而用户完全看不到任何提示。这条与 H1 组合，构成一条**完整的、能解释你朋友现象的因果链**：DXGI 枚举异常 → `gpus` 为空 → 静默 return → **未注入 `HSA_OVERRIDE_GFX_VERSION`** → HIP 按错误/默认架构初始化 → `hipGetDeviceCount=0` → `cuInit=100`。 |
| H3 | 🟠 | `:101-105` | **RDNA 4 检测只认字符串 `"9070"`，漏掉同属 RDNA 4 的 RX 9060 / 9060 XT / 9070 GRE** —— 这些卡的 DXGI 名称不含 `"9070"`，因此不会被识别为 RDNA 4，也就**不会注入 `HSA_OVERRIDE_GFX_VERSION`**。同时 `:102-105` 的 `rdna 4` / `navi 4` / `gfx120` 三个分支是**死代码**，因为 DXGI `Description` 的实际内容是 `"AMD Radeon RX 9070 XT"` 这类型号名，永远不包含这些字样。建议改按 9000 系列数字规律或 device id 判断。 |
| H4 | 🟠 | `:112` | 对任何含 `"9070"` 的卡无脑注入 `12.0.1`（假定 Navi 48 / gfx1201），未区分 `gfx1200` 与 `gfx1201`。同系列不同核心时可能注入错误架构号。 |
| H5 | 🟡 | `:86-94` | 多张 AMD 卡时取**第一个**满足条件的，与 `:72`（取显存最大）的选卡逻辑不一致；若首张 AMD 卡是小显存型号会被误选。 |
| H6 | 🟡 | `:109` | `GetEnvironmentVariableA(...)` 返回 0 既可能是"未设置"，也可能是"设置为空字符串"。后者会被当作未设置而覆盖掉用户的显式配置。 |
| H7 | 🟡 | `:114` / `:128` / `:139` | 用 `fprintf(stderr, ...)` 输出 `[GPU-AutoConfig]` 提示。在 `dlssnr_gui.exe` 图形界面下 stderr 不可见，**用户完全看不到"检测到 RDNA 4 / 已注入 12.0.1"这类关键信息**——这正是排查你朋友问题时最需要看到的一行。建议同时写入日志文件。 |
| H8 | 🟢 | `:112-113` / `:124-125` / `:135-136` | `SetEnvironmentVariableA` 与 `_putenv` 重复调用（MSVC 的 `_putenv` 内部已调用前者）。无害但冗余。 |
| H9 | 🟢 | `:23-35` | 虚拟适配器用硬编码黑名单（`virtual`/`todesk`/`gameviewer`/`iddsample`/`remote`…），需持续维护新型号；且名字中含 `"virtual"` 的**真实**显卡会被误判为虚拟卡而跳过。 |

## I. `README.md` 全文

| # | 级别 | 位置 | 问题 |
|---|---|---|---|
| I1 | 🟠 | `:149` / `:233` | 宣称 `diagnose_gpu.bat` 能"验证 ZLUDA 与 RDNA 4 (gfx1200/gfx1201) 环境兼容性，**精准排查 `cuInit failed: 100`**"。但按 **E1 / E2**，该工具的 `[4]` 段测的根本不是 HIP_PATH 那份 DLL、`[6]` 段三次 override 全部无效 —— **文档把用户导向一个会给出假结论的工具**。你朋友的排查正是被这条指引带偏的。 |
| I2 | 🟠 | `:162`（FAQ Q2） | 把 `cuInit 100` 单一归因为"旧版 HIP SDK 的 DLL 覆盖了环境变量"，**完全没提程序会通过 `gpu_detection.h` 自动注入 `HSA_OVERRIDE_GFX_VERSION=12.0.1`**，也没告诉用户如何验证注入是否真的生效（例如看 stderr 里的 `[GPU-AutoConfig]` 行——但见 H7，GUI 下根本看不到）。 |
| I3 | 🟡 | `:112` | "首次可能耗时 10~30 秒，编译完成后将**永久缓存**在本地，后续运行秒级秒启" —— 与 `docs/issue_blank_race_report.md`（缓存**不持久化**，每次重翻译 15 个模块 40-90 秒）直接矛盾（同 G1② / G2，此为 README 版本）。 |
| I4 | 🟡 | `:9` | release badge 写 `v2026.09.10-multipass`，与打包脚本 `v2026.09.11`、发布说明 `2026-09-11`、窗口标题 `v2026.09.10` 不一致 —— 这是 A14 的**第四处**版本号。 |
| I5 | 🟡 | `:126` | `--parallel auto` 描述为"智能硬件守护，自动检测显存与 CPU 安全分配"，但 `hardware_budget.h:119` 只会返回 1 或 2（F3），"智能分配"的实际上限就是 2。 |
| I6 | 🟢 | `:108` / `:208` | 要求"请解压至全英文路径"——与 C27（concat 列表 UTF-8 无 BOM）呼应，Unicode 路径是已知限制；README 已提示，属可接受，但根因未修。 |
| I7 | ✅ | 已核实**无**问题 | `:127` 关于 "`--passes 2` 时自动激活双引擎级联乒乓时序管线" 的描述，与 `image_processor.cpp:627` 的 `settings.is_video && passes >= 2 && s->intermediate` 判定一致。 |

## J. 杂项

| # | 级别 | 位置 | 问题 |
|---|---|---|---|
| J1 | 🟢 | 工作区根目录 | 残留 staging 目录 `DLSSNRFilter-v2026.09.11-multipass-nodlssnr\`（内含发布说明、诊断脚本等副本）—— 印证 A17：`package_release.py` 打包后不清理。 |

---

---

# 第四轮补充审查（K / L 段）

审查对象：`dlss_layer/frame_blit.cpp`、`tests/processor_smoke.cpp`、`core/image_processor.h`。
新增 **10 条**（K0-K8、L1-L4，其中 K0/K6/K7/K8、L4 为更正/排除项）。

> **重要更正**：原 **D4**（`frame_blit` 跨设备提交）已被证明**不成立** —— 见下方 K0，请勿再按原描述排查。

| # | 级别 | 位置 | 问题 |
|---|---|---|---|
| K0 | ✅更正 | `frame_blit.cpp:180-181` | **原 D4 已失效。** 现代码为 `if (g.ready && g.device == device) return true;` + `if (g.ready) shutdown();`，device 身份已校验、设备变化会自动重建。**残余边缘风险**：新 device 若复用旧 device 的指针地址会误判为同一设备。 |
| K1 | 🔴 | `:324` / `:358` / `:400` | **描述符堆槽位冲突（本轮最严重）。** 三个函数一律从 `GetCPUDescriptorHandleForHeapStart()` 开始写描述符，且都用 `SetComputeRootDescriptorTable(0, GetGPUDescriptorHandleForHeapStart())`：`to_shared` 占 slot 0/1，`raw_rgb48_to_shared` 占 slot 0/1/2，`to_backbuffer` 占 slot 0。`view_heap` 只有 8 槽（`:296`），注释写明"8 slots for overlapping passes"，但**代码从不轮转偏移量**。描述符是 CPU 立即写入、GPU 延迟执行的 —— 若同一条命令列表里连续记录两次 blit，第二次会覆盖第一次的槽位，GPU 执行第一个 dispatch 时读到的是第二个的描述符 → **读到错误纹理 / 画面错乱**。修法：加一个环形游标，每次调用后累加 `view_stride` 并对 8 取模。 |
| K2 | 🟠 | `:419` | `ID3D12Resource *target_back = dst_backbuffer ? dst_backbuffer : dst_color;` —— 当调用方给 `raw_rgb48_to_shared` 传 `nullptr` 时，**backbuffer 被悄悄别名回 color**。发布说明"Factor 1 修复"宣称"彻底解除 Backbuffer 与 Output 共用同一纹理的指针别名"，但只要该可选参数为 null，别名就回来了、自反馈递归模糊问题复现。需确认 `dlss_cuda.cpp:1512` 附近是否**总是**传非 null。 |
| K3 | 🟠 | `:186-190` | `g.vs` 编译成功但 `g.ps` 失败时，仅 `cs->Release()` 后 return false —— **`g.vs` 既未释放也未置空** → 泄漏 + 悬垂；后续若调 `shutdown()`（`:444`）会**二次 Release**。 |
| K4 | 🟠 | `:222-226` / `:260-263` / `:291` / `:298-310` | **多条失败路径不回滚。** 任一 `make_root_signature` / `CreateComputePipelineState` / `CreateDescriptorHeap` 失败时，前面已成功创建的 root signature、PSO、`g.vs`/`g.ps` 全部泄漏，且 `g.ready` 仍为 false → **每次重试 `init()` 都会再泄漏一批**。 |
| K5 | 🟡 | `:58-59` | 最后一个像素处 `src.Load((byte_idx & ~3u) + 4u)` 会**越界读 2 字节**。D3D12 对 `ByteAddressBuffer` 越界读返回 0，且实际用到的字节都在界内，因此结果正确 —— 但依赖该保证，属可容忍的边界写法。 |
| K6 | ✅ | 已核实**无**问题 | `:57-62` 的 RGB48 解包（6 字节非 4 字节对齐的 R/G/B 拆分）在 `byte_idx & 3 == 0` 与 `== 2` 两种情形下**均正确**；`& ~3u` 也满足 `ByteAddressBuffer.Load` 的 4 字节对齐要求。 |
| K7 | ✅ | 已核实**无**问题 | `:406` `NumElements = (width*height*6+3)/4` 计算正确，且 4K/8K 下 `width*height*6` 不会 uint32 溢出。 |
| K8 | ✅ | 已核实**无**问题 | `shutdown()` 末尾 `g = State{}` 会整体复位（`:448`）。 |
| L1 | 🟠 | `tests/processor_smoke.cpp:95` | `CoInitializeEx` 返回值被忽略（失败仍继续），且**全程从不 `CoUninitialize`**。 |
| L2 | 🟡 | `processor_smoke.cpp:96` | `wic` 创建后，**所有返回路径**（`:104/118/126/132` 的 `return 1` 与 `:138` 的 `return 0`）**都不 `Release`**，也都不 `CoUninitialize`。 |
| L3 | 🟡 | `processor_smoke.cpp:118-121` / `:126-129` | `start()` / `process()` 失败时直接 `return 1`，**从不调用 `processor.stop()`**（与 D8 相关）。作为仓库里"接口使用范本"，它示范了错误的清理方式，容易被照抄。 |
| L4 | ✅ | 已核实**无**问题 | ① `:42-45` 的 `resize(width*height*4)` + stride `width*8` + 缓冲 `pixels.size()*2` 三者一致；② `:125` 的 `Settings settings;` 是安全的 —— `image_processor.h:31-60` 中 `Settings` **所有字段都有默认成员初始化器**，不存在未初始化成员（排除了"时好时平源于未初始化 Settings"这一假设）。 |

---

---

# 第五轮补充审查（M / N 段）

审查对象：`dlss_layer/dlss_cuda.h`、`dlss_layer/cuda_min.h`、`ngx_runtime/ngx_runtime.cpp`。
新增 **15 条**（M1-M7、N1-N8，其中 M2/M6/M7、N8 为排除/已证实项）。

> **至此，仓库内全部源文件已审查完毕**（见文末"审查覆盖情况"）。

## M. `dlss_cuda.h` / `cuda_min.h`

| # | 级别 | 位置 | 问题 |
|---|---|---|---|
| M1 | 🟠 | `dlss_cuda.h:118` + `image_processor.cpp:513` / `:845` | **`perf_quality` 恒为 2，确认 C10。** 头文件默认值 `int32_t perf_quality = 2`，`image_processor.cpp` 两处又**硬编码** `feature.perf_quality = 2`。DLSS 从不接收超分质量档，`--upscale-mode` 的放大 100% 由 ffmpeg lanczos 完成。 |
| M2 | ✅ | 已核实**无**影响 | `dlss_cuda.h:147` `FrameDesc::reset_accumulation = false` 与 `image_processor.h:53` `Settings::reset_accumulation = true` 默认值不一致 —— 但 `image_processor.cpp:677 / 686 / 702 / 949 / 956 / 971` **全部显式从 settings 赋值**，header 默认值从未被依赖。仅属可读性陷阱，非 bug。 |
| M3 | 🟠 | `image_processor.cpp:476-484` / `:813-821` | **重建条件不考虑 `passes` 减小。** 判定式为 `尺寸变化 \|\| (need_intermediate && !s->intermediate)`。当 `passes` 从 2 降到 1 而尺寸不变时不触发重建，`s->intermediate`（输出尺寸的 RGBA16F UAV）会**一直残留**并持续占用显存 —— 与 D7（多建第二个 feature）叠加，在显存吃紧的 ZLUDA 环境下更糟。 |
| M4 | 🟡 | `dlss_cuda.h:184` | `evaluate_backbuffer(void *back_buffer, ..., int format)` —— 参数声明为 `void*`，靠注释说明实为 `ID3D12Resource`；`format` 用裸 `int` 而非 `DXGI_FORMAT`。类型不安全，传错编译器拦不住。 |
| M5 | 🟡 | `dlss_cuda.h:198` / `:213` | `debug_read_shared_colour`、`hold_device_memory` 等纯诊断函数暴露在公开头文件中，且 `hold_device_memory` 注释明说"Takes device memory and never gives it back"。 |
| M6 | ✅ | 已核实**无**问题 | `cuda_min.h` 各结构体与官方 CUDA 头文件逐字段比对一致：`CUDA_EXTERNAL_MEMORY_HANDLE_DESC`(`:36-49`)、`CUDA_ARRAY_DESCRIPTOR`(`:59-64`)、`CUDA_MEMCPY2D`(`:74-88`)、`CUDA_ARRAY3D_DESCRIPTOR`(`:90-97`)、`CUDA_EXTERNAL_MEMORY_MIPMAPPED_ARRAY_DESC`(`:99-104`)、`CUDA_RESOURCE_DESC`(`:113-123`)、`CUDA_TEXTURE_DESC`(`:129-140`)；常量 `CU_CTX_SCHED_AUTO=0`(`:142`)、`CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR/MINOR=75/76`(`:146-147`) 亦正确。仅 `:118-119` 用 `void *devPtr` 而官方为 `CUdeviceptr`（x64 下同宽，无害）。 |
| M7 | ✅ | 已证实 | **A20 属实**：`dlss_layer\ngx_cuda.h` 与 `ngx_runtime\ngx_cuda.h` 的 SHA256 **完全相同**（`C796BEB317D6251C...`），确为字节级重复文件。 |

## N. `ngx_runtime/ngx_runtime.cpp`

| # | 级别 | 位置 | 问题 |
|---|---|---|---|
| N1 | 🟠 | `:232` / `:246-259` | **确认 D14。** `ngxrt_load` 半成功（DLL 加载成功但缺某个必需导出，如拿错版本的 `nvngx_dlssnr.dll`）时，`g_snippet` 已非 null 且**从不 `FreeLibrary`**；重试时 `:232` 直接 `return nullptr`（表示成功），而 `s_create` 等仍为 null → 后续失败信息完全失真，真正原因（缺导出）再也不会报出来。 |
| N2 | 🟠 | `:76-83` vs `:88 / :93 / :99 / :104 / :109 / :139` | `Set` 各重载写入 union 的不同成员（`.u64`/`.i64`/`.f64`/`.ptr`），而 `Get` 在类型不匹配时**读取 union 的其他成员**（典型 union type punning）。x64 下位宽相同故实践可用，但严格属 UB。其中 `:104` 的 `(unsigned long long)e->v.f64` 在值为负或 > 2^64 时是**明确的 UB**（同 D24）。 |
| N3 | 🟠 | `:169-170` | `g_init_params` / `g_feature_params` 是两个全局 `RuntimeParameters`（各含 512 × ~112B ≈ 57 KB 的 `entries_`，`:126-127`），**无任何同步**。`ngxrt_populate_parameters` 先 `reset_all()` 再 `s_populate()`；与 D12（dlss_cuda 全局单例无锁）叠加，多实例或并发调用会互相踩踏参数块。 |
| N4 | 🟡 | `:331` | `params->Set("Input1", g_input1)` **未判空**；若调用方传 `params == nullptr` 且 `g_use_input_params` 为真则崩溃。值得注意的是 `:334` 本身却向 `s_create1` 传了 `nullptr` 作为首参 —— 同一函数对 `params` 的空值假设自相矛盾。 |
| N5 | 🟡 | `:238` | `static char asked[512]` 为函数内静态缓冲，多线程共享；且 `WideCharToMultiByte` 在路径超过 511 字节时会截断并**不保证 NUL 结尾**，返回给调用方的 `const char*` 可能非 NUL 终止（调用方 `printf` 会越界读）。 |
| N6 | 🟢 | `:375-382` | `ngxrt_scratch_size` 上方的注释块是**三个函数注释的拼接** —— 混入了 `ngxrt_set_inputs`（"Selects the CreateFeature1 path…"）与 `ngxrt_trace_params`（"Turns the parameter trace on…"）的说明，而 `:414` / `:421` 那两个函数本身反而没有注释。说明注释被移动过，属文档错乱。 |
| N7 | 🟣 | `:72` + `ngx_cuda.h` | **架构风险**：`RuntimeParameters : public NVSDK_NGX_Parameter` 依赖 `ngx_cuda.h` 这份**手工复刻**的 NGX SDK 头文件，其虚函数表顺序必须与 snippet 二进制期望的完全一致（`:36-47` 记录了逆向得到的 vtable 偏移 `[vt+0x58]` / `[vt+0x40]`）。snippet 版本一变、vtable 布局不符就会直接崩溃或静默错乱。而 `ngx_cuda.h` 存在两份相同副本（M7），改一处不会同步 —— 建议尽快合并为单一文件。 |
| N8 | ✅ | 已核实**无**问题 | ① `:159` 容量溢出时返回函数内静态 `discard` 槽并丢弃写入（`:124-125` 注释已说明设计意图），不会越界；② `:74 reset_all()` 只置 `count_=0`，而 `find` 仅扫描 `0..count_`，旧数据不可见；③ `:441 DllMain` 只 `return TRUE`，未做任何重活，无 loader lock 风险；④ `:127 entries_[]` 为全局对象成员（在 .bss），无栈溢出风险。 |

---

# 审查覆盖情况

| 文件 | 状态 |
|---|---|
| `tools/video_filter.cpp`（2847 行） | ✅ 全量 |
| `gui/batch_window.cpp/.h`、`compare_view.cpp/.h`、`batch_main.cpp` | ✅ 全量 |
| `gui/main_window.cpp/.h`、`main.cpp` | ✅ 全量 |
| `core/image_processor.cpp/.h`、`precompile.cpp/.h`、`gpu_detection.h` | ✅ 全量 |
| `dlss_layer/dlss_cuda.cpp/.h`、`frame_blit.cpp/.h`、`cuda_min.h`、`ngx_cuda.h` | ✅ 全量 |
| `ngx_runtime/ngx_runtime.cpp`、`ngx_cuda.h` | ✅ 全量 |
| `tests/processor_smoke.cpp`、`gui/half_float.h` | ✅ 全量 |
| `CMakeLists.txt`、`build.bat`、`build_smoke.bat`、`tools/package_release.py`、`tools/hardware_budget.h`、`tools/analyze_vopd.py` | ✅ 全量 |
| `diagnose_gpu.ps1/.bat`、`fix_tdr.ps1/.bat` | ✅ 全量 |
| `README.md`、`发布包说明.txt`、`docs/issue_blank_race_report.md` | ✅ 全量 |

**未覆盖（静态审查的天然盲区，需运行时验证）**：显存碎片与长时运行 TDR、ZLUDA/HIP 的非确定性（"时好时平"）、多实例并发、中文/Unicode 路径全链路、DPI/多显示器实际表现。

---

# 建议修复优先级

**第零梯队（GPU 检测，直接决定程序能否在 RDNA 4 上跑起来）**
**H1 / H2**（枚举异常会静默跳过自动注入，能完整解释 `cuInit failed: 100`）→ **H3 / H7**（RDNA 4 漏检 + 提示不可见）

**第一梯队（发版链路 + 诊断可信度，不做则出不了正确的包、也查不出真因）**
A1 / A2 / A3 / A4 / A5 / A6 / A7 → **E1 / E2**（诊断脚本给出假结论，会让你往错误方向排查）
→ **G1**（`docs/` 里那份未修复的空白/挂死实测报告，其 workaround 未合入主仓库，建议作为验收标准）

**GPU 渲染正确性（画面会错乱，且与宣传的零拷贝路径直接相关）**
**K1**（描述符堆槽位冲突，同命令列表内连续 blit 会读到错误纹理）→ **K2**（backbuffer 别名回 color，"Factor 1 修复"失效）

**第二梯队（会误导排查方向或造成静默错误结果）**
C1 / C2 / C7 / C8 → D1 / D4 / D6 / D15 → B1 / B2 / B22 → E5 / F1

**第三梯队（功能不达预期与交互缺陷）**
C9 / C10 / C12 / C13 / C14 → B5 / B6 / B7 / B9 / B11

**第四梯队（泄漏与清理）**
C3 / D2 / D3 / D8 / D10 / D11

**最后（死代码、不一致、边缘情况）**
其余 🟢 条目
