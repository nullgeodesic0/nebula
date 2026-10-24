using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows.Forms;

[assembly: System.Reflection.AssemblyTitle("Nebula Plus")]

namespace Nebula.Plus
{
    internal static class PlusProgram
    {
        /// <summary>
        /// NebulaPlus.exe [--root PATH] [--update]                     the window (--update checks at once)
        /// NebulaPlus.exe --cli install|update|rollback|build-mods|import-saves|status|detect-mods [options]
        ///   --root PATH  --nebula OFFICIAL_ROOT  --disc IMAGE  --package ZIP|DIR  --channel release|branch  --branch NAME
        ///   --detect-mods  --mod KEY=PATH  --import-saves  --import-settings  --shared-saves  --own-saves  --module-isa SSE2|AVX2
        ///   --accept-build-tools-license  --allow-python-venv  --allow-unsigned  --jobs N  --force-modules  --force-mods
        ///   --skip-mods  --check
        /// </summary>
        [STAThread]
        private static int Main(string[] args)
        {
            var options = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            var mods = new Dictionary<string, string>(StringComparer.Ordinal);
            string command = null;
            var valued = new HashSet<string>(StringComparer.OrdinalIgnoreCase) { "root", "nebula", "disc", "package", "channel", "branch", "mod", "jobs", "module-isa", "cli" };
            for (int i = 0; i < args.Length; i++)
            {
                string arg = args[i];
                if (!arg.StartsWith("--")) continue;
                string name = arg.Substring(2);
                string value = valued.Contains(name) && i + 1 < args.Length ? args[++i] : "";
                if (name == "mod")
                {
                    int eq = value.IndexOf('=');
                    if (eq > 0) mods[value.Substring(0, eq)] = value.Substring(eq + 1);
                    continue;
                }
                if (name == "cli") { command = value; continue; }
                options[name] = value;
            }
            if (command == null)
            {
                NativeMethods.SetProcessDpiAwarenessContext(new IntPtr(-4));
                Application.EnableVisualStyles();
                Application.SetCompatibleTextRenderingDefault(false);
                Application.Run(new PlusForm(options));
                return 0;
            }
            AttachConsole(-1);
            try { return Cli(command, options, mods); }
            catch (Exception error)
            {
                Console.Error.WriteLine("ERROR: " + error.Message);
                return 1;
            }
        }

        public static string RootFrom(Dictionary<string, string> options)
        {
            if (options.ContainsKey("root") && options["root"].Length != 0) return Path.GetFullPath(options["root"]);
            // NebulaPlus.exe inside an install (versions\<v>\) manages that install.
            string self = Path.GetDirectoryName(Path.GetFullPath(System.Reflection.Assembly.GetExecutingAssembly().Location));
            string versions = Path.GetDirectoryName(self);
            if (versions != null && string.Equals(Path.GetFileName(versions), "versions", StringComparison.OrdinalIgnoreCase))
                return Path.GetDirectoryName(versions);
            return PlusLayout.DefaultRoot;
        }

        private sealed class ConsoleProgress : IPlusProgress
        {
            private readonly Dictionary<string, string> options;
            private string last;
            public ConsoleProgress(Dictionary<string, string> options) { this.options = options; }
            public void Report(double overall, string step)
            {
                if (step == last) return;
                last = step;
                Console.WriteLine(string.Format("[{0,3:0}%] {1}", overall * 100, step));
            }
            public void Log(string line) { if (options.ContainsKey("verbose")) Console.WriteLine("  " + line); }
            public bool Ask(string question)
            {
                Console.WriteLine("QUESTION (answered no; pass the matching option to agree): " + question);
                return false;
            }
        }

        private static int Cli(string command, Dictionary<string, string> options, Dictionary<string, string> mods)
        {
            var layout = new PlusLayout(RootFrom(options));
            var progress = new ConsoleProgress(options);
            switch (command)
            {
                case "detect-mods":
                    foreach (var pair in ModInput.Detect()) Console.WriteLine(pair.Key + "=" + pair.Value);
                    return 0;
                case "status":
                    Console.WriteLine(Status(layout));
                    return 0;
                case "rollback":
                    Console.WriteLine(Rollback(layout, Console.WriteLine));
                    return 0;
                case "import-saves":
                {
                    string target = SavesImport.ImportSaves(layout, Console.WriteLine);
                    if (options.ContainsKey("import-settings")) SavesImport.ImportSettings(layout, Console.WriteLine);
                    return target != null ? 0 : 1;
                }
                case "install":
                case "update":
                case "build-mods":
                    break;
                default:
                    throw new ArgumentException("Unknown command " + command + ".");
            }
            var request = RequestFrom(layout, options, mods);
            if (command == "build-mods")
            {
                var installed = PlusInstalled.Read(layout);
                if (installed == null) throw new InvalidOperationException("Nebula Plus is not installed in " + layout.Root + ".");
                request.Package = Path.Combine(installed.Directory, "package");
                request.Channel = installed.Channel ?? "release";
                request.ForceMods = true;
            }
            else if (command == "update" || request.Package == null)
            {
                var config = PlusConfig.Load(layout);
                if (options.ContainsKey("channel")) config.Channel = options["channel"];
                if (options.ContainsKey("branch")) config.Branch = options["branch"];
                var installed = command == "update" ? PlusInstalled.Read(layout) : null;
                Console.WriteLine("Checking " + config.Repository + " (" + (config.Channel == "branch" ? "branch " + config.Branch : "releases") + ")...");
                var update = PlusUpdater.FindNewer(config, installed);
                if (update == null)
                {
                    Console.WriteLine(installed != null ? "Nebula Plus is up to date (" + installed.Version + ")." : "No Nebula Plus release is published yet; pass --package.");
                    return installed != null ? 0 : 1;
                }
                Console.WriteLine("Available: " + update.Describe());
                if (options.ContainsKey("check")) return 0;
                request.Channel = config.Channel;
                request.Package = Fetch(layout, config, update, options.ContainsKey("allow-unsigned"), Console.WriteLine);
            }
            var engine = new PlusEngine(request, progress, CancellationToken.None);
            Console.WriteLine("Log: " + engine.LogPath);
            engine.Run();
            Console.WriteLine(engine.Summary);
            return 0;
        }

        public static PlusRequest RequestFrom(PlusLayout layout, Dictionary<string, string> options, Dictionary<string, string> mods)
        {
            var request = new PlusRequest
            {
                Root = layout.Root,
                Package = options.ContainsKey("package") ? Path.GetFullPath(options["package"]) : null,
                OfficialRoot = options.ContainsKey("nebula") ? options["nebula"] : null,
                Disc = options.ContainsKey("disc") ? options["disc"] : null,
                ImportSaves = options.ContainsKey("import-saves"),
                ImportSettings = options.ContainsKey("import-settings"),
                ForceModules = options.ContainsKey("force-modules"),
                ForceMods = options.ContainsKey("force-mods"),
                SkipMods = options.ContainsKey("skip-mods"),
                AcceptBuildToolsLicense = options.ContainsKey("accept-build-tools-license"),
                AllowPythonVenv = options.ContainsKey("allow-python-venv"),
                SavesMode = options.ContainsKey("shared-saves") ? "shared" : options.ContainsKey("own-saves") ? "own" : null,
                ModuleIsa = options.ContainsKey("module-isa") ? options["module-isa"].ToUpperInvariant() : null,
                Jobs = options.ContainsKey("jobs") ? int.Parse(options["jobs"]) : 0
            };
            if (request.Package != null && !File.Exists(request.Package) && !Directory.Exists(request.Package))
                throw new FileNotFoundException("No package at " + request.Package + ".");
            var inputs = new Dictionary<string, string>(StringComparer.Ordinal);
            if (options.ContainsKey("detect-mods"))
            {
                var current = PlusConfig.Load(layout).ModInputs;
                foreach (var pair in ModInput.Detect()) if (!current.ContainsKey(pair.Key)) inputs[pair.Key] = pair.Value;
            }
            foreach (var pair in mods)
            {
                if (ModInput.ByKey(pair.Key) == null) throw new ArgumentException("Unknown mod input " + pair.Key + ".");
                inputs[pair.Key] = pair.Value;
            }
            if (inputs.Count != 0) request.ModInputs = inputs;
            return request;
        }

        /// <summary>The update's package: a downloaded, authenticated zip, or a folder built from the branch's commit.</summary>
        public static string Fetch(PlusLayout layout, PlusConfig config, PlusUpdate update, bool allowUnsigned, Action<string> log)
        {
            string downloads = Path.Combine(layout.Root, "downloads");
            Directory.CreateDirectory(downloads);
            if (update.Kind == "release")
                return PlusUpdater.DownloadRelease(update, downloads, allowUnsigned, log, null, CancellationToken.None);
            string work = Path.Combine(downloads, "branch-" + update.Commit.Substring(0, 12));
            if (Directory.Exists(work)) FileUtil.DeleteTree(work);
            Directory.CreateDirectory(work);
            return PlusUpdater.BuildCommit(config, update, work, log, CancellationToken.None);
        }

        public static string Status(PlusLayout layout)
        {
            var installed = PlusInstalled.Read(layout);
            var config = PlusConfig.Load(layout);
            var lines = new List<string> { "Nebula Plus folder: " + layout.Root };
            if (installed == null) lines.Add("Not installed.");
            else
            {
                lines.Add("Installed: " + installed.Version + ", commit " + installed.Commit + " (" + (installed.Channel ?? "release") + " channel)");
                lines.Add("Game modules: " + installed.ModuleIsa + ", module key " + (installed.ModuleKey ?? "").Substring(0, Math.Min(16, (installed.ModuleKey ?? "").Length)));
                lines.Add("Previous version (rollback): " + (installed.Previous ?? "none"));
                string modsJson = Path.Combine(layout.Mods, "plus-mods.json");
                if (File.Exists(modsJson))
                {
                    var mods = Json.Load(modsJson);
                    var built = Json.List(mods, "built");
                    var skipped = Json.List(mods, "skipped");
                    lines.Add("Mods: " + (built != null && built.Count != 0 ? string.Join(", ", built.Cast<object>()) : "none"));
                    if (skipped != null) foreach (var s in skipped) lines.Add("  skipped: " + s);
                }
            }
            lines.Add("Updates: " + config.Repository + " " + (config.Channel == "branch" ? "branch " + config.Branch : "releases (" + PlusUpdater.TagPrefix + "*)"));
            lines.Add("Saves: " + (config.SavesMode == "shared" ? OfficialNebula.Saves + " (shared with the official Nebula)" : layout.Saves));
            return string.Join("\r\n", lines);
        }

        /// <summary>Switch back to the retained previous version (and its mods when they are still there).</summary>
        public static string Rollback(PlusLayout layout, Action<string> log)
        {
            if (Nebula.Setup.Running.Find(layout.Root, "NebulaRuntime") != null)
                throw new InvalidOperationException("Close the game first.");
            var current = PlusInstalled.Read(layout);
            if (current == null || current.Previous == null) throw new InvalidOperationException("No previous version is available.");
            var previous = PlusInstalled.ReadVersion(layout, current.Previous);
            if (previous == null) throw new InvalidOperationException("The previous version " + current.Previous + " is incomplete.");
            string oldMods = layout.Mods + ".previous";
            string oldModsJson = Path.Combine(oldMods, "plus-mods.json");
            if (previous.ModsKey != current.ModsKey && File.Exists(oldModsJson) && Json.Str(Json.Load(oldModsJson), "modsKey") == previous.ModsKey)
            {
                string aside = layout.Mods + ".swap";
                FileUtil.MoveDirectoryInPlace(layout.Mods, aside);
                FileUtil.MoveDirectoryInPlace(oldMods, layout.Mods);
                FileUtil.MoveDirectoryInPlace(aside, oldMods);
                log("Restored the previous version's mods.");
            }
            Json.Save(layout.Current, new Dictionary<string, object> { { "version", previous.Version }, { "previous", current.Version } });
            Launchers.WriteCurrent(layout, previous.Version, PlusUpdater.PackageVersion(previous), previous.Commit, previous.BuildId);
            return "Rolled back to " + previous.Version + " (commit " + previous.Commit + "); " + current.Version + " is kept and can be restored the same way.";
        }

        [DllImport("kernel32.dll")]
        private static extern bool AttachConsole(int processId);
    }
}
