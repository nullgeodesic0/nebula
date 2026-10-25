using System;
using System.IO;

namespace Nebula.Plus
{
    /// <summary>
    /// The official Nebula install (Nebula-Setup.exe), when there is one. Plus only reads it: a Nebula 2 install's
    /// packed game spares the disc import, and its saves and settings can be copied. Nothing is ever written, moved or
    /// deleted there. Nebula Plus needs no official install: the player's disc image is enough.
    /// </summary>
    internal sealed class OfficialNebula
    {
        public const int ContentFormatVersion = 1;

        public string Root { get; private set; }
        public string Version { get; private set; }
        /// <summary>Its installed version runs the native game (Nebula 2); a recompiled install (1.x) packed another format.</summary>
        public bool Native { get; private set; }

        public string Content { get { return Path.Combine(Root, "content"); } }

        /// <summary>The official data root: %LOCALAPPDATA%\Nebula (NEBULA_DATA_ROOT in tests).</summary>
        public static string DataRoot { get { return NebulaPaths.DataRoot; } }
        public static string Saves { get { return NebulaPaths.Saves; } }
        public static string RuntimeSettings { get { return Path.Combine(DataRoot, "runtime_settings.ini"); } }
        public static string RenderConfig { get { return Path.Combine(DataRoot, "config.ini"); } }

        /// <summary>The install at <paramref name="root"/>, else the registered one, else the default folder; null when none.</summary>
        public static OfficialNebula Find(string root)
        {
            var candidates = new System.Collections.Generic.List<string>();
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
                try
                {
                    found.Version = Json.Str(Json.Load(Path.Combine(candidate, "current.json")), "version");
                    string install = Path.Combine(candidate, "versions", found.Version ?? "", "install.json");
                    found.Native = File.Exists(install) && Json.Str(Json.Load(install), "game") == "native";
                }
                catch (Exception) { }
                return found;
            }
            return null;
        }

        /// <summary>A Nebula 2 install's packed game, complete (nebula-import's content: game.pak and files\).</summary>
        public bool ContentValid() { return Native && ContentFolderValid(Content); }

        public static bool ContentFolderValid(string folder)
        {
            string marker = Path.Combine(folder, "content.json");
            if (!File.Exists(marker) || !File.Exists(Path.Combine(folder, "game.pak"))) return false;
            try
            {
                var json = Json.Load(marker);
                return Json.Long(json, "formatVersion") == ContentFormatVersion && Json.Str(json, "importer") != "nebula-recomp";
            }
            catch (Exception) { return false; }
        }
    }
}
