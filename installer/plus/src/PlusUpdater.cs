using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Threading;

namespace Nebula.Plus
{
    /// <summary>A build newer than the installed one: a signed release package, or a branch commit to build here.</summary>
    internal sealed class PlusUpdate
    {
        public string Kind;            // "release" or "branch"
        public string Version, Tag, Notes, Commit;
        public string PackageUrl, SignatureUrl, PackageSha256;
        public long PackageSize;

        public string Describe()
        {
            return Kind == "release" ? "Nebula Plus " + Version + " (" + Tag + ")"
                : "commit " + Commit.Substring(0, Math.Min(12, Commit.Length)) + (string.IsNullOrEmpty(Notes) ? "" : ": " + Notes);
        }
    }

    /// <summary>
    /// Update discovery on the GitHub repository in plus.json (BuildInfo.Repository by default), over HTTPS.
    /// Release packages run only when their RSA-SHA256 signature verifies against the Plus release key built into
    /// this program; GitHub's own SHA-256 of the asset is checked as well when the API reports one.
    /// </summary>
    internal static class PlusUpdater
    {
        // Nebula Plus for Nebula 2. The Nebula 1 (recompiled) Plus releases are tagged plus-v and stay apart.
        public const string TagPrefix = "plus2-v";

        public static PlusUpdate FindNewer(PlusConfig config, PlusInstalled installed)
        {
            return config.Channel == "branch" ? NewerCommit(config, installed) : NewerRelease(config, installed);
        }

        private static PlusUpdate NewerRelease(PlusConfig config, PlusInstalled installed)
        {
            string json = Downloader.GetString("https://api.github.com/repos/" + config.Repository + "/releases?per_page=50",
                "application/vnd.github+json");
            return SelectRelease(json, installed == null ? null : PackageVersion(installed));
        }

        /// <summary>The newest non-draft plus-v release with a package and its signature, newer than <paramref name="installedVersion"/>.</summary>
        internal static PlusUpdate SelectRelease(string json, string installedVersion)
        {
            PlusUpdate best = null;
            foreach (var item in (System.Collections.IList)Json.ParseAny(json))
            {
                var release = (Dictionary<string, object>)item;
                if (Json.Bool(release, "draft")) continue;
                string tag = Json.Str(release, "tag_name") ?? "";
                if (!tag.StartsWith(TagPrefix, StringComparison.Ordinal)) continue;
                string version = tag.Substring(TagPrefix.Length);
                if (installedVersion != null && CompareVersions(version, installedVersion) <= 0) continue;
                if (best != null && CompareVersions(version, best.Version) <= 0) continue;
                string zipName = "NebulaPlus-" + version + ".zip";
                Dictionary<string, object> zip = null, sig = null;
                foreach (Dictionary<string, object> asset in Json.List(release, "assets") ?? new object[0])
                {
                    string name = Json.Str(asset, "name");
                    if (name == zipName) zip = asset;
                    if (name == zipName + ".sig") sig = asset;
                }
                if (zip == null || sig == null) continue;
                string digest = Json.Str(zip, "digest");
                best = new PlusUpdate
                {
                    Kind = "release", Version = version, Tag = tag, Notes = Json.Str(release, "body") ?? "",
                    PackageUrl = Json.Str(zip, "browser_download_url"), SignatureUrl = Json.Str(sig, "browser_download_url"),
                    PackageSize = Json.Long(zip, "size"),
                    PackageSha256 = digest != null && digest.StartsWith("sha256:") ? digest.Substring(7).ToLowerInvariant() : null
                };
            }
            return best;
        }

        private static PlusUpdate NewerCommit(PlusConfig config, PlusInstalled installed)
        {
            string json = Downloader.GetString("https://api.github.com/repos/" + config.Repository + "/commits/" +
                Uri.EscapeDataString(config.Branch), "application/vnd.github+json");
            var commit = Json.Parse(json);
            string sha = Json.Str(commit, "sha");
            if (string.IsNullOrEmpty(sha) || sha.Length != 40) throw new InvalidDataException("GitHub returned no commit for branch " + config.Branch + ".");
            if (installed != null && string.Equals(installed.Commit, sha, StringComparison.OrdinalIgnoreCase)) return null;
            if (installed != null && !string.IsNullOrEmpty(installed.Commit))
            {
                // Only a commit after the installed one: a branch reset to an older commit is not an update.
                string status = null;
                try
                {
                    status = Json.Str(Json.Parse(Downloader.GetString("https://api.github.com/repos/" + config.Repository + "/compare/" +
                        installed.Commit + "..." + sha, "application/vnd.github+json")), "status");
                }
                catch (System.Net.WebException) { /* the installed commit is not on GitHub (a local build): offer the branch */ }
                if (status == "behind" || status == "identical") return null;
            }
            string message = Json.Str(Json.Obj(commit, "commit") ?? new Dictionary<string, object>(), "message") ?? "";
            return new PlusUpdate { Kind = "branch", Commit = sha, Version = "branch-" + sha.Substring(0, 12), Notes = message.Split('\n')[0].Trim() };
        }

        public static string PackageVersion(PlusInstalled installed)
        {
            string version = installed.Version;
            int suffix = version.IndexOf("+r", StringComparison.Ordinal);
            return suffix >= 0 ? version.Substring(0, suffix) : version;
        }

        /// <summary>
        /// The signed package beside this program (the setup zip from the release page holds NebulaPlus.exe, the package
        /// zip and its .sig), so a first install needs no other download; null when there is none or it does not verify.
        /// </summary>
        public static string BundledPackage(Action<string> log)
        {
            string here = Path.GetDirectoryName(Path.GetFullPath(System.Reflection.Assembly.GetExecutingAssembly().Location));
            foreach (var zip in Directory.GetFiles(here, "NebulaPlus-*.zip").OrderByDescending(f => f, StringComparer.OrdinalIgnoreCase))
            {
                if (Path.GetFileName(zip).StartsWith("NebulaPlus-Setup", StringComparison.OrdinalIgnoreCase)) continue;
                if (File.Exists(zip + ".sig") && PlusPackage.VerifySignature(zip, zip + ".sig"))
                {
                    log("Using the signed package that came with Nebula Plus: " + zip);
                    return zip;
                }
                log("Ignored " + zip + ": its signature does not verify against the Nebula Plus release key.");
            }
            return null;
        }

        /// <summary>Downloads a release package and authenticates it; returns the zip's path.</summary>
        public static string DownloadRelease(PlusUpdate update, string directory, bool allowUnsigned, Action<string> log,
            Action<long> onBytes, CancellationToken cancel)
        {
            string zip = Path.Combine(directory, "NebulaPlus-" + update.Version + ".zip");
            string sig = zip + ".sig";
            if (File.Exists(zip)) File.Delete(zip);
            Downloader.Fetch(update.SignatureUrl, sig, null, cancel);
            Downloader.Fetch(update.PackageUrl, zip, onBytes, cancel);
            if (update.PackageSha256 != null)
            {
                string actual = FileUtil.Sha256File(zip);
                if (actual != update.PackageSha256)
                {
                    File.Delete(zip);
                    throw new InvalidDataException("The downloaded package does not match GitHub's SHA-256 (" + actual + "). It was deleted.");
                }
                log("The package matches GitHub's SHA-256 " + update.PackageSha256 + ".");
            }
            if (PlusPackage.VerifySignature(zip, sig)) { log("The package is signed with the Nebula Plus release key."); return zip; }
            if (allowUnsigned && update.PackageSha256 != null)
            {
                log("WARNING: the package's signature does not verify against this program's release key; installing on the " +
                    "strength of HTTPS and GitHub's SHA-256 alone (--allow-unsigned).");
                return zip;
            }
            File.Delete(zip);
            throw new InvalidDataException("The downloaded Nebula Plus " + update.Version + " package is not signed by the Nebula Plus " +
                "release key. It was deleted and not installed.");
        }

        /// <summary>
        /// The branch channel: GitHub's archive of the exact commit (HTTPS), built on this PC with
        /// installer/plus/build-plus-package.ps1. Needs the developer tools (Git, Rust, Visual Studio C++ tools).
        /// </summary>
        public static string BuildCommit(PlusConfig config, PlusUpdate update, string staging, Action<string> log, CancellationToken cancel)
        {
            string missing = DeveloperTools.Missing();
            if (missing != null)
                throw new InvalidOperationException("The branch channel builds the runtime on this PC and needs " + missing +
                    ". Install them, or switch to the release channel (Nebula Plus window > Updates).");
            string zip = Path.Combine(staging, "source-" + update.Commit + ".zip");
            Downloader.Fetch("https://codeload.github.com/" + config.Repository + "/zip/" + update.Commit, zip, null, cancel);
            string src = Path.Combine(staging, "branch-source");
            PathUtil.ExtractStrippingTop(zip, src);
            string output = Path.Combine(staging, "branch-package");
            string script = Path.Combine(src, "installer", "plus", "build-plus-package.ps1");
            if (!File.Exists(script)) throw new InvalidDataException("Commit " + update.Commit + " has no installer/plus/build-plus-package.ps1.");
            string system = Environment.GetFolderPath(Environment.SpecialFolder.System);
            int code = Proc.Run(Path.Combine(system, @"WindowsPowerShell\v1.0\powershell.exe"), new[]
                {
                    "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", script, "-SourceDir", src, "-Commit", update.Commit,
                    "-Repository", config.Repository, "-Branch", config.Branch, "-Version", update.Version, "-OutputDirectory", output
                }, src, null, log, cancel);
            if (code != 0) throw new InvalidOperationException("Building commit " + update.Commit.Substring(0, 12) + " failed (exit " + code + "). See the log.");
            return output;
        }

        /// <summary>Compare MAJOR.MINOR.PATCH[-pre.N]; a release sorts after its previews.</summary>
        public static int CompareVersions(string a, string b)
        {
            long[] x = Key(a), y = Key(b);
            for (int i = 0; i < x.Length; i++) if (x[i] != y[i]) return x[i].CompareTo(y[i]);
            return 0;
        }

        private static long[] Key(string version)
        {
            var key = new long[5];
            if (string.IsNullOrEmpty(version)) return key;
            string[] parts = version.Split(new[] { '-' }, 2);
            string[] numbers = parts[0].Split('.');
            for (int i = 0; i < 3 && i < numbers.Length; i++) long.TryParse(numbers[i], out key[i]);
            key[3] = parts.Length == 1 ? 1 : 0;
            if (parts.Length == 2)
            {
                int dot = parts[1].LastIndexOf('.');
                if (dot >= 0) long.TryParse(parts[1].Substring(dot + 1), out key[4]);
            }
            return key;
        }
    }

    /// <summary>The tools a source build needs: Rust (cargo), Visual Studio's C++ tools and LLVM (clang).</summary>
    internal static class DeveloperTools
    {
        public static string Missing()
        {
            var missing = new List<string>();
            string cargo = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), ".cargo", "bin", "cargo.exe");
            if (!File.Exists(cargo)) missing.Add("Rust (cargo, https://rustup.rs)");
            string vswhere = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86), @"Microsoft Visual Studio\Installer\vswhere.exe");
            if (!File.Exists(vswhere)) missing.Add("Visual Studio 2022 or its Build Tools with the C++ workload");
            if (!File.Exists(@"C:\Program Files\LLVM\bin\clang.exe")) missing.Add("LLVM (clang, for the game module)");
            return missing.Count == 0 ? null : string.Join(" and ", missing);
        }
    }
}
