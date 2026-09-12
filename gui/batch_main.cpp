#include "batch_window.h"
#include "gpu_detection.h"
#include "precompile.h"

#include <QApplication>
#include <QGuiApplication>
#include <QIcon>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace {
void kill_children_when_this_process_ends() {
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) return;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof info) ||
        !AssignProcessToJobObject(job, GetCurrentProcess())) {
        CloseHandle(job);
    }
}
} // namespace

int main(int argc, char **argv) {
    kill_children_when_this_process_ends();

    // Detect GPU architecture and inject RDNA 4 environment if needed
    dlssnr::auto_configure_gpu_environment();

    // Windows reports fractional scaling -- 125 %, 150 % -- and Qt rounds those
    // to whole numbers unless told otherwise. Rounded, the window is laid out
    // at one scale and drawn at another. Has to be set before the application
    // object exists, which is why it is out here rather than in the window.
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    QApplication application(argc, argv);
    QApplication::setApplicationName(QStringLiteral("DLSSNRFilter"));
    QApplication::setOrganizationName(QStringLiteral("dlssnrfilter"));

    const QStringList arguments = QApplication::arguments();
    if (arguments.size() == 4 && arguments[1] == QLatin1String("--compile-one"))
        return enhancer::compile_one(arguments[2].toStdWString(), arguments[3].toStdWString());

    batch::BatchWindow window;
    window.show();
    return QApplication::exec();
}
