using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Security.Cryptography;

namespace Nebula.Plus
{
    /// <summary>
    /// A Nebula Plus package (installer/plus/build-plus-package.ps1): the prebuilt game (NebulaNative.exe and
    /// smg_game.dll, built only from this repository's source, the game module from the Petari decompilation), the
    /// disc import tool, Nebula Plus itself, the exact source revision (for the mod builders) and plus-package.json with
    /// every file's SHA-256. It holds nothing from a game disc: game.pak and the mods are made on the player's PC.
    /// </summary>
    internal sealed class PlusPackage
    {
        public const string ManifestName = "plus-package.json";
        public const string Schema = "nebula.plus.package.v2";

        /// <summary>The files a version folder gets from the package (those present).</summary>
        public static readonly string[] AppFiles =
        {
            "NebulaNative.exe", "smg_game.dll", "nebula-import.exe", "NebulaPlus.exe", "NebulaPlusCli.exe",
            "material-names.tsv", "rt-materials.tsv", "rt-suns.tsv", "LICENSE", "THIRD-PARTY-NOTICES.md", "README.md", "NEBULA_PLUS.md"
        };

        private Dictionary<string, object> manifest;

        public string Directory { get; private set; }
        public string Version { get { return Json.Str(manifest, "version"); } }
        public string Commit { get { return Json.Str(manifest, "commit"); } }
        public string Repository { get { return Json.Str(manifest, "repository"); } }
        public string Branch { get { return Json.Str(manifest, "branch"); } }
        /// <summary>The mod builders (tools/animations).</summary>
        public string ModToolsKey { get { return Json.Str(manifest, "modToolsKey"); } }
        /// <summary>SHA-256 of smg_game.dll: online play needs the same game module on both PCs.</summary>
        public string GameSha256 { get { return Json.Str(Json.Obj(manifest, "files"), "smg_game.dll"); } }

        public string File(string name) { return Path.Combine(Directory, name.Replace('/', '\\')); }

        /// <summary>A package folder, checked file by file against its manifest.</summary>
        public static PlusPackage OpenFolder(string folder)
        {
            var package = new PlusPackage { Directory = Path.GetFullPath(folder) };
            string manifestPath = Path.Combine(package.Directory, ManifestName);
            if (!System.IO.File.Exists(manifestPath)) throw new InvalidDataException(folder + " is not a Nebula Plus package (no " + ManifestName + ").");
            package.manifest = Json.Load(manifestPath);
            if (Json.Str(package.manifest, "schema") != Schema)
                throw new InvalidDataException("This is not a Nebula Plus package for Nebula 2 (" + manifestPath + ").");
            var files = Json.Obj(package.manifest, "files");
            if (files == null || files.Count == 0 || string.IsNullOrEmpty(package.Commit))
                throw new InvalidDataException("The package manifest is incomplete.");
            foreach (var pair in files)
            {
                string path = PathUtil.SafeJoin(package.Directory, pair.Key);
                if (!System.IO.File.Exists(path)) throw new InvalidDataException("The package is missing " + pair.Key + ".");
                if (FileUtil.Sha256File(path) != Convert.ToString(pair.Value))
                    throw new InvalidDataException("The package file " + pair.Key + " does not match its manifest.");
            }
            foreach (var required in new[] { "NebulaNative.exe", "smg_game.dll", "nebula-import.exe", "NebulaPlus.exe", "nebula-source.zip", "source-manifest.json" })
                if (!files.ContainsKey(required)) throw new InvalidDataException("The package has no " + required + ".");
            return package;
        }

        /// <summary>Unpacks a package zip into an empty <paramref name="target"/> and opens it.</summary>
        public static PlusPackage Extract(string zip, string target)
        {
            if (System.IO.Directory.Exists(target)) FileUtil.DeleteTree(target);
            System.IO.Directory.CreateDirectory(target);
            using (var archive = ZipFile.OpenRead(zip))
            {
                foreach (var entry in archive.Entries)
                {
                    string name = entry.FullName.Replace('\\', '/');
                    if (name.EndsWith("/")) continue;
                    string path = PathUtil.SafeJoin(target, name);
                    System.IO.Directory.CreateDirectory(Path.GetDirectoryName(path));
                    entry.ExtractToFile(path, false);
                }
            }
            return OpenFolder(target);
        }

        /// <summary>
        /// The package's source tree (for the mod builders) under <paramref name="sources"/>, checked file by file
        /// against source-manifest.json; a copy checked before is reused.
        /// </summary>
        public string EnsureSource(string sources, Action<string> log)
        {
            var sourceManifest = Json.Load(File("source-manifest.json"));
            var files = Json.Obj(sourceManifest, "files");
            string target = Path.Combine(sources, Commit.Substring(0, Math.Min(12, Commit.Length)));
            if (System.IO.Directory.Exists(target) && SourceMatches(target, files)) { log("Using the checked source at " + target + "."); return target; }
            string partial = target + ".partial";
            PathUtil.ExtractStrippingTop(File("nebula-source.zip"), partial);
            if (!SourceMatches(partial, files)) throw new InvalidDataException("The package's source does not match its manifest.");
            if (System.IO.Directory.Exists(target)) FileUtil.DeleteTree(target);
            FileUtil.MoveDirectory(partial, target, log);
            log("Checked " + files.Count + " source files of revision " + Commit + ".");
            return target;
        }

        private static bool SourceMatches(string root, Dictionary<string, object> files)
        {
            foreach (var pair in files)
            {
                string path = Path.Combine(root, pair.Key.Replace('/', '\\'));
                if (!System.IO.File.Exists(path) || FileUtil.Sha256File(path) != Convert.ToString(pair.Value)) return false;
            }
            return System.IO.Directory.GetFiles(root, "*", SearchOption.AllDirectories).Length == files.Count;
        }

        /// <summary>RSA-SHA256 over the file, against the Nebula Plus release key built into this program.</summary>
        public static bool VerifySignature(string file, string signatureFile)
        {
            if (string.IsNullOrEmpty(BuildInfo.PlusReleasePublicKeyXml)) return false;
            byte[] signature;
            try { signature = Convert.FromBase64String(System.IO.File.ReadAllText(signatureFile).Trim()); }
            catch (FormatException) { return false; }
            using (var rsa = new RSACryptoServiceProvider())
            {
                rsa.PersistKeyInCsp = false;
                rsa.FromXmlString(BuildInfo.PlusReleasePublicKeyXml);
                using (var stream = System.IO.File.OpenRead(file))
                using (var sha = SHA256.Create())
                    return rsa.VerifyHash(sha.ComputeHash(stream), CryptoConfig.MapNameToOID("SHA256"), signature);
            }
        }
    }
}
