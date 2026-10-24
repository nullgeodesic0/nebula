using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;

namespace Nebula.Plus
{
    /// <summary>
    /// One Nebula Plus folder. Program files (versions, content, toolchain, sources) are replaceable; data\ (saves,
    /// settings, shader cache, netplay cache, logs) and plus.json belong to the user and are never written by an update.
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
        public string Mods { get { return Path.Combine(Content, "mods"); } }
        public string GameInputs { get { return Path.Combine(root, "game-inputs"); } }
        public string Toolchains { get { return Path.Combine(root, "toolchain"); } }
        public string Sources { get { return Path.Combine(root, "source"); } }
        public string Python { get { return Path.Combine(root, "python"); } }
        public string Staging { get { return Path.Combine(root, "staging"); } }
        public string Logs { get { return Path.Combine(root, "logs"); } }
        public string Config { get { return Path.Combine(root, "plus.json"); } }

        // User data: created once, never replaced.
        public string Data { get { return Path.Combine(root, "data"); } }
        public string Saves { get { return Path.Combine(Data, "saves"); } }
        public string RuntimeSettings { get { return Path.Combine(Data, "runtime_settings.ini"); } }
        public string RenderConfig { get { return Path.Combine(Data, "config.ini"); } }
        public string ShaderCache { get { return Path.Combine(Data, "shadercache"); } }
        public string NetplayCache { get { return Path.Combine(Data, "netplay"); } }
        public string CrashDumps { get { return Path.Combine(Data, "crashdumps"); } }
        public string GameLogs { get { return Path.Combine(Data, "logs"); } }
        /// <summary>Optional user hook the launchers call (extra GALAXY_* variables); never written by Plus.</summary>
        public string LaunchEnvHook { get { return Path.Combine(Data, "launch-env.cmd"); } }

        public string VersionDir(string version) { return Path.Combine(Versions, version); }

        /// <summary>Folders an install or update may replace; anything else under the root is left alone.</summary>
        public IEnumerable<string> ProgramFolders
        {
            get { return new[] { Versions, Content, GameInputs, Toolchains, Sources, Staging }; }
        }
    }

    /// <summary>plus.json: where updates come from, the official install and the user's mod input files.</summary>
    internal sealed class PlusConfig
    {
        public string Repository = BuildInfo.Repository;
        /// <summary>"release": signed packages from GitHub releases; "branch": build the branch's newest commit here.</summary>
        public string Channel = "release";
        public string Branch = "plus";
        public string OfficialRoot;
        public string Disc;
        /// <summary>"own": data\saves; "shared": the official Nebula's saves folder, used in place.</summary>
        public string SavesMode = "own";
        public string ModuleIsa = "SSE2";
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
            config.ModuleIsa = Json.Str(json, "moduleIsa") ?? config.ModuleIsa;
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
                { "schema", "nebula.plus.config.v1" },
                { "repository", Repository }, { "channel", Channel }, { "branch", Branch },
                { "officialRoot", OfficialRoot }, { "disc", Disc }, { "savesMode", SavesMode },
                { "moduleIsa", ModuleIsa }, { "pythonVenvAllowed", PythonVenvAllowed }, { "modInputs", mods }
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
        public string Commit { get { return Json.Str(install, "commit"); } }
        public string Channel { get { return Json.Str(install, "channel"); } }
        public string ModuleKey { get { return Json.Str(install, "moduleKey"); } }
        public string ModuleIsa { get { return Json.Str(install, "moduleIsa"); } }
        public string ToolchainId { get { return Json.Str(install, "toolchain"); } }
        public string ModsKey { get { return Json.Str(install, "modsKey"); } }
        public string BuildId { get { return Json.Str(install, "netBuildId"); } }

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
                if (info.Previous != null && !System.IO.Directory.Exists(layout.VersionDir(info.Previous))) info.Previous = null;
                return info;
            }
            catch (Exception) { return null; }
        }

        public bool FilesIntact(params string[] names)
        {
            var files = Json.Obj(install, "files");
            foreach (var name in names)
            {
                string path = Path.Combine(Directory, name);
                if (files == null || !File.Exists(path) || FileUtil.Sha256File(path) != Json.Str(files, name)) return false;
            }
            return true;
        }
    }
}
