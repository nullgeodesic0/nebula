using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Linq;

namespace Nebula.Plus
{
    /// <summary>
    /// One Nebula Plus folder. Program files (versions, content, sources) are replaceable; data\ (saves, settings,
    /// shader cache, netplay cache, the mods folder) belongs to the player and is never written by an update, except
    /// the one mod folder Nebula Plus builds there from the player's own files (marked by plus-mod.json).
    /// </summary>
    internal sealed class PlusLayout
    {
        public const string DefaultRoot = @"C:\NebulaPlus";

        private readonly string root;

        public PlusLayout(string root) { this.root = Path.GetFullPath(root); }

        public string Root { get { return root; } }
        public string Versions { get { return Path.Combine(root, "versions"); } }
        public string Current { get { return Path.Combine(root, "current.json"); } }
        public string CurrentCmd { get { return Path.Combine(root, "current.cmd"); } }
        public string Content { get { return Path.Combine(root, "content"); } }
        public string Sources { get { return Path.Combine(root, "source"); } }
        public string Python { get { return Path.Combine(root, "python"); } }
        public string Staging { get { return Path.Combine(root, "staging"); } }
        public string Logs { get { return Path.Combine(root, "logs"); } }
        public string Config { get { return Path.Combine(root, "plus.json"); } }
        /// <summary>The previous build of the Odyssey mod, kept outside the mods folder (there it would load too).</summary>
        public string PreviousMod { get { return Path.Combine(root, "mod-previous"); } }

        // User data: created once, never replaced. The launchers set LOCALAPPDATA to data\, so the game's own
        // %LOCALAPPDATA%\Nebula (settings, mods, shader cache, crash reports, netplay cache) is data\Nebula.
        public string Data { get { return Path.Combine(root, "data"); } }
        public string GameData { get { return Path.Combine(Data, "Nebula"); } }
        public string Saves { get { return Path.Combine(Data, "saves"); } }
        public string ModsFolder { get { return Path.Combine(GameData, "mods"); } }
        public string RuntimeSettings { get { return Path.Combine(GameData, "runtime_settings.ini"); } }
        public string RenderConfig { get { return Path.Combine(GameData, "config.ini"); } }
        public string GameLogs { get { return Path.Combine(Data, "logs"); } }
        /// <summary>Optional user hook the launchers call (extra GALAXY_* variables); never written by Plus.</summary>
        public string LaunchEnvHook { get { return Path.Combine(Data, "launch-env.cmd"); } }

        public string VersionDir(string version) { return Path.Combine(Versions, version); }
    }

    /// <summary>plus.json: where updates come from, the official install and the player's mod input files.</summary>
    internal sealed class PlusConfig
    {
        public string Repository = BuildInfo.Repository;
        /// <summary>"release": signed packages from GitHub releases; "branch": build the branch's newest commit here.</summary>
        public string Channel = "release";
        public string Branch = "nebula2";
        public string OfficialRoot;
        public string Disc;
        /// <summary>"own": data\saves; "shared": the official Nebula's saves folder, used in place.</summary>
        public string SavesMode = "own";
        public bool PythonVenvAllowed;
        public Dictionary<string, string> ModInputs = new Dictionary<string, string>(StringComparer.Ordinal);

        public static PlusConfig Load(PlusLayout layout)
        {
            var config = new PlusConfig();
            if (!File.Exists(layout.Config)) return config;
            var json = Json.Load(layout.Config);
            config.Repository = Json.Str(json, "repository") ?? config.Repository;
            config.Channel = Json.Str(json, "channel") ?? config.Channel;
            config.Branch = Json.Str(json, "branch") ?? config.Branch;
            config.OfficialRoot = Json.Str(json, "officialRoot");
            config.Disc = Json.Str(json, "disc");
            config.SavesMode = Json.Str(json, "savesMode") ?? config.SavesMode;
            config.PythonVenvAllowed = Json.Bool(json, "pythonVenvAllowed");
            var mods = Json.Obj(json, "modInputs");
            if (mods != null)
                foreach (var pair in mods)
                    if (pair.Value is string && ((string)pair.Value).Length != 0) config.ModInputs[pair.Key] = (string)pair.Value;
            return config;
        }

        public void Save(PlusLayout layout)
        {
            var mods = new Dictionary<string, object>();
            foreach (var pair in ModInputs.OrderBy(p => p.Key, StringComparer.Ordinal)) mods[pair.Key] = pair.Value;
            Directory.CreateDirectory(layout.Root);
            FileUtil.WriteAllTextAtomic(layout.Config, Json.Serialize(new Dictionary<string, object>
            {
                { "schema", "nebula.plus.config.v2" },
                { "repository", Repository }, { "channel", Channel }, { "branch", Branch },
                { "officialRoot", OfficialRoot }, { "disc", Disc }, { "savesMode", SavesMode },
                { "pythonVenvAllowed", PythonVenvAllowed }, { "modInputs", mods }
            }));
        }
    }

    /// <summary>current.json and the current version's plus-install.json.</summary>
    internal sealed class PlusInstalled
    {
        private Dictionary<string, object> install;
        public string Version { get; private set; }
        public string Previous { get; private set; }
        public string Directory { get; private set; }
        public string PackageVersion { get { return Json.Str(install, "packageVersion") ?? Version; } }
        public string Commit { get { return Json.Str(install, "commit"); } }
        public string Channel { get { return Json.Str(install, "channel"); } }
        public string ModsKey { get { return Json.Str(install, "modsKey"); } }

        public static PlusInstalled Read(PlusLayout layout) { return ReadVersion(layout, null); }

        /// <summary>The version in current.json, or <paramref name="version"/> when given.</summary>
        public static PlusInstalled ReadVersion(PlusLayout layout, string version)
        {
            if (!File.Exists(layout.Current)) return null;
            try
            {
                var current = Json.Load(layout.Current);
                var info = new PlusInstalled();
                info.Version = version ?? Json.Str(current, "version");
                info.Previous = version == null ? Json.Str(current, "previous") : null;
                info.Directory = layout.VersionDir(info.Version);
                string installJson = Path.Combine(info.Directory, "plus-install.json");
                if (!File.Exists(installJson)) return null;
                info.install = Json.Load(installJson);
                if (Json.Str(info.install, "schema") != "nebula.plus.install.v2") return null; // a Nebula 1 (recompiled) Plus
                if (info.Previous != null && !System.IO.Directory.Exists(layout.VersionDir(info.Previous))) info.Previous = null;
                return info;
            }
            catch (Exception) { return null; }
        }
    }

    /// <summary>The game, the launcher or Nebula Plus running from a folder.</summary>
    internal static class Running
    {
        /// <summary>The first of <paramref name="names"/> running from inside <paramref name="folder"/>, as "name.exe (pid)"; null when none.</summary>
        public static string Find(string folder, params string[] names)
        {
            string prefix = Path.GetFullPath(folder).TrimEnd('\\') + "\\";
            foreach (var name in names)
            {
                foreach (var process in Process.GetProcessesByName(name))
                {
                    using (process)
                    {
                        string path = null;
                        try { path = process.MainModule.FileName; } catch (Exception) { }
                        if (path != null && path.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
                            return name + ".exe (" + process.Id + ")";
                    }
                }
            }
            return null;
        }
    }

    internal static class PathUtil
    {
        public static string Sha256Text(string text) { return FileUtil.Sha256Bytes(System.Text.Encoding.UTF8.GetBytes(text)); }

        /// <summary>root\relative, refused when it would leave root (archive entries).</summary>
        public static string SafeJoin(string root, string relative)
        {
            string full = Path.GetFullPath(Path.Combine(root, relative));
            string prefix = Path.GetFullPath(root).TrimEnd('\\') + "\\";
            if (!full.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("Archive entry escapes its folder: " + relative);
            return full;
        }

        /// <summary>An archive's files below its top folder (git archive --prefix, GitHub's archive) into <paramref name="target"/>.</summary>
        public static void ExtractStrippingTop(string zip, string target)
        {
            if (System.IO.Directory.Exists(target)) FileUtil.DeleteTree(target);
            System.IO.Directory.CreateDirectory(target);
            using (var archive = System.IO.Compression.ZipFile.OpenRead(zip))
            {
                foreach (var entry in archive.Entries)
                {
                    string name = entry.FullName.Replace('\\', '/');
                    int slash = name.IndexOf('/');
                    if (slash < 0 || name.EndsWith("/")) continue;
                    string path = SafeJoin(target, name.Substring(slash + 1));
                    System.IO.Directory.CreateDirectory(Path.GetDirectoryName(path));
                    entry.ExtractToFile(path, true);
                }
            }
        }
    }

    /// <summary>A program to completion with its output lines, in a chosen environment (the shared ProcessRunner has none).</summary>
    internal static class Proc
    {
        public static int Run(string exe, IList<string> arguments, string workingDirectory, IDictionary<string, string> environment,
            Action<string> onLine, System.Threading.CancellationToken cancel)
        {
            var start = new ProcessStartInfo(exe, ProcessRunner.JoinArguments(arguments));
            start.UseShellExecute = false;
            start.CreateNoWindow = true;
            start.RedirectStandardOutput = true;
            start.RedirectStandardError = true;
            start.StandardOutputEncoding = System.Text.Encoding.UTF8;
            start.StandardErrorEncoding = System.Text.Encoding.UTF8;
            start.WorkingDirectory = workingDirectory ?? Path.GetTempPath();
            if (environment != null)
            {
                start.EnvironmentVariables.Clear();
                foreach (var pair in environment) start.EnvironmentVariables[pair.Key] = pair.Value;
            }
            using (var process = new Process { StartInfo = start })
            {
                DataReceivedEventHandler handler = delegate(object sender, DataReceivedEventArgs e) { if (e.Data != null && onLine != null) onLine(e.Data); };
                process.OutputDataReceived += handler;
                process.ErrorDataReceived += handler;
                process.Start();
                process.BeginOutputReadLine();
                process.BeginErrorReadLine();
                while (!process.WaitForExit(200))
                {
                    if (cancel.IsCancellationRequested)
                    {
                        try { process.Kill(); } catch (Exception) { }
                        process.WaitForExit();
                        throw new OperationCanceledByUserException();
                    }
                }
                process.WaitForExit();
                return process.ExitCode;
            }
        }
    }
}
