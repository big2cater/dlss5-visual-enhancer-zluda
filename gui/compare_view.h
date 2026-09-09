// Before/after comparison: one picture, a divider the window drags.
//
// Both images are drawn into the same rectangle at the same size, the second
// one clipped to everything right of the divider, so what is on either side is
// the same part of the picture and the only thing that changes across the line
// is which of the two it came from.

#pragma once

#include <QDialog>
#include <QPixmap>
#include <QStringList>

class QLabel;
class QMouseEvent;
class QPaintEvent;
class QResizeEvent;

namespace batch {

// Whether a file can be handed to QPixmap directly. A video cannot, and has to
// have a frame pulled out of it first.
bool is_image_file(const QString &path);

class CompareView : public QWidget {
    Q_OBJECT
public:
    CompareView(const QPixmap &before, const QPixmap &after, QWidget *parent = nullptr);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    // Where the picture goes: as large as the widget allows, keeping its shape.
    QRect image_rect() const;

    QPixmap before_;
    QPixmap after_;
    double split_ = 0.5;
    bool dragging_ = false;
};

// Loads the two files and shows them. A video cannot be handed to the view
// directly, so one frame of each is pulled out with ffmpeg first and deleted
// again when the window closes.
class CompareDialog : public QDialog {
    Q_OBJECT
public:
    CompareDialog(const QString &before_path, const QString &after_path, QWidget *parent = nullptr);
    ~CompareDialog() override;

private:
    // Pulls the first frame out of a video. Returns the empty string on failure.
    QString frame_of(const QString &video, const QString &stem);

    QStringList temporary_;
};

} // namespace batch
