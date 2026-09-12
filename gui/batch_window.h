// The batch window: the same job the C# DLSSNRFilter window does, in Qt.
//
// The window does no processing of its own. It collects paths and settings,
// turns them into the command line video_filter already understands, and shows
// what the child prints while it works. That split is why the same job can be
// started from a script, from this window or from the C# one, and why a crash
// inside the network cannot take the interface down with it.
//
// Nothing here scales with the DPI by hand. Qt's layout engine asks every
// control how much room it needs at the font and scale factor actually in use,
// and gives it that -- which is the whole reason for this window existing: the
// WinForms one wrote every size down in 96-dpi pixels and then multiplied it,
// so every alignment in it had to be found and fixed by hand.

#pragma once

#include <QMainWindow>
#include <QByteArray>
#include <QProcess>
#include <QRegularExpression>

class QCheckBox;
class QCloseEvent;
class QComboBox;
class QDoubleSpinBox;
class QDragEnterEvent;
class QDropEvent;
class QFormLayout;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QRadioButton;
class QSlider;
class QSpinBox;
class QTimer;

namespace batch {

class BatchWindow : public QMainWindow {
    Q_OBJECT
public:
    BatchWindow();

protected:
    void closeEvent(QCloseEvent *event) override;
    // Dropping a file on the window is the shortest way to fill in the input.
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    QWidget *build_files();
    QWidget *build_parameters();
    QWidget *build_left_column();
    QWidget *build_composite_column();
    QWidget *build_video_column();
    QWidget *build_run();
    QLineEdit *add_path_row(QFormLayout *form, const QString &caption, bool open);

    void browse(QLineEdit *field, bool open);
    // follow_input: also switch between video and image mode to match the file
    // that was picked. False when the radios themselves were the change, so
    // choosing a mode by hand is not undone straight away.
    void auto_fill_output(bool follow_input);
    void update_mode_ui();
    void reset_effects();
    void apply_composite_preset(int mix, int detail, int shadow, int glow);

    void start_run();
    void start_preview();
    void start_frame_hold_compare();
    void stop_run();
    void open_compare();
    void probe_total();
    void read_error();
    void on_finished(int code, QProcess::ExitStatus status);
    void on_second_tick();
    // Translates the network into ZLUDA's cache ahead of the first job: the
    // same work the first run would trigger, offered on its own with the
    // module progress streaming into the log.
    void start_prewarm();
    void on_prewarm_output();
    void on_prewarm_finished(int code, QProcess::ExitStatus status);
    void hint_cold_cache();

    QStringList arguments() const;
    bool validate(QString *problem) const;
    void log_line(const QString &line, const QColor &colour);
    void load_settings();
    void save_settings() const;

    static bool is_image(const QString &path);
    static QString application_dir();
    static QString number(double value);

    // ---- files ----------------------------------------------------------
    QLineEdit *input_ = nullptr;
    QLineEdit *output_ = nullptr;
    QLineEdit *snippet_ = nullptr;
    QLineEdit *driver_ = nullptr;
    QLineEdit *runtime_ = nullptr;
    QLineEdit *nvapi_ = nullptr;
    QRadioButton *video_mode_ = nullptr;
    QRadioButton *image_mode_ = nullptr;

    // ---- effects --------------------------------------------------------
    QSlider *intensity_ = nullptr;
    QSlider *global_tone_ = nullptr;
    QSlider *local_tone_ = nullptr;
    QSlider *local_structure_ = nullptr;
    QSlider *skin_structure_ = nullptr;
    QLabel *intensity_value_ = nullptr;
    QLabel *global_tone_value_ = nullptr;
    QLabel *local_tone_value_ = nullptr;
    QLabel *local_structure_value_ = nullptr;
    QLabel *skin_structure_value_ = nullptr;
    QComboBox *style_ = nullptr;
    QComboBox *preset_ = nullptr;
    QComboBox *model_ = nullptr;
    QDoubleSpinBox *gamma_ = nullptr;
    QSpinBox *image_passes_ = nullptr;
    QCheckBox *auto_mask_ = nullptr;

    // ---- video ----------------------------------------------------------
    QComboBox *reset_ = nullptr;
    QSpinBox *reset_every_ = nullptr;
    QLineEdit *cut_ = nullptr;
    QSpinBox *passes_ = nullptr;
    QSpinBox *crf_ = nullptr;
    QComboBox *codec_ = nullptr;
    QComboBox *parallel_ = nullptr;
    QLineEdit *fps_ = nullptr;
    QLineEdit *max_frames_ = nullptr;
    QComboBox *model_scale_ = nullptr;
    QComboBox *upscale_ = nullptr;
    QCheckBox *dump_ = nullptr;
    QLineEdit *dump_dir_ = nullptr;
    QCheckBox *flow_ = nullptr;
    QCheckBox *audio_ = nullptr;

    QWidget *video_panel_ = nullptr;
    QWidget *composite_panel_ = nullptr;
    QWidget *image_passes_row_ = nullptr;

    // ---- composite (schemes 3, 4, 5) ------------------------------------
    QSlider *output_mix_ = nullptr;
    QSlider *detail_boost_ = nullptr;
    QSlider *shadow_protect_ = nullptr;
    QSlider *glow_control_ = nullptr;
    QLabel *output_mix_value_ = nullptr;
    QLabel *detail_boost_value_ = nullptr;
    QLabel *shadow_protect_value_ = nullptr;
    QLabel *glow_control_value_ = nullptr;
    QPushButton *btn_preset_film_ = nullptr;
    QPushButton *btn_preset_soft_ = nullptr;
    QPushButton *btn_preset_macro_ = nullptr;
    QPushButton *btn_preset_black_ = nullptr;
    QPushButton *btn_preset_motion_ = nullptr;

    // ---- run ------------------------------------------------------------
    QPushButton *start_button_ = nullptr;
    QPushButton *preview_button_ = nullptr;
    QPushButton *frame_hold_button_ = nullptr;
    QPushButton *stop_button_ = nullptr;
    QPushButton *compare_button_ = nullptr;
    QPushButton *prewarm_button_ = nullptr;
    QProgressBar *progress_ = nullptr;
    QLabel *status_ = nullptr;
    QPlainTextEdit *log_ = nullptr;

    QProcess process_;
    // The warm-up runs video_filter --precompile on its own process so it
    // never contends with a job for process_.
    QProcess prewarm_process_;
    QTimer *timer_ = nullptr;
    bool running_ = false;
    bool prewarm_running_ = false;
    bool is_preview_ = false;
    bool is_frame_hold_ = false;
    QString preview_output_path_;
    QString frame_hold_in_;
    QString frame_hold_out_;
    long frames_done_ = 0;
    long chunk0_max_ = 0;
    long chunk1_max_ = 0;
    long frames_total_ = -1;
    int seconds_ = 0;
    QString last_frame_detail_;
    // The output path was derived from the input, so it follows it until the
    // user types one of their own.
    bool auto_output_ = true;
    bool user_stopped_ = false;
    // Guards the fields while they are being filled in programmatically: the
    // text-changed signal cannot tell that from the user typing.
    bool filling_ = false;

    QRegularExpression frame_re_;
    QByteArray error_buffer_;
};

} // namespace batch
