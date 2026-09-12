#include "batch_window.h"

#include "compare_view.h"
#include "../core/precompile.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QColor>
#include <QComboBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QUrl>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTimer>
#include <QVBoxLayout>

namespace batch {
namespace {

// A slider and the number it stands for. The network takes these settings as
// fractions, so the slider counts hundredths and the label divides back.
QWidget *slider_row(int maximum, int value, QSlider *&slider, QLabel *&readout) {
    auto *row = new QWidget;
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);

    slider = new QSlider(Qt::Horizontal);
    slider->setRange(0, maximum);
    slider->setValue(value);
    slider->setTickPosition(QSlider::NoTicks);

    readout = new QLabel(QString::number(value / 100.0, 'f', 2));
    readout->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    // Wide enough for the longest reading the slider can produce, so the number
    // never shifts the slider as it changes.
    readout->setMinimumWidth(
        readout->fontMetrics().horizontalAdvance(QStringLiteral("-0.00")) + 10);

    layout->addWidget(slider, 1);
    layout->addWidget(readout);

    QObject::connect(slider, &QSlider::valueChanged, readout, [readout](int v) {
        readout->setText(QString::number(v / 100.0, 'f', 2));
    });
    return row;
}

QWidget *slider_percent_row(int maximum, int value, QSlider *&slider, QLabel *&readout) {
    auto *row = new QWidget;
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);

    slider = new QSlider(Qt::Horizontal);
    slider->setRange(0, maximum);
    slider->setValue(value);
    slider->setTickPosition(QSlider::NoTicks);

    readout = new QLabel(QString::number(value) + QStringLiteral("%"));
    readout->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    readout->setMinimumWidth(
        readout->fontMetrics().horizontalAdvance(QStringLiteral("200%")) + 10);

    layout->addWidget(slider, 1);
    layout->addWidget(readout);

    QObject::connect(slider, &QSlider::valueChanged, readout, [readout](int v) {
        readout->setText(QString::number(v) + QStringLiteral("%"));
    });
    return row;
}

QString settings_path() {
    return QCoreApplication::applicationDirPath() + QStringLiteral("/dlssnr_gui.ini");
}

} // namespace

BatchWindow::BatchWindow() {
    setWindowTitle(tr("DLSS 5 神经渲染滤镜 (AMD/ZLUDA) - v2026.09.10-multipass"));
    setAcceptDrops(true);

    auto *central = new QWidget;
    auto *layout = new QVBoxLayout(central);
    layout->addWidget(build_files());
    layout->addWidget(build_parameters());
    layout->addWidget(build_run());
    layout->addWidget(new QLabel(tr("处理消息")), 0);

    log_ = new QPlainTextEdit;
    log_->setReadOnly(true);
    log_->setMaximumBlockCount(2000);
    log_->setPlaceholderText(tr("video_filter 的输出显示在这里"));
    layout->addWidget(log_, 1);

    setCentralWidget(central);
    // Keep both parameter columns visible on first launch. The old fixed
    // 1000px width clipped the right video controls on ordinary desktops.
    setMinimumSize(950, 700);
    const QRect screen = QGuiApplication::primaryScreen()
                              ? QGuiApplication::primaryScreen()->availableGeometry()
                              : QRect(0, 0, 1440, 900);
    resize(qMin(1360, qMax(1100, screen.width() - 60)),
           qMin(920, qMax(720, screen.height() - 60)));

    // Match the frame summary anywhere in a line. ffmpeg can prefix inherited
    // diagnostics and different builds use 5 or 6 frame digits.
    frame_re_ = QRegularExpression(
        QStringLiteral(R"(\[\s*(\d+)\]\s+([\d.]+)\s+ms\s+\(avg\s+([\d.]+),\s*reset=(\d+),\s*blanks=(\d+)\))"));

    timer_ = new QTimer(this);
    timer_->setInterval(1000);
    connect(timer_, &QTimer::timeout, this, &BatchWindow::on_second_tick);

    connect(&process_, &QProcess::readyReadStandardError, this, &BatchWindow::read_error);
    connect(&process_, &QProcess::readyReadStandardOutput, this, [this]() { process_.readAllStandardOutput(); });
    connect(&process_, &QProcess::finished, this, &BatchWindow::on_finished);

    connect(prewarm_button_, &QPushButton::clicked, this, &BatchWindow::start_prewarm);
    connect(&prewarm_process_, &QProcess::readyReadStandardError, this, &BatchWindow::on_prewarm_output);
    connect(&prewarm_process_, &QProcess::readyReadStandardOutput, this, &BatchWindow::on_prewarm_output);
    connect(&prewarm_process_, &QProcess::finished, this, &BatchWindow::on_prewarm_finished);

    load_settings();
    update_mode_ui();
    hint_cold_cache();
}

QString BatchWindow::application_dir() { return QCoreApplication::applicationDirPath(); }

QString BatchWindow::number(double value) { return QString::number(value, 'f', 2); }

bool BatchWindow::is_image(const QString &path) { return is_image_file(path); }

// ---------------------------------------------------------------------------
// Building the window
// ---------------------------------------------------------------------------

QLineEdit *BatchWindow::add_path_row(QFormLayout *form, const QString &caption, bool open) {
    auto *row = new QWidget;
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);

    auto *field = new QLineEdit;
    auto *button = new QPushButton(tr("选择…"));
    layout->addWidget(field, 1);
    layout->addWidget(button);

    connect(button, &QPushButton::clicked, this, [this, field, open] { browse(field, open); });

    form->addRow(caption, row);
    return field;
}

QWidget *BatchWindow::build_files() {
    auto *box = new QGroupBox(tr("文件"));
    auto *form = new QFormLayout(box);

    input_ = add_path_row(form, tr("输入视频/图片"), true);
    output_ = add_path_row(form, tr("输出文件"), false);
    snippet_ = add_path_row(form, tr("nvngx_dlssnr.dll"), true);
    driver_ = add_path_row(form, tr("nvcuda.dll (ZLUDA)"), true);
    runtime_ = add_path_row(form, tr("nvngx.dll"), true);
    nvapi_ = add_path_row(form, tr("nvapi64.dll"), true);

    // Picking an image switches to image mode and the other way round; the
    // radios are still there to override it afterwards.
    connect(input_, &QLineEdit::textChanged, this, [this] {
        if (!filling_) {
            auto_output_ = true;
            auto_fill_output(true);
        }
    });
    connect(output_, &QLineEdit::textChanged, this, [this] {
        if (!filling_) auto_output_ = false;
    });

    auto *modes = new QWidget;
    auto *mode_layout = new QHBoxLayout(modes);
    mode_layout->setContentsMargins(0, 0, 0, 0);
    video_mode_ = new QRadioButton(tr("视频滤镜"));
    image_mode_ = new QRadioButton(tr("单图滤镜"));
    video_mode_->setChecked(true);
    mode_layout->addWidget(video_mode_);
    mode_layout->addWidget(image_mode_);
    auto *defaults = new QPushButton(tr("恢复默认路径"));
    mode_layout->addWidget(defaults);
    auto *repo_btn = new QPushButton(tr("GitHub 仓库"));
    auto *update_btn = new QPushButton(tr("检查更新"));
    mode_layout->addWidget(repo_btn);
    mode_layout->addWidget(update_btn);
    mode_layout->addStretch(1);

    connect(repo_btn, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://github.com/big2cater/dlss5-visual-enhancer-zluda")));
    });
    connect(update_btn, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://github.com/big2cater/dlss5-visual-enhancer-zluda/releases")));
    });

    connect(video_mode_, &QRadioButton::toggled, this, [this](bool on) {
        if (on && !filling_) { auto_fill_output(false); update_mode_ui(); }
    });
    connect(image_mode_, &QRadioButton::toggled, this, [this](bool on) {
        if (on && !filling_) { auto_fill_output(false); update_mode_ui(); }
    });
    connect(defaults, &QPushButton::clicked, this, [this] {
        const QDir dir = application_dir();
        snippet_->setText(dir.filePath(QStringLiteral("nvngx_dlssnr.dll")));
        driver_->setText(dir.filePath(QStringLiteral("nvcuda.dll")));
        runtime_->setText(dir.filePath(QStringLiteral("nvngx.dll")));
        nvapi_->setText(dir.filePath(QStringLiteral("nvapi64.dll")));
    });

    // A stretch rather than a blank row: the two columns in this form are only
    // as wide as their widest caption, and the window is free to be wider.
    form->addRow(modes);
    return box;
}

QWidget *BatchWindow::build_parameters() {
    auto *box = new QGroupBox(tr("参数设置"));
    auto *columns = new QHBoxLayout(box);
    columns->setSpacing(12);

    columns->addWidget(build_left_column(), 1);
    composite_panel_ = build_composite_column();
    columns->addWidget(composite_panel_, 1);
    video_panel_ = build_video_column();
    columns->addWidget(video_panel_, 1);

    return box;
}

QWidget *BatchWindow::build_left_column() {
    auto *column = new QWidget;
    auto *col_layout = new QVBoxLayout(column);
    col_layout->setContentsMargins(0, 0, 0, 0);
    col_layout->setSpacing(6);

    auto *header = new QLabel(tr("<b>【DLSS 核心设置】</b>"));
    header->setStyleSheet(QStringLiteral("color: #4da6ff; margin-bottom: 2px;"));
    col_layout->addWidget(header);

    auto *form = new QFormLayout;
    form->setContentsMargins(0, 0, 0, 0);

    form->addRow(tr("强度"), slider_row(200, 100, intensity_, intensity_value_));
    form->addRow(tr("全局色调"), slider_row(100, 0, global_tone_, global_tone_value_));
    form->addRow(tr("局部色调"), slider_row(200, 0, local_tone_, local_tone_value_));
    form->addRow(tr("局部结构"), slider_row(200, 100, local_structure_, local_structure_value_));
    form->addRow(tr("皮肤结构 (防塑料感)"), slider_row(100, 10, skin_structure_, skin_structure_value_));
    skin_structure_->setToolTip(
        tr("人脸/皮肤纹理防过度平滑保护：调高此项可在强力降噪的同时保护人像面部微毛孔与天然皮肤质感，避免塑料脸或假面感。"));

    style_ = new QComboBox;
    style_->addItems({tr("0 - 默认 (平衡)"), tr("1 - 自然 (低时序延迟·紧跟人物)"), tr("2 - 电影 (高平滑稳定)")});
    style_->setCurrentIndex(2); // 默认电影风格 (Cinematic)
    style_->setToolTip(tr(
        "风格选择与时序响应特性：\n"
        "· 0 - 默认：标准时序累积平衡，适合普通视频。\n"
        "· 1 - 自然 (推荐解决拖影)：极低时序惯性与衰减延迟，紧贴人物动作与转头面部，杜绝慢半拍拖影不跟人现象！\n"
        "· 2 - 电影：重度时序平滑与胶片降噪（静态或微动场景质感极佳，但大动作时容易产生历史帧拖尾滞后感）。"));
    form->addRow(tr("风格"), style_);

    preset_ = new QComboBox;
    preset_->addItems({tr("0 - 自动 (内置 Preset 1)"), tr("1 - Preset #1 (内置核心权重)"),
                       tr("2 - Preset #2 (回退至 Preset 1)"), tr("3 - Preset #3 (回退至 Preset 1)")});
    preset_->setToolTip(
        tr("DLSS 神经渲染模型预设。注意：当前官方 nvngx_dlssnr.dll (v310.8) 内部仅打包内置了 Preset 1 单套神经网络权重，其余预设底层均自动回退至 Preset 1。"));
    form->addRow(tr("DLSS模型预设"), preset_);

    model_ = new QComboBox;
    model_->addItems({tr("默认"), QStringLiteral("J"), QStringLiteral("K"), QStringLiteral("L"),
                      QStringLiteral("M")});
    model_->setEnabled(true);
    model_->setToolTip(tr("选择 DLSS 模型预设（默认/J/K/L/M）。通过底层环境变量 DLSS_PRESET 传递。"));
    form->addRow(tr("DLSS模型"), model_);

    gamma_ = new QDoubleSpinBox;
    gamma_->setRange(0.5, 3.0);
    gamma_->setSingleStep(0.1);
    gamma_->setDecimals(1);
    gamma_->setValue(1.0);
    form->addRow(tr("输出伽马"), gamma_);

    auto_mask_ = new QCheckBox(tr("自动遮罩 (保护肤色/主体识别)"));
    auto_mask_->setChecked(true);
    auto_mask_->setToolTip(
        tr("自动生成 AI 语义遮罩，精准隔离人物肤色与背景，防止背景暖色光源污染面部肤色，保留字幕/UI。建议始终开启。"));
    form->addRow(QString(), auto_mask_);

    auto *reset_button = new QPushButton(tr("恢复默认参数"));
    connect(reset_button, &QPushButton::clicked, this, &BatchWindow::reset_effects);
    form->addRow(QString(), reset_button);

    col_layout->addLayout(form);
    col_layout->addStretch(1);
    return column;
}

QWidget *BatchWindow::build_composite_column() {
    auto *column = new QWidget;
    auto *col_layout = new QVBoxLayout(column);
    col_layout->setContentsMargins(0, 0, 0, 0);
    col_layout->setSpacing(6);

    auto *header = new QLabel(tr("<b>【画面合成 & 防起雾 (通用)】</b>"));
    header->setStyleSheet(QStringLiteral("color: #4da6ff; margin-bottom: 2px;"));
    col_layout->addWidget(header);

    auto *form = new QFormLayout;
    form->setContentsMargins(0, 0, 0, 0);

    form->addRow(tr("AI混合浓度"), slider_percent_row(100, 100, output_mix_, output_mix_value_));
    output_mix_->setToolTip(tr("AI 画面与原图的全局混合浓度 (方案 5)。100% 为完全 AI 输出；80% 或更低能带来柔美的真实写真感，化解数码过度锐化。视频与单图均生效。"));

    form->addRow(tr("细节清晰度"), slider_percent_row(200, 100, detail_boost_, detail_boost_value_));
    detail_boost_->setToolTip(tr("细节清晰度与高频提取增强 (方案 4)。100% 为原生 AI 细节；>100% 锐化睫毛和发丝，<100% 柔化画面。视频与单图均生效。"));

    form->addRow(tr("暗部保护"), slider_percent_row(200, 100, shadow_protect_, shadow_protect_value_));
    shadow_protect_->setToolTip(tr("针对暗部被 AI 抬升的压制强度 (方案 3)。默认 100% 保持原生对比度；拉到 0% 锁定纯黑底色，彻底杜绝任何泛灰起雾。视频与单图均生效。"));

    form->addRow(tr("高光辉光"), slider_percent_row(200, 100, glow_control_, glow_control_value_));
    glow_control_->setToolTip(tr("高光反射和眼神光的 AI 表现调节 (方案 3)。100% 为标准高光表现。视频与单图均生效。"));

    // Single-image repeating passes row:
    image_passes_ = new QSpinBox;
    image_passes_->setRange(1, 12);
    image_passes_->setValue(3);
    image_passes_->setToolTip(tr("单图模式重复处理同一张图片的次数。次数越高效果越强，也更慢。"));
    image_passes_row_ = new QWidget;
    auto *passes_layout = new QHBoxLayout(image_passes_row_);
    passes_layout->setContentsMargins(0, 0, 0, 0);
    auto *passes_caption = new QLabel(tr("图片处理次数"));
    passes_layout->addWidget(passes_caption);
    passes_layout->addWidget(image_passes_);
    passes_layout->addStretch(1);
    form->addRow(image_passes_row_);

    col_layout->addLayout(form);

    // Preset buttons 2x2 grid
    auto *preset_grid_widget = new QWidget;
    auto *grid = new QGridLayout(preset_grid_widget);
    grid->setContentsMargins(0, 4, 0, 4);
    grid->setSpacing(6);

    btn_preset_film_ = new QPushButton(tr("原生电影(推荐)"));
    btn_preset_soft_ = new QPushButton(tr("柔和写真"));
    btn_preset_macro_ = new QPushButton(tr("极致微距"));
    btn_preset_black_ = new QPushButton(tr("纯黑无雾"));
    btn_preset_motion_ = new QPushButton(tr("⚡ 灵动跟人 (防拖影·极速响应)"));

    btn_preset_film_->setToolTip(tr("【原生电影 (推荐)】：混合 100% | 细节 100% | 暗部保护 50% | 局部色调 0 | 风格 电影"));
    btn_preset_soft_->setToolTip(tr("【柔和写真 (防发脆)】：混合 80% | 细节 90% | 暗部保护 50% | 消除过度数码锐化，自然写真质感"));
    btn_preset_macro_->setToolTip(tr("【极致微距 (锐利)】：混合 100% | 细节 115% | 暗部保护 20% | 睫毛发丝根根分明，高频微距锐化"));
    btn_preset_black_->setToolTip(tr("【纯黑强化 (绝对无雾)】：混合 100% | 细节 100% | 暗部保护 0% | 彻底锁定原图纯黑，零灰雾"));
    btn_preset_motion_->setToolTip(tr("【灵动跟人 (防拖影/极速响应/自然白皙)】：风格 默认(自然冷白·无偏黄) | 开启自动遮罩(隔离肤色) | 强度 85% | 混合 90% | 细节 105% | 自动开启运动向量引导 | 专治人物转头/大幅度动作滞后与皮肤泛黄"));
    btn_preset_motion_->setStyleSheet(QStringLiteral("font-weight: bold; color: #4da6ff;"));

    grid->addWidget(btn_preset_film_, 0, 0);
    grid->addWidget(btn_preset_soft_, 0, 1);
    grid->addWidget(btn_preset_macro_, 1, 0);
    grid->addWidget(btn_preset_black_, 1, 1);
    grid->addWidget(btn_preset_motion_, 2, 0, 1, 2);
    col_layout->addWidget(preset_grid_widget);

    connect(btn_preset_film_, &QPushButton::clicked, this, [this] {
        apply_composite_preset(100, 100, 50, 100);
    });
    connect(btn_preset_soft_, &QPushButton::clicked, this, [this] {
        apply_composite_preset(80, 90, 50, 100);
    });
    connect(btn_preset_macro_, &QPushButton::clicked, this, [this] {
        apply_composite_preset(100, 115, 20, 110);
    });
    connect(btn_preset_black_, &QPushButton::clicked, this, [this] {
        apply_composite_preset(100, 100, 0, 100);
    });
    connect(btn_preset_motion_, &QPushButton::clicked, this, [this] {
        apply_composite_preset(90, 105, 50, 100);
        style_->setCurrentIndex(0); // 0 - 默认 (自然通透冷白皮，零泛黄，低时序惯性)
        intensity_->setValue(85);   // 85% 灵动响应
        if (auto_mask_) auto_mask_->setChecked(true); // 开启主体人物自动遮罩，杜绝背景黄光污染肤色
        if (flow_) flow_->setChecked(true); // 开启运动向量光流引导
    });

    auto *desc_label = new QLabel(tr(
        "【方案说明 (3/4/5协同运作，互不冲突)】\n"
        "• 方案 3 (暗部保护): 抑制暗部灰雾，0%锁定纯黑\n"
        "• 方案 4 (细节清晰): 频域高频分离，>100%发丝锐利\n"
        "• 方案 5 (AI混合浓度): 原图与AI柔和融合，80%呈现写真感"));
    desc_label->setStyleSheet(QStringLiteral("color: #888899; font-size: 11px;"));
    desc_label->setWordWrap(true);
    col_layout->addWidget(desc_label);

    col_layout->addStretch(1);
    return column;
}

QWidget *BatchWindow::build_video_column() {
    auto *column = new QWidget;
    auto *col_layout = new QVBoxLayout(column);
    col_layout->setContentsMargins(0, 0, 0, 0);
    col_layout->setSpacing(6);

    auto *header = new QLabel(tr("<b>【视频流与编码】</b>"));
    header->setStyleSheet(QStringLiteral("color: #4da6ff; margin-bottom: 2px;"));
    col_layout->addWidget(header);

    auto *form = new QFormLayout;
    form->setContentsMargins(0, 0, 0, 0);

    reset_ = new QComboBox;
    reset_->addItems({tr("auto - 自动切场复位"), tr("always - 每帧复位"),
                      tr("never - 全程不断"), tr("every - 每N帧复位")});
    form->addRow(tr("复位模式"), reset_);

    reset_every_ = new QSpinBox;
    reset_every_->setRange(1, 100000);
    reset_every_->setValue(60);
    form->addRow(tr("每N帧"), reset_every_);

    cut_ = new QLineEdit(QStringLiteral("0.30"));
    cut_->setToolTip(
        tr("判定切场的平均亮度差，越大越不容易复位。调低会频繁清空累积历史，画面一跳一跳。"));
    form->addRow(tr("切场阈值"), cut_);

    passes_ = new QSpinBox;
    passes_->setRange(1, 2);
    passes_->setValue(1);
    passes_->setToolTip(
        tr("每帧降噪轮数 (Multipass)：\n"
           "· 1: 标准单轮降噪 (默认推荐，速度最快，画质平衡)\n"
           "· 2: 深度迭代降噪 (双引擎独立时序级联，针对高 ISO/强噪点与颗粒感，两轮净化且完全防闪烁)"));
    form->addRow(tr("降噪轮数(Multipass)"), passes_);

    crf_ = new QSpinBox;
    crf_->setRange(0, 40);
    crf_->setValue(18);
    form->addRow(tr("CRF(画质)"), crf_);

    codec_ = new QComboBox;
    codec_->addItems({tr("自动 (优先 AMF 硬件加速)"),
                      tr("H.264 (AMD AMF 硬件加速)"),
                      tr("HEVC / H.265 (AMD AMF 硬件加速 - 推荐)"),
                      tr("H.264 (CPU libx264 软件编码)")});
    codec_->setCurrentIndex(0);
    codec_->setToolTip(
        tr("选择视频编码器。AMD AMF 提供高速硬件压制；HEVC/H.265 拥有更高压缩率和更小文件体积。"));
    form->addRow(tr("视频编码格式"), codec_);

    parallel_ = new QComboBox;
    parallel_->addItems({tr("🚀 自动 (智能硬件守护·显存/CPU防卡死)"),
                         tr("2 进程分片并行 (加速~2倍·需12G+显存)"),
                         tr("关闭 (单进程标准模式)")});
    parallel_->setCurrentIndex(0);
    parallel_->setToolTip(
        tr("视频分段切片并行加速：\n"
           "· 自动：根据显卡显存、系统内存和 CPU 核心数自动评估并分配安全并发度，杜绝爆显存与 CPU 100% 死机；\n"
           "· 2 进程并行：将长视频分段双实例并发处理，完成后无损拼合，耗时减半；\n"
           "· 关闭：单进程标准逐帧处理。"));
    form->addRow(tr("🚀 并行加速"), parallel_);

    fps_ = new QLineEdit(QStringLiteral("0"));
    form->addRow(tr("输出fps(0=原)"), fps_);

    max_frames_ = new QLineEdit(QStringLiteral("0"));
    form->addRow(tr("最大帧数(0=全)"), max_frames_);

    model_scale_ = new QComboBox;
    model_scale_->addItems({tr("100% (原画质量 - 最清晰)"),
                            tr("75% (画质平衡 - 提速约1.8x)"),
                            tr("50% (极速性能 - 提速约3x)")});
    model_scale_->setCurrentIndex(0);
    model_scale_->setToolTip(
        tr("内部模型渲染比例：降低送入 DLSS 5 模型的内部渲染分辨率以极大地提高处理速度，再通过 Lanczos 放大至输出分辨率。"));
    form->addRow(tr("模型渲染比例"), model_scale_);

    upscale_ = new QComboBox;
    upscale_->addItems({tr("DLAA / native (1x)"), tr("Quality (1.5x)"), tr("Balanced (1.724x)"),
                        tr("Performance (2x)"), tr("Ultra Performance (3x)")});
    upscale_->setCurrentIndex(0);
    upscale_->setEnabled(true);
    upscale_->setToolTip(
        tr("输出放大模式：经 DLSS 5 神经增强后，通过高质量 Lanczos-3 算法放大至指定倍率的目标分辨率并进行硬件编码。"));
    form->addRow(tr("Upscaling 输出"), upscale_);

    dump_ = new QCheckBox(tr("抽帧到目录"));
    dump_dir_ = new QLineEdit;
    auto *dump_button = new QPushButton(tr("选择…"));
    auto *dump_row = new QWidget;
    auto *dump_layout = new QHBoxLayout(dump_row);
    dump_layout->setContentsMargins(0, 0, 0, 0);
    dump_layout->addWidget(dump_);
    dump_layout->addWidget(dump_dir_, 1);
    dump_layout->addWidget(dump_button);
    connect(dump_button, &QPushButton::clicked, this, [this] {
        const QString chosen = QFileDialog::getExistingDirectory(this, tr("抽帧到目录"),
                                                                dump_dir_->text());
        if (!chosen.isEmpty()) dump_dir_->setText(chosen);
    });
    form->addRow(dump_row);

    flow_ = new QCheckBox(tr("运动向量引导 (光流对齐·防拖影)"));
    flow_->setToolTip(tr("启用 D3D12 GPU 运动向量引导（光流时间对齐）。\n"
                         "神经网络根据人物运动轨迹动态对齐历史帧，解决人物转头或快速运动时滤镜不跟人与重影问题。"));
    audio_ = new QCheckBox(tr("音轨直通"));
    auto *switches = new QWidget;
    auto *switch_layout = new QHBoxLayout(switches);
    switch_layout->setContentsMargins(0, 0, 0, 0);
    switch_layout->addWidget(flow_);
    switch_layout->addWidget(audio_);
    switch_layout->addStretch(1);
    form->addRow(switches);

    col_layout->addLayout(form);
    col_layout->addStretch(1);
    return column;
}

QWidget *BatchWindow::build_run() {
    auto *box = new QGroupBox(tr("运行"));
    auto *layout = new QHBoxLayout(box);

    prewarm_button_ = new QPushButton(tr("预热缓存"));
    prewarm_button_->setToolTip(tr("把网络的代码提前翻译进本机缓存：首次使用或更换版本/显卡后点一次，"
                                   "之后的处理直接从缓存启动。NVIDIA 显卡无需预热。"));
    start_button_ = new QPushButton(tr("开始"));
    preview_button_ = new QPushButton(tr("预览 3 秒"));
    preview_button_->setToolTip(tr("仅处理前 3 秒视频快速验证效果，完成后自动打开对比窗口。"));
    frame_hold_button_ = new QPushButton(tr("单帧定帧对比"));
    frame_hold_button_->setToolTip(tr("瞬间提取参考帧并以当前参数增强（约0.1秒），立即打开分屏对比，调参极速反馈！"));
    stop_button_ = new QPushButton(tr("停止"));
    compare_button_ = new QPushButton(tr("对比结果"));
    stop_button_->setEnabled(false);

    progress_ = new QProgressBar;
    progress_->setRange(0, 100);
    progress_->setTextVisible(true);
    progress_->setFormat(QStringLiteral("%p%"));
    status_ = new QLabel(tr("就绪"));
    status_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    layout->addWidget(prewarm_button_);
    layout->addWidget(start_button_);
    layout->addWidget(preview_button_);
    layout->addWidget(frame_hold_button_);
    layout->addWidget(stop_button_);
    layout->addWidget(compare_button_);
    layout->addWidget(progress_, 1);
    layout->addWidget(status_);

    connect(start_button_, &QPushButton::clicked, this, &BatchWindow::start_run);
    connect(preview_button_, &QPushButton::clicked, this, &BatchWindow::start_preview);
    connect(frame_hold_button_, &QPushButton::clicked, this, &BatchWindow::start_frame_hold_compare);
    connect(stop_button_, &QPushButton::clicked, this, &BatchWindow::stop_run);
    connect(compare_button_, &QPushButton::clicked, this, &BatchWindow::open_compare);

    return box;
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

void BatchWindow::load_settings() {
    QSettings settings(settings_path(), QSettings::IniFormat);
    filling_ = true;

    input_->setText(settings.value(QStringLiteral("input")).toString());
    output_->setText(settings.value(QStringLiteral("output")).toString());

    const QDir dir = application_dir();
    // Beside this program is where the four libraries sit in a release package,
    // so an empty field is filled in from there rather than left blank.
    const auto beside = [&dir](const QString &name) { return dir.filePath(name); };
    snippet_->setText(settings.value(QStringLiteral("snippet"), beside(QStringLiteral("nvngx_dlssnr.dll"))).toString());
    driver_->setText(settings.value(QStringLiteral("driver"), beside(QStringLiteral("nvcuda.dll"))).toString());
    runtime_->setText(settings.value(QStringLiteral("runtime"), beside(QStringLiteral("nvngx.dll"))).toString());
    nvapi_->setText(settings.value(QStringLiteral("nvapi"), beside(QStringLiteral("nvapi64.dll"))).toString());
    dump_dir_->setText(
        settings.value(QStringLiteral("dumpDir"), beside(QStringLiteral("dump"))).toString());

    const bool image = settings.value(QStringLiteral("image"), false).toBool();
    image_mode_->setChecked(image);
    video_mode_->setChecked(!image);
    auto_output_ = settings.value(QStringLiteral("autoOutput"), true).toBool();

    intensity_->setValue(settings.value(QStringLiteral("intensity"), 100).toInt());
    global_tone_->setValue(settings.value(QStringLiteral("globalTone"), 0).toInt());
    local_tone_->setValue(settings.value(QStringLiteral("localTone"), 0).toInt());
    local_structure_->setValue(settings.value(QStringLiteral("localStructure"), 100).toInt());
    skin_structure_->setValue(settings.value(QStringLiteral("skinStructure"), 10).toInt());
    style_->setCurrentIndex(settings.value(QStringLiteral("style"), 2).toInt());
    preset_->setCurrentIndex(settings.value(QStringLiteral("preset"), 0).toInt());
    if (model_) model_->setCurrentIndex(settings.value(QStringLiteral("model"), 0).toInt());
    if (image_passes_) image_passes_->setValue(settings.value(QStringLiteral("imagePasses"), 1).toInt());
    double loaded_gamma = settings.value(QStringLiteral("gamma"), 1.0).toDouble();
    if (std::abs(loaded_gamma - 1.4) < 0.05) {
        loaded_gamma = 1.0; // 升级旧版临时 1.4 补偿值回正至物理正确的 1.0
    }
    gamma_->setValue(loaded_gamma);
    auto_mask_->setChecked(settings.value(QStringLiteral("autoMask"), true).toBool());

    if (output_mix_) output_mix_->setValue(settings.value(QStringLiteral("outputMix"), 100).toInt());
    if (detail_boost_) detail_boost_->setValue(settings.value(QStringLiteral("detailBoost"), 100).toInt());
    if (shadow_protect_) shadow_protect_->setValue(settings.value(QStringLiteral("shadowProtect"), 100).toInt());
    if (glow_control_) glow_control_->setValue(settings.value(QStringLiteral("glowControl"), 100).toInt());

    reset_->setCurrentIndex(settings.value(QStringLiteral("reset"), 0).toInt());
    reset_every_->setValue(settings.value(QStringLiteral("resetEvery"), 60).toInt());
    cut_->setText(settings.value(QStringLiteral("cut"), QStringLiteral("0.30")).toString());
    passes_->setValue(settings.value(QStringLiteral("passes"), 1).toInt());
    crf_->setValue(settings.value(QStringLiteral("crf"), 18).toInt());
    codec_->setCurrentIndex(settings.value(QStringLiteral("codec"), 0).toInt());
    if (parallel_) parallel_->setCurrentIndex(settings.value(QStringLiteral("parallel"), 0).toInt());
    fps_->setText(settings.value(QStringLiteral("fps"), QStringLiteral("0")).toString());
    max_frames_->setText(
        settings.value(QStringLiteral("maxFrames"), QStringLiteral("0")).toString());
    model_scale_->setCurrentIndex(settings.value(QStringLiteral("modelScale"), 0).toInt());
    upscale_->setCurrentIndex(settings.value(QStringLiteral("upscale"), 0).toInt());
    dump_->setChecked(settings.value(QStringLiteral("dump"), false).toBool());
    flow_->setChecked(settings.value(QStringLiteral("flow"), false).toBool());
    audio_->setChecked(settings.value(QStringLiteral("audio"), true).toBool());

    const QString curr_out = output_->text().trimmed();
    const bool is_out_png = curr_out.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive);
    const bool is_out_vid = curr_out.endsWith(QStringLiteral(".mp4"), Qt::CaseInsensitive) ||
                            curr_out.endsWith(QStringLiteral(".mkv"), Qt::CaseInsensitive);
    if (curr_out.isEmpty() || auto_output_ || (image && is_out_vid) || (!image && is_out_png)) {
        auto_fill_output(false);
    }

    filling_ = false;
}

void BatchWindow::save_settings() const {
    QSettings settings(settings_path(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("input"), input_->text());
    settings.setValue(QStringLiteral("output"), output_->text());
    settings.setValue(QStringLiteral("autoOutput"), auto_output_);
    settings.setValue(QStringLiteral("snippet"), snippet_->text());
    settings.setValue(QStringLiteral("driver"), driver_->text());
    settings.setValue(QStringLiteral("runtime"), runtime_->text());
    settings.setValue(QStringLiteral("nvapi"), nvapi_->text());
    settings.setValue(QStringLiteral("dumpDir"), dump_dir_->text());
    settings.setValue(QStringLiteral("image"), image_mode_->isChecked());

    settings.setValue(QStringLiteral("intensity"), intensity_->value());
    settings.setValue(QStringLiteral("globalTone"), global_tone_->value());
    settings.setValue(QStringLiteral("localTone"), local_tone_->value());
    settings.setValue(QStringLiteral("localStructure"), local_structure_->value());
    settings.setValue(QStringLiteral("skinStructure"), skin_structure_->value());
    settings.setValue(QStringLiteral("style"), style_->currentIndex());
    settings.setValue(QStringLiteral("preset"), preset_->currentIndex());
    settings.setValue(QStringLiteral("model"), model_->currentIndex());
    settings.setValue(QStringLiteral("gamma"), gamma_->value());
    settings.setValue(QStringLiteral("imagePasses"), image_passes_->value());
    settings.setValue(QStringLiteral("autoMask"), auto_mask_->isChecked());
    if (output_mix_) settings.setValue(QStringLiteral("outputMix"), output_mix_->value());
    if (detail_boost_) settings.setValue(QStringLiteral("detailBoost"), detail_boost_->value());
    if (shadow_protect_) settings.setValue(QStringLiteral("shadowProtect"), shadow_protect_->value());
    if (glow_control_) settings.setValue(QStringLiteral("glowControl"), glow_control_->value());

    settings.setValue(QStringLiteral("reset"), reset_->currentIndex());
    settings.setValue(QStringLiteral("resetEvery"), reset_every_->value());
    settings.setValue(QStringLiteral("cut"), cut_->text());
    settings.setValue(QStringLiteral("passes"), passes_->value());
    settings.setValue(QStringLiteral("crf"), crf_->value());
    settings.setValue(QStringLiteral("codec"), codec_->currentIndex());
    if (parallel_) settings.setValue(QStringLiteral("parallel"), parallel_->currentIndex());
    settings.setValue(QStringLiteral("fps"), fps_->text());
    settings.setValue(QStringLiteral("maxFrames"), max_frames_->text());
    settings.setValue(QStringLiteral("modelScale"), model_scale_->currentIndex());
    settings.setValue(QStringLiteral("upscale"), upscale_->currentIndex());
    settings.setValue(QStringLiteral("dump"), dump_->isChecked());
    settings.setValue(QStringLiteral("flow"), flow_->isChecked());
    settings.setValue(QStringLiteral("audio"), audio_->isChecked());
}

// ---------------------------------------------------------------------------
// Between the controls
// ---------------------------------------------------------------------------

void BatchWindow::browse(QLineEdit *field, bool open) {
    const QString chosen =
        open ? QFileDialog::getOpenFileName(this, tr("选择文件"), field->text())
             : QFileDialog::getSaveFileName(this, tr("输出文件"), field->text());
    if (!chosen.isEmpty()) field->setText(chosen);
}

void BatchWindow::auto_fill_output(bool follow_input) {
    const QString source = input_->text().trimmed();
    if (source.isEmpty()) return;

    // Follow the input's type: picking an image switches to image mode, and the
    // output name changes extension with it. Not done when the radios were what
    // changed -- that would put the mode straight back and leave no way to
    // choose one by hand.
    if (follow_input) {
        filling_ = true;
        const bool image = is_image(source);
        image_mode_->setChecked(image);
        video_mode_->setChecked(!image);
        filling_ = false;
        // The radio-button handlers intentionally ignore signals while the
        // fields are being filled programmatically. Apply the visual mode
        // explicitly after that guard is released; otherwise selecting an
        // image changes the radio label but leaves the video controls visible.
        update_mode_ui();
    }

    const bool is_img = image_mode_->isChecked();
    const QString derived =
        QFileInfo(source).completeBaseName().isEmpty()
            ? source
            : QFileInfo(source).path() + QStringLiteral("/")
                  + QFileInfo(source).completeBaseName()
                  + (is_img ? QStringLiteral("_dlss.png") : QStringLiteral("_dlss.mp4"));
    if (!auto_output_) return;

    filling_ = true;
    output_->setText(derived);
    filling_ = false;
}

void BatchWindow::update_mode_ui() {
    const bool image = image_mode_->isChecked();
    video_panel_->setVisible(!image);
    image_passes_row_->setVisible(image);
}

void BatchWindow::reset_effects() {
    intensity_->setValue(100);
    global_tone_->setValue(0);
    local_tone_->setValue(0);
    local_structure_->setValue(100);
    skin_structure_->setValue(10);
    style_->setCurrentIndex(2);
    preset_->setCurrentIndex(0);
    model_->setCurrentIndex(0);
    model_scale_->setCurrentIndex(0);
    upscale_->setCurrentIndex(0);
    codec_->setCurrentIndex(0);
    auto_mask_->setChecked(true);
    gamma_->setValue(1.0);
    image_passes_->setValue(3);
    passes_->setValue(1);
    reset_->setCurrentIndex(0);
    reset_every_->setValue(60);
    cut_->setText(QStringLiteral("0.30"));
    flow_->setChecked(false);
    if (output_mix_) output_mix_->setValue(100);
    if (detail_boost_) detail_boost_->setValue(100);
    if (shadow_protect_) shadow_protect_->setValue(100);
    if (glow_control_) glow_control_->setValue(100);
}

void BatchWindow::apply_composite_preset(int mix, int detail, int shadow, int glow) {
    if (output_mix_) output_mix_->setValue(mix);
    if (detail_boost_) detail_boost_->setValue(detail);
    if (shadow_protect_) shadow_protect_->setValue(shadow);
    if (glow_control_) glow_control_->setValue(glow);
    if (local_tone_) local_tone_->setValue(0);
    if (style_) style_->setCurrentIndex(2); // 电影
    if (skin_structure_) skin_structure_->setValue(10); // 0.10
    if (auto_mask_) auto_mask_->setChecked(true);
}

// ---------------------------------------------------------------------------
// Running
// ---------------------------------------------------------------------------

bool BatchWindow::validate(QString *problem) const {
    if (input_->text().trimmed().isEmpty()) {
        *problem = tr("请先选择输入文件。");
        return false;
    }
    if (!QFileInfo::exists(input_->text().trimmed())) {
        *problem = tr("输入文件不存在：%1").arg(input_->text().trimmed());
        return false;
    }
    if (output_->text().trimmed().isEmpty()) {
        *problem = tr("无法确定输出文件。");
        return false;
    }
    struct Named { const QString name; const QLineEdit *field; };
    for (const Named &dll : {Named{tr("nvngx_dlssnr.dll"), snippet_},
                             Named{tr("nvcuda.dll"), driver_},
                             Named{tr("nvngx.dll"), runtime_},
                             Named{tr("nvapi64.dll"), nvapi_}}) {
        if (!QFileInfo::exists(dll.field->text().trimmed())) {
            *problem = tr("找不到 %1：%2").arg(dll.name, dll.field->text().trimmed());
            return false;
        }
    }
    return true;
}

QStringList BatchWindow::arguments() const {
    QStringList args;
    const bool image = image_mode_->isChecked();
    const QString in_path = input_->text().trimmed();
    const QString out_file = (is_preview_ && !preview_output_path_.isEmpty()) ? preview_output_path_ : output_->text().trimmed();
    const QString snip = snippet_->text().trimmed();
    const QString driv = driver_->text().trimmed();
    const QString runt = runtime_->text().trimmed();
    const QString nvap = nvapi_->text().trimmed();

    if (is_frame_hold_) {
        args << QStringLiteral("--image") << frame_hold_in_ << frame_hold_out_
             << snip << driv << runt << nvap;
        args << QStringLiteral("--passes") << QString::number(image_passes_->value());
        args << QStringLiteral("--retries") << QStringLiteral("3");
    } else if (image) {
        args << QStringLiteral("--image") << in_path << out_file
             << snip << driv << runt << nvap;
        // A still has no neighbours to disagree with, so repeating the
        // evaluation only deepens the blend -- which is what makes it settle.
        args << QStringLiteral("--passes") << QString::number(image_passes_->value());
        args << QStringLiteral("--retries") << QStringLiteral("3");
    } else {
        args << in_path << out_file << snip << driv << runt << nvap;

        static const char *reset_modes[] = {"auto", "always", "never", "every"};
        int r_idx = reset_->currentIndex();
        if (r_idx < 0 || r_idx >= 4) r_idx = 0;
        QString mode = QString::fromLatin1(reset_modes[r_idx]);
        if (mode == QStringLiteral("every"))
            mode += QStringLiteral("=") + QString::number(reset_every_->value());
        args << QStringLiteral("--reset") << mode;

        // One evaluation per frame. Two or more stack the effect and amplify
        // frame-to-frame differences, which is the flicker.
        args << QStringLiteral("--passes") << QString::number(passes_->value());
        args << QStringLiteral("--flow") << (flow_->isChecked() ? QStringLiteral("1")
                                                               : QStringLiteral("0"));

        bool ok = false;
        const double cut = cut_->text().trimmed().toDouble(&ok);
        if (ok) args << QStringLiteral("--cut-threshold") << number(cut);

        args << QStringLiteral("--crf") << QString::number(crf_->value());

        const double fps = fps_->text().trimmed().toDouble(&ok);
        if (ok && fps > 0) args << QStringLiteral("--fps") << number(fps);

        if (is_preview_) {
            const double eff_fps = (ok && fps > 0) ? fps : 30.0;
            const int preview_frames = (frames_total_ > 0) ? (int)frames_total_ : qMax(30, (int)std::round(eff_fps * 3.0));
            args << QStringLiteral("--max-frames") << QString::number(preview_frames);
            args << QStringLiteral("--parallel") << QStringLiteral("off");
        } else {
            const int max_frames = max_frames_->text().trimmed().toInt(&ok);
            if (ok && max_frames > 0)
                args << QStringLiteral("--max-frames") << QString::number(max_frames);
            if (parallel_) {
                const int p_idx = parallel_->currentIndex();
                if (p_idx == 0)
                    args << QStringLiteral("--parallel") << QStringLiteral("auto");
                else if (p_idx == 1)
                    args << QStringLiteral("--parallel") << QStringLiteral("2");
                else if (p_idx == 2)
                    args << QStringLiteral("--parallel") << QStringLiteral("off");
            }
        }

        static const char *codecs[] = {"auto", "h264_amf", "hevc_amf", "x264"};
        if (codec_->currentIndex() > 0 && codec_->currentIndex() < 4)
            args << QStringLiteral("--encoder") << QString::fromLatin1(codecs[codec_->currentIndex()]);

        if (!audio_->isChecked()) args << QStringLiteral("--no-audio");
        if (dump_->isChecked() && !dump_dir_->text().trimmed().isEmpty())
            args << QStringLiteral("--dump-frames") << dump_dir_->text().trimmed();

        static const double model_scales[] = {1.0, 0.75, 0.50};
        if (model_scale_->currentIndex() > 0 && model_scale_->currentIndex() < 3)
            args << QStringLiteral("--model-scale") << number(model_scales[model_scale_->currentIndex()]);

        static const char *upscale_modes[] = {"native", "quality", "balanced", "performance", "ultra"};
        if (upscale_->currentIndex() > 0 && upscale_->currentIndex() < 5)
            args << QStringLiteral("--upscale-mode") << QString::fromLatin1(upscale_modes[upscale_->currentIndex()]);
    }

    args << QStringLiteral("--intensity") << number(intensity_->value() / 100.0);
    args << QStringLiteral("--global-tone") << number(global_tone_->value() / 100.0);
    args << QStringLiteral("--local-tone") << number(local_tone_->value() / 100.0);
    args << QStringLiteral("--local-structure") << number(local_structure_->value() / 100.0);
    args << QStringLiteral("--skin-structure") << number(skin_structure_->value() / 100.0);
    args << QStringLiteral("--style") << QString::number(qMax(0, style_->currentIndex()));
    args << QStringLiteral("--preset") << QString::number(qMax(0, preset_->currentIndex()));
    if (model_ && model_->currentIndex() > 0)
        args << QStringLiteral("--dlss-model-preset") << model_->currentText();
    args << QStringLiteral("--gamma") << number(gamma_->value());
    if (!auto_mask_->isChecked()) args << QStringLiteral("--no-auto-mask");
    if (output_mix_ && output_mix_->value() != 100)
        args << QStringLiteral("--output-mix") << number(output_mix_->value() / 100.0);
    if (detail_boost_ && detail_boost_->value() != 100)
        args << QStringLiteral("--detail-boost") << number(detail_boost_->value() / 100.0);
    if (shadow_protect_ && shadow_protect_->value() != 100)
        args << QStringLiteral("--shadow-protect") << number(shadow_protect_->value() / 100.0);
    if (glow_control_ && glow_control_->value() != 100)
        args << QStringLiteral("--glow-control") << number(glow_control_->value() / 100.0);
    // Translate first, then run: the parallel translation and the first
    // evaluation contending on the GPU is what produced black frames.
    args << QStringLiteral("--precompile-wait");
    return args;
}

void BatchWindow::start_run() {
    is_preview_ = false;
    preview_output_path_.clear();
    QString problem;
    if (!validate(&problem)) {
        log_line(problem, QColor(255, 90, 60));
        return;
    }
    save_settings();

    const QString exe = application_dir() + QStringLiteral("/video_filter.exe");
    if (!QFileInfo::exists(exe)) {
        log_line(tr("找不到 video_filter.exe：%1").arg(exe), QColor(255, 90, 60));
        return;
    }

    frames_done_ = 0;
    chunk0_max_ = 0;
    chunk1_max_ = 0;
    frames_total_ = -1;
    seconds_ = 0;
    last_frame_detail_.clear();
    error_buffer_.clear();
    progress_->setValue(0);
    status_->setText(tr("启动中…"));

    process_.setProgram(exe);
    process_.setArguments(arguments());
    process_.setWorkingDirectory(application_dir());
    process_.start();

    if (!process_.waitForStarted(5000)) {
        log_line(tr("启动失败：%1").arg(process_.errorString()), QColor(255, 90, 60));
        return;
    }

    user_stopped_ = false;
    running_ = true;
    start_button_->setEnabled(false);
    if (preview_button_) preview_button_->setEnabled(false);
    if (frame_hold_button_) frame_hold_button_->setEnabled(false);
    stop_button_->setEnabled(true);
    timer_->start();
    log_line(tr(">>> 开始%1处理").arg(image_mode_->isChecked() ? tr("图片") : tr("视频")),
             QColor(135, 206, 250));
    if (video_mode_->isChecked()) probe_total();
}

void BatchWindow::start_preview() {
    if (image_mode_->isChecked()) {
        start_run();
        return;
    }
    QString problem;
    if (!validate(&problem)) {
        log_line(problem, QColor(255, 90, 60));
        return;
    }
    save_settings();

    const QString exe = application_dir() + QStringLiteral("/video_filter.exe");
    if (!QFileInfo::exists(exe)) {
        log_line(tr("找不到 video_filter.exe：%1").arg(exe), QColor(255, 90, 60));
        return;
    }

    const QFileInfo out_fi(output_->text().trimmed());
    preview_output_path_ = out_fi.path() + QStringLiteral("/")
                         + out_fi.completeBaseName() + QStringLiteral("_preview3s.")
                         + out_fi.suffix();

    double rate = 0;
    bool ok = false;
    const double user_fps = fps_->text().trimmed().toDouble(&ok);
    if (ok && user_fps > 0) {
        rate = user_fps;
    } else {
        QProcess probe;
        probe.start(QStringLiteral("ffprobe"),
                    {QStringLiteral("-v"), QStringLiteral("error"),
                     QStringLiteral("-select_streams"), QStringLiteral("v:0"),
                     QStringLiteral("-show_entries"), QStringLiteral("stream=r_frame_rate"),
                     QStringLiteral("-of"), QStringLiteral("default=noprint_wrappers=1:nokey=1"),
                     input_->text().trimmed()});
        if (probe.waitForFinished(2000)) {
            const QString out = QString::fromUtf8(probe.readAllStandardOutput()).trimmed();
            if (out.contains(QLatin1Char('/'))) {
                const QStringList bits = out.split(QLatin1Char('/'));
                bool n_ok = false, d_ok = false;
                const double num = bits.value(0).toDouble(&n_ok);
                const double den = bits.value(1).toDouble(&d_ok);
                if (n_ok && d_ok && den > 0) rate = num / den;
            } else {
                rate = out.toDouble();
            }
        }
    }
    const double eff_fps = (rate > 0) ? rate : 30.0;
    const int preview_frames = qMax(30, (int)std::round(eff_fps * 3.0));

    frames_done_ = 0;
    chunk0_max_ = 0;
    chunk1_max_ = 0;
    frames_total_ = preview_frames;
    seconds_ = 0;
    last_frame_detail_.clear();
    error_buffer_.clear();
    progress_->setValue(0);
    status_->setText(tr("启动快速预览中…"));

    is_preview_ = true;

    process_.setProgram(exe);
    process_.setArguments(arguments());
    process_.setWorkingDirectory(application_dir());
    process_.start();

    if (!process_.waitForStarted(5000)) {
        log_line(tr("启动失败：%1").arg(process_.errorString()), QColor(255, 90, 60));
        is_preview_ = false;
        return;
    }

    user_stopped_ = false;
    running_ = true;
    start_button_->setEnabled(false);
    if (preview_button_) preview_button_->setEnabled(false);
    if (frame_hold_button_) frame_hold_button_->setEnabled(false);
    stop_button_->setEnabled(true);
    timer_->start();
    log_line(tr(">>> 开始 3 秒快速预览（共 %1 帧）-> %2").arg(frames_total_).arg(preview_output_path_),
             QColor(135, 206, 250));
}

void BatchWindow::start_frame_hold_compare() {
    QString problem;
    if (!validate(&problem)) {
        log_line(problem, QColor(255, 90, 60));
        return;
    }
    save_settings();

    const QString exe = application_dir() + QStringLiteral("/video_filter.exe");
    if (!QFileInfo::exists(exe)) {
        log_line(tr("找不到 video_filter.exe：%1").arg(exe), QColor(255, 90, 60));
        return;
    }

    const QString source = input_->text().trimmed();
    const QString temp_dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    frame_hold_out_ = temp_dir + QStringLiteral("/dlssnr_frame_hold_out.png");

    if (image_mode_->isChecked() || is_image(source)) {
        frame_hold_in_ = source;
    } else {
        frame_hold_in_ = temp_dir + QStringLiteral("/dlssnr_frame_hold_in.png");
        log_line(tr(">>> 正在从视频截取单帧..."), QColor(135, 206, 250));
        QProcess extract;
        extract.start(QStringLiteral("ffmpeg"),
                      {QStringLiteral("-y"), QStringLiteral("-ss"), QStringLiteral("00:00:01"),
                       QStringLiteral("-i"), source,
                       QStringLiteral("-frames:v"), QStringLiteral("1"),
                       frame_hold_in_});
        if (!extract.waitForFinished(5000) || !QFileInfo::exists(frame_hold_in_)) {
            extract.start(QStringLiteral("ffmpeg"),
                          {QStringLiteral("-y"), QStringLiteral("-i"), source,
                           QStringLiteral("-frames:v"), QStringLiteral("1"),
                           frame_hold_in_});
            if (!extract.waitForFinished(5000) || !QFileInfo::exists(frame_hold_in_)) {
                log_line(tr("从视频截取单帧失败，请检查视频文件与 ffmpeg。"), QColor(255, 90, 60));
                return;
            }
        }
    }

    frames_done_ = 0;
    chunk0_max_ = 0;
    chunk1_max_ = 0;
    frames_total_ = 1;
    seconds_ = 0;
    last_frame_detail_.clear();
    error_buffer_.clear();
    progress_->setValue(0);
    status_->setText(tr("单帧画质增强中…"));

    is_preview_ = false;
    is_frame_hold_ = true;

    process_.setProgram(exe);
    process_.setArguments(arguments());
    process_.setWorkingDirectory(application_dir());
    process_.start();

    if (!process_.waitForStarted(5000)) {
        log_line(tr("启动失败：%1").arg(process_.errorString()), QColor(255, 90, 60));
        is_frame_hold_ = false;
        return;
    }

    user_stopped_ = false;
    running_ = true;
    start_button_->setEnabled(false);
    if (preview_button_) preview_button_->setEnabled(false);
    if (frame_hold_button_) frame_hold_button_->setEnabled(false);
    stop_button_->setEnabled(true);
    timer_->start();
    log_line(tr(">>> 开始单帧定帧降噪增强 -> 准备对比"), QColor(135, 206, 250));
}

void BatchWindow::stop_run() {
    if (prewarm_running_) {
        user_stopped_ = true;
        prewarm_process_.kill();
        log_line(tr(">>> 已请求停止预热"), QColor(255, 165, 0));
        return;
    }
    if (!running_) return;
    user_stopped_ = true;
    process_.kill();
    is_preview_ = false;
    is_frame_hold_ = false;
    log_line(tr(">>> 已请求停止"), QColor(255, 165, 0));
}

// The warm-up needs only the network and the driver: it never touches the
// input, output or ffmpeg side. On NVIDIA machines there is nothing to
// translate -- the driver runs the network as shipped -- so the button is
// best left alone there, and a fully warm cache answers instantly.
void BatchWindow::start_prewarm() {
    if (running_ || prewarm_running_) return;
    if (snippet_->text().isEmpty() || driver_->text().isEmpty()) {
        log_line(tr("预热需要先填入网络库（nvngx_dlssnr.dll）与 CUDA 驱动（nvcuda.dll）路径。"),
                 QColor(255, 170, 60));
        return;
    }
    if (enhancer::precompile_cache_is_warm(snippet_->text().toStdWString(),
                                 driver_->text().toStdWString())) {
        log_line(tr("翻译缓存已是热的，无需预热，可直接开始处理。"), QColor(120, 220, 120));
        return;
    }
    const QString exe = application_dir() + QStringLiteral("/video_filter.exe");
    if (!QFileInfo::exists(exe)) {
        log_line(tr("找不到 video_filter.exe：%1").arg(exe), QColor(255, 90, 60));
        return;
    }

    prewarm_running_ = true;
    start_button_->setEnabled(false);
    if (preview_button_) preview_button_->setEnabled(false);
    if (frame_hold_button_) frame_hold_button_->setEnabled(false);
    prewarm_button_->setEnabled(false);
    stop_button_->setEnabled(true);
    status_->setText(tr("正在预热翻译缓存…"));
    log_line(tr(">>> 开始预热翻译缓存（每个模块落地时都会在这里出现一行）"),
             QColor(135, 206, 250));

    prewarm_process_.setProgram(exe);
    prewarm_process_.setArguments({QStringLiteral("--precompile"), snippet_->text(),
                                   driver_->text()});
    prewarm_process_.setWorkingDirectory(application_dir());
    prewarm_process_.start();
    if (!prewarm_process_.waitForStarted(5000)) {
        log_line(tr("预热进程启动失败：%1").arg(prewarm_process_.errorString()),
                 QColor(255, 90, 60));
        prewarm_running_ = false;
        start_button_->setEnabled(true);
        prewarm_button_->setEnabled(true);
        stop_button_->setEnabled(false);
        status_->setText(tr("就绪"));
    }
}

void BatchWindow::on_prewarm_output() {
    const QByteArray err = prewarm_process_.readAllStandardError();
    const QByteArray out = prewarm_process_.readAllStandardOutput();
    for (const QByteArray *channel : {&err, &out}) {
        const QList<QByteArray> lines = channel->split('\n');
        for (const QByteArray &raw : lines) {
            QString line = QString::fromLocal8Bit(raw).trimmed();
            if (!line.isEmpty()) log_line(line, QColor(200, 200, 200));
        }
    }
}

void BatchWindow::on_prewarm_finished(int code, QProcess::ExitStatus status) {
    prewarm_running_ = false;
    start_button_->setEnabled(true);
    prewarm_button_->setEnabled(true);
    stop_button_->setEnabled(false);
    if (user_stopped_) {
        status_->setText(tr("预热已停止"));
        return;
    }
    if (status == QProcess::CrashExit || code != 0) {
        status_->setText(tr("预热失败"));
        log_line(tr(">>> 预热失败（退出码 %1）。未完成的模块会在下次预热或处理时重试。").arg(code),
                 QColor(255, 90, 60));
        return;
    }
    status_->setText(tr("缓存已就绪，处理将直接从缓存启动。"));
    log_line(tr(">>> 预热完成：全部模块已在本机缓存中，之后每次启动都是秒级。"),
             QColor(120, 220, 120));
}

// Startup guidance, mirroring start_prewarm's own checks: point at the
// button before a first run disappears into a long silent translation.
void BatchWindow::hint_cold_cache() {
    if (snippet_->text().isEmpty() || driver_->text().isEmpty()) return;
    if (enhancer::precompile_cache_is_warm(snippet_->text().toStdWString(),
                                 driver_->text().toStdWString()))
        return;
    log_line(tr("提示：首次使用建议先点击【预热缓存】——把网络翻译进本机缓存（约 20~40 "
                "分钟，仅此一次）。跳过也可以，但首个任务会长时间停留在编译阶段。"),
             QColor(255, 200, 90));
}

void BatchWindow::probe_total() {
    // Off the interface thread would be better, but ffprobe only reads the
    // container here -- it does not decode -- so it returns in well under a
    // second even on a long video, while the run itself goes on regardless.
    QProcess probe;
    probe.start(QStringLiteral("ffprobe"),
                {QStringLiteral("-v"), QStringLiteral("error"),
                 QStringLiteral("-select_streams"), QStringLiteral("v:0"),
                 QStringLiteral("-show_entries"),
                 QStringLiteral("stream=r_frame_rate:format=duration"),
                 QStringLiteral("-of"), QStringLiteral("default=noprint_wrappers=1:nokey=1"),
                 input_->text().trimmed()});
    if (!probe.waitForFinished(5000)) return;

    // Line 0 is r_frame_rate, Line 1 is format=duration.
    double rate = 0;
    double duration = 0;
    const QString out = QString::fromUtf8(probe.readAllStandardOutput());
    const QStringList lines = out.split(QRegularExpression(QStringLiteral("[\r\n]+")),
                                         Qt::SkipEmptyParts);
    for (int i = 0; i < lines.size(); ++i) {
        const QString text = lines[i].trimmed();
        if (i == 0) {
            if (text.contains(QLatin1Char('/'))) {
                const QStringList bits = text.split(QLatin1Char('/'));
                bool n_ok = false, d_ok = false;
                const double num = bits.value(0).toDouble(&n_ok);
                const double den = bits.value(1).toDouble(&d_ok);
                if (n_ok && d_ok && den > 0) rate = num / den;
            } else {
                bool ok = false;
                const double r = text.toDouble(&ok);
                if (ok && r > 0) rate = r;
            }
        } else if (i == 1) {
            bool ok = false;
            const double val = text.toDouble(&ok);
            if (ok && val > 0) duration = val;
        }
    }
    if (rate > 0 && duration > 0) frames_total_ = (long)(rate * duration);
}

void BatchWindow::read_error() {
    // video_filter writes its whole log -- and its ffmpeg children's stderr,
    // which they inherit -- to standard error, and it writes UTF-8.
    error_buffer_.append(process_.readAllStandardError());
    while (true) {
        const int newline = error_buffer_.indexOf('\n');
        if (newline < 0) break;
        const QByteArray raw = error_buffer_.left(newline);
        error_buffer_.remove(0, newline + 1);
        const QString line = QString::fromUtf8(raw).trimmed();
        if (line.isEmpty()) continue;

        const auto frame = frame_re_.match(line);
        if (frame.hasMatch()) {
            const long reported_frame = frame.captured(1).toLongLong();
            if (frames_total_ > 0) {
                const long mid = frames_total_ / 2;
                if (reported_frame >= mid) {
                    chunk1_max_ = qMax(chunk1_max_, reported_frame - mid + 1);
                } else {
                    chunk0_max_ = qMax(chunk0_max_, reported_frame + 1);
                }
                frames_done_ = qMin(frames_total_, chunk0_max_ + chunk1_max_);
            } else {
                frames_done_ = qMax(frames_done_, reported_frame + 1);
            }
            last_frame_detail_ = tr("帧 %1 · 本帧 %2 ms · 平均 %3 ms · 复位 %4 · 空白 %5")
                                 .arg(frames_done_)
                                 .arg(frame.captured(2), frame.captured(3), frame.captured(4),
                                      frame.captured(5));
            status_->setText(tr("已用 %1 秒 · %2").arg(seconds_).arg(last_frame_detail_));
            if (frames_total_ > 0)
                progress_->setValue(qMin(100, (int)(100.0 * frames_done_ / frames_total_)));
            log_line(line, QColor(245, 245, 245));
            continue;
        }

        static const QRegularExpression pre(
            QStringLiteral(R"(\[precompile\] translated (\d+) of (\d+))"));
        const auto translating = pre.match(line);
        if (translating.hasMatch()) {
            const long long done = translating.captured(1).toLongLong();
            const long long total = translating.captured(2).toLongLong();
            if (total > 0) {
                const long long clamped_done = qBound(0LL, done, total);
                progress_->setValue(qMin(99, (int)(clamped_done * 99 / total)));
            }
            log_line(line, QColor(255, 215, 0));
            continue;
        }

        if (line.startsWith(QStringLiteral("[info]")))
            log_line(line, QColor(192, 192, 192));
        else if (line.startsWith(QStringLiteral("[composite]")))
            log_line(line, QColor(100, 220, 255));
        else if (line.contains(QStringLiteral("[warn]")))
            log_line(line, QColor(255, 165, 0));
        else if (line.startsWith(QStringLiteral("[FAIL]")))
            log_line(line, QColor(255, 90, 60));
        else if (line.startsWith(QStringLiteral("[done]"))) {
            progress_->setValue(100);
            log_line(line, QColor(144, 238, 144));
        } else
            log_line(line, QColor(245, 245, 245));
    }
}

void BatchWindow::on_finished(int code, QProcess::ExitStatus exit_status) {
    // Anything still in the pipe: the last lines can arrive with the exit.
    read_error();
    if (!error_buffer_.isEmpty()) {
        const QString tail = QString::fromUtf8(error_buffer_).trimmed();
        error_buffer_.clear();
        if (!tail.isEmpty()) log_line(tail, QColor(245, 245, 245));
    }

    running_ = false;
    start_button_->setEnabled(true);
    if (preview_button_) preview_button_->setEnabled(true);
    if (frame_hold_button_) frame_hold_button_->setEnabled(true);
    stop_button_->setEnabled(false);
    timer_->stop();

    if (frames_done_ == 0 && frames_total_ < 0 && progress_->value() < 100)
        progress_->setValue(0);  // failed early: do not leave a bar part-filled

    if (user_stopped_) {
        status_->setText(tr("已由用户停止"));
        log_line(tr(">>> 处理已被用户中止"), QColor(255, 165, 0));
    } else if (exit_status == QProcess::CrashExit) {
        status_->setText(tr("处理异常崩溃（exit=%1），见日志").arg(code));
        log_line(tr(">>> 进程异常崩溃 exit=%1").arg(code), QColor(255, 90, 60));
    } else if (code != 0) {
        status_->setText(tr("处理失败（exit=%1），见日志").arg(code));
        log_line(tr(">>> 结束 exit=%1").arg(code), QColor(255, 90, 60));
    } else {
        status_->setText(frames_done_ > 0 ? tr("完成：%1 帧").arg(frames_done_) : tr("处理结束"));
        log_line(tr(">>> 结束 exit=%1").arg(code), QColor(144, 238, 144));
    }
    user_stopped_ = false;

    if (is_preview_) {
        const QString prev_out = preview_output_path_;
        is_preview_ = false;
        if (code == 0 && QFileInfo::exists(prev_out)) {
            log_line(tr(">>> 3 秒快速预览完成，正在自动打开对比窗口..."), QColor(144, 238, 144));
            auto *dialog = new CompareDialog(input_->text().trimmed(), prev_out, this);
            dialog->setAttribute(Qt::WA_DeleteOnClose);
            dialog->show();
        }
    }

    if (is_frame_hold_) {
        is_frame_hold_ = false;
        if (code == 0 && QFileInfo::exists(frame_hold_out_)) {
            log_line(tr(">>> 单帧定帧增强完成，正在自动打开对比窗口..."), QColor(144, 238, 144));
            auto *dialog = new CompareDialog(frame_hold_in_, frame_hold_out_, this);
            dialog->setAttribute(Qt::WA_DeleteOnClose);
            dialog->show();
        }
    }
}

void BatchWindow::on_second_tick() {
    if (!running_) return;
    seconds_++;
    if (!last_frame_detail_.isEmpty()) {
        status_->setText(tr("已用 %1 秒 · %2").arg(seconds_).arg(last_frame_detail_));
    } else {
        status_->setText(tr("处理中… 已用 %1 秒 · 帧 %2%3")
                             .arg(seconds_)
                             .arg(frames_done_)
                             .arg(frames_total_ > 0 ? tr(" / ~%1").arg(frames_total_) : QString()));
    }
}

void BatchWindow::log_line(const QString &line, const QColor &colour) {
    QTextCharFormat format;
    format.setForeground(colour);
    QTextCursor cursor(log_->document());
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(line + QLatin1Char('\n'), format);
    log_->ensureCursorVisible();
}

void BatchWindow::open_compare() {
    const QString before = input_->text().trimmed();
    const QString after = output_->text().trimmed();
    if (!QFileInfo::exists(before) || !QFileInfo::exists(after)) {
        log_line(tr("请先确认原文件和输出文件都存在，再打开对比。"), QColor(255, 165, 0));
        return;
    }
    auto *dialog = new CompareDialog(before, after, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
}

// ---------------------------------------------------------------------------
// Window events
// ---------------------------------------------------------------------------

void BatchWindow::dragEnterEvent(QDragEnterEvent *event) {
    if (event->mimeData()->hasUrls()) event->acceptProposedAction();
}

void BatchWindow::dropEvent(QDropEvent *event) {
    const QList<QUrl> urls = event->mimeData()->urls();
    if (urls.isEmpty()) return;
    input_->setText(urls.first().toLocalFile());
}

void BatchWindow::closeEvent(QCloseEvent *event) {
    if (running_) process_.kill();
    save_settings();
    QMainWindow::closeEvent(event);
}

} // namespace batch
