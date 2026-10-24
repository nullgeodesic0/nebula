using System;
using System.Collections.Generic;
using System.IO;

namespace Nebula.Plus
{
    /// <summary>
    /// The official Nebula install (Nebula-Setup.exe). Plus only reads it: the retained game inputs and packed
    /// content spare the disc, its toolchain spares a 400 MB download, and its saves can be copied. Nothing is
    /// ever written, moved or deleted there.
    /// </summary>
    internal sealed class OfficialNebula
    {
        /// <summary>Files under game-inputs that regeneration needs (InstallEngine.GameInputFiles).</summary>
        public static readonly string[] GameInputFiles =
        {
            "sys/main.dol", "sys/boot.bin", "sys/bi2.bin", "sys/apploader.img", "sys/fst.bin",
            "files/ModuleData/HomeButtonMenuWrapperRSO.rso", "files/ModuleData/product.sel"
        };

        public const int ContentFormatVersion = 1;

        public string Root { get; private set; }
        public string Version { get; private set; }

        public string GameInputs { get { return Path.Combine(Root, "game-inputs"); } }
        public string Content { get { return Path.Combine(Root, "content"); } }
        public string Toolchains { get { return Path.Combine(Root, "toolchain"); } }
        public string VersionDir { get { return Version == null ? null : Path.Combine(Root, "versions", Version); } }

        /// <summary>The official data root: %LOCALAPPDATA%\Nebula (NEBULA_DATA_ROOT in tests).</summary>
        public static string DataRoot { get { return NebulaPaths.DataRoot; } }
        public static string Saves { get { return NebulaPaths.Saves; } }
        public static string RuntimeSettings { get { return Path.Combine(DataRoot, "runtime_settings.ini"); } }
        public static string RenderConfig { get { return Path.Combine(DataRoot, "config.ini"); } }

        /// <summary>The install at <paramref name="root"/>, else the registered one, else the default folder; null when none.</summary>
        public static OfficialNebula Find(string root)
        {
            var candidates = new List<string>();
            if (!string.IsNullOrEmpty(root)) candidates.Add(root);
            else
            {
                try { if (NebulaPaths.RegisteredInstallRoot != null) candidates.Add(NebulaPaths.RegisteredInstallRoot); }
                catch (Exception) { }
                candidates.Add(NebulaPaths.DefaultInstallRoot);
            }
            foreach (var candidate in candidates)
            {
                if (!File.Exists(Path.Combine(candidate, "current.json"))) continue;
                var found = new OfficialNebula { Root = Path.GetFullPath(candidate) };
                try { found.Version = Json.Str(Json.Load(Path.Combine(candidate, "current.json")), "version"); }
                catch (Exception) { }
                return found;
            }
            return null;
        }

        /// <summary>The retained game inputs match the hashes Setup recorded.</summary>
        public bool GameInputsValid()
        {
            return InputsValid(GameInputs);
        }

        public static bool InputsValid(string folder)
        {
            string manifest = Path.Combine(folder, "inputs.json");
            if (!File.Exists(manifest)) return false;
            try
            {
                var hashes = Json.Load(manifest);
                foreach (var relative in GameInputFiles)
                {
                    string path = Path.Combine(folder, relative.Replace('/', '\\'));
                    if (!File.Exists(path) || FileUtil.Sha256File(path) != Json.Str(hashes, relative)) return false;
                }
                return true;
            }
            catch (Exception) { return false; }
        }

        public bool ContentValid() { return ContentFolderValid(Content); }

        public static bool ContentFolderValid(string folder)
        {
            string marker = Path.Combine(folder, "content.json");
            if (!File.Exists(marker) || !File.Exists(Path.Combine(folder, "game.pak"))) return false;
            try { return Json.Long(Json.Load(marker), "formatVersion") == ContentFormatVersion; }
            catch (Exception) { return false; }
        }

        /// <summary>The official toolchain of this id, when Setup finished unpacking it.</summary>
        public string ToolchainDir(string id)
        {
            string dir = Path.Combine(Toolchains, id);
            return File.Exists(Path.Combine(dir, "toolchain-ready.json")) ? dir : null;
        }

        /// <summary>App-local C++ runtime DLLs of the installed version.</summary>
        public IEnumerable<string> RuntimeDlls()
        {
            if (VersionDir == null || !Directory.Exists(VersionDir)) yield break;
            foreach (var dll in Directory.GetFiles(VersionDir, "*.dll"))
                if (!Path.GetFileName(dll).StartsWith("RMGE01_", StringComparison.OrdinalIgnoreCase)) yield return dll;
        }
    }
}
