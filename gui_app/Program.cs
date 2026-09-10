using System;
using System.Threading;
using System.Windows.Forms;

namespace DlssnrFilter
{
    internal static class Program
    {
        [STAThread]
        private static void Main()
        {
            // A readable dialog instead of the JIT debug prompt when anything
            // unexpected escapes; the log window still shows run-time messages.
            // The owner is the main window so the dialog always comes to front
            // (an ownerless MessageBox can sit invisibly behind the window and
            // make the app look frozen).
            Application.SetUnhandledExceptionMode(UnhandledExceptionMode.CatchException);
            AppDomain.CurrentDomain.UnhandledException += (_, e) =>
                MessageBox.Show(Owner(), "未处理的异常:\n\n" + e.ExceptionObject,
                                "DLSSNRFilter", MessageBoxButtons.OK, MessageBoxIcon.Error);
            Application.ThreadException += (_, e) =>
                MessageBox.Show(Owner(), "界面线程异常:\n\n" + e.Exception,
                                "DLSSNRFilter", MessageBoxButtons.OK, MessageBoxIcon.Error);

            ApplicationConfiguration.Initialize();

            // Single instance: a second launch must not create a hidden twin
            // that keeps running after its window is "closed" (and then locks
            // the exe against updates). The mutex lives for the whole run.
            using var single = new Mutex(true, @"Global\DLSSNRFilter_SingleInstance_Mutex", out bool isFirst);
            if (!isFirst)
            {
                MessageBox.Show(Owner(),
                                "DLSSNRFilter 已经在运行中。\n如果看不到窗口，请在任务栏或任务管理器中找到 DLSSNRFilter.exe。",
                                "DLSSNRFilter", MessageBoxButtons.OK, MessageBoxIcon.Information);
                return;
            }

            Application.Run(new MainForm());

            // Hard exit: whatever the message loop left behind, the process
            // must not linger headless after the window has closed.
            Environment.Exit(0);
        }

        private static IWin32Window Owner() =>
            Application.OpenForms.Count > 0 ? Application.OpenForms[0] : null;
    }
}