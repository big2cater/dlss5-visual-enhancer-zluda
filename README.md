基于DLSS5-image-enhancer-zluda项目修改
AMD可用的DLSSNR神经滤镜，作用于视频或者图片
使用方法：
1. 解压整个文件夹，不要只复制单个 DLL。
2. 双击 dlssnr_gui.exe。
3. 视频处理需要 ffmpeg.exe 和 ffprobe.exe；请安装 FFmpeg 并把 bin 目录加入 PATH。
4. 首次运行可能需要较长时间翻译 CUDA 模块，之后会使用本机缓存。

说明：
- 本包包含当前修复后的 GUI、视频处理程序、ZLUDA nvcuda.dll、nvapi64.dll、nvngx.dll。
- 这是 AMD/ZLUDA 版本，显卡和驱动兼容性取决于本机环境。
- 如果 Windows Defender 或安全软件拦截 DLL，请确认文件来自可信来源后再允许。
- 视频转换如遇闪烁请使用默认参数并调节模型预设挡位。
DLSS 模型预设	对应的 DLSS 超分档位	核心说明
Preset K	DLAA (原生抗锯齿)、质量 (Quality)、平衡 (Balanced)	基础模型，适用于对画质要求最高的场景。
Preset M	性能 (Performance)	针对“性能”档位优化，在帧数和画质间取得平衡。
Preset L	超级性能 (Ultra Performance)	针对“超级性能”档位优化，为4K高分辨率下的极限帧率设计。
