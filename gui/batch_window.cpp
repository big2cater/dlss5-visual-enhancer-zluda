#include "batch_window.h"

#include "compare_view.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QColor>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDropEvent>
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

QString settings_path() {
    return QCoreApplication::applicationDirPath() + QStringLiteral("/dlssnr_gui.ini");
}

} // namespace

BatchWindow::BatchWindow() {
    setWindowTitle(tr("DLSS 5 神经渲染滤镜 (AMD/ZLUDA)"));
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
    setMinimumSize(900, 700);
    const QRect screen = QGuiApplication::primaryScreen()
                              ? QGuiApplication::primaryScreen()->availableGeometry()
                              : QRect(0, 0, 1440, 900);
    resize(qMin(1280, qMax(1000, screen.width() - 80)),
           qMin(900, qMax(700, screen.height() - 80)));

    // Match the frame summary anywhere in a line. ffmpeg can prefix inherited
    // diagnostics and different builds use 5 or 6 frame digits.
    frame_re_ = QRegularExpression(
        QStringLiteral(R"(\[\s*(\d+)\]\s+([\d.]+)\s+ms\s+\(avg\s+([\d.]+),\s*reset=(\d+),\s*blanks=(\d+)\))"));

    timer_ = new QTimer(this);
    timer_->setInterval(1000);
    connect(timer_, &QTimer::timeout, this, &BatchWindow::on_second_tick);

    connect(&process_, &QProcess::readyReadStandardError, this, &BatchWindow::read_error);
    connect(&process_, &QProcess::finished, this, &BatchWindow::on_finished);

    load_settings();
    update_mode_ui();
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
        if (!filling_) auto_fill_output(true);
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
    mode_layout->addStretch(1);

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
    auto *box = new QGroupBox(tr("参数"));
    auto *columns = new QHBoxLayout(box);
    columns->addWidget(build_left_column(), 1);
    columns->addWidget(build_right_column(), 1);
    return box;
}

QWidget *BatchWindow::build_left_column() {
    auto *column = new QWidget;
    auto *form = new QFormLayout(column);

    form->addRow(tr("强度"), slider_row(200, 100, intensity_, intensity_value_));
    form->addRow(tr("全局色调"), slider_row(100, 0, global_tone_, global_tone_value_));
    form->addRow(tr("局部色调"), slider_row(200, 100, local_tone_, local_tone_value_));
    form->addRow(tr("局部结构"), slider_row(200, 100, local_structure_, local_structure_value_));
    form->addRow(tr("皮肤结构 (防塑料感)"), slider_row(100, 0, skin_structure_, skin_structure_value_));
    skin_structure_->setToolTip(
        tr("人脸/皮肤纹理防过度平滑保护：调高此项可在强力降噪的同时保护人像面部微毛孔与天然皮肤质感，避免塑料脸或假面感。"));

    style_ = new QComboBox;
    style_->addItems({tr("0 - 默认"), tr("1 - 自然"), tr("2 - 电影")});
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

    // Lives in this column, not the video panel, so it shows in both modes:
    // the video panel is hidden in single-image mode, which had left the
    // checkbox out of reach there. The backend takes --no-auto-mask either way.
    auto_mask_ = new QCheckBox(tr("自动遮罩"));
    auto_mask_->setChecked(true);
    auto_mask_->setToolTip(
        tr("自动生成遮罩，只对被识别为画面的区域施加滤镜，保留字幕/UI 等。视频和单图模式都生效。"));
    form->addRow(QString(), auto_mask_);

    // Only for a still: repeating the evaluation is the nearest thing to a
    // scene standing still, and video cannot have it (see passes_ below).
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

    // Below 输出伽马, in a row of its own: up among the other settings it read
    // as belonging to the ones above it rather than to the column as a whole.
    auto *reset_button = new QPushButton(tr("恢复默认参数"));
    connect(reset_button, &QPushButton::clicked, this, &BatchWindow::reset_effects);
    form->addRow(QString(), reset_button);

    return column;
}

QWidget *BatchWindow::build_right_column() {
    auto *column = new QWidget;
    auto *stack = new QVBoxLayout(column);
    stack->setContentsMargins(0, 0, 0, 0);

    // ---- video ----------------------------------------------------------
    video_panel_ = new QWidget;
    auto *form = new QFormLayout(video_panel_);
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
        tr("超分输出模式：经 DLSS 5 神经增强后，超分放大至指定倍率的目标分辨率并进行硬件编码。"));
    form->addRow(tr("Upscaling 输出"), upscale_);

    // Three columns rather than one flow row: the path field takes whatever the
    // check box and the button leave, which is the most it can get, and nothing
    // wraps to a second line that is not there.
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

    flow_ = new QCheckBox(tr("运动向量引导"));
    flow_->setToolTip(tr("优先使用 D3D12 GPU 计算运动向量，再自动回退 CPU。默认关。"));
    audio_ = new QCheckBox(tr("音轨直通"));
    // auto_mask_ lives in the left column so it is visible in single-image
    // mode too; see build_left_column.
    auto *switches = new QWidget;
    auto *switch_layout = new QHBoxLayout(switches);
    switch_layout->setContentsMargins(0, 0, 0, 0);
    switch_layout->addWidget(flow_);
    switch_layout->addWidget(audio_);
    switch_layout->addStretch(1);
    form->addRow(switches);

    stack->addWidget(video_panel_);

    // ---- image ----------------------------------------------------------
    image_panel_ = new QWidget;
    auto *image_layout = new QVBoxLayout(image_panel_);
    image_layout->setContentsMargins(0, 0, 0, 0);
    auto *note = new QLabel(tr("单图模式\r\n重复处理次数和左侧参数会应用到当前图片。\r\n"
                               "输出格式：PNG。"));
    note->setTextFormat(Qt::PlainText);
    note->setWordWrap(true);
    note->setAlignment(Qt::AlignTop);
    image_layout->addWidget(note);
    image_layout->addStretch(1);

    stack->addWidget(image_panel_);
    return column;
}

QWidget *BatchWindow::build_run() {
    auto *box = new QGroupBox(tr("运行"));
    auto *layout = new QHBoxLayout(box);

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

    intensity_->setValue(settings.value(QStringLiteral("intensity"), 100).toInt());
    global_tone_->setValue(settings.value(QStringLiteral("globalTone"), 0).toInt());
    local_tone_->setValue(settings.value(QStringLiteral("localTone"), 100).toInt());
    local_structure_->setValue(settings.value(QStringLiteral("localStructure"), 100).toInt());
    skin_structure_->setValue(settings.value(QStringLiteral("skinStructure"), 0).toInt());
    style_->setCurrentIndex(settings.value(QStringLiteral("style"), 0).toInt());
    preset_->setCurrentIndex(settings.value(QStringLiteral("preset"), 0).toInt());
    model_->setCurrentIndex(settings.value(QStringLiteral("model"), 0).toInt());
    gamma_->setValue(settings.value(QStringLiteral("gamma"), 1.0).toDouble());
    image_passes_->setValue(settings.value(QStringLiteral("imagePasses"), 3).toInt());
    auto_mask_->setChecked(settings.value(QStringLiteral("autoMask"), true).toBool());

    reset_->setCurrentIndex(settings.value(QStringLiteral("reset"), 0).toInt());
    reset_every_->setValue(settings.value(QStringLiteral("resetEvery"), 60).toInt());
    cut_->setText(settings.value(QStringLiteral("cut"), QStringLiteral("0.30")).toString());
    passes_->setValue(settings.value(QStringLiteral("passes"), 1).toInt());
    crf_->setValue(settings.value(QStringLiteral("crf"), 18).toInt());
    codec_->setCurrentIndex(settings.value(QStringLiteral("codec"), 0).toInt());
    fps_->setText(settings.value(QStringLiteral("fps"), QStringLiteral("0")).toString());
    max_frames_->setText(
        settings.value(QStringLiteral("maxFrames"), QStringLiteral("0")).toString());
    model_scale_->setCurrentIndex(settings.value(QStringLiteral("modelScale"), 0).toInt());
    upscale_->setCurrentIndex(settings.value(QStringLiteral("upscale"), 0).toInt());
    dump_->setChecked(settings.value(QStringLiteral("dump"), false).toBool());
    flow_->setChecked(settings.value(QStringLiteral("flow"), false).toBool());
    audio_->setChecked(settings.value(QStringLiteral("audio"), true).toBool());

    filling_ = false;
}

void BatchWindow::save_settings() const {
    QSettings settings(settings_path(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("input"), input_->text());
    settings.setValue(QStringLiteral("output"), output_->text());
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

    settings.setValue(QStringLiteral("reset"), reset_->currentIndex());
    settings.setValue(QStringLiteral("resetEvery"), reset_every_->value());
    settings.setValue(QStringLiteral("cut"), cut_->text());
    settings.setValue(QStringLiteral("passes"), passes_->value());
    settings.setValue(QStringLiteral("crf"), crf_->value());
    settings.setValue(QStringLiteral("codec"), codec_->currentIndex());
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

    const QString derived =
        QFileInfo(source).completeBaseName().isEmpty()
            ? source
            : QFileInfo(source).path() + QStringLiteral("/")
                  + QFileInfo(source).completeBaseName()
                  + (is_image(source) ? QStringLiteral("_dlss.png") : QStringLiteral("_dlss.mp4"));
    if (!auto_output_) return;

    filling_ = true;
    output_->setText(derived);
    filling_ = false;
}

void BatchWindow::update_mode_ui() {
    const bool image = image_mode_->isChecked();
    video_panel_->setVisible(!image);
    image_panel_->setVisible(image);
    image_passes_row_->setVisible(image);
}

void BatchWindow::reset_effects() {
    intensity_->setValue(100);
    global_tone_->setValue(0);
    local_tone_->setValue(100);
    local_structure_->setValue(100);
    skin_structure_->setValue(0);
    style_->setCurrentIndex(0);
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
    const QString out_file = (is_preview_ && !preview_output_path_.isEmpty()) ? preview_output_path_ : output_->text();

    if (is_frame_hold_) {
        args << QStringLiteral("--image") << frame_hold_in_ << frame_hold_out_
             << snippet_->text() << driver_->text() << runtime_->text() << nvapi_->text();
        args << QStringLiteral("--passes") << QString::number(image_passes_->value());
    } else if (image) {
        args << QStringLiteral("--image") << input_->text() << out_file
             << snippet_->text() << driver_->text() << runtime_->text() << nvapi_->text();
        // A still has no neighbours to disagree with, so repeating the
        // evaluation only deepens the blend -- which is what makes it settle.
        args << QStringLiteral("--passes") << QString::number(image_passes_->value());
    } else {
        args << input_->text() << out_file << snippet_->text() << driver_->text()
             << runtime_->text() << nvapi_->text();

        static const char *reset_modes[] = {"auto", "always", "never", "every"};
        QString mode = QString::fromLatin1(reset_modes[reset_->currentIndex()]);
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
        } else {
            const int max_frames = max_frames_->text().trimmed().toInt(&ok);
            if (ok && max_frames > 0)
                args << QStringLiteral("--max-frames") << QString::number(max_frames);
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
    args << QStringLiteral("--style") << QString::number(style_->currentIndex());
    args << QStringLiteral("--preset") << QString::number(preset_->currentIndex());
    if (model_->currentIndex() > 0)
        args << QStringLiteral("--dlss-model-preset") << model_->currentText();
    args << QStringLiteral("--gamma") << number(gamma_->value());
    if (!auto_mask_->isChecked()) args << QStringLiteral("--no-auto-mask");
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
    frames_total_ = -1;
    seconds_ = 0;
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
    frames_total_ = preview_frames;
    seconds_ = 0;
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
    frame_hold_out_ = application_dir() + QStringLiteral("/frame_hold_out.png");

    if (image_mode_->isChecked() || is_image(source)) {
        frame_hold_in_ = source;
    } else {
        frame_hold_in_ = application_dir() + QStringLiteral("/frame_hold_in.png");
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
    frames_total_ = 1;
    seconds_ = 0;
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

    running_ = true;
    start_button_->setEnabled(false);
    if (preview_button_) preview_button_->setEnabled(false);
    if (frame_hold_button_) frame_hold_button_->setEnabled(false);
    stop_button_->setEnabled(true);
    timer_->start();
    log_line(tr(">>> 开始单帧定帧降噪增强 -> 准备对比"), QColor(135, 206, 250));
}

void BatchWindow::stop_run() {
    if (!running_) return;
    process_.kill();
    is_preview_ = false;
    is_frame_hold_ = false;
    log_line(tr(">>> 已请求停止"), QColor(255, 165, 0));
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
                 input_->text()});
    if (!probe.waitForFinished(5000)) return;

    // Parse values independently. CSV output can combine them on one line,
    // which previously left frames_total_ unset and the progress bar at 0%.
    double rate = 0;
    double duration = 0;
    const QString out = QString::fromUtf8(probe.readAllStandardOutput());
    for (const QString &part : out.split(QRegularExpression(QStringLiteral("[\\s,]+")),
                                         Qt::SkipEmptyParts)) {
        const QString text = part.trimmed();
        if (text.contains(QLatin1Char('/'))) {
            const QStringList bits = text.split(QLatin1Char('/'));
            bool n_ok = false, d_ok = false;
            const double num = bits.value(0).toDouble(&n_ok);
            const double den = bits.value(1).toDouble(&d_ok);
            if (n_ok && d_ok && den > 0) rate = num / den;
        } else {
            bool ok = false;
            const double value = text.toDouble(&ok);
            if (ok && value > 0 && value != rate) duration = value;
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
            frames_done_ = qMax(frames_done_, reported_frame + 1);
            status_->setText(tr("帧 %1 · 本帧 %2 ms · 平均 %3 ms · 复位 %4 · 空白 %5")
                                 .arg(frames_done_)
                                 .arg(frame.captured(2), frame.captured(3), frame.captured(4),
                                      frame.captured(5)));
            if (frames_total_ > 0)
                progress_->setValue(qMin(100, (int)(100.0 * frames_done_ / frames_total_)));
            log_line(line, QColor(245, 245, 245));
            continue;
        }

        static const QRegularExpression pre(
            QStringLiteral(R"(\[precompile\] translated (\d+) of (\d+))"));
        const auto translating = pre.match(line);
        if (translating.hasMatch()) {
            const int done = translating.captured(1).toInt();
            const int total = translating.captured(2).toInt();
            if (total > 0) progress_->setValue(qMin(99, done * 99 / total));
            log_line(line, QColor(255, 215, 0));
            continue;
        }

        if (line.startsWith(QStringLiteral("[info]")))
            log_line(line, QColor(192, 192, 192));
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

void BatchWindow::on_finished(int code, QProcess::ExitStatus) {
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
    status_->setText(frames_done_ > 0 ? tr("完成：%1 帧").arg(frames_done_) : tr("处理结束"));
    if (code != 0) status_->setText(tr("处理失败（exit=%1），见日志").arg(code));
    log_line(tr(">>> 结束 exit=%1").arg(code),
             code == 0 ? QColor(144, 238, 144) : QColor(255, 90, 60));

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
    status_->setText(tr("处理中… 已用 %1 秒 · 帧 %2%3")
                         .arg(seconds_)
                         .arg(frames_done_)
                         .arg(frames_total_ > 0 ? tr(" / ~%1").arg(frames_total_) : QString()));
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
