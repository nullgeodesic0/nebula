using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Threading;
using System.Windows.Forms;

namespace Nebula.Plus
{
    /// <summary>
    /// The Nebula Plus window: install into a folder from the official Nebula and the user's mod files, check for and
    /// install updates, roll back, rebuild the mods, copy the official saves, and start the game or an online session.
    /// </summary>
    internal sealed class PlusForm : Form, IPlusProgress
    {
        private readonly Dictionary<string, string> options;
        private readonly TextBox root = new TextBox(), official = new TextBox(), disc = new TextBox();
        private readonly Dictionary<string, TextBox> modBoxes = new Dictionary<string, TextBox>();
        private readonly CheckBox importSaves = new CheckBox(), importSettings = new CheckBox(), sharedSaves = new CheckBox();
        private readonly ComboBox channel = new ComboBox();
        private readonly Label status = new Label(), step = new Label();
        private readonly ProgressBar bar = new ProgressBar();
        private readonly TextBox log = new TextBox();
        private readonly List<Button> actions = new List<Button>();
        private Button install, update, rollback, rebuild, play, host, join;
        private CancellationTokenSource running;

        public PlusForm(Dictionary<string, string> options)
        {
            this.options = options;
            Text = "Nebula Plus";
            try { Icon = Icon.ExtractAssociatedIcon(System.Reflection.Assembly.GetExecutingAssembly().Location); } catch (Exception) { }
            ClientSize = new Size(900, 760);
            MinimumSize = new Size(760, 600);
            Font = new Font("Segoe UI", 9f);
            var layout = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 3, Padding = new Padding(10), AutoScroll = true };
            layout.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 250));
            layout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            layout.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 90));
            Controls.Add(layout);

            status.AutoSize = true;
            status.Font = new Font(Font, FontStyle.Bold);
            AddWide(layout, status);
            AddRow(layout, "Nebula Plus folder", root, true, false);
            AddRow(layout, "Official Nebula install (read only)", official, true, false);
            AddRow(layout, "Game disc (only if the official install has no game files)", disc, false, false);
            channel.DropDownStyle = ComboBoxStyle.DropDownList;
            channel.Items.AddRange(new object[] { "release: signed builds from GitHub releases", "branch: build the branch's newest commit on this PC (developer tools)" });
            AddRow(layout, "Updates", channel, false, false, false);

            var savesRow = new FlowLayoutPanel { AutoSize = true, Dock = DockStyle.Fill };
            importSaves.Text = "Copy the official saves now (once; never moves or changes them)";
            importSaves.AutoSize = true;
            importSettings.Text = "Copy the official settings";
            importSettings.AutoSize = true;
            sharedSaves.Text = "Share the official saves folder instead of Plus's own";
            sharedSaves.AutoSize = true;
            savesRow.Controls.AddRange(new Control[] { importSaves, importSettings, sharedSaves });
            AddLabeled(layout, "Saves and settings", savesRow);

            var modsTitle = new Label { Text = "Mod input files (your own; nothing is uploaded or copied out of them):", AutoSize = true, Padding = new Padding(0, 8, 0, 0) };
            AddWide(layout, modsTitle);
            foreach (var input in ModInput.All)
            {
                var box = new TextBox();
                modBoxes[input.Key] = box;
                AddRow(layout, input.Label, box, input.Folder, true);
            }

            var buttons = new FlowLayoutPanel { AutoSize = true, Dock = DockStyle.Fill, WrapContents = true };
            install = Action(buttons, "Install", delegate { RunInstall(null); });
            update = Action(buttons, "Check for updates", delegate { RunUpdate(); });
            rollback = Action(buttons, "Roll back", delegate { RunRollback(); });
            rebuild = Action(buttons, "Rebuild mods", delegate { RunRebuildMods(); });
            Action(buttons, "Find my mod files", delegate { DetectMods(true); });
            play = Action(buttons, "Play", delegate { Launch("Play Nebula Plus.cmd", null); });
            host = Action(buttons, "Host online", delegate { Launch("Host online game.cmd", null); });
            join = Action(buttons, "Join online...", delegate
            {
                string address = Prompt("Host address (name or IP, optionally :port):");
                if (!string.IsNullOrEmpty(address)) Launch("Join online game.cmd", address);
            });
            Action(buttons, "Open folder", delegate { Process.Start("explorer.exe", "\"" + root.Text + "\""); });
            AddWide(layout, buttons);

            step.AutoSize = true;
            AddWide(layout, step);
            bar.Dock = DockStyle.Fill;
            bar.Maximum = 1000;
            AddWide(layout, bar);
            log.Multiline = true;
            log.ReadOnly = true;
            log.ScrollBars = ScrollBars.Vertical;
            log.Dock = DockStyle.Fill;
            log.Height = 200;
            log.Font = new Font("Consolas", 8.5f);
            AddWide(layout, log);

            LoadState(RootFromOptions());
            root.Leave += delegate { LoadState(root.Text); };
            Shown += delegate
            {
                UiScale.Apply(this);
                if (options.ContainsKey("update") && PlusInstalled.Read(new PlusLayout(root.Text)) != null) RunUpdate();
            };
            FormClosing += delegate(object sender, FormClosingEventArgs e)
            {
                if (running != null && MessageBox.Show(this, "Nebula Plus is still working. Stop it? Nothing is switched until it finishes.",
                        Text, MessageBoxButtons.YesNo, MessageBoxIcon.Warning) != DialogResult.Yes) e.Cancel = true;
                else if (running != null) running.Cancel();
            };
        }

        private string RootFromOptions() { return PlusProgram.RootFrom(options); }

        // ---- layout helpers ------------------------------------------------------

        private static void AddWide(TableLayoutPanel layout, Control control)
        {
            layout.Controls.Add(control);
            layout.SetColumnSpan(control, 3);
        }

        private static void AddLabeled(TableLayoutPanel layout, string label, Control control)
        {
            layout.Controls.Add(new Label { Text = label, AutoSize = true, Anchor = AnchorStyles.Left, Padding = new Padding(0, 6, 0, 0) });
            layout.Controls.Add(control);
            layout.SetColumnSpan(control, 2);
        }

        private void AddRow(TableLayoutPanel layout, string label, Control field, bool folder, bool allowFile, bool browse = true)
        {
            layout.Controls.Add(new Label { Text = label, AutoSize = true, Anchor = AnchorStyles.Left, Padding = new Padding(0, 6, 0, 0) });
            field.Dock = DockStyle.Fill;
            layout.Controls.Add(field);
            if (!browse) { layout.Controls.Add(new Label()); return; }
            var button = new Button { Text = "Browse...", AutoSize = true };
            button.Click += delegate
            {
                if (folder)
                {
                    using (var dialog = new FolderBrowserDialog { SelectedPath = field.Text })
                        if (dialog.ShowDialog(this) == DialogResult.OK) field.Text = dialog.SelectedPath;
                }
                else
                {
                    using (var dialog = new OpenFileDialog { FileName = field.Text, CheckFileExists = true })
                        if (dialog.ShowDialog(this) == DialogResult.OK) field.Text = dialog.FileName;
                }
            };
            layout.Controls.Add(button);
        }

        private Button Action(FlowLayoutPanel panel, string text, EventHandler handler)
        {
            var button = new Button { Text = text, AutoSize = true, Padding = new Padding(6, 2, 6, 2) };
            button.Click += handler;
            panel.Controls.Add(button);
            actions.Add(button);
            return button;
        }

        // ---- state ---------------------------------------------------------------

        private void LoadState(string folder)
        {
            if (string.IsNullOrWhiteSpace(folder)) return;
            var layout = new PlusLayout(folder);
            root.Text = layout.Root;
            var config = PlusConfig.Load(layout);
            var found = OfficialNebula.Find(config.OfficialRoot);
            official.Text = config.OfficialRoot ?? (found != null ? found.Root : "");
            disc.Text = config.Disc ?? "";
            channel.SelectedIndex = config.Channel == "branch" ? 1 : 0;
            sharedSaves.Checked = config.SavesMode == "shared";
            foreach (var pair in modBoxes) pair.Value.Text = config.ModInputs.ContainsKey(pair.Key) ? config.ModInputs[pair.Key] : "";
            var installed = PlusInstalled.Read(layout);
            importSaves.Checked = installed == null && !Directory.Exists(layout.Saves);
            importSettings.Checked = installed == null && !File.Exists(layout.RuntimeSettings);
            if (installed == null && config.ModInputs.Count == 0) DetectMods(false);
            UpdateStatus(layout, installed);
        }

        private void UpdateStatus(PlusLayout layout, PlusInstalled installed)
        {
            status.Text = installed == null ? "Nebula Plus is not installed in " + layout.Root + "."
                : "Installed: Nebula Plus " + installed.Version + " (commit " + (installed.Commit ?? "?").Substring(0, Math.Min(12, (installed.Commit ?? "?").Length)) + ")" +
                  (installed.Previous != null ? "; previous " + installed.Previous + " kept for rollback" : "");
            install.Text = installed == null ? "Install" : "Repair / apply changes";
            foreach (var button in new[] { update, rollback, rebuild, play, host, join }) button.Enabled = installed != null && running == null;
            rollback.Enabled = rollback.Enabled && installed.Previous != null;
        }

        private void DetectMods(bool report)
        {
            var found = ModInput.Detect();
            int filled = 0;
            foreach (var pair in found)
                if (modBoxes.ContainsKey(pair.Key) && modBoxes[pair.Key].Text.Length == 0) { modBoxes[pair.Key].Text = pair.Value; filled++; }
            if (report) Log("Found " + found.Count + " mod input files (" + filled + " new): " + string.Join(", ", found.Keys));
        }

        private PlusRequest RequestFromForm()
        {
            var layout = new PlusLayout(root.Text);
            var request = new PlusRequest
            {
                Root = layout.Root, OfficialRoot = official.Text.Length != 0 ? official.Text : null, Disc = disc.Text.Length != 0 ? disc.Text : null,
                ImportSaves = importSaves.Checked, ImportSettings = importSettings.Checked, SavesMode = sharedSaves.Checked ? "shared" : "own",
                Channel = channel.SelectedIndex == 1 ? "branch" : "release",
                ModInputs = modBoxes.ToDictionary(p => p.Key, p => p.Value.Text.Trim())
            };
            if (request.Disc != null && OfficialNebula.Find(request.OfficialRoot) != null && OfficialNebula.Find(request.OfficialRoot).ContentValid())
                request.Disc = null; // the official install's game files are used; the disc is kept in plus.json for later
            return request;
        }

        // ---- actions -------------------------------------------------------------

        private void RunInstall(Func<PlusRequest, PlusRequest> adjust)
        {
            var request = RequestFromForm();
            if (adjust != null) request = adjust(request);
            var layout = new PlusLayout(request.Root);
            if (sharedSaves.Checked && MessageBox.Show(this, "With shared saves, Nebula Plus plays the official Nebula's own save files in " + OfficialNebula.Saves +
                    ". Co-op and the mods can write to them. Use shared saves?", Text, MessageBoxButtons.YesNo, MessageBoxIcon.Warning) != DialogResult.Yes) return;
            Work("Installing", delegate(CancellationToken cancel)
            {
                var config = PlusConfig.Load(layout);
                config.Channel = request.Channel;
                if (request.Package == null)
                {
                    var installed = PlusInstalled.Read(layout);
                    if (installed != null && Directory.Exists(Path.Combine(installed.Directory, "package")))
                        request.Package = Path.Combine(installed.Directory, "package");
                    else
                    {
                        var available = PlusUpdater.FindNewer(config, null);
                        if (available == null) throw new InvalidOperationException("No Nebula Plus release is published on " + config.Repository + " yet.");
                        Log("Downloading " + available.Describe());
                        request.Package = PlusProgram.Fetch(layout, config, available, false, Log);
                    }
                }
                var engine = new PlusEngine(request, this, cancel);
                Log("Log: " + engine.LogPath);
                engine.Run();
                if (config.Channel != request.Channel) { config = PlusConfig.Load(layout); config.Channel = request.Channel; config.Save(layout); }
                return engine.Summary;
            });
        }

        private void RunUpdate()
        {
            var layout = new PlusLayout(root.Text);
            Work("Checking for updates", delegate(CancellationToken cancel)
            {
                var config = PlusConfig.Load(layout);
                var installed = PlusInstalled.Read(layout);
                Log("Checking " + config.Repository + " (" + (config.Channel == "branch" ? "branch " + config.Branch : "releases") + ")");
                var available = PlusUpdater.FindNewer(config, installed);
                if (available == null) return "Nebula Plus is up to date (" + installed.Version + ").";
                bool yes = false;
                Invoke((MethodInvoker)delegate
                {
                    yes = MessageBox.Show(this, "Install " + available.Describe() + "?\r\n\r\n" + Shorten(available.Notes) +
                        "\r\n\r\nSaves, settings and your mod files are not touched; the current version is kept for rollback.",
                        Text, MessageBoxButtons.YesNo, MessageBoxIcon.Question) == DialogResult.Yes;
                });
                if (!yes) return "Update not installed.";
                var request = new PlusRequest { Root = layout.Root, Channel = config.Channel };
                request.Package = PlusProgram.Fetch(layout, config, available, false, Log);
                var engine = new PlusEngine(request, this, cancel);
                Log("Log: " + engine.LogPath);
                engine.Run();
                return engine.Summary;
            });
        }

        private void RunRebuildMods()
        {
            var layout = new PlusLayout(root.Text);
            var installed = PlusInstalled.Read(layout);
            if (installed == null) return;
            RunInstall(delegate(PlusRequest r)
            {
                r.Package = Path.Combine(installed.Directory, "package");
                r.ForceMods = true;
                r.ImportSaves = r.ImportSettings = false;
                return r;
            });
        }

        private void RunRollback()
        {
            var layout = new PlusLayout(root.Text);
            Work("Rolling back", delegate(CancellationToken cancel) { return PlusProgram.Rollback(layout, Log); });
        }

        private void Launch(string launcher, string argument)
        {
            string path = Path.Combine(root.Text, launcher);
            if (!File.Exists(path)) { MessageBox.Show(this, path + " is missing. Repair Nebula Plus.", Text); return; }
            var start = new ProcessStartInfo(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "cmd.exe"),
                "/c \"\"" + path + "\"" + (argument != null ? " \"" + argument.Replace("\"", "") + "\"" : "") + "\"");
            start.WorkingDirectory = root.Text;
            start.UseShellExecute = false;
            Process.Start(start);
        }

        private void Work(string title, Func<CancellationToken, string> work)
        {
            if (running != null) return;
            running = new CancellationTokenSource();
            var token = running.Token;
            foreach (var button in actions) button.Enabled = false;
            Report(0, title);
            var thread = new Thread(delegate()
            {
                string message;
                bool failed = false;
                try { message = work(token); }
                catch (Exception error) { message = error.Message; failed = true; Log("ERROR: " + error); }
                BeginInvoke((MethodInvoker)delegate
                {
                    running = null;
                    foreach (var button in actions) button.Enabled = true;
                    var layout = new PlusLayout(root.Text);
                    UpdateStatus(layout, PlusInstalled.Read(layout));
                    step.Text = failed ? "Failed." : "Done.";
                    if (!string.IsNullOrEmpty(message))
                        MessageBox.Show(this, message, Text, MessageBoxButtons.OK, failed ? MessageBoxIcon.Error : MessageBoxIcon.Information);
                });
            });
            thread.IsBackground = true;
            thread.Start();
        }

        private static string Shorten(string text)
        {
            text = (text ?? "").Trim();
            return text.Length > 600 ? text.Substring(0, 600) + "..." : text;
        }

        private string Prompt(string question)
        {
            using (var dialog = new Form { Text = Text, ClientSize = new Size(420, 110), FormBorderStyle = FormBorderStyle.FixedDialog, StartPosition = FormStartPosition.CenterParent, MinimizeBox = false, MaximizeBox = false })
            {
                var label = new Label { Text = question, Left = 10, Top = 10, Width = 400 };
                var box = new TextBox { Left = 10, Top = 35, Width = 400 };
                var ok = new Button { Text = "Join", Left = 250, Top = 70, DialogResult = DialogResult.OK };
                var cancel = new Button { Text = "Cancel", Left = 335, Top = 70, DialogResult = DialogResult.Cancel };
                dialog.Controls.AddRange(new Control[] { label, box, ok, cancel });
                dialog.AcceptButton = ok;
                dialog.CancelButton = cancel;
                return dialog.ShowDialog(this) == DialogResult.OK ? box.Text.Trim() : null;
            }
        }

        // ---- IPlusProgress ---------------------------------------------------------

        public void Report(double overall, string text)
        {
            if (InvokeRequired) { BeginInvoke((MethodInvoker)delegate { Report(overall, text); }); return; }
            bar.Value = Math.Max(0, Math.Min(1000, (int)(overall * 1000)));
            step.Text = text;
        }

        public void Log(string line)
        {
            if (InvokeRequired) { BeginInvoke((MethodInvoker)delegate { Log(line); }); return; }
            if (log.TextLength > 200000) log.Text = log.Text.Substring(100000);
            log.AppendText(line + "\r\n");
        }

        public bool Ask(string question)
        {
            bool yes = false;
            Invoke((MethodInvoker)delegate
            {
                yes = MessageBox.Show(this, question, Text, MessageBoxButtons.YesNo, MessageBoxIcon.Question) == DialogResult.Yes;
            });
            return yes;
        }
    }
}
