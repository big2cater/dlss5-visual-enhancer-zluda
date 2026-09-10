using System;
using System.Threading;
using System.Windows.Forms;

namespace DlssnrFilter
{
    internal static class Program
    {
        [STAThread]
        private static void Main(string[] args)
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

            if (args != null && args.Length > 0 && args[0] == "--test-update")
            {
                var testChangelog = "### DLSS 5 神经渲染多轮级联与色彩物理映射更新\r\n\r\n"
                    + "1. **双引擎时序级联 (Multi-Pass Dual-Engine)**: 彻底解决残影与频闪问题\r\n"
                    + "2. **色彩物理线性化** (Gamma 1.0 完美映射)\r\n"
                    + "3. **防 TDR 崩溃机制**: 自适应让出显卡时间，杜绝驱动重置\r\n"
                    + "4. **RDNA 4 (RX 9070 XT) 极限加速**: 零内存堆分配 + 100% WGP 模式";
                var dlg = new UpdateDialog("v2026.09.10-multipass", "v2026.09.10-multipass", "2026-09-10T14:40:00Z",
                    testChangelog, MainForm.ReleasesPageUrl,
                    "https://github.com/big2cater/dlss5-visual-enhancer-zluda/releases/download/v2026.09.10-multipass/DLSSNRFilter-v2026.09.10-multipass-nodlssnr.zip",
                    "DLSSNRFilter-v2026.09.10-multipass-nodlssnr.zip", 36467460, 1.25f);
                Application.Run(dlg);
                return;
            }

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