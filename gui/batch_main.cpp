#include "batch_window.h"

#include <QApplication>
#include <QGuiApplication>
#include <QIcon>

int main(int argc, char **argv) {
    // Windows reports fractional scaling -- 125 %, 150 % -- and Qt rounds those
    // to whole numbers unless told otherwise. Rounded, the window is laid out
    // at one scale and drawn at another. Has to be set before the application
    // object exists, which is why it is out here rather than in the window.
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    QApplication application(argc, argv);
    QApplication::setApplicationName(QStringLiteral("DLSSNRFilter"));
    QApplication::setOrganizationName(QStringLiteral("dlssnrfilter"));

    batch::BatchWindow window;
    window.show();
    return QApplication::exec();
}
