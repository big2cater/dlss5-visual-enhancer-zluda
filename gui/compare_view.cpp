#include "compare_view.h"

#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QProcess>
#include <QStandardPaths>
#include <QVBoxLayout>

namespace batch {

bool is_image_file(const QString &path) {
    const QString suffix = QFileInfo(path).suffix().toLower();
    return suffix == QStringLiteral("png") || suffix == QStringLiteral("jpg")
           || suffix == QStringLiteral("jpeg") || suffix == QStringLiteral("bmp")
           || suffix == QStringLiteral("webp") || suffix == QStringLiteral("tif")
           || suffix == QStringLiteral("tiff");
}

CompareView::CompareView(const QPixmap &before, const QPixmap &after, QWidget *parent)
    : QWidget(parent), before_(before), after_(after) {
    setCursor(Qt::SplitHCursor);
    setMinimumSize(320, 200);
}

QRect CompareView::image_rect() const {
    if (before_.isNull()) return {};
    const QSize box = size();
    const double scale =
        qMin((double)box.width() / before_.width(), (double)box.height() / before_.height());
    const int w = qMax(1, qRound(before_.width() * scale));
    const int h = qMax(1, qRound(before_.height() * scale));
    return QRect((box.width() - w) / 2, (box.height() - h) / 2, w, h);
}

void CompareView::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.fillRect(rect(), QColor(24, 24, 28));
    if (before_.isNull()) return;

    const QRect target = image_rect();
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawPixmap(target, before_);

    const int x = qRound(width() * split_);
    painter.save();
    painter.setClipRect(QRect(x, 0, width() - x, height()));
    painter.drawPixmap(target, after_);
    painter.restore();

    painter.setPen(QPen(Qt::white, 2));
    painter.drawLine(x, 0, x, height());

    painter.setPen(Qt::white);
    painter.drawText(QRect(8, 8, 120, 22), Qt::AlignLeft | Qt::AlignVCenter, tr("原文件"));
    painter.drawText(QRect(width() - 128, 8, 120, 22), Qt::AlignRight | Qt::AlignVCenter,
                     tr("处理结果"));
}

void CompareView::mousePressEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton) return;
    dragging_ = true;
    split_ = qBound(0.02, (double)event->position().x() / qMax(1, width()), 0.98);
    update();
}

void CompareView::mouseMoveEvent(QMouseEvent *event) {
    if (!dragging_) return;
    split_ = qBound(0.02, (double)event->position().x() / qMax(1, width()), 0.98);
    update();
}

void CompareView::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) dragging_ = false;
}

void CompareView::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    update();
}

// ---------------------------------------------------------------------------

CompareDialog::CompareDialog(const QString &before_path, const QString &after_path, QWidget *parent)
    : QDialog(parent) {
    setWindowTitle(tr("原文件 / 处理结果对比"));
    resize(1100, 760);

    QString before = before_path;
    QString after = after_path;
    if (!is_image_file(before)) before = frame_of(before, QStringLiteral("before"));
    if (!is_image_file(after)) after = frame_of(after, QStringLiteral("after"));

    QPixmap before_pixmap(before);
    QPixmap after_pixmap(after);

    auto *layout = new QVBoxLayout(this);
    if (before_pixmap.isNull() || after_pixmap.isNull()) {
        layout->addWidget(new QLabel(tr("无法读取这两个文件进行对比。"), this));
        return;
    }
    auto *view = new CompareView(before_pixmap, after_pixmap, this);
    layout->addWidget(view, 1);
    layout->addWidget(new QLabel(tr("拖动分割线比较原文件和处理结果"), this));
}

CompareDialog::~CompareDialog() {
    for (const QString &path : temporary_) QFile::remove(path);
}

QString CompareDialog::frame_of(const QString &video, const QString &stem) {
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::TempLocation) + QStringLiteral("/dlssnr");
    QDir().mkpath(dir);
    const QString png = dir + QStringLiteral("/compare_") + stem + QStringLiteral("_")
                        + QString::number((quintptr)this, 16) + QStringLiteral(".png");

    QProcess ffmpeg;
    ffmpeg.start(QStringLiteral("ffmpeg"),
                 {QStringLiteral("-nostdin"), QStringLiteral("-v"), QStringLiteral("error"),
                  QStringLiteral("-i"), video, QStringLiteral("-frames:v"), QStringLiteral("1"),
                  QStringLiteral("-y"), png});
    if (!ffmpeg.waitForFinished(60000) || ffmpeg.exitCode() != 0) {
        QFile::remove(png);
        return {};
    }
    // Removed when the window closes, along with the other one.
    temporary_.append(png);
    return png;
}

} // namespace batch
