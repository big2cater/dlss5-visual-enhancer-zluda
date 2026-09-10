using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Windows.Forms;

namespace DlssnrFilter
{
    internal sealed class CompareForm : Form
    {
        private readonly CompareView _view;

        public CompareForm(string beforePath, string afterPath, string title)
        {
            Text = title;
            StartPosition = FormStartPosition.CenterParent;
            ClientSize = new Size(1100, 760);
            MinimumSize = new Size(640, 420);
            BackColor = Color.FromArgb(24, 24, 28);
            var layout = new TableLayoutPanel { Dock = DockStyle.Fill, RowCount = 2, ColumnCount = 1 };
            layout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 42));
            _view = new CompareView(Image.FromFile(beforePath), Image.FromFile(afterPath)) { Dock = DockStyle.Fill };
            var hint = new Label { Dock = DockStyle.Fill, ForeColor = Color.WhiteSmoke,
                Text = "拖动分割线比较原文件和处理结果", TextAlign = ContentAlignment.MiddleCenter };
            layout.Controls.Add(_view, 0, 0);
            layout.Controls.Add(hint, 0, 1);
            Controls.Add(layout);
            FormClosed += (_, _) => _view.DisposeImages();
        }

        private sealed class CompareView : Control
        {
            private readonly Image _before;
            private readonly Image _after;
            private float _split = 0.5f;
            private bool _dragging;

            public CompareView(Image before, Image after)
            {
                _before = before;
                _after = after;
                DoubleBuffered = true;
                SetStyle(ControlStyles.ResizeRedraw | ControlStyles.UserMouse, true);
                Cursor = Cursors.SizeWE;
            }

            public void DisposeImages()
            {
                _before.Dispose();
                _after.Dispose();
            }

            protected override void OnMouseDown(MouseEventArgs e)
            {
                if (e.Button == MouseButtons.Left) { _dragging = true; SetSplit(e.X); }
                base.OnMouseDown(e);
            }

            protected override void OnMouseMove(MouseEventArgs e)
            {
                if (_dragging) SetSplit(e.X);
                base.OnMouseMove(e);
            }

            protected override void OnMouseUp(MouseEventArgs e)
            {
                if (e.Button == MouseButtons.Left) _dragging = false;
                base.OnMouseUp(e);
            }

            private void SetSplit(int x)
            {
                _split = Math.Clamp((float)x / Math.Max(1, ClientSize.Width), 0.02f, 0.98f);
                Invalidate();
            }

            protected override void OnPaint(PaintEventArgs e)
            {
                base.OnPaint(e);
                e.Graphics.Clear(Color.FromArgb(24, 24, 28));
                var target = FitRect(_before.Size, ClientRectangle);
                e.Graphics.InterpolationMode = InterpolationMode.HighQualityBicubic;
                e.Graphics.PixelOffsetMode = PixelOffsetMode.HighQuality;
                e.Graphics.DrawImage(_before, target);
                var splitX = (int)Math.Round(ClientSize.Width * _split);
                var state = e.Graphics.Save();
                e.Graphics.SetClip(new Rectangle(splitX, 0, ClientSize.Width - splitX, ClientSize.Height));
                e.Graphics.DrawImage(_after, target);
                e.Graphics.Restore(state);
                using var pen = new Pen(Color.White, 2);
                e.Graphics.DrawLine(pen, splitX, 0, splitX, ClientSize.Height);
                using var brush = new SolidBrush(Color.FromArgb(210, Color.Black));
                e.Graphics.FillRectangle(brush, Math.Max(0, splitX - 34), 10, 68, 24);
                using var font = new Font(Font.FontFamily, 9F);
                TextRenderer.DrawText(e.Graphics, "原文件", Font, new Rectangle(8, 10, 72, 24), Color.White,
                    TextFormatFlags.Left | TextFormatFlags.VerticalCenter);
                TextRenderer.DrawText(e.Graphics, "处理结果", Font, new Rectangle(ClientSize.Width - 82, 10, 74, 24), Color.White,
                    TextFormatFlags.Right | TextFormatFlags.VerticalCenter);
                TextRenderer.DrawText(e.Graphics, "◀ ▶", font, new Rectangle(splitX - 34, 10, 68, 24), Color.White,
                    TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter);
            }

            private static Rectangle FitRect(Size image, Rectangle box)
            {
                if (image.Width <= 0 || image.Height <= 0) return Rectangle.Empty;
                var scale = Math.Min((double)box.Width / image.Width, (double)box.Height / image.Height);
                var w = Math.Max(1, (int)Math.Round(image.Width * scale));
                var h = Math.Max(1, (int)Math.Round(image.Height * scale));
                return new Rectangle((box.Width - w) / 2, (box.Height - h) / 2, w, h);
            }
        }
    }
}
