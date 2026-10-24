using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Security.Cryptography;

namespace Nebula.Plus
{
    /// <summary>
    /// A Nebula Plus build package (installer/plus/build-plus-package.ps1): the prebuilt runtime and recompiler,
    /// built only from repository source, the exact source revision, and plus-package.json with every file's
    /// SHA-256. It holds nothing derived from a game: the modules, content and mods are built on this PC.
    /// </summary>
    internal sealed class PlusPackage
    {
        public const string ManifestName = "plus-package.json";

        private Dictionary<string, object> manifest;

        public string Directory { get; private set; }
        public string Version { get { return Json.Str(manifest, "version"); } }
        public string Commit { get { return Json.Str(manifest, "commit"); } }
        public string Repository { get { return Json.Str(manifest, "repository"); } }
        public string Branch { get { return Json.Str(manifest, "branch"); } }
        public string RuntimeIsa { get { return Json.Str(manifest, "runtimeIsa") ?? "SSE2"; } }
        /// <summary>Everything the compiled game modules depend on (generator, metadata, module headers, float/DSP libraries).</summary>
        public string ModuleKey { get { return Json.Str(manifest, "moduleKey"); } }
        /// <summary>The mod builders (tools/animations).</summary>
        public string ModToolsKey { get { return Json.Str(manifest, "modToolsKey"); } }

        public string File(string name) { return Path.Combine(Directory, name.Replace('/', '\\')); }

        /// <summary>A package folder, checked file by file against its manifest.</summary>
        public static PlusPackage OpenFolder(string folder)
        {
            var package = new PlusPackage { Directory = Path.GetFullPath(folder) };
            string manifestPath = Path.Combine(package.Directory, ManifestName);
            if (!System.IO.File.Exists(manifestPath)) throw new InvalidDataException(folder + " is not a Nebula Plus package (no " + ManifestName + ").");
            package.manifest = Json.Load(manifestPath);
            if (Json.Str(package.manifest, "schema") != "nebula.plus.package.v1")
                throw new InvalidDataException("Unknown package format in " + manifestPath + ".");
            var files = Json.Obj(package.manifest, "files");
            if (files == null || files.Count == 0 || string.IsNullOrEmpty(package.Commit) || string.IsNullOrEmpty(package.ModuleKey))
                throw new InvalidDataException("The package manifest is incomplete.");
            foreach (var pair in files)
            {
                string path = Nebula.Setup.Toolchain.SafeJoin(package.Directory, pair.Key);
                if (!System.IO.File.Exists(path)) throw new InvalidDataException("The package is missing " + pair.Key + ".");
                if (FileUtil.Sha256File(path) != Convert.ToString(pair.Value))
                    throw new InvalidDataException("The package file " + pair.Key + " does not match its manifest.");
            }
            foreach (var required in new[] { "NebulaRuntime.exe", "nebula-recomp.exe", "NebulaPlus.exe", "nebula-source.zip",
                "source-manifest.json", "module-compatibility.json", "toolchain.json",
                "galaxy_ppc_float.lib", "galaxy_softfloat.lib", "galaxy_dsp_alu.lib" })
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
                    string path = Nebula.Setup.Toolchain.SafeJoin(target, name);
                    System.IO.Directory.CreateDirectory(Path.GetDirectoryName(path));
                    entry.ExtractToFile(path, false);
                }
            }
            return OpenFolder(target);
        }

        public Dictionary<string, object> SourceManifest() { return Json.Load(File("source-manifest.json")); }
        public Dictionary<string, object> ToolchainPins() { return Json.Load(File("toolchain.json")); }
        public Dictionary<string, object> ModuleContract() { return Json.Load(File("module-compatibility.json")); }

        /// <summary>RSA-SHA256 over the file, against the release key built into this program.</summary>
        public static bool VerifySignature(string file, string signatureFile)
        {
            if (string.IsNullOrEmpty(BuildInfo.ReleasePublicKeyXml)) return false;
            byte[] signature;
            try { signature = Convert.FromBase64String(System.IO.File.ReadAllText(signatureFile).Trim()); }
            catch (FormatException) { return false; }
            using (var rsa = new RSACryptoServiceProvider())
            {
                rsa.PersistKeyInCsp = false;
                rsa.FromXmlString(BuildInfo.ReleasePublicKeyXml);
                using (var stream = System.IO.File.OpenRead(file))
                using (var sha = SHA256.Create())
                    return rsa.VerifyHash(sha.ComputeHash(stream), CryptoConfig.MapNameToOID("SHA256"), signature);
            }
        }
    }
}
