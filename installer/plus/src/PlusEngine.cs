using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using System.Threading;

namespace Nebula.Plus
{
    internal interface IPlusProgress
    {
        /// <summary>Overall fraction 0..1 and the current step.</summary>
        void Report(double overall, string step);
        void Log(string line);
        /// <summary>A yes/no question; unattended runs answer from their options.</summary>
        bool Ask(string question);
    }

    internal sealed class PlusRequest
    {
        public string Root;
        /// <summary>A package folder or zip (a zip from the updater has been authenticated already).</summary>
        public string Package;
        public string Channel = "release";
        /// <summary>Official install root (null: the registered or default one, if any).</summary>
        public string OfficialRoot;
        /// <summary>The player's Super Mario Galaxy disc image or extracted folder (packed with nebula-import).</summary>
        public string Disc;
        public Dictionary<string, string> ModInputs;
        public bool ImportSaves, ImportSettings, ForceMods, SkipMods, AllowPythonVenv;
        public string SavesMode;
    }

    /// <summary>
    /// Install and update of a Nebula Plus folder for Nebula 2 (the native game). A first install packs the player's
    /// disc into content\game.pak with nebula-import (or copies an official Nebula 2 install's packed game), installs
    /// the prebuilt game from the package, builds the Odyssey mod from the player's own files into the mods folder and
    /// writes the launchers: one player, co-op, host and join online. An update installs the new version beside the
    /// current one and switches only when it is complete; the previous version stays for rollback. data\ (saves,
    /// settings, caches) is created when missing and otherwise never written; the official install is only read.
    /// </summary>
    internal sealed class PlusEngine
    {
        public const string ModName = "Odyssey Mario";
        private const string ModMarker = "plus-mod.json";

        private readonly PlusRequest request;
        private readonly IPlusProgress progress;
        private readonly CancellationToken cancel;
        private readonly PlusLayout layout;
        private readonly StreamWriter logFile;
        private readonly Stopwatch clock = Stopwatch.StartNew();
        private readonly object logLock = new object();
        private string staging;
        private double stageBase, stageWeight;
        private string stepText = "";

        public PlusEngine(PlusRequest request, IPlusProgress progress, CancellationToken cancel)
        {
            this.request = request;
            this.progress = progress;
            this.cancel = cancel;
            layout = new PlusLayout(request.Root);
            Directory.CreateDirectory(layout.Logs);
            LogPath = Path.Combine(layout.Logs, "install-" + DateTime.Now.ToString("yyyyMMdd-HHmmss") + ".log");
            logFile = new StreamWriter(new FileStream(LogPath, FileMode.Append, FileAccess.Write, FileShare.Read));
            logFile.AutoFlush = true;
        }

        public string LogPath { get; private set; }
        public string Summary { get; private set; }

        public void Run()
        {
            using (var mutex = new Mutex(false, @"Local\NebulaPlus-" + FileUtil.Sha256Bytes(System.Text.Encoding.UTF8.GetBytes(layout.Root.ToLowerInvariant())).Substring(0, 16)))
            {
                bool owned;
                try { owned = mutex.WaitOne(0); } catch (AbandonedMutexException) { owned = true; }
                if (!owned) throw new InvalidOperationException("Nebula Plus is already installing or updating " + layout.Root + ".");
                try { RunLocked(); }
                finally { mutex.ReleaseMutex(); logFile.Dispose(); }
            }
        }

        private void RunLocked()
        {
            Log("Nebula Plus " + BuildInfo.Version + " (" + BuildInfo.Commit + ") into " + layout.Root);
            var problems = Requirements(layout.Root);
            if (problems.Count != 0) throw new InvalidOperationException(string.Join("\r\n", problems));
            Directory.CreateDirectory(layout.Staging);
            foreach (var dir in Directory.GetDirectories(layout.Staging))
                try { FileUtil.DeleteTree(dir); Log("Removed unfinished work: " + dir); } catch (Exception) { }
            staging = Path.Combine(layout.Staging, DateTime.UtcNow.ToString("yyyyMMdd-HHmmss") + "-" + Guid.NewGuid().ToString("N").Substring(0, 8));
            Directory.CreateDirectory(staging);
            try { Install(); }
            catch (Exception error) { Log("FAILED: " + error); throw; }
            finally
            {
                try { FileUtil.DeleteTree(staging); } catch (Exception error) { Log("Could not remove staging: " + error.Message); }
            }
        }

        public static List<string> Requirements(string root)
        {
            var problems = new List<string>();
            if (!Environment.Is64BitOperatingSystem || Environment.OSVersion.Version.Major < 10) problems.Add("Nebula Plus needs 64-bit Windows 10 or 11.");
            // NebulaNative.exe is built for AVX2, as upstream's; without it the game stops at its first AVX2 instruction.
            if (!IsProcessorFeaturePresent(40)) problems.Add("Nebula needs a CPU with AVX2 (most PCs from 2013 on).");
            try
            {
                string probe = root;
                while (!Directory.Exists(probe) && Path.GetDirectoryName(probe) != null) probe = Path.GetDirectoryName(probe);
                string fs = FileUtil.FileSystemName(probe);
                if (!string.Equals(fs, "NTFS", StringComparison.OrdinalIgnoreCase)) problems.Add("The Nebula Plus folder must be on an NTFS drive (this one is " + fs + ").");
                long free = FileUtil.FreeBytes(probe);
                if (free < (8L << 30)) problems.Add("Nebula Plus needs about 8 GB free; the drive has " + FileUtil.FormatBytes(free) + ".");
            }
            catch (Exception error) { problems.Add("Cannot inspect the Nebula Plus folder: " + error.Message); }
            return problems;
        }

        /// <summary>Plus's own packed game: complete and packed by nebula-import.</summary>
        public static bool ContentValid(string content)
        {
            string marker = Path.Combine(content, "content.json");
            if (!File.Exists(marker) || !File.Exists(Path.Combine(content, "game.pak"))) return false;
            try
            {
                var json = Json.Load(marker);
                return Json.Long(json, "formatVersion") == OfficialNebula.ContentFormatVersion && Json.Str(json, "importer") == "nebula-import";
            }
            catch (Exception) { return false; }
        }

        private void Install()
        {
            var config = PlusConfig.Load(layout);
            bool configChanged = !File.Exists(layout.Config);
            if (request.OfficialRoot != null) { config.OfficialRoot = request.OfficialRoot; configChanged = true; }
            if (request.Disc != null) { config.Disc = request.Disc; configChanged = true; }
            if (request.SavesMode != null) { config.SavesMode = request.SavesMode; configChanged = true; }
            if (request.AllowPythonVenv) { config.PythonVenvAllowed = true; configChanged = true; }
            if (request.Channel != null && request.Channel != config.Channel) { config.Channel = request.Channel; configChanged = true; }
            if (request.ModInputs != null)
            {
                foreach (var pair in request.ModInputs)
                    if (string.IsNullOrEmpty(pair.Value)) config.ModInputs.Remove(pair.Key); else config.ModInputs[pair.Key] = pair.Value;
                configChanged = true;
            }

            Stage(0.00, 0.03, "Checking the Nebula Plus package");
            PlusPackage package = File.Exists(request.Package)
                ? PlusPackage.Extract(request.Package, Path.Combine(staging, "package"))
                : PlusPackage.OpenFolder(request.Package);
            Log("Package " + package.Version + ", commit " + package.Commit + " (" + package.Repository + " " + package.Branch + ")");
            var current = PlusInstalled.Read(layout);
            if (current != null) Log("Installed: " + current.Version + " (commit " + current.Commit + ")" + (current.Previous != null ? ", previous " + current.Previous : ""));
            var official = OfficialNebula.Find(config.OfficialRoot);
            Log(official == null ? "No official Nebula install (not needed)." :
                "Official Nebula " + official.Version + " at " + official.Root + (official.Native ? " (Nebula 2; read only)" : " (a recompiled 1.x install: its game files are not used)"));

            // ---- the packed game ----
            bool newContent = request.Disc != null || !ContentValid(layout.Content);
            string content = layout.Content;
            if (newContent)
            {
                content = Path.Combine(staging, "content");
                PackGame(package, official, request.Disc ?? config.Disc, content);
            }
            else Log("The packed game in " + layout.Content + " is kept.");

            // ---- the game ----
            Stage(0.40, 0.05, "Installing Nebula Plus " + package.Version);
            string app = Path.Combine(staging, "app");
            Directory.CreateDirectory(app);
            foreach (var name in PlusPackage.AppFiles)
                if (File.Exists(package.File(name))) File.Copy(package.File(name), Path.Combine(app, name));
            foreach (var name in new[] { "NebulaNative.exe", "smg_game.dll" })
                if (new FileInfo(Path.Combine(app, name)).Length == 0) throw new InvalidOperationException("The package's " + name + " is empty.");
            string src = package.EnsureSource(layout.Sources, Log);

            // ---- mods ----
            string modsKey = ModBuilder.Key(package.ModToolsKey, config.ModInputs, Path.Combine(content, "content.json"));
            string keptModsKey = current != null && current.ModsKey != null ? current.ModsKey : "not-built";
            string modTarget = ModTarget();
            bool modCurrent = current != null && current.ModsKey == modsKey && !newContent && (Directory.Exists(modTarget) || !AnyInput(config));
            string newMod = null;
            ModBuilder.Result mods = null;
            if (request.SkipMods) { modsKey = keptModsKey; Log("Mods: not built (--skip-mods); the installed mod is kept."); }
            else if (modCurrent && !request.ForceMods) Log("Mods: the builders and your input files did not change; the installed mod is kept.");
            else
            {
                Stage(0.45, 0.45, "Building your custom mod");
                mods = BuildMods(config, src, content, out newMod);
                if (mods == null) modsKey = keptModsKey;
            }

            // ---- assemble ----
            Stage(0.90, 0.05, "Finishing");
            var files = new Dictionary<string, object>();
            foreach (var path in Directory.GetFiles(app)) files[Path.GetFileName(path)] = FileUtil.Sha256File(path);
            var record = new Dictionary<string, object>
            {
                { "schema", "nebula.plus.install.v2" }, { "game", "native" }, { "packageVersion", package.Version }, { "commit", package.Commit },
                { "repository", package.Repository }, { "branch", package.Branch }, { "channel", config.Channel },
                { "modsKey", modsKey }, { "installedUtc", DateTime.UtcNow.ToString("o") }, { "files", files }
            };
            if (mods != null) { record["modsBuilt"] = mods.Built.ToArray(); record["modsSkipped"] = mods.Skipped.ToArray(); }
            Json.Save(Path.Combine(app, "plus-install.json"), record);
            // The package stays with its version: a mod rebuild or repair needs no download.
            FileUtil.CopyTree(package.Directory, Path.Combine(app, "package"));

            Stage(0.95, 0.05, "Switching to the new version");
            WaitUntilClosed();
            string versionName = package.Version;
            for (int n = 1; Directory.Exists(layout.VersionDir(versionName)); n++) versionName = package.Version + "+r" + n;
            Directory.CreateDirectory(layout.Versions);
            FileUtil.MoveDirectory(app, layout.VersionDir(versionName), Log);
            if (newContent)
            {
                Swap(content, layout.Content, Path.Combine(staging, "content-old"));
            }
            if (newMod != null) SwapMod(newMod, modTarget);
            Json.Save(layout.Current, new Dictionary<string, object>
            {
                { "version", versionName }, { "previous", current != null ? current.Version : null }
            });
            Launchers.WriteCurrent(layout, versionName, package.Version, package.Commit);

            // ---- from here on Plus is installed: data folders, launchers, imports, tidying ----
            if (configChanged) config.Save(layout);
            foreach (var dir in new[] { layout.Data, layout.GameData, layout.Saves, layout.GameLogs, layout.ModsFolder })
                Directory.CreateDirectory(dir);
            Launchers.WriteAll(layout, config);
            if (request.ImportSaves)
            {
                try { SavesImport.ImportSaves(layout, Log); }
                catch (Exception error) { Log("Could not copy the official saves: " + error.Message); }
            }
            if (request.ImportSettings)
            {
                try { SavesImport.ImportSettings(layout, Log); }
                catch (Exception error) { Log("Could not copy the official settings: " + error.Message); }
            }
            foreach (var dir in Directory.GetDirectories(layout.Versions))
            {
                string name = Path.GetFileName(dir);
                if (name == versionName || (current != null && name == current.Version)) continue;
                try { FileUtil.DeleteTree(dir); } catch (Exception error) { Log("Could not remove " + dir + ": " + error.Message); }
            }
            var summary = new List<string> { "Nebula Plus " + package.Version + " (" + package.Commit.Substring(0, 12) + ") is installed in " + layout.Root + "." };
            summary.Add("Play: \"Play Nebula Plus.cmd\" (one player), \"Play co-op.cmd\", \"Host online game.cmd\", \"Join online game.cmd\".");
            if (mods != null)
            {
                if (mods.Built.Count != 0) summary.Add("Custom mod built from your files (Settings > Mods): " + string.Join(", ", mods.Built) + ".");
                if (mods.Skipped.Count != 0) summary.Add("Could not build: " + string.Join(", ", mods.Skipped) + ".");
            }
            Summary = string.Join("\r\n", summary);
            Log(Summary);
            Log(string.Format("Completed in {0:mm\\:ss}.", clock.Elapsed));
            Report(1.0, "Nebula Plus " + package.Version + " is installed.");
        }

        private static bool AnyInput(PlusConfig config)
        {
            return ModInput.All.Any(i => config.ModInputs.ContainsKey(i.Key) && i.Present(config.ModInputs[i.Key]));
        }

        /// <summary>
        /// The mods folder's "Odyssey Mario". A folder of that name that Plus did not build (no plus-mod.json) is the
        /// player's own and is left alone; Plus then builds "Odyssey Mario (Nebula Plus)".
        /// </summary>
        private string ModTarget()
        {
            string plain = Path.Combine(layout.ModsFolder, ModName);
            if (!Directory.Exists(plain) || File.Exists(Path.Combine(plain, ModMarker))) return plain;
            return Path.Combine(layout.ModsFolder, ModName + " (Nebula Plus)");
        }

        // ---- steps ---------------------------------------------------------------

        private void PackGame(PlusPackage package, OfficialNebula official, string disc, string content)
        {
            if (request.Disc == null && official != null && official.ContentValid())
            {
                Stage(0.03, 0.37, "Copying the packed game from the official Nebula install");
                CopyTreeWithProgress(official.Content, content);
            }
            else
            {
                if (disc == null)
                    throw new InvalidOperationException("Choose your Super Mario Galaxy disc image (ISO, WBFS, RVZ, ... of the USA disc, RMGE01) or its extracted folder.");
                string importer = package.File("nebula-import.exe");
                Stage(0.03, 0.02, "Checking your game");
                var lines = new List<string>();
                int code = RunTool(importer, new[] { "identify", disc }, lines.Add);
                string json = string.Join("\n", lines);
                Log(json);
                Dictionary<string, object> identity;
                try { identity = Json.Parse(json.Substring(Math.Max(0, json.IndexOf('{')))); }
                catch (Exception) { throw new InvalidOperationException(lines.Count > 0 ? lines[lines.Count - 1] : "The selected file could not be read."); }
                if (code != 0 || !Json.Bool(identity, "supported")) throw new InvalidOperationException(Json.Str(identity, "message"));
                Stage(0.05, 0.35, "Packing the game files from your disc");
                code = RunTool(importer, new[] { "import", disc, "--output", content }, delegate(string line)
                {
                    var match = Regex.Match(line, @"^PROGRESS (\d+) (\d+)$");
                    if (match.Success) { StepProgress(null, double.Parse(match.Groups[1].Value) / Math.Max(1, double.Parse(match.Groups[2].Value))); return; }
                    if (line.StartsWith("STEP ")) StepProgress(line.Substring(line.IndexOf(' ', 5) + 1), -1);
                    Log(line);
                });
                if (code != 0) throw new InvalidOperationException("nebula-import failed (exit " + code + "). See " + LogPath + ".");
            }
            Json.Save(Path.Combine(content, "content.json"), new Dictionary<string, object>
            {
                { "formatVersion", OfficialNebula.ContentFormatVersion }, { "importer", "nebula-import" }, { "createdUtc", DateTime.UtcNow.ToString("o") }
            });
        }

        private void CopyTreeWithProgress(string from, string to)
        {
            from = Path.GetFullPath(from).TrimEnd('\\');
            var all = Directory.GetFiles(from, "*", SearchOption.AllDirectories)
                .Where(f => !Path.GetFileName(f).Equals("content.json", StringComparison.OrdinalIgnoreCase)).ToList();
            long total = all.Sum(f => new FileInfo(f).Length), done = 0;
            var buffer = new byte[4 << 20];
            foreach (var file in all)
            {
                string target = Path.Combine(to, file.Substring(from.Length + 1));
                Directory.CreateDirectory(Path.GetDirectoryName(target));
                using (var input = new FileStream(file, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 16, FileOptions.SequentialScan))
                using (var output = new FileStream(target, FileMode.CreateNew, FileAccess.Write))
                {
                    int read;
                    while ((read = input.Read(buffer, 0, buffer.Length)) > 0)
                    {
                        cancel.ThrowIfCancellationRequested();
                        output.Write(buffer, 0, read);
                        done += read;
                        StepProgress(null, done / (double)Math.Max(1, total));
                    }
                }
            }
            Log("Copied " + all.Count + " files (" + FileUtil.FormatBytes(total) + ") from " + from + ".");
        }

        /// <summary>The Odyssey mod from the player's inputs into staging; null when it could not be built (no Python).</summary>
        private ModBuilder.Result BuildMods(PlusConfig config, string src, string content, out string newMod)
        {
            newMod = null;
            if (!AnyInput(config))
            {
                Log("Custom mods: none chosen.");
                return new ModBuilder.Result();
            }
            foreach (var pair in config.ModInputs)
            {
                var input = ModInput.ByKey(pair.Key);
                if (input != null && !input.Present(pair.Value)) Log("Mod input " + input.Label + " not found at " + pair.Value + ".");
            }
            var python = PythonEnv.Find(layout, Log);
            if (python == null)
            {
                Log("Custom mods: they are built with Python 3.9+, which is not installed. Install it from https://www.python.org, then use Rebuild mods.");
                return null;
            }
            Log("Mods: " + python.Description + (python.HasPackages ? "" : ", missing " + python.Missing));
            if (!python.HasPackages)
            {
                bool allowed = request.AllowPythonVenv || config.PythonVenvAllowed || progress.Ask(
                    "The mod builders need the Python packages " + python.Missing + " (numpy, Pillow and soundfile: about 60 MB from PyPI). " +
                    "Install them into Nebula Plus's own Python environment (" + layout.Python + ")? Your Python installation is not changed.");
                if (!allowed) { Log("Mods: Python packages not installed; the mod was not built."); return null; }
                StepProgress("Installing the Python packages", -1);
                python = PythonEnv.CreateVenv(python, layout, Log, cancel);
            }
            var builder = new ModBuilder(python, src, Log, cancel);
            string work = Path.Combine(staging, "modwork");
            var result = builder.Build(content, work, config.ModInputs, delegate(string text, double f) { StepProgress(text, f); });
            string packed = Path.Combine(staging, "mod");
            if (builder.Pack(work, packed))
            {
                Json.Save(Path.Combine(packed, ModMarker), new Dictionary<string, object>
                {
                    { "builtBy", "Nebula Plus " + BuildInfo.Version }, { "built", result.Built.ToArray() }, { "skipped", result.Skipped.ToArray() },
                    { "builtUtc", DateTime.UtcNow.ToString("o") },
                    { "note", "Built on this PC from the player's own files. Nintendo-derived: never share or upload it." }
                });
                newMod = packed;
            }
            return result;
        }

        /// <summary>The new mod replaces Plus's own mod folder; the old one moves to mod-previous (outside the mods folder).</summary>
        private void SwapMod(string newMod, string target)
        {
            if (Directory.Exists(layout.PreviousMod)) FileUtil.DeleteTree(layout.PreviousMod);
            if (Directory.Exists(target)) FileUtil.MoveDirectoryInPlace(target, layout.PreviousMod);
            Directory.CreateDirectory(Path.GetDirectoryName(target));
            try { FileUtil.MoveDirectory(newMod, target, Log); }
            catch (Exception)
            {
                if (Directory.Exists(layout.PreviousMod) && !Directory.Exists(target)) FileUtil.MoveDirectoryInPlace(layout.PreviousMod, target);
                throw;
            }
            Log("The custom mod is in " + target + " (Settings > Mods turns it on or off).");
        }

        private void Swap(string fresh, string destination, string aside)
        {
            bool existed = Directory.Exists(destination);
            if (existed) FileUtil.MoveDirectoryInPlace(destination, aside);
            try { FileUtil.MoveDirectory(fresh, destination, Log); }
            catch (Exception)
            {
                if (existed)
                {
                    try { FileUtil.MoveDirectoryInPlace(aside, destination); }
                    catch (Exception error) { Log("Could not put " + destination + " back from " + aside + ": " + error.Message); }
                }
                throw;
            }
        }

        /// <summary>The game reads the content and the mods: it must be closed for the switch. Plus never closes it.</summary>
        private void WaitUntilClosed()
        {
            while (true)
            {
                string running = Running.Find(layout.Root, "NebulaNative");
                if (running == null) return;
                Log("Waiting for " + running + " to close.");
                if (!progress.Ask("Nebula Plus is running (" + running + "). Close the game, then choose Yes to finish the update (No cancels it)."))
                    throw new InvalidOperationException("The game is running from " + layout.Root + ". Close it and try again; nothing was changed.");
            }
        }

        // ---- helpers -------------------------------------------------------------

        private int RunTool(string file, IList<string> arguments, Action<string> onLine)
        {
            Log("> " + Path.GetFileName(file) + " " + ProcessRunner.JoinArguments(arguments));
            return Proc.Run(file, arguments, staging, null, onLine, cancel);
        }

        private void Stage(double start, double weight, string text)
        {
            cancel.ThrowIfCancellationRequested();
            stageBase = start;
            stageWeight = weight;
            stepText = text;
            Log("== " + text);
            Report(start, text);
        }

        private void StepProgress(string text, double fraction)
        {
            if (text != null) stepText = text;
            Report(stageBase + stageWeight * Math.Max(0, Math.Min(1, fraction)), stepText);
        }

        private void Report(double overall, string text) { progress.Report(overall, text); }

        private void WriteLog(string line)
        {
            lock (logLock) logFile.WriteLine(DateTime.Now.ToString("HH:mm:ss.fff") + " " + line);
        }

        private void Log(string line)
        {
            WriteLog(line);
            progress.Log(line);
        }

        [System.Runtime.InteropServices.DllImport("kernel32.dll")]
        private static extern bool IsProcessorFeaturePresent(uint feature);
    }
}
