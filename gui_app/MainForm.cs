using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Windows.Forms;

namespace DlssnrFilter
{
    // ---------------------------------------------------------------------
    // Persisted settings
    // ---------------------------------------------------------------------
    public class GuiSettings
    {
        public string Mode { get; set; } = "video";          // video | image
        public string Input { get; set; } = "";
        public string Output { get; set; } = "";
        public string Snippet { get; set; } = "";
        public string Driver { get; set; } = "";
        public string Runtime { get; set; } = "";
        public string Nvapi { get; set; } = "";

        public int Intensity { get; set; } = 100;            // /100
        public int GlobalTone { get; set; } = 0;             // /100
        public int LocalTone { get; set; } = 100;            // /100
        public int LocalStructure { get; set; } = 100;       // /100
        public int SkinStructure { get; set; } = 0;          // /100
        public int Style { get; set; } = 0;
        public int Preset { get; set; } = 0;
        public string DlssModelPreset { get; set; } = "default";
        public bool AutoMask { get; set; } = true;

        // Evaluations per frame. For VIDEO this stays at 1: the network blends
        // with its own previous output, so evaluating a frame more than once
        // drives it three times as far from the source and amplifies whatever
        // differs between neighbouring frames -- which is exactly the flicker.
        // Stills want the opposite, hence the separate value below.
        public int Passes { get; set; } = 1;                 // video: 1 = temporally stable
        public int ImagePasses { get; set; } = 3;            // stills: repeat so the blend settles
        public string Reset { get; set; } = "auto";
        public int ResetEvery { get; set; } = 60;
        // Higher than the command line's 0.15 on purpose. Every reset throws
        // the accumulation history away, and the frame that follows it comes
        // out at a visibly different level from the ones before it; a cut test
        // that fires on ordinary motion reads as flicker. 0.30 is a mean luma
        // change only a real cut reaches.
        public string CutThreshold { get; set; } = "0.30";
        public double Gamma { get; set; } = 1.4;      // 1.4 = optimal balanced tone transfer
        public int Crf { get; set; } = 18;
        public string Fps { get; set; } = "0";
        public string MaxFrames { get; set; } = "0";
        public bool Audio { get; set; } = true;
        public bool DumpFrames { get; set; } = false;
        public string DumpDir { get; set; } = "";
        // Motion-vector guidance. Off by default: the estimator is a coarse
        // CPU block matcher (a 1/16-resolution grid, whole-pixel SAD, ties
        // resolved by scan order), so its field jitters from frame to frame and
        // the network reprojects its history by that jitter -- local crawling
        // and shimmer. Off, the history is blended as-is, which is stable; the
        // price is some ghosting behind fast motion. Turn it on for that.
        public bool Flow { get; set; } = false;
        public string UpscaleMode { get; set; } = "native";
    }

    // ---------------------------------------------------------------------
    // Main window
    // ---------------------------------------------------------------------
    public class MainForm : Form
    {
        private readonly string _runDir = AppContext.BaseDirectory;

        private readonly GuiSettings _s = new();

        // path rows
        private readonly List<string[]> _pathRows = new();   // [label, key]

        private TextBox _tbInput, _tbOutput, _tbSnippet, _tbDriver, _tbRuntime, _tbNvapi;
        private RadioButton _rbVideo, _rbImage;
        private TrackBar _tbIntensity, _tbGlobalTone, _tbLocalTone, _tbLocalStruct, _tbSkinStruct;
        private Label _lblIntensity, _lblGlobalTone, _lblLocalTone, _lblLocalStruct, _lblSkinStruct;
        private ComboBox _cbStyle, _cbPreset, _cbReset, _cbUpscale, _cbDlssModel;
        private CheckBox _ckAutoMask, _ckAudio, _ckDump, _ckFlow;
        private NumericUpDown _nPasses, _nImagePasses, _nEvery, _nCrf, _nGamma;
        private TextBox _tbCut, _tbFps, _tbMaxFrames, _tbDumpDir;
        private Button _btnStart, _btnStop, _btnFrameHold, _btnDumpBrowse, _btnResetEffects, _btnCompare;
        private readonly ToolTip _tip = new();
        private Panel _videoPanel;
        private FlowLayoutPanel _stillActions;
        private ProgressBar _progress;
        private Label _lblStatus;
        private RichTextBox _log;
        private GroupBox _filesBox, _paramsBox;
        private TableLayoutPanel _filesTlp, _paramsTlp, _root, _leftTlp, _vtTlp, _rightTlp;
        private bool _stackedParams;
        private System.Windows.Forms.Timer _runTimer;   // 1 s status heartbeat
        private int _runElapsed;

        private Process _proc;
        private bool _running;
        private bool _isFrameHold;
        private string _frameHoldIn;
        private string _frameHoldOut;
        private bool _autoOutput;    // output path was derived from the input
        private bool _settingText;   // programmatic control updates
        private long _framesDone;
        private bool _selftest;      // diagnostic mode driving a real run
        private int _hbTicks;        // UI-thread heartbeat counter
        private long _framesTotal = -1;
        private readonly Regex _reFrame = new(@"\[(\d{5})\] ([\d.]+) ms \(avg ([\d.]+), reset=(\d+), blanks=(\d+)\)");
        private readonly Regex _rePre = new(@"\[precompile\] translated (\d+) of (\d+)");

        public MainForm()
        {
            Text = "DLSS 5 神经渲染滤镜 (AMD/ZLUDA)";
            // Layout geometry is computed from the runtime DPI ourselves (S()),
            // so no AutoScaleMode magic is needed on top.
            // Layout sizes are explicitly scaled with DeviceDpi via S();
            // automatic WinForms scaling here would apply the DPI twice.
            AutoScaleMode = AutoScaleMode.None;
            _scale = DeviceDpi / 96.0f;
            Font = new Font("Microsoft YaHei UI", 9F);
            // The rest of this form is laid out with S(), so the form itself
            // must use the same DPI scale.  Leaving these two values at their
            // 96-DPI sizes makes the right/bottom controls fall outside the
            // client area at 125%/150% scaling.
            //
            // These are *physical* pixels: with AutoScaleMode.None under
            // PerMonitorV2 WinForms does no scaling of its own, so a value set
            // here reaches the screen unchanged (measured: ClientSize 1250x1044
            // comes back from GetClientRect as exactly 1250x1044 physical).
            // S() therefore grows the window with the display, which is right
            // -- the display, measured in the same physical pixels, grows with
            // it too.
            ClientSize = new Size(S(1000), S(1020));
            MinimumSize = new Size(S(960), S(820));
            AutoScroll = true;
            StartPosition = FormStartPosition.CenterScreen;

            LoadSettings();
            BuildUi();
            ClientSizeChanged += (_, _) => UpdateParameterLayout();
            SyncUiFromSettings();

            // Diagnostic: with DLSSNRFILTER_DUMP_LAYOUT=1 the control tree
            // geometry is written to gui_layout.txt beside the exe, so layout
            // problems can be checked without looking at a screen.
            if (Environment.GetEnvironmentVariable("DLSSNRFILTER_DUMP_LAYOUT") == "1")
                DumpLayout("", Controls);
            if (Environment.GetEnvironmentVariable("DLSSNRFILTER_SELFTEST") == "1")
            {
                _selftest = true;
                // UI-thread heartbeat: 4 ticks/s, stamped into a file every
                // second. If the window truly freezes, the file stops growing
                // and the watchdog thread (see Watchdog) records the moment.
                _hbTimer = new System.Windows.Forms.Timer { Interval = 250 };
                _hbTimer.Tick += (_, _) =>
                {
                    _hbTicks++;
                    if ((_hbTicks % 4) == 0)
                        File.AppendAllText(Path.Combine(_runDir, "gui_heartbeat.txt"),
                                           _hbTicks + Environment.NewLine);
                };
                _hbTimer.Start();
                Task.Run(Watchdog);
                RunSelfTest();
            }
        }

        private System.Windows.Forms.Timer _hbTimer;

        // Background watchdog: if the heartbeat stops advancing for 10 s the
        // UI thread is stuck; record the stall so the freeze can be diagnosed
        // without a debugger attached.
        private void Watchdog()
        {
            try
            {
                var path = Path.Combine(_runDir, "gui_heartbeat.txt");
                var last = 0;
                var stalledFor = 0;
                for (var i = 0; i < 2400; i++)
                {
                    Thread.Sleep(250);
                    var ticks = 0;
                    try
                    {
                        var text = File.ReadAllText(path);
                        int.TryParse(text.Trim().Split('\n').LastOrDefault(), out ticks);
                    }
                    catch { }
                    if (ticks == last) { if (++stalledFor == 40) break; }  // 10 s without progress
                    else { last = ticks; stalledFor = 0; }
                }
                File.AppendAllText(Path.Combine(_runDir, "gui_selftest2.txt"),
                    "watchdog: hbTicks=" + _hbTicks +
                    (stalledFor >= 40 ? " UI THREAD STALLED (no heartbeat)" : " (heartbeat alive)") +
                    Environment.NewLine);
            }
            catch { }
        }

        // ---- diagnostic trace -------------------------------------------
        // Appends a tick + thread marker for the forensic trace: the frozen
        // function is the last entry without a matching exit.
        private void Tr(string what)
        {
            try
            {
                File.AppendAllText(Path.Combine(_runDir, "gui_trace.txt"),
                    Environment.TickCount + " " + (InvokeRequired ? "bg " : "UI ") + what +
                    Environment.NewLine);
            }
            catch { }
        }

        // Diagnostic driver: performs a real image run exactly like the user's
        // (same StartRun path), while the heartbeat/watchdog watch for a UI
        // freeze. Result text is appended to gui_selftest2.txt.
        private void RunSelfTest()
        {
            try
            {
                var log = Path.Combine(_runDir, "gui_selftest2.txt");
                File.WriteAllText(log, "selftest start, input=" + _tbInput.Text + Environment.NewLine);
                var photo = File.Exists(_s.Input) ? _s.Input : Path.Combine(_runDir, "input_frame_00003.png");
                if (!IsImagePath(photo)) photo = Path.Combine(_runDir, "input_frame_00003.png");
                _tbInput.Text = photo;
                AutoFillOutput();
                File.AppendAllText(log, "starting run: " + photo + " -> " + _tbOutput.Text + Environment.NewLine);

                StartRun();

                var poll = new System.Windows.Forms.Timer { Interval = 1000 };
                var started = Environment.TickCount;
                poll.Tick += (_, _) =>
                {
                    try
                    {
                        var el = Environment.TickCount - started;
                        if (!_running)
                        {
                            poll.Stop();
                            File.AppendAllText(log, "run finished after " + (el / 1000) +
                                "s, hbTicks=" + _hbTicks + ", exiting" + Environment.NewLine);
                            Environment.Exit(0);
                        }
                        else if (el > 300000)
                        {
                            poll.Stop();
                            File.AppendAllText(log, "run still active after 300s, exiting" + Environment.NewLine);
                            Environment.Exit(0);
                        }
                    }
                    catch { }
                };
                poll.Start();
            }
            catch (Exception ex)
            {
                try { File.AppendAllText(Path.Combine(_runDir, "gui_selftest2.txt"), "EXC " + ex + Environment.NewLine); }
                catch { }
            }
        }

        private void DumpLayout(string indent, Control.ControlCollection controls)
        {
            try
            {
                var sb = new StringBuilder();
                // The screen the window opened on, in the same logical pixels
                // as everything below: a layout that does not fit is first a
                // disagreement between these two numbers.
                sb.AppendLine($"screen physical={Screen.PrimaryScreen.Bounds} "
                              + $"client={ClientSize} minimum={MinimumSize} "
                              + $"deviceDpi={DeviceDpi} scale={_scale:0.###}");
                DumpInto(sb, indent, controls);
                File.WriteAllText(Path.Combine(_runDir, "gui_layout.txt"), sb.ToString());
            }
            catch { }
        }

        private void DumpInto(StringBuilder sb, string indent, Control.ControlCollection controls)
        {
            foreach (Control c in controls)
            {
                sb.AppendLine($"{indent}{c.GetType().Name} '{c.Text}' {c.Width}x{c.Height} @({c.Left},{c.Top}) vis={c.Visible}");
                if (c.Controls.Count > 0) DumpInto(sb, indent + "  ", c.Controls);
            }
        }

        // =================================================================
        // UI construction
        // =================================================================
        private float _scale = 1.0f;       // DeviceDpi / 96
        private int S(int v) => (int)MathF.Round(v * _scale);   // DPI-scaled size

        private void BuildUi()
        {
            var root = new TableLayoutPanel { Dock = DockStyle.Fill, Padding = new Padding(S(8)),
                RowCount = 4, ColumnCount = 1, AutoScroll = true };
            _root = root;
            // Fixed, DPI-scaled sections (AutoSize sections collapse under
            // TableLayoutPanel, so sizes are explicit); the log takes the rest.
            root.RowStyles.Add(new RowStyle(SizeType.Absolute, S(224))); // files
            root.RowStyles.Add(new RowStyle(SizeType.Absolute, S(340))); // params (9 rows + margins)
            root.RowStyles.Add(new RowStyle(SizeType.Absolute, S(76)));  // run bar
            root.RowStyles.Add(new RowStyle(SizeType.Percent, 100));     // log

            // ---- files ----------------------------------------------------
            var files = new GroupBox { Text = "文件", Dock = DockStyle.Fill };
            _filesBox = files;
            var ft = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 3, RowCount = 8,
                Padding = new Padding(S(6)) };
            _filesTlp = ft;
            ft.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, S(150)));
            ft.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            ft.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, S(90)));

            AddPathRow(ft, 0, "输入视频/图片", out _tbInput, true);
            AddPathRow(ft, 1, "输出文件", out _tbOutput, false);
            AddPathRow(ft, 2, "nvngx_dlssnr.dll", out _tbSnippet, true);
            AddPathRow(ft, 3, "nvcuda.dll (ZLUDA)", out _tbDriver, true);
            AddPathRow(ft, 4, "nvngx.dll", out _tbRuntime, true);
            AddPathRow(ft, 5, "nvapi64.dll", out _tbNvapi, true);

            var modeBar = new FlowLayoutPanel { Dock = DockStyle.Fill, FlowDirection = FlowDirection.LeftToRight,
                Padding = new Padding(S(6), 0, S(6), 0), AutoSize = false };
            _rbVideo = new RadioButton { Text = "视频滤镜", AutoSize = true, Checked = true };
            _rbImage = new RadioButton { Text = "单图滤镜", AutoSize = true };
            _rbVideo.CheckedChanged += (_, _) => UpdateModeUi();
            _rbImage.CheckedChanged += (_, _) => UpdateModeUi();
            var btnDefaults = new Button { Text = "恢复默认路径", AutoSize = true };
            btnDefaults.Click += (_, _) => { ApplyDefaults(); SyncUiFromSettings(); };
            modeBar.Controls.Add(_rbVideo);
            modeBar.Controls.Add(_rbImage);
            modeBar.Controls.Add(btnDefaults);
            ft.Controls.Add(modeBar, 0, 7);
            ft.SetColumnSpan(modeBar, 3);
            // rows 6 and 7 have no AddPathRow styles of their own yet
            ft.RowStyles.Add(new RowStyle(SizeType.Absolute, S(32)));
            ft.RowStyles.Add(new RowStyle(SizeType.Absolute, S(32)));

            files.Controls.Add(ft);
            root.Controls.Add(files, 0, 0);

            // ---- parameters ------------------------------------------------
            var paramsBox = new GroupBox { Text = "参数", Dock = DockStyle.Fill };
            _paramsBox = paramsBox;
            var pt = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2, RowCount = 1,
                Padding = new Padding(S(6)) };
            _paramsTlp = pt;
            pt.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50));
            pt.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50));

            // left: strengths + style/preset/mask
            var left = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 3, RowCount = 11 };
            _leftTlp = left;
            left.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, S(110)));
            left.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            left.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, S(60)));

            AddSliderRow(left, 0, "强度", out _tbIntensity, out _lblIntensity, 0, 200, 100, "{0:0.00}");
            AddSliderRow(left, 1, "全局色调", out _tbGlobalTone, out _lblGlobalTone, 0, 100, 0, "{0:0.00}");
            AddSliderRow(left, 2, "局部色调", out _tbLocalTone, out _lblLocalTone, 0, 200, 100, "{0:0.00}");
            AddSliderRow(left, 3, "局部结构", out _tbLocalStruct, out _lblLocalStruct, 0, 200, 100, "{0:0.00}");
            AddSliderRow(left, 4, "皮肤结构", out _tbSkinStruct, out _lblSkinStruct, 0, 100, 0, "{0:0.00}");

            _cbStyle = new ComboBox { Dock = DockStyle.Fill, DropDownStyle = ComboBoxStyle.DropDownList };
            _cbStyle.Items.AddRange(new object[] { "0 - 默认", "1 - 自然", "2 - 电影" });
            _cbPreset = new ComboBox { Dock = DockStyle.Fill, DropDownStyle = ComboBoxStyle.DropDownList };
            _cbPreset.Items.AddRange(new object[] {
                "0 - 自动", "1 - Preset #1", "2 - Preset #2", "3 - Preset #3" });
            _cbDlssModel = new ComboBox { Dock = DockStyle.Fill, DropDownStyle = ComboBoxStyle.DropDownList };
            _cbDlssModel.Items.AddRange(new object[] { "默认", "J", "K", "L", "M" });
            // Placed with the other switches in the video panel, not in this
            // column -- see the flow row holding 运动向量引导 further down.
            _ckAutoMask = new CheckBox { Text = "自动遮罩", AutoSize = true, Checked = true };
            AddComboRow(left, 5, "风格", _cbStyle);
            AddComboRow(left, 6, "DLSS模型预设", _cbPreset);
            // Short on purpose: the caption column is 110 design pixels wide,
            // and "DLSS Model J/K/L" needs more, so it wrapped onto two lines
            // and spilled out of its 32-pixel row.
            AddComboRow(left, 7, "DLSS模型", _cbDlssModel);
            _nGamma = new NumericUpDown { Dock = DockStyle.Fill, Minimum = 0.5m, Maximum = 3.0m,
                Increment = 0.1m, DecimalPlaces = 1, Value = 1.0m };
            AddPair(left, 9, "输出伽马", _nGamma);
            _nImagePasses = new NumericUpDown { Dock = DockStyle.Fill, Minimum = 1, Maximum = 12, Value = 3 };
            _stillActions = new FlowLayoutPanel { Dock = DockStyle.Fill, FlowDirection = FlowDirection.LeftToRight,
                WrapContents = false, Padding = new Padding(0, S(2), 0, 0) };
            _stillActions.Controls.Add(new Label { Text = "图片处理次数", AutoSize = true,
                TextAlign = ContentAlignment.MiddleLeft, Padding = new Padding(0, S(5), S(8), 0) });
            _nImagePasses.Width = S(90);
            _stillActions.Controls.Add(_nImagePasses);
            // The pass count only means something for a still, so in video
            // mode the row goes away entirely (see UpdateModeUi).
            EnsureRowStyle(left, 8, new RowStyle(SizeType.Absolute, S(38)));
            left.Controls.Add(_stillActions, 0, 8);
            left.SetColumnSpan(_stillActions, 3);
            // The reset goes below 输出伽马 in a row of its own: up among the
            // effect controls it read as belonging to the ones immediately
            // above it rather than to the column as a whole.
            _btnResetEffects = new Button { Text = "恢复默认参数", Width = S(150), Height = S(32),
                Margin = new Padding(0, S(3), 0, 0) };
            _btnResetEffects.Click += (_, _) => ResetEffectControls();
            EnsureRowStyle(left, 10, new RowStyle(SizeType.Absolute, S(38)));
            left.Controls.Add(_btnResetEffects, 1, 10);
            // Freeze every parameter row after all controls have been added.
            // This prevents TableLayoutPanel from expanding row 10 and
            // vertically separating the gamma label from its editor.
            left.RowStyles.Clear();
            for (var i = 0; i < 5; i++)
                left.RowStyles.Add(new RowStyle(SizeType.Absolute, S(34)));
            for (var i = 5; i <= 7; i++)
                left.RowStyles.Add(new RowStyle(SizeType.Absolute, S(32)));
            left.RowStyles.Add(new RowStyle(SizeType.Absolute, S(38))); // image passes
            left.RowStyles.Add(new RowStyle(SizeType.Absolute, S(32))); // gamma
            left.RowStyles.Add(new RowStyle(SizeType.Absolute, S(38))); // reset
            // A trailing spacer takes whatever height the section is given but
            // does not need. Without it that slack lands on the last real row
            // -- 输出伽马 -- which then stretches to three times its height:
            // the caption, docked and centred, drifts to the middle of the
            // cell while the editor stays at the top, and the two end up a
            // good forty pixels apart looking like unrelated controls.
            left.RowCount = 12;
            left.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            _tip.SetToolTip(_cbPreset,
                "选择 DLSS 神经渲染模型预设。当前官方 DLL 主要内置 Preset 1；选择 0 (自动) 或 1 均使用默认模型网络。");
            _tip.SetToolTip(_cbStyle, "NR Style：默认、自然或电影风格。");
            _tip.SetToolTip(_tbIntensity, "整体神经渲染效果强度。视频建议从 1.00 开始。");
            _tip.SetToolTip(_tbSkinStruct,
                "人脸/皮肤防塑料感保护强度 (0.00~1.00)。值越高越能保留人脸与皮肤毛孔细节，避免过度平滑和假面感。");
            _tip.SetToolTip(_nImagePasses,
                "单图模式重复处理同一张图片的次数。次数越高效果越强，但也更慢；视频模式不使用此项。");

            // right: a single content column; the video panel and the image panel
            // share the same cell and only one is visible at a time.
            var right = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1 };
            _rightTlp = right;
            right.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));

            _videoPanel = new Panel { Dock = DockStyle.Fill };
            var vt = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2, RowCount = 10 };
            _vtTlp = vt;
            vt.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, S(120)));
            vt.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));

            _cbReset = new ComboBox { Dock = DockStyle.Fill, DropDownStyle = ComboBoxStyle.DropDownList };
            _cbReset.Items.AddRange(new object[] { "auto - 自动切场复位", "always - 每帧复位", "never - 全程不断", "every - 每N帧复位" });
            _nEvery = new NumericUpDown { Dock = DockStyle.Fill, Minimum = 1, Maximum = 100000, Value = 60 };
            _tbCut = new TextBox { Dock = DockStyle.Fill, Text = "0.30" };
            _nPasses = new NumericUpDown { Dock = DockStyle.Fill, Minimum = 1, Maximum = 2, Value = 1 };
            _nCrf = new NumericUpDown { Dock = DockStyle.Fill, Minimum = 0, Maximum = 40, Value = 18 };
            _tbFps = new TextBox { Dock = DockStyle.Fill, Text = "0" };
            _tbMaxFrames = new TextBox { Dock = DockStyle.Fill, Text = "0" };
            _ckAudio = new CheckBox { Text = "音轨直通", AutoSize = true, Checked = true };
            _ckDump = new CheckBox { Text = "抽帧到目录", AutoSize = true };
            // A width, not a dock: this box sits in a FlowLayoutPanel, where
            // Dock=Fill collapses it to its minimum -- about 75 px, enough for
            // "D:\Downl" of a path the user cannot read let alone check.
            _tbDumpDir = new TextBox { Anchor = AnchorStyles.Left | AnchorStyles.Right };
            _btnDumpBrowse = new Button { Text = "选择…", AutoSize = true, Anchor = AnchorStyles.Left };
            _ckFlow = new CheckBox { Text = "运动向量引导", AutoSize = true, Checked = false };
            _cbUpscale = new ComboBox { Dock = DockStyle.Fill, DropDownStyle = ComboBoxStyle.DropDownList };
            _cbUpscale.Items.AddRange(new object[] { "DLAA / native (1x)", "Quality (1.5x)", "Balanced (1.724x)", "Performance (2x)", "Ultra Performance (3x)" });

            AddPair(vt, 0, "复位模式", _cbReset);
            AddPair(vt, 1, "每N帧", _nEvery);
            AddPair(vt, 2, "切场阈值", _tbCut);
            AddPair(vt, 3, "帧pass次数", _nPasses);
            AddPair(vt, 4, "CRF(画质)", _nCrf);
            AddPair(vt, 5, "输出fps(0=原)", _tbFps);
            AddPair(vt, 6, "最大帧数(0=全)", _tbMaxFrames);
            AddPair(vt, 7, "Upscaling 输出", _cbUpscale);
            // Three columns instead of a flow row: a flow panel wraps, and with
            // a path field wide enough to read there is no room for a second
            // line, so the browse button ended up below the panel's bottom edge
            // with no height at all. Here the path takes whatever the check box
            // and the button leave, which is the most it can get.
            var dumpRow = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 3, RowCount = 1,
                Margin = new Padding(0) };
            dumpRow.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            dumpRow.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            dumpRow.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            // Anchored to the left edge only, and with no vertical margins, so
            // all three sit on one centred baseline. Left as placed they were
            // top-aligned instead, and three different control heights put
            // three different centres on one row. The zero margin also stops
            // the row starting three pixels right of the combo box above it.
            // The three-pixel left margin puts the check box on the same x as
            // the combo box in the row above: that control sits three pixels
            // into its cell too, and with the margin gone this row started
            // visibly left of it.
            _ckDump.Margin = new Padding(3, 0, S(6), 0);
            _ckDump.Anchor = AnchorStyles.Left;
            _tbDumpDir.Margin = new Padding(0);
            _btnDumpBrowse.Margin = new Padding(S(6), 0, 0, 0);
            dumpRow.Controls.Add(_ckDump, 0, 0);
            dumpRow.Controls.Add(_tbDumpDir, 1, 0);
            dumpRow.Controls.Add(_btnDumpBrowse, 2, 0);
            vt.RowStyles.Add(new RowStyle(SizeType.Absolute, S(32))); // row 7, beyond AddPair's set
            vt.Controls.Add(dumpRow, 1, 8);
            var flowRow = new FlowLayoutPanel { Dock = DockStyle.Fill, AutoSize = false, Padding = new Padding(0, S(2), 0, 0) };
            flowRow.Controls.Add(_ckFlow);
            flowRow.Controls.Add(_ckAudio);
            flowRow.Controls.Add(_ckAutoMask);
            vt.RowStyles.Add(new RowStyle(SizeType.Absolute, S(32))); // row 8
            vt.Controls.Add(flowRow, 1, 9);
            vt.RowStyles.Clear();
            for (var i = 0; i < 10; i++)
                vt.RowStyles.Add(new RowStyle(SizeType.Absolute, S(32)));
            // Same spacer as the left column: without it the section's spare
            // height stretches the last row, and 运动向量引导 / 音轨直通 end
            // up floating a hundred-odd pixels below the row they belong with.
            vt.RowCount = 11;
            vt.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            _tip.SetToolTip(_cbUpscale, "输出分辨率倍率：native 保持原尺寸；其余模式由 DLSS 直接输出更高分辨率。");
            _tip.SetToolTip(_ckFlow,
                "优先使用 D3D12 GPU 计算运动向量，再自动回退 CPU。开：快速运动边缘更干净；"
                + "如果驱动不支持会自动回退，不会阻止视频输出。默认关。");
            _tip.SetToolTip(_nPasses,
                "多轮降噪级联：1 为标准单次降噪；2 为双引擎级联（独立维持时序缓冲，消除视频频闪，深度降噪）。");
            _tip.SetToolTip(_tbCut,
                "判定切场的平均亮度差，越大越不容易复位。调低会频繁清空累积历史，画面一跳一跳。");
            _videoPanel.Controls.Add(vt);

            var imagePanel = new Panel { Dock = DockStyle.Fill, Visible = false };
            var it = new Label { Dock = DockStyle.Fill, AutoSize = false,
                Padding = new Padding(S(8)), TextAlign = ContentAlignment.TopLeft,
                Text = "单图模式\r\n重复处理次数和左侧参数会应用到当前图片。\r\n输出格式：PNG。" };
            imagePanel.Controls.Add(it);

            right.Controls.Add(_videoPanel, 0, 0);
            right.Controls.Add(imagePanel, 0, 0);
            _imagePanel = imagePanel;

            pt.Controls.Add(left, 0, 0);
            pt.Controls.Add(right, 1, 0);
            paramsBox.Controls.Add(pt);
            root.Controls.Add(paramsBox, 0, 1);

            // ---- run bar ---------------------------------------------------
            var runBar = new GroupBox { Text = "运行", Dock = DockStyle.Fill };
            var rt = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 3, RowCount = 1,
                AutoSize = false, Padding = new Padding(S(6)) };
            // Prevent preferred-size propagation from making the children
            // taller than the GroupBox's fixed run row at high DPI.
            rt.RowStyles.Add(new RowStyle(SizeType.Absolute, S(48)));
            rt.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, S(420)));
            rt.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            rt.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 40));

            // Keep both commands on one row.  The previous two-row layout was
            // taller than the fixed run section, so the Stop button was
            // visibly clipped on normal windows.
            var commandPanel = new FlowLayoutPanel { Dock = DockStyle.Fill,
                FlowDirection = FlowDirection.LeftToRight, WrapContents = false,
                Padding = new Padding(0, S(4), 0, 0) };
            _btnStart = new Button { Text = "开始", Width = S(80), Height = S(34) };
            _btnStart.Click += (_, _) => StartRun();
            _btnStop = new Button { Text = "停止", Width = S(80), Height = S(34), Enabled = false };
            _btnStop.Click += (_, _) => StopRun();
            _btnFrameHold = new Button { Text = "单帧定帧对比", Width = S(110), Height = S(34) };
            _btnFrameHold.Click += (_, _) => StartFrameHold();
            _btnCompare = new Button { Text = "对比结果", Width = S(80), Height = S(34) };
            _btnCompare.Click += (_, _) => OpenCompare();
            commandPanel.Controls.Add(_btnStart);
            commandPanel.Controls.Add(_btnStop);
            commandPanel.Controls.Add(_btnFrameHold);
            commandPanel.Controls.Add(_btnCompare);
            _tip.SetToolTip(_btnFrameHold, "截取视频单帧并在秒级内运行 DLSS 降噪，立即弹出分屏滑动对比，无需等待整段视频处理。");
            var progressHost = new Panel { Dock = DockStyle.Fill };
            _progress = new ProgressBar { Dock = DockStyle.Fill, Minimum = 0, Maximum = 100 };
            progressHost.Controls.Add(_progress);
            _lblStatus = new Label { Dock = DockStyle.Fill, AutoSize = false,
                Text = "就绪", TextAlign = ContentAlignment.MiddleLeft, Margin = Padding.Empty };

            rt.Controls.Add(commandPanel, 0, 0);
            rt.Controls.Add(progressHost, 1, 0);
            rt.Controls.Add(_lblStatus, 2, 0);
            runBar.Controls.Add(rt);
            root.Controls.Add(runBar, 0, 2);

            // ---- log --------------------------------------------------------
            _log = new RichTextBox { Dock = DockStyle.Fill, ReadOnly = true, Font = new Font("Consolas", 9F),
                BackColor = Color.FromArgb(20, 20, 24), ForeColor = Color.WhiteSmoke };
            _log.HideSelection = false;
            var logHost = new Panel { Dock = DockStyle.Fill, Padding = new Padding(0, 4, 0, 0) };
            logHost.Controls.Add(_log);
            root.Controls.Add(logHost, 0, 3);

            Controls.Add(root);
            HookAutoOutput();
        }

        // -----------------------------------------------------------------
        // Auto-derive the output path from the input path
        // -----------------------------------------------------------------
        private void HookAutoOutput()
        {
            _tbOutput.TextChanged += (_, _) => { if (!_settingText) _autoOutput = false; };
            _tbInput.TextChanged += (_, _) => { if (!_settingText) AutoFillOutput(); };
            _rbVideo.CheckedChanged += (_, _) => { if (_settingText) return; if (_autoOutput) AutoFillOutput(); };
            _rbImage.CheckedChanged += (_, _) => { if (_settingText) return; if (_autoOutput) AutoFillOutput(); };
        }

        private static bool IsImagePath(string path)
        {
            var ext = Path.GetExtension(path).ToLowerInvariant();
            return ext is ".png" or ".jpg" or ".jpeg" or ".bmp" or ".webp" or ".tif" or ".tiff";
        }

        private void AutoFillOutput()
        {
            var input = _tbInput.Text.Trim();
            if (string.IsNullOrEmpty(input)) return;
            var imageType = IsImagePath(input);

            // Follow the input's type automatically: picking an image switches
            // to image mode (and vice versa). The radios still allow manual
            // override afterwards.
            if (!_settingText)
            {
                _settingText = true;
                if (imageType && !_rbImage.Checked) _rbImage.Checked = true;
                else if (!imageType && _rbImage.Checked) _rbVideo.Checked = true;
                _settingText = false;
            }

            var derived = Path.ChangeExtension(input, null) + (imageType ? "_dlss.png" : "_dlss.mp4");
            if (string.Equals(_tbOutput.Text.Trim(), derived, StringComparison.OrdinalIgnoreCase))
                return;
            _settingText = true;
            _tbOutput.Text = derived;
            _settingText = false;
            _autoOutput = true;
            UpdateModeUi();
        }

        private Panel _imagePanel;

        private void AddPathRow(TableLayoutPanel table, int row, string label, out TextBox box, bool open)
        {
            table.RowStyles.Add(new RowStyle(SizeType.Absolute, S(28)));
            var lb = new Label { Text = label, Dock = DockStyle.Fill, TextAlign = ContentAlignment.MiddleLeft };
            box = new TextBox { Dock = DockStyle.Fill };
            var localBox = box; // lambdas below cannot touch the out parameter itself
            var btn = new Button { Text = open ? "选择…" : "另存为…", Dock = DockStyle.Fill };
            btn.Click += (_, _) =>
            {
                if (open)
                {
                    using var dlg = new OpenFileDialog { FileName = localBox.Text };
                    if (dlg.ShowDialog(this) == DialogResult.OK) localBox.Text = dlg.FileName;
                }
                else
                {
                    using var dlg = new SaveFileDialog { FileName = localBox.Text };
                    if (dlg.ShowDialog(this) == DialogResult.OK) localBox.Text = dlg.FileName;
                }
            };
            table.Controls.Add(lb, 0, row);
            table.Controls.Add(box, 1, row);
            table.Controls.Add(btn, 2, row);
        }

        private static void EnsureRowStyle(TableLayoutPanel table, int row, RowStyle style)
        {
            while (table.RowStyles.Count <= row)
                table.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            table.RowStyles[row] = style;
        }

        private void AddSliderRow(TableLayoutPanel table, int row, string label,
            out TrackBar bar, out Label valueLabel, int min, int max, int def, string fmt)
        {
            EnsureRowStyle(table, row, new RowStyle(SizeType.Absolute, S(34)));
            var lb = new Label { Text = label, Dock = DockStyle.Fill, TextAlign = ContentAlignment.MiddleLeft };
            bar = new TrackBar { Dock = DockStyle.Fill, Minimum = min, Maximum = max, Value = def,
                TickStyle = TickStyle.None, AutoSize = false, Height = S(24) };
            valueLabel = new Label { Dock = DockStyle.Fill, TextAlign = ContentAlignment.MiddleLeft,
                Text = string.Format(System.Globalization.CultureInfo.InvariantCulture, fmt, def / 100.0) };
            var localBar = bar;      // lambdas below cannot touch the out parameters
            var localLabel = valueLabel;
            var inv = fmt;
            localBar.ValueChanged += (_, _) => localLabel.Text =
                string.Format(System.Globalization.CultureInfo.InvariantCulture, inv, localBar.Value / 100.0);
            table.Controls.Add(lb, 0, row);
            table.Controls.Add(bar, 1, row);
            table.Controls.Add(valueLabel, 2, row);
        }

        private void AddComboRow(TableLayoutPanel table, int row, string label, ComboBox combo)
        {
            EnsureRowStyle(table, row, new RowStyle(SizeType.Absolute, S(32)));
            var lb = new Label { Text = label, Dock = DockStyle.Fill, TextAlign = ContentAlignment.MiddleLeft };
            table.Controls.Add(lb, 0, row);
            table.Controls.Add(combo, 1, row);
            table.SetColumnSpan(combo, 2);
        }

        private void AddCheckRow(TableLayoutPanel table, int row, CheckBox check)
        {
            EnsureRowStyle(table, row, new RowStyle(SizeType.Absolute, S(32)));
            table.Controls.Add(check, 1, row);
            table.SetColumnSpan(check, 2);
        }

        private void AddPair(TableLayoutPanel table, int row, string label, Control control)
        {
            EnsureRowStyle(table, row, new RowStyle(SizeType.Absolute, S(32)));
            var lb = new Label { Text = label, Dock = DockStyle.Fill, TextAlign = ContentAlignment.MiddleLeft };
            table.Controls.Add(lb, 0, row);
            table.Controls.Add(control, 1, row);
        }

        private void ResetEffectControls()
        {
            _tbIntensity.Value = 100;
            _tbGlobalTone.Value = 0;
            _tbLocalTone.Value = 100;
            _tbLocalStruct.Value = 100;
            _tbSkinStruct.Value = 0;
            _cbStyle.SelectedIndex = 0;
            _cbPreset.SelectedIndex = 0;
            _ckAutoMask.Checked = true;
            _nGamma.Value = 1.4m;
            _nImagePasses.Value = 3;
            _nPasses.Value = 1;
            _cbReset.SelectedIndex = 0;
            _nEvery.Value = 60;
            _tbCut.Text = "0.30";
            _ckFlow.Checked = false;
        }

        // =================================================================
        // Settings <-> UI
        // =================================================================
        private string SettingsPath => Path.Combine(_runDir, "gui_settings.json");

        private void LoadSettings()
        {
            try
            {
                if (File.Exists(SettingsPath))
                {
                    var text = File.ReadAllText(SettingsPath);
                    JsonSerializer.Deserialize<GuiSettings>(text)?.CopyTo(_s);
                    // A file written before the no-flicker defaults existed
                    // carries the old ones: several passes per frame and a cut
                    // threshold that fires on ordinary motion. That combination
                    // is the flicker, so an old file is migrated instead of
                    // being trusted -- otherwise the fix never reaches anyone
                    // who has run this before.
                    if (!text.Contains("\"ImagePasses\""))
                    {
                        _s.Passes = 1;
                        _s.CutThreshold = "0.30";
                        _s.Flow = false;
                    }
                }
            }
            catch { /* corrupt or missing settings: keep defaults */ }
            if (string.IsNullOrEmpty(_s.Snippet)) ApplyDefaults();
        }

        private void ApplyDefaults()
        {
            _s.Snippet = Path.Combine(_runDir, "nvngx_dlssnr.dll");
            _s.Driver = Path.Combine(_runDir, "nvcuda.dll");
            _s.Runtime = Path.Combine(_runDir, "nvngx.dll");
            _s.Nvapi = Path.Combine(_runDir, "nvapi64.dll");
            if (string.IsNullOrEmpty(_s.DumpDir))
                _s.DumpDir = Path.Combine(_runDir, "dump");
        }

        private void SaveSettings()
        {
            try
            {
                File.WriteAllText(SettingsPath, JsonSerializer.Serialize(_s, new JsonSerializerOptions { WriteIndented = true }));
            }
            catch { }
        }

        private void SyncUiFromSettings()
        {
            _settingText = true;
            _rbVideo.Checked = _s.Mode != "image";
            _rbImage.Checked = _s.Mode == "image";
            _tbInput.Text = _s.Input;
            _tbOutput.Text = _s.Output;
            _tbSnippet.Text = _s.Snippet;
            _tbDriver.Text = _s.Driver;
            _tbRuntime.Text = _s.Runtime;
            _tbNvapi.Text = _s.Nvapi;

            _tbIntensity.Value = Clamp(_s.Intensity, 0, 200);
            _tbGlobalTone.Value = Clamp(_s.GlobalTone, 0, 100);
            _tbLocalTone.Value = Clamp(_s.LocalTone, 0, 200);
            _tbLocalStruct.Value = Clamp(_s.LocalStructure, 0, 200);
            _tbSkinStruct.Value = Clamp(_s.SkinStructure, 0, 100);
            _cbStyle.SelectedIndex = Clamp(_s.Style, 0, _cbStyle.Items.Count - 1);
            _cbPreset.SelectedIndex = Clamp(_s.Preset, 0, _cbPreset.Items.Count - 1);
            _cbDlssModel.SelectedIndex = _s.DlssModelPreset switch { "J" => 1, "K" => 2, "L" => 3, "M" => 4, _ => 0 };
            _ckAutoMask.Checked = _s.AutoMask;
            _nPasses.Value = Clamp(_s.Passes, 1, 8);
            _nImagePasses.Value = Clamp(_s.ImagePasses, 1, 12);
            _nCrf.Value = Clamp(_s.Crf, 0, 40);
            _cbReset.SelectedIndex = _s.Reset switch
            {
                "always" => 1, "never" => 2, "every" => 3, _ => 0
            };
            _nEvery.Value = Clamp(_s.ResetEvery, 1, 100000);
            _tbCut.Text = _s.CutThreshold;
            _tbFps.Text = _s.Fps;
            _tbMaxFrames.Text = _s.MaxFrames;
            if (Math.Abs(_s.Gamma - 1.0) < 0.05)
                _s.Gamma = 1.4;
            _nGamma.Value = (decimal)Math.Clamp(_s.Gamma, 0.5, 3.0);
            _ckAudio.Checked = _s.Audio;
            _ckFlow.Checked = _s.Flow;
            _cbUpscale.SelectedIndex = _s.UpscaleMode switch { "quality" => 1, "balanced" => 2, "performance" => 3, "ultra" => 4, _ => 0 };
            _ckDump.Checked = _s.DumpFrames;
            _tbDumpDir.Text = _s.DumpDir;
            _settingText = false;
            // Always re-sync mode and output with the loaded input: persisted
            // settings from an older session may hold a stale combination
            // (e.g. a png input with a *_dlss.mp4 output).
            AutoFillOutput();
            UpdateModeUi();
        }

        private void CollectFromUi()
        {
            _s.Mode = _rbImage.Checked ? "image" : "video";
            _s.Input = _tbInput.Text.Trim();
            _s.Output = _tbOutput.Text.Trim();
            _s.Snippet = _tbSnippet.Text.Trim();
            _s.Driver = _tbDriver.Text.Trim();
            _s.Runtime = _tbRuntime.Text.Trim();
            _s.Nvapi = _tbNvapi.Text.Trim();
            _s.Intensity = _tbIntensity.Value;
            _s.GlobalTone = _tbGlobalTone.Value;
            _s.LocalTone = _tbLocalTone.Value;
            _s.LocalStructure = _tbLocalStruct.Value;
            _s.SkinStructure = _tbSkinStruct.Value;
            _s.Style = _cbStyle.SelectedIndex;
            _s.Preset = _cbPreset.SelectedIndex;
            _s.DlssModelPreset = _cbDlssModel.SelectedIndex switch { 1 => "J", 2 => "K", 3 => "L", 4 => "M", _ => "default" };
            _s.AutoMask = _ckAutoMask.Checked;
            _s.Passes = (int)_nPasses.Value;
            _s.ImagePasses = (int)_nImagePasses.Value;
            _s.Crf = (int)_nCrf.Value;
            _s.Reset = _cbReset.SelectedIndex switch { 1 => "always", 2 => "never", 3 => "every", _ => "auto" };
            _s.ResetEvery = (int)_nEvery.Value;
            _s.CutThreshold = _tbCut.Text.Trim();
            _s.Gamma = (double)_nGamma.Value;
            _s.Fps = _tbFps.Text.Trim();
            _s.MaxFrames = _tbMaxFrames.Text.Trim();
            _s.Audio = _ckAudio.Checked;
            _s.Flow = _ckFlow.Checked;
            _s.UpscaleMode = _cbUpscale.SelectedIndex switch { 1 => "quality", 2 => "balanced", 3 => "performance", 4 => "ultra", _ => "native" };
            _s.DumpFrames = _ckDump.Checked;
            _s.DumpDir = _tbDumpDir.Text.Trim();
            SaveSettings();
        }

        private void UpdateParameterLayout()
        {
            if (_paramsTlp == null || _leftTlp == null || _rightTlp == null) return;
            var narrow = ClientSize.Width < S(1050);
            if (narrow == _stackedParams) return;
            _stackedParams = narrow;

            _paramsTlp.SuspendLayout();
            _paramsTlp.Controls.Clear();
            _paramsTlp.ColumnStyles.Clear();
            _paramsTlp.RowStyles.Clear();
            if (narrow)
            {
                _paramsTlp.ColumnCount = 1;
                _paramsTlp.RowCount = 2;
                _paramsTlp.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
                _paramsTlp.RowStyles.Add(new RowStyle(SizeType.AutoSize));
                _paramsTlp.RowStyles.Add(new RowStyle(SizeType.AutoSize));
                _paramsTlp.Controls.Add(_leftTlp, 0, 0);
                _paramsTlp.Controls.Add(_rightTlp, 0, 1);
            }
            else
            {
                _paramsTlp.ColumnCount = 2;
                _paramsTlp.RowCount = 1;
                _paramsTlp.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50));
                _paramsTlp.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50));
                _paramsTlp.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
                _paramsTlp.Controls.Add(_leftTlp, 0, 0);
                _paramsTlp.Controls.Add(_rightTlp, 1, 0);
            }
            _paramsTlp.ResumeLayout(true);
            FitSectionHeights();
        }

        private void FitSectionHeights()
        {
            if (_root == null || _filesTlp == null || _leftTlp == null || _vtTlp == null) return;
            _root.RowStyles[0].Height = _filesTlp.PreferredSize.Height + S(28);
            var leftH = Math.Max(_leftTlp.PreferredSize.Height, S(430));
            var rightH = Math.Max(_vtTlp.PreferredSize.Height, S(340));
            if (_imagePanel != null && _imagePanel.Visible)
                rightH = Math.Max(rightH, S(130));
            var paramsH = _stackedParams ? leftH + rightH + S(46) : Math.Max(leftH, rightH) + S(34);
            _root.RowStyles[1].Height = paramsH;

            // The log row is Percent, so it takes whatever the fixed sections
            // leave -- and on a short window they leave nothing, making the log
            // zero rows tall with no way to reach it. A Percent row also adds
            // nothing to the panel's preferred size, so AutoScroll sees no
            // overflow and never offers a scrollbar either. Reserving a floor
            // for the log inside the scrollable area is what brings both back:
            // the log keeps a usable height and the rest is scrolled to.
            _root.AutoScrollMinSize = new Size(0,
                (int)(_root.RowStyles[0].Height + _root.RowStyles[1].Height
                      + _root.RowStyles[2].Height + S(180)));
            _root.PerformLayout();
        }

        private static int Clamp(int v, int lo, int hi) => Math.Max(lo, Math.Min(hi, v));

        private void UpdateModeUi()
        {
            var image = _rbImage.Checked;
            _videoPanel.Visible = !image;
            _imagePanel.Visible = image;
            if (_stillActions != null && _leftTlp != null)
            {
                // The pass count only means something for a still, so in video
                // mode the row goes away entirely. The reset used to live in
                // it and had to be kept visible; it has its own row now.
                _stillActions.Visible = image;
                _leftTlp.RowStyles[8].Height = image ? S(38) : 0;
                _leftTlp.PerformLayout();
            }
            if (image) _imagePanel.Bounds = _videoPanel.Bounds; // same cell, same size
            _root?.PerformLayout();
            FitSectionHeights();
        }

        // =================================================================
        // Running
        // =================================================================
        private void StartRun()
        {
            Tr("StartRun entry");
            // A previous run can leave strays behind: translation children
            // (video_filter.exe --compile-one) sometimes outlive their parent
            // when a job object cannot be assigned, and the extra HIP/driver
            // hands contending on the GPU are exactly what freezes the next
            // run's initialization. Sweep before starting a fresh run.
            if (_proc == null || _proc.HasExited)
            {
                foreach (var name in new[] { "video_filter", "ffmpeg", "ffprobe" })
                {
                    try
                    {
                        foreach (var p in Process.GetProcessesByName(name))
                        {
                            try { p.Kill(); } catch { }
                            p.Dispose();
                        }
                    }
                    catch { }
                }
            }
            if (string.IsNullOrEmpty(_tbInput.Text.Trim()))
            {
                AppendLog("请先选择输入文件（点“选择…”）", Color.OrangeRed);
                return;
            }
            // An empty output field gets a derived name, and an output whose
            // type contradicts the input (png input with *_dlss.mp4 left over
            // from an older session) is corrected the same way.
            if (string.IsNullOrEmpty(_tbOutput.Text.Trim()) ||
                IsImagePath(_tbInput.Text) != IsImagePath(_tbOutput.Text))
                AutoFillOutput();
            CollectFromUi();

            if (!File.Exists(_s.Input)) { AppendLog("输入文件不存在: " + _s.Input, Color.OrangeRed); return; }
            if (string.IsNullOrEmpty(_s.Output)) { AppendLog("无法确定输出文件", Color.OrangeRed); return; }
            foreach (var (dll, key) in new[] { (_s.Snippet, "nvngx_dlssnr.dll"), (_s.Driver, "nvcuda.dll"),
                                               (_s.Runtime, "nvngx.dll"), (_s.Nvapi, "nvapi64.dll") })
            {
                if (!File.Exists(dll)) { AppendLog($"找不到 {key}: {dll}", Color.OrangeRed); return; }
            }
            var exe = Path.Combine(_runDir, "video_filter.exe");
            if (!File.Exists(exe)) { AppendLog("找不到 video_filter.exe: " + exe, Color.OrangeRed); return; }

            var args = BuildArgs();
            _framesDone = 0;
            _framesTotal = -1;
            _progress.Value = 0;
            _lblStatus.Text = "启动中…";
            if (_s.Mode == "video") EstimateTotalFrames();

            _proc = new Process
            {
                StartInfo = new ProcessStartInfo
                {
                    FileName = exe,
                    Arguments = args,
                    UseShellExecute = false,
                    CreateNoWindow = true,
                    // video_filter emits UTF-8 (including the precompile
                    // status text). Without this, .NET decodes it with the
                    // Windows ANSI code page and Chinese becomes mojibake.
                    StandardErrorEncoding = Encoding.UTF8,
                    // video_filter writes its entire log (and its ffmpeg
                    // children's stderr, which they inherit) to standard
                    // error, so that is the only stream redirected.
                    RedirectStandardError = true,
                    WorkingDirectory = _runDir
                },
                EnableRaisingEvents = true
            };
            _proc.ErrorDataReceived += OnData;
            _proc.Exited += OnExited;

            try
            {
                _proc.Start();
                Tr("StartRun: process started");
                _proc.BeginErrorReadLine();
                Tr("StartRun: BeginErrorReadLine done");
            }
            catch (Exception ex)
            {
                AppendLog("启动失败: " + ex.Message, Color.OrangeRed);
                try { _proc?.Kill(true); } catch { }
                _proc?.Dispose();
                _proc = null;
                return;
            }

            _running = true;
            _isFrameHold = false;
            _btnStart.Enabled = false;
            _btnStop.Enabled = true;
            _btnFrameHold.Enabled = false;
            _runElapsed = 0;
            if (_runTimer == null)
            {
                _runTimer = new System.Windows.Forms.Timer { Interval = 1000 };
                _runTimer.Tick += (_, _) =>
                {
                    if (!_running) { _runTimer.Stop(); return; }
                    _runElapsed++;
                    _lblStatus.Text = $"处理中… 已用 {_runElapsed} 秒 · 帧 {_framesDone}" +
                        (_framesTotal > 0 ? $" / ~{_framesTotal}" : "");
                };
            }
            _runTimer.Start();
            AppendLog(">>> 开始 " + (_s.Mode == "video" ? "视频" : "图片") + "处理", Color.LightSkyBlue);
            Tr("StartRun done");
        }

        private void StartFrameHold()
        {
            Tr("StartFrameHold entry");
            if (_proc != null && !_proc.HasExited)
            {
                AppendLog("已有任务正在运行，请先停止。", Color.Orange);
                return;
            }
            if (string.IsNullOrEmpty(_tbInput.Text.Trim()))
            {
                AppendLog("请先选择输入文件（点“选择…”）", Color.OrangeRed);
                return;
            }
            CollectFromUi();
            if (!File.Exists(_s.Input)) { AppendLog("输入文件不存在: " + _s.Input, Color.OrangeRed); return; }
            foreach (var (dll, key) in new[] { (_s.Snippet, "nvngx_dlssnr.dll"), (_s.Driver, "nvcuda.dll"),
                                               (_s.Runtime, "nvngx.dll"), (_s.Nvapi, "nvapi64.dll") })
            {
                if (!File.Exists(dll)) { AppendLog($"找不到 {key}: {dll}", Color.OrangeRed); return; }
            }
            var exe = Path.Combine(_runDir, "video_filter.exe");
            if (!File.Exists(exe)) { AppendLog("找不到 video_filter.exe: " + exe, Color.OrangeRed); return; }

            _frameHoldOut = Path.Combine(_runDir, "frame_hold_out.png");
            if (IsImagePath(_s.Input))
            {
                _frameHoldIn = _s.Input;
            }
            else
            {
                _frameHoldIn = Path.Combine(_runDir, "frame_hold_in.png");
                AppendLog(">>> 正在从视频截取单帧...", Color.LightSkyBlue);
                if (!ExtractVideoFrame(_s.Input, _frameHoldIn))
                {
                    AppendLog("从视频截取单帧失败，请确认视频文件和 FFmpeg 可用", Color.OrangeRed);
                    return;
                }
            }

            var sb = new StringBuilder();
            sb.Append("--image \"").Append(_frameHoldIn).Append("\" \"")
              .Append(_frameHoldOut).Append("\" \"")
              .Append(_s.Snippet).Append("\" \"")
              .Append(_s.Driver).Append("\" \"")
              .Append(_s.Runtime).Append("\" \"")
              .Append(_s.Nvapi).Append('"');
            sb.Append(" --passes ").Append(_s.ImagePasses);
            sb.Append(" --intensity ").Append((_s.Intensity / 100.0).ToString("0.00", System.Globalization.CultureInfo.InvariantCulture));
            sb.Append(" --global-tone ").Append((_s.GlobalTone / 100.0).ToString("0.00", System.Globalization.CultureInfo.InvariantCulture));
            sb.Append(" --local-tone ").Append((_s.LocalTone / 100.0).ToString("0.00", System.Globalization.CultureInfo.InvariantCulture));
            sb.Append(" --local-structure ").Append((_s.LocalStructure / 100.0).ToString("0.00", System.Globalization.CultureInfo.InvariantCulture));
            sb.Append(" --skin-structure ").Append((_s.SkinStructure / 100.0).ToString("0.00", System.Globalization.CultureInfo.InvariantCulture));
            sb.Append(" --style ").Append(_s.Style);
            sb.Append(" --preset ").Append(_s.Preset);
            sb.Append(" --gamma ").Append(_s.Gamma.ToString("0.00", System.Globalization.CultureInfo.InvariantCulture));
            if (!_s.AutoMask) sb.Append(" --no-auto-mask");
            sb.Append(" --precompile-wait");

            _framesDone = 0;
            _framesTotal = 1;
            _progress.Value = 0;
            _lblStatus.Text = "单帧画质增强中…";
            _isFrameHold = true;

            _proc = new Process
            {
                StartInfo = new ProcessStartInfo
                {
                    FileName = exe,
                    Arguments = sb.ToString(),
                    UseShellExecute = false,
                    CreateNoWindow = true,
                    StandardErrorEncoding = Encoding.UTF8,
                    RedirectStandardError = true,
                    WorkingDirectory = _runDir
                },
                EnableRaisingEvents = true
            };
            _proc.ErrorDataReceived += OnData;
            _proc.Exited += OnExited;

            try
            {
                _proc.Start();
                _proc.BeginErrorReadLine();
            }
            catch (Exception ex)
            {
                AppendLog("启动失败: " + ex.Message, Color.OrangeRed);
                _isFrameHold = false;
                try { _proc?.Kill(true); } catch { }
                _proc?.Dispose();
                _proc = null;
                return;
            }

            _running = true;
            _btnStart.Enabled = false;
            _btnStop.Enabled = true;
            _btnFrameHold.Enabled = false;
            _runElapsed = 0;
            if (_runTimer == null)
            {
                _runTimer = new System.Windows.Forms.Timer { Interval = 1000 };
                _runTimer.Tick += (_, _) =>
                {
                    if (!_running) { _runTimer.Stop(); return; }
                    _runElapsed++;
                    _lblStatus.Text = $"单帧处理中… 已用 {_runElapsed} 秒";
                };
            }
            _runTimer.Start();
            AppendLog(">>> 开始单帧定帧降噪增强 -> 准备对比", Color.LightSkyBlue);
        }

        private string BuildArgs()
        {
            var sb = new StringBuilder();
            if (_s.Mode == "image")
            {
                sb.Append("--image ");
                sb.Append('"').Append(_s.Input).Append("\" \"")
                  .Append(_s.Output).Append("\" \"")
                  .Append(_s.Snippet).Append("\" \"")
                  .Append(_s.Driver).Append("\" \"")
                  .Append(_s.Runtime).Append("\" \"")
                  .Append(_s.Nvapi).Append('"');
                // A still has no neighbours to disagree with, so repeating the
                // evaluation only deepens the blend -- which is what makes the
                // effect settle. Video cannot have this (see Passes above).
                sb.Append(" --passes ").Append(_s.ImagePasses);
            }
            else
            {
                sb.Append('"').Append(_s.Input).Append("\" \"")
                  .Append(_s.Output).Append("\" \"")
                  .Append(_s.Snippet).Append("\" \"")
                  .Append(_s.Driver).Append("\" \"")
                  .Append(_s.Runtime).Append("\" \"")
                  .Append(_s.Nvapi).Append('"');
                // The rebuilt video_filter contains the scene-detector size
                // fix, so preserve the user's real auto-reset choice.
                var resetArg = _s.Reset;
                sb.Append(" --reset ").Append(resetArg);
                if (resetArg == "every") sb.Append("=").Append(_s.ResetEvery);
                // One evaluation per frame. Two or more stack the effect and
                // amplify frame-to-frame differences, which is the flicker.
                sb.Append(" --passes ").Append(_s.Passes);
                // Motion-vector guidance: off unless asked for, because the
                // estimated field jitters and the network reprojects by it.
                sb.Append(" --flow ").Append(_s.Flow ? "1" : "0");
                sb.Append(" --upscale-mode ").Append(_s.UpscaleMode);
                sb.Append(" --dlss-model-preset ").Append(_s.DlssModelPreset);
                double cut;
                if (double.TryParse(_s.CutThreshold, System.Globalization.CultureInfo.InvariantCulture, out cut))
                    sb.Append(" --cut-threshold ").Append(cut.ToString("0.00", System.Globalization.CultureInfo.InvariantCulture));
                sb.Append(" --crf ").Append(_s.Crf);
                double fps;
                if (double.TryParse(_s.Fps, System.Globalization.CultureInfo.InvariantCulture, out fps) && fps > 0)
                    sb.Append(" --fps ").Append(fps.ToString("0.00", System.Globalization.CultureInfo.InvariantCulture));
                if (int.TryParse(_s.MaxFrames, out var max) && max > 0) sb.Append(" --max-frames ").Append(max);
                if (!_s.Audio) sb.Append(" --no-audio");
                if (_s.DumpFrames && !string.IsNullOrEmpty(_s.DumpDir))
                    sb.Append(" --dump-frames \"").Append(_s.DumpDir).Append('"');
            }
            sb.Append(" --intensity ").Append((_s.Intensity / 100.0).ToString("0.00", System.Globalization.CultureInfo.InvariantCulture));
            sb.Append(" --global-tone ").Append((_s.GlobalTone / 100.0).ToString("0.00", System.Globalization.CultureInfo.InvariantCulture));
            sb.Append(" --local-tone ").Append((_s.LocalTone / 100.0).ToString("0.00", System.Globalization.CultureInfo.InvariantCulture));
            sb.Append(" --local-structure ").Append((_s.LocalStructure / 100.0).ToString("0.00", System.Globalization.CultureInfo.InvariantCulture));
            sb.Append(" --skin-structure ").Append((_s.SkinStructure / 100.0).ToString("0.00", System.Globalization.CultureInfo.InvariantCulture));
            sb.Append(" --style ").Append(_s.Style);
            sb.Append(" --preset ").Append(_s.Preset);
            sb.Append(" --gamma ").Append(_s.Gamma.ToString("0.00", System.Globalization.CultureInfo.InvariantCulture));
            if (!_s.AutoMask) sb.Append(" --no-auto-mask");
            // 严格串行预热：先单独翻译完，主流程不再并发编译（压低黑屏竞态）
            sb.Append(" --precompile-wait");
            return sb.ToString();
        }

        private void EstimateTotalFrames()
        {
            // Off the UI thread: ffprobe can stall, and the window must keep
            // repainting while the run is going.
            _ = Task.Run(() =>
            {
                var total = ProbeTotalFrames();
                TryInvoke(() => { _framesTotal = total; });
            });
        }

        private long ProbeTotalFrames()
        {
            Tr("ProbeTotalFrames bg entry");
            try
            {
                var pi = new ProcessStartInfo
                {
                    FileName = "ffprobe",
                    Arguments = "-v error -select_streams v:0 -show_entries stream=r_frame_rate -of csv=p=0 \"" + _s.Input + "\"",
                    UseShellExecute = false, CreateNoWindow = true,
                    RedirectStandardOutput = true, RedirectStandardError = true
                };
                using var p = Process.Start(pi);
                var rate = p.StandardOutput.ReadToEnd().Trim();
                p.WaitForExit(5000);
                var m = Regex.Match(rate, @"(\d+)/(\d+)");
                double fps = m.Success ? int.Parse(m.Groups[1].Value) / (double)int.Parse(m.Groups[2].Value) : 0;

                var pi2 = new ProcessStartInfo
                {
                    FileName = "ffprobe",
                    Arguments = "-v error -show_entries format=duration -of csv=p=0 \"" + _s.Input + "\"",
                    UseShellExecute = false, CreateNoWindow = true,
                    RedirectStandardOutput = true, RedirectStandardError = true
                };
                using var p2 = Process.Start(pi2);
                var dur = p2.StandardOutput.ReadToEnd().Trim();
                p2.WaitForExit(5000);
                if (double.TryParse(dur, System.Globalization.CultureInfo.InvariantCulture, out var seconds) && seconds > 0 && fps > 0)
                    return (long)(seconds * fps);
            }
            catch { /* probe failed: keep indeterminate */ }
            return -1;
        }

        private void OnData(object sender, DataReceivedEventArgs e)
        {
            if (string.IsNullOrEmpty(e.Data)) return;
            var line = e.Data;
            Tr("OnData bg");

            if (TryInvoke(() =>
            {
                var mFrame = _reFrame.Match(line);
                if (mFrame.Success)
                {
                    _framesDone++;
                    var ms = mFrame.Groups[2].Value;
                    var avg = mFrame.Groups[3].Value;
                    var resets = mFrame.Groups[4].Value;
                    var blanks = mFrame.Groups[5].Value;
                    _lblStatus.Text = $"帧 {_framesDone} · 本帧 {ms} ms · 平均 {avg} ms · 复位 {resets} · 空白 {blanks}";
                    if (_framesTotal > 0)
                    {
                        var percent = (int)Math.Min(100, 100.0 * _framesDone / _framesTotal);
                        _progress.Value = percent;
                    }
                    AppendLog(line, Color.WhiteSmoke);
                    return;
                }
                var mPre = _rePre.Match(line);
                if (mPre.Success)
                {
                    AppendLog(line, Color.Gold);
                    var done = int.Parse(mPre.Groups[1].Value);
                    var total = int.Parse(mPre.Groups[2].Value);
                    _progress.Value = total > 0 ? Math.Min(99, done * 99 / total) : 0;
                    return;
                }
                if (line.StartsWith("[info]")) { AppendLog(line, Color.Silver); return; }
                if (line.Contains("[warn]")) { AppendLog(line, Color.Orange); return; }
                if (line.StartsWith("[FAIL]")) { AppendLog(line, Color.OrangeRed); return; }
                if (line.StartsWith("[done]")) { AppendLog(line, Color.LightGreen); _progress.Value = 100; return; }
                AppendLog(line, Color.WhiteSmoke);
            })) { }
        }

        private void OnExited(object sender, EventArgs e)
        {
            Tr("OnExited bg");
            // Stop new callbacks before the object is disposed; the reader
            // thread (BeginErrorReadLine) is not blocked now that TryInvoke
            // uses BeginInvoke, so Dispose cannot join a thread that is
            // waiting on this one -- that wait was the freeze.
            if (_proc != null)
            {
                try { _proc.ErrorDataReceived -= OnData; } catch { }
                try { _proc.Exited -= OnExited; } catch { }
            }
            TryInvoke(() =>
            {
                if (!_running) return;
                _running = false;
                _btnStart.Enabled = true;
                _btnStop.Enabled = false;
                _btnFrameHold.Enabled = true;
                if (_framesDone == 0 && _framesTotal < 0 && _progress.Value < 100)
                {
                    // 快速失败,把进度条复位
                    _progress.Value = 0;
                }
                _lblStatus.Text = _framesDone > 0 ? $"完成：{_framesDone} 帧" : "处理结束";
                var code = -1;
                try { code = _proc.ExitCode; } catch { }
                if (code != 0) _lblStatus.Text = "处理失败（exit=" + code + "），见日志";
                AppendLog($">>> 结束 exit={code}", code == 0 ? Color.LightGreen : Color.OrangeRed);
                if (_runTimer != null) _runTimer.Stop();
                _proc.Dispose();
                _proc = null;

                var wasFrameHold = _isFrameHold;
                _isFrameHold = false;
                if (wasFrameHold && code == 0 && File.Exists(_frameHoldOut))
                {
                    AppendLog(">>> 单帧定帧增强完成，正在自动打开对比窗口...", Color.LightGreen);
                    using var form = new CompareForm(_frameHoldIn, _frameHoldOut, "单帧原图 / 增强结果对比");
                    form.ShowDialog(this);
                }
            });
        }

        private void StopRun()
        {
            if (!_running) return;
            _isFrameHold = false;
            try { _proc?.Kill(true); } catch { }
            AppendLog(">>> 已请求停止", Color.Orange);
        }

        private void OpenCompare()
        {
            var input = _tbInput.Text.Trim();
            var output = _tbOutput.Text.Trim();
            if (!File.Exists(input) || !File.Exists(output))
            {
                AppendLog("请先确认原文件和输出文件都存在，再打开对比", Color.Orange);
                return;
            }
            var temp = new List<string>();
            try
            {
                if (!IsImagePath(input))
                {
                    var stem = Path.Combine(Path.GetTempPath(), "dlssnr_compare_" + Guid.NewGuid().ToString("N"));
                    var before = stem + "_before.png";
                    var after = stem + "_after.png";
                    if (!ExtractVideoFrame(input, before) || !ExtractVideoFrame(output, after))
                    {
                        AppendLog("无法抽取视频帧进行对比，请确认 FFmpeg 可用", Color.OrangeRed);
                        return;
                    }
                    temp.Add(before); temp.Add(after);
                    input = before; output = after;
                }
                using var form = new CompareForm(input, output, "原文件 / 处理结果对比");
                form.ShowDialog(this);
            }
            catch (Exception ex)
            {
                AppendLog("打开对比失败: " + ex.Message, Color.OrangeRed);
            }
            finally
            {
                foreach (var path in temp) { try { File.Delete(path); } catch { } }
            }
        }

        private static bool ExtractVideoFrame(string video, string png)
        {
            var ffmpeg = Environment.GetEnvironmentVariable("FFMPEG_PATH");
            if (string.IsNullOrWhiteSpace(ffmpeg)) ffmpeg = "ffmpeg";
            else if (Directory.Exists(ffmpeg)) ffmpeg = Path.Combine(ffmpeg, "ffmpeg.exe");
            try
            {
                using var p = new Process { StartInfo = new ProcessStartInfo
                {
                    FileName = ffmpeg, UseShellExecute = false, CreateNoWindow = true,
                    RedirectStandardError = true
                }};
                foreach (var arg in new[] { "-nostdin", "-v", "error", "-i", video,
                    "-frames:v", "1", "-y", png }) p.StartInfo.ArgumentList.Add(arg);
                if (!p.Start() || !p.WaitForExit(60000)) { try { p.Kill(true); } catch { } return false; }
                return p.ExitCode == 0 && File.Exists(png);
            }
            catch { return false; }
        }

        private void AppendLog(string line, Color color)
        {
            Tr("AppendLog entry");
            // Mirror every line to a file beside the exe, so a run can be
            // examined after the fact ("看看日志").
            try
            {
                var logPath = Path.Combine(_runDir, "gui_log.txt");
                if (new FileInfo(logPath).Length > 4 * 1024 * 1024)
                    File.WriteAllText(logPath, "");
                File.AppendAllText(logPath,
                    DateTime.Now.ToString("HH:mm:ss ") + line + Environment.NewLine);
            }
            catch { }
            _log.SelectionStart = _log.TextLength;
            _log.SelectionColor = color;
            _log.AppendText(line + Environment.NewLine);
            _log.SelectionStart = _log.TextLength;
            _log.ScrollToCaret();
            if (_log.TextLength > 200000) _log.Text = _log.Text.Substring(_log.TextLength / 2);
            Tr("AppendLog exit");
        }

        private bool TryInvoke(Action action)
        {
            if (IsDisposed) return false;
            try
            {
                // BeginInvoke, never Invoke: the background stderr reader must
                // never block waiting for this thread. (A synchronous Invoke
                // plus Process.Dispose on this thread deadlocks: Dispose joins
                // the reader thread while the reader waits for the UI to
                // process the Invoke -- the window froze exactly there at the
                // end of every run.)
                if (InvokeRequired) BeginInvoke(action);
                else action();
                return true;
            }
            catch { return false; }
        }

        protected override void OnShown(EventArgs e)
        {
            base.OnShown(e);
            // Size the fixed sections to their real content, so no font size /
            // DPI can clip a row inside a GroupBox. Row styles are absolute, so
            // (re)setting their heights here is the single reliable lever.
            try
            {
                if (_filesTlp != null && _leftTlp != null && _vtTlp != null)
                {
                    FitSectionHeights();
                    // Second dump after the fit, so the diagnostic shows the
                    // final geometry rather than the pre-fit one.
                    if (Environment.GetEnvironmentVariable("DLSSNRFILTER_DUMP_LAYOUT") == "1")
                        DumpLayout("", Controls);
                }
            }
            catch { }
        }

        protected override void OnFormClosing(FormClosingEventArgs e)
        {
            if (_running)
            {
                try { _proc?.Kill(true); } catch { }
            }
            SaveSettings();
            base.OnFormClosing(e);
        }

        protected override void OnFormClosed(FormClosedEventArgs e)
        {
            // Belt and braces: the window is gone, so nothing may keep the
            // process alive (stray modal dialogs, queued callbacks, ffmpeg
            // children that outlived the kill).
            try
            {
                if (_proc != null)
                {
                    try { if (!_proc.HasExited) _proc.Kill(true); } catch { }
                    _proc.Dispose();
                    _proc = null;
                }
            }
            catch { }
            base.OnFormClosed(e);
        }
    }

    internal static class SettingsExtensions
    {
        public static void CopyTo(this GuiSettings from, GuiSettings to)
        {
            to.Mode = from.Mode; to.Input = from.Input; to.Output = from.Output;
            to.Snippet = from.Snippet; to.Driver = from.Driver;
            to.Runtime = from.Runtime; to.Nvapi = from.Nvapi;
            to.Intensity = from.Intensity; to.GlobalTone = from.GlobalTone;
            to.LocalTone = from.LocalTone; to.LocalStructure = from.LocalStructure;
            to.SkinStructure = from.SkinStructure;
            to.Style = from.Style; to.Preset = from.Preset; to.DlssModelPreset = from.DlssModelPreset; to.AutoMask = from.AutoMask;
            to.Passes = from.Passes; to.ImagePasses = from.ImagePasses;
            to.Reset = from.Reset; to.ResetEvery = from.ResetEvery; to.Flow = from.Flow; to.UpscaleMode = from.UpscaleMode;
            to.CutThreshold = from.CutThreshold; to.Crf = from.Crf;
            to.Fps = from.Fps; to.MaxFrames = from.MaxFrames;
            to.Audio = from.Audio; to.DumpFrames = from.DumpFrames; to.DumpDir = from.DumpDir;
        }
    }
}
