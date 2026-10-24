using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Text.RegularExpressions;
using System.Threading;
using Nebula.Setup;

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
        /// <summary>Official install root (null: the registered or default one).</summary>
        public string OfficialRoot;
        /// <summary>The game disc image or extracted folder, when the official install's game files cannot be used.</summary>
        public string Disc;
        public Dictionary<string, string> ModInputs;
        public bool ImportSaves, ImportSettings, ForceModules, ForceMods, SkipMods, AcceptBuildToolsLicense, AllowPythonVenv;
        public string SavesMode;
        public string ModuleIsa;
        public int Jobs;
    }

    /// <summary>
    /// Install and update of a Nebula Plus folder. Everything is assembled in staging; the folder switches to the new
    /// version only when it is complete and its modules load. The previous version stays for rollback. data\ (saves,
    /// settings, caches) is created when missing and otherwise never written, and the official install is only read.
    /// </summary>
    internal sealed class PlusEngine
    {
        private static readonly string[] ModuleFiles = { "RMGE01_game.dll", "RMGE01_home_button.dll", "RMGE01_dsp.dll", "RMGE01_boot_image.bin" };
        private static readonly string[] RuntimeLibraries = { "galaxy_ppc_float.lib", "galaxy_softfloat.lib", "galaxy_dsp_alu.lib" };

        private readonly PlusRequest request;
        private readonly IPlusProgress progress;
        private readonly CancellationToken cancel;
        private readonly PlusLayout layout;
        private readonly StreamWriter logFile;
        private readonly Stopwatch clock = Stopwatch.StartNew();
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
            using (var mutex = new Mutex(false, @"Local\NebulaPlus-" + FileUtil.Sha256Text(layout.Root.ToLowerInvariant()).Substring(0, 16)))
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
            try
            {
                string probe = root;
                while (!Directory.Exists(probe) && Path.GetDirectoryName(probe) != null) probe = Path.GetDirectoryName(probe);
                string fs = FileUtil.FileSystemName(probe);
                if (!string.Equals(fs, "NTFS", StringComparison.OrdinalIgnoreCase)) problems.Add("The Nebula Plus folder must be on an NTFS drive (this one is " + fs + ").");
                long free = FileUtil.FreeBytes(probe);
                if (free < (12L << 30)) problems.Add("Nebula Plus needs about 12 GB free; the drive has " + FileUtil.FormatBytes(free) + ".");
            }
            catch (Exception error) { problems.Add("Cannot inspect the Nebula Plus folder: " + error.Message); }
            return problems;
        }

        private void Install()
        {
            var config = PlusConfig.Load(layout);
            bool configChanged = !File.Exists(layout.Config);
            if (request.OfficialRoot != null) { config.OfficialRoot = request.OfficialRoot; configChanged = true; }
            if (request.Disc != null) { config.Disc = request.Disc; configChanged = true; }
            if (request.SavesMode != null) { config.SavesMode = request.SavesMode; configChanged = true; }
            if (request.ModuleIsa != null) { config.ModuleIsa = request.ModuleIsa; configChanged = true; }
            if (request.AllowPythonVenv) { config.PythonVenvAllowed = true; configChanged = true; }
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
            Log("Package " + package.Version + ", commit " + package.Commit + " (" + package.Repository + " " + package.Branch + "), runtime " + package.RuntimeIsa);
            var current = PlusInstalled.Read(layout);
            if (current != null) Log("Installed: " + current.Version + " (commit " + current.Commit + ")" + (current.Previous != null ? ", previous " + current.Previous : ""));
            var official = OfficialNebula.Find(config.OfficialRoot);
            Log(official != null ? "Official Nebula " + official.Version + " at " + official.Root + " (read only)" : "No official Nebula install found.");
            string isa = string.Equals(config.ModuleIsa, "AVX2", StringComparison.OrdinalIgnoreCase) && InstallEngine.HostSupportsAvx2() ? "AVX2" : "SSE2";

            // ---- game files ----
            bool newGameFiles = request.Disc != null || !OfficialNebula.ContentFolderValid(layout.Content) || !OfficialNebula.InputsValid(layout.GameInputs);
            string inputs = layout.GameInputs, content = layout.Content;
            if (newGameFiles)
            {
                inputs = Path.Combine(staging, "game-inputs");
                content = Path.Combine(staging, "content");
                PrepareGameFiles(official, request.Disc ?? (current == null ? null : config.Disc), inputs, content);
                Json.Save(Path.Combine(content, "content.json"), new Dictionary<string, object>
                {
                    { "formatVersion", OfficialNebula.ContentFormatVersion }, { "createdUtc", DateTime.UtcNow.ToString("o") }
                });
            }

            // ---- source ----
            Stage(0.16, 0.03, "Checking the Nebula Plus source");
            var source = new SourceTree(package.SourceManifest());
            source.EmbeddedArchive = delegate { return File.OpenRead(package.File("nebula-source.zip")); };
            string src = source.Ensure(layout.Sources, null, delegate(string text, double f) { StepProgress(text, f); }, Log, cancel);

            // ---- runtime, recompiler and C++ runtime ----
            string app = Path.Combine(staging, "app");
            Directory.CreateDirectory(app);
            string runtimeIsa = package.RuntimeIsa == "SSE2+AVX2" ? (InstallEngine.HostSupportsAvx2() ? "AVX2" : "SSE2") : package.RuntimeIsa;
            if (runtimeIsa == "AVX2" && !InstallEngine.HostSupportsAvx2()) throw new InvalidOperationException("This package's runtime needs AVX2, which this CPU does not have.");
            Func<string, string> runtimeFile = name => package.RuntimeIsa == "SSE2+AVX2" && runtimeIsa == "AVX2" ? package.File("avx2/" + name) : package.File(name);
            File.Copy(runtimeFile("NebulaRuntime.exe"), Path.Combine(app, "NebulaRuntime.exe"));
            foreach (var name in new[] { "nebula-recomp.exe", "NebulaPlus.exe", "LICENSE", "THIRD-PARTY-NOTICES.md", "README.md", "NEBULA_PLUS.md" })
                if (File.Exists(package.File(name))) File.Copy(package.File(name), Path.Combine(app, name));
            var toolchain = new Toolchain(package.ToolchainPins(), layout.Toolchains);

            // ---- game modules ----
            bool reuse = !request.ForceModules && !newGameFiles && current != null && current.ModuleKey == package.ModuleKey &&
                current.ModuleIsa == isa && current.ToolchainId == toolchain.Id && current.FilesIntact(ModuleFiles);
            if (reuse)
            {
                Stage(0.20, 0.50, "Reusing the compiled game (its code generator did not change)");
                foreach (var name in ModuleFiles) File.Copy(Path.Combine(current.Directory, name), Path.Combine(app, name));
            }
            else
            {
                Log(current == null ? "Compiling the game modules." : "Recompiling the game modules: " +
                    (request.ForceModules ? "asked to" : newGameFiles ? "new game files" : current.ModuleKey != package.ModuleKey ?
                    "the code generator, its metadata or the module headers changed" : current.ModuleIsa != isa ? "another instruction set" :
                    current.ToolchainId != toolchain.Id ? "another compiler toolchain" : "the installed modules are not intact") + ".");
                EnsureToolchain(toolchain, official);
                Stage(0.22, 0.03, "Recompiling the game code");
                string gen = Path.Combine(staging, "generated");
                RunRecomp(package.File("nebula-recomp.exe"), new[] { "generate", inputs, "--output", gen });
                BuildModules(package, toolchain, src, gen, app, isa, runtimeIsa);
            }
            bool haveToolchain = toolchain.IsReady;
            IEnumerable<string> dlls = haveToolchain ? toolchain.RuntimeDlls() : (official != null ? official.RuntimeDlls() : Enumerable.Empty<string>());
            if (!haveToolchain && current != null)
                dlls = Directory.GetFiles(current.Directory, "*.dll").Where(d => !Path.GetFileName(d).StartsWith("RMGE01_", StringComparison.OrdinalIgnoreCase));
            foreach (var dll in dlls)
            {
                string target = Path.Combine(app, Path.GetFileName(dll));
                if (!File.Exists(target)) File.Copy(dll, target);
            }
            if (!File.Exists(Path.Combine(app, "vcruntime140.dll"))) Log("WARNING: no app-local C++ runtime DLLs; the Visual C++ 2015-2022 redistributable must be installed.");
            Stage(0.72, 0.02, "Checking the compiled game");
            ModuleCompatibility.Validate(app, package.ModuleContract());
            Log("The game modules load and match the runtime's module contract.");

            // ---- mods ----
            // The content's marker (written once per packing) is part of the key, so new game files rebuild the mods.
            string wantedModsKey = ModBuilder.Key(package.ModToolsKey, config.ModInputs, Path.Combine(content, "content.json"));
            string keptModsKey = current != null && current.ModsKey != null ? current.ModsKey : "not-built";
            string modsKey;
            string newMods = null;
            ModBuilder.Result mods = null;
            bool modsCurrent = current != null && current.ModsKey == wantedModsKey && Directory.Exists(layout.Mods) && !newGameFiles;
            if (request.SkipMods) { modsKey = keptModsKey; Log("Mods: not built (--skip-mods); the installed mods are kept."); }
            else if (modsCurrent && !request.ForceMods) { modsKey = wantedModsKey; Log("Mods: the builders and your input files did not change; the installed mods are kept."); }
            else
            {
                Stage(0.74, 0.18, "Building the mods from your files");
                mods = BuildMods(config, src, content);
                if (mods != null) newMods = Path.Combine(staging, "modwork", "mods");
                // Not built (no Python): the next update tries again.
                modsKey = mods != null ? wantedModsKey : keptModsKey;
            }

            // ---- assemble ----
            Stage(0.92, 0.04, "Finishing");
            string modsForId = newMods ?? layout.Mods;
            string buildId = "plus1:" + package.Commit.Substring(0, Math.Min(16, package.Commit.Length)) + ":" + package.ModuleKey.Substring(0, 16) +
                ":" + isa + ":" + toolchain.Id + ":" + ModsDigest(modsForId).Substring(0, 16);
            var files = new Dictionary<string, object>();
            foreach (var path in Directory.GetFiles(app)) files[Path.GetFileName(path)] = FileUtil.Sha256File(path);
            var record = new Dictionary<string, object>
            {
                { "schema", "nebula.plus.install.v1" }, { "packageVersion", package.Version }, { "commit", package.Commit },
                { "repository", package.Repository }, { "branch", package.Branch }, { "channel", request.Channel },
                { "moduleKey", package.ModuleKey }, { "moduleIsa", isa }, { "runtimeIsa", runtimeIsa }, { "toolchain", toolchain.Id },
                { "modsKey", modsKey }, { "netBuildId", buildId }, { "installedUtc", DateTime.UtcNow.ToString("o") },
                { "managerVersion", BuildInfo.Version }, { "files", files }
            };
            if (mods != null) { record["modsBuilt"] = mods.Built.ToArray(); record["modsSkipped"] = mods.Skipped.ToArray(); }
            Json.Save(Path.Combine(app, "plus-install.json"), record);
            if (newMods != null)
            {
                Directory.CreateDirectory(newMods);
                Json.Save(Path.Combine(newMods, "plus-mods.json"), new Dictionary<string, object>
                {
                    { "modsKey", modsKey }, { "built", mods.Built.ToArray() }, { "skipped", mods.Skipped.ToArray() }, { "builtUtc", DateTime.UtcNow.ToString("o") }
                });
            }

            // The package stays with its version: a mod rebuild or repair needs no download.
            FileUtil.CopyTree(package.Directory, Path.Combine(app, "package"));

            Stage(0.96, 0.04, "Switching to the new version");
            WaitUntilClosed();
            string versionName = package.Version;
            for (int n = 1; Directory.Exists(layout.VersionDir(versionName)); n++) versionName = package.Version + "+r" + n;
            Directory.CreateDirectory(layout.Versions);
            FileUtil.MoveDirectory(app, layout.VersionDir(versionName), Log);
            if (newGameFiles)
            {
                // The installed mods move into the new content unless new ones replace them.
                if (newMods == null && Directory.Exists(layout.Mods)) FileUtil.MoveDirectoryInPlace(layout.Mods, Path.Combine(content, "mods"));
                InstallEngine.Swap(content, layout.Content, Path.Combine(staging, "content-old"), Log);
                InstallEngine.Swap(inputs, layout.GameInputs, Path.Combine(staging, "game-inputs-old"), Log);
            }
            if (newMods != null) SwapMods(newMods);
            Json.Save(layout.Current, new Dictionary<string, object>
            {
                { "version", versionName }, { "previous", current != null ? current.Version : null }
            });
            Launchers.WriteCurrent(layout, versionName, package.Version, package.Commit, buildId);

            // ---- from here on Plus is installed: tidying, data folders, launchers ----
            if (configChanged) config.Save(layout);
            foreach (var dir in new[] { layout.Data, layout.Saves, layout.GameLogs, layout.ShaderCache, layout.CrashDumps, layout.NetplayCache })
                Directory.CreateDirectory(dir);
            Launchers.WriteAll(layout, config);
            if (request.ImportSaves)
            {
                try { SavesImport.ImportSaves(layout, Log); }
                catch (Exception error) { Log("Could not import the official saves: " + error.Message); }
            }
            if (request.ImportSettings)
            {
                try { SavesImport.ImportSettings(layout, Log); }
                catch (Exception error) { Log("Could not import the official settings: " + error.Message); }
            }
            foreach (var dir in Directory.GetDirectories(layout.Versions))
            {
                string name = Path.GetFileName(dir);
                if (name == versionName || (current != null && name == current.Version)) continue;
                try { FileUtil.DeleteTree(dir); } catch (Exception error) { Log("Could not remove " + dir + ": " + error.Message); }
            }
            var summary = new List<string> { "Nebula Plus " + package.Version + " (" + package.Commit.Substring(0, 12) + ") is installed in " + layout.Root + "." };
            summary.Add(reuse ? "The compiled game was reused." : "The game modules were compiled (" + isa + ").");
            if (mods != null)
            {
                if (mods.Built.Count != 0) summary.Add("Mods built: " + string.Join(", ", mods.Built) + ".");
                foreach (var skipped in mods.Skipped) summary.Add("Skipped: " + skipped + ".");
            }
            else if (!request.SkipMods) summary.Add("Mods: unchanged.");
            Summary = string.Join("\r\n", summary);
            Log(Summary);
            Log(string.Format("Completed in {0:mm\\:ss}.", clock.Elapsed));
            Report(1.0, "Nebula Plus " + package.Version + " is installed.");
        }

        // ---- steps ---------------------------------------------------------------

        private void PrepareGameFiles(OfficialNebula official, string disc, string inputs, string content)
        {
            if (disc == null && official != null && official.GameInputsValid() && official.ContentValid())
            {
                Stage(0.03, 0.13, "Copying the game files from the official Nebula install");
                CopyTreeWithProgress(official.GameInputs, inputs, null);
                if (!OfficialNebula.InputsValid(inputs)) throw new InvalidDataException("The copied game inputs do not match their hashes.");
                CopyTreeWithProgress(official.Content, content, "mods");
                File.Delete(Path.Combine(content, "content.json"));
                return;
            }
            if (disc == null)
                throw new InvalidOperationException(official == null
                    ? "No official Nebula install was found. Choose your Super Mario Galaxy disc image (ISO, WBFS, RVZ) or extracted folder."
                    : "The official Nebula install at " + official.Root + " has no complete game files. Choose your Super Mario Galaxy disc image.");
            Stage(0.03, 0.02, "Checking your game");
            string recomp = Path.Combine(staging, "package", "nebula-recomp.exe");
            if (!File.Exists(recomp)) recomp = Path.Combine(request.Package, "nebula-recomp.exe");
            var lines = new List<string>();
            int code = RunTool(recomp, new[] { "identify", disc, "--json" }, null, lines.Add);
            string json = string.Join("\n", lines);
            Log(json);
            Dictionary<string, object> identity;
            try { identity = Json.Parse(json.Substring(Math.Max(0, json.IndexOf('{')))); }
            catch (Exception) { throw new InvalidOperationException(lines.Count > 0 ? lines[lines.Count - 1] : "The selected file could not be read."); }
            if (code != 0 || !Json.Bool(identity, "supported")) throw new InvalidOperationException(Json.Str(identity, "message"));
            string data = disc;
            if (Directory.Exists(disc))
            {
                string nested = Path.Combine(disc, "DATA");
                if (File.Exists(Path.Combine(nested, "sys", "main.dol"))) data = nested;
            }
            else
            {
                Stage(0.05, 0.06, "Extracting your game");
                string extracted = Path.Combine(staging, "extracted");
                RunRecomp(recomp, new[] { "extract", disc, "--output", extracted });
                data = Path.Combine(extracted, "DATA");
            }
            var hashes = new Dictionary<string, object>();
            foreach (var relative in OfficialNebula.GameInputFiles)
            {
                string to = Path.Combine(inputs, relative.Replace('/', '\\'));
                Directory.CreateDirectory(Path.GetDirectoryName(to));
                File.Copy(Path.Combine(data, relative.Replace('/', '\\')), to);
                hashes[relative] = FileUtil.Sha256File(to);
            }
            Json.Save(Path.Combine(inputs, "inputs.json"), hashes);
            Stage(0.11, 0.05, "Packing the game files");
            RunRecomp(recomp, new[] { "package-content", data, "--output", content });
        }

        /// <summary>Copies a tree (leaving out the top-level folder <paramref name="skipTop"/>), reporting progress by bytes.</summary>
        private void CopyTreeWithProgress(string from, string to, string skipTop)
        {
            from = Path.GetFullPath(from).TrimEnd('\\');
            var all = Directory.GetFiles(from, "*", SearchOption.AllDirectories)
                .Where(f => skipTop == null || !f.Substring(from.Length + 1).StartsWith(skipTop + "\\", StringComparison.OrdinalIgnoreCase)).ToList();
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

        private void EnsureToolchain(Toolchain toolchain, OfficialNebula official)
        {
            if (toolchain.IsReady) { Log("Using the compiler toolchain " + toolchain.Id + "."); return; }
            string officialDir = official == null ? null : official.ToolchainDir(toolchain.Id);
            if (officialDir != null)
            {
                Stage(0.20, 0.02, "Copying the compiler toolchain from the official Nebula install");
                string target = Path.Combine(layout.Toolchains, toolchain.Id);
                if (Directory.Exists(target)) FileUtil.DeleteTree(target);
                CopyTreeWithProgress(officialDir, target + ".partial", null);
                FileUtil.MoveDirectory(target + ".partial", target, Log);
                if (toolchain.IsReady) { Log("Copied the official Nebula's toolchain " + toolchain.Id + "."); return; }
                Log("The copied toolchain is incomplete; downloading it instead.");
            }
            if (!request.AcceptBuildToolsLicense && !progress.Ask("Nebula Plus compiles the game with Microsoft's C++ libraries and the Windows SDK, " +
                    "downloaded from Microsoft (about " + FileUtil.FormatBytes(toolchain.DownloadBytes) + "). By continuing you accept the Microsoft " +
                    "C++ Build Tools license: " + toolchain.LicenseUrl + "\r\n\r\nDownload the compiler toolchain?"))
                throw new InvalidOperationException("The compiler toolchain is needed to build the game. Nothing was changed.");
            Stage(0.20, 0.02, "Downloading the compiler toolchain");
            toolchain.Ensure(delegate(string text, double f) { StepProgress(text, f); }, Log, cancel);
        }

        /// <summary>One clang-cl build of the game, Home Menu and DSP modules (InstallEngine.Build).</summary>
        private void BuildModules(PlusPackage package, Toolchain toolchain, string src, string gen, string app, string isa, string runtimeIsa)
        {
            var env = toolchain.BuildEnvironment();
            int jobs = request.Jobs > 0 ? request.Jobs : InstallEngine.DefaultCompileJobs();
            Func<string, string> lib = name => (package.RuntimeIsa == "SSE2+AVX2" && runtimeIsa == "AVX2" ? package.File("avx2/" + name) : package.File(name)).Replace('\\', '/');
            Stage(0.25, 0.47, "Compiling the game (" + jobs + " at a time)");
            string build = Path.Combine(staging, "build");
            var configure = new List<string> { "-S", gen, "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
                "-DGALAXY_RUNTIME_INCLUDE=" + Path.Combine(src, "runtime", "include").Replace('\\', '/'),
                "-DGALAXY_PPC_FLOAT_LIBRARY=" + lib(RuntimeLibraries[0]), "-DGALAXY_SOFTFLOAT_LIBRARY=" + lib(RuntimeLibraries[1]),
                "-DGALAXY_DSP_ALU_LIBRARY=" + lib(RuntimeLibraries[2]),
                "-DGALAXY_NATIVE_ISA=" + isa, "-DGALAXY_MODULE_COMPILE_JOBS=" + jobs };
            configure.AddRange(toolchain.CMakeToolArguments());
            CMake(toolchain, configure, env);
            CMake(toolchain, new List<string> { "--build", build, "--parallel", jobs.ToString() }, env);
            File.Copy(Path.Combine(build, "game", "RMGE01_game.dll"), Path.Combine(app, "RMGE01_game.dll"));
            File.Copy(Path.Combine(build, "home", "RMGE01_home_button.dll"), Path.Combine(app, "RMGE01_home_button.dll"));
            File.Copy(Path.Combine(build, "dsp", "RMGE01_dsp.dll"), Path.Combine(app, "RMGE01_dsp.dll"));
            File.Copy(Path.Combine(gen, "RMGE01_boot_image.bin"), Path.Combine(app, "RMGE01_boot_image.bin"));
        }

        /// <summary>The mods from the user's inputs into staging\modwork\mods; null when none could be built (no Python).</summary>
        private ModBuilder.Result BuildMods(PlusConfig config, string src, string content)
        {
            var present = ModInput.All.Where(i => config.ModInputs.ContainsKey(i.Key) && i.Present(config.ModInputs[i.Key])).ToList();
            if (present.Count == 0)
            {
                Log("Mods: no mod input files are set (Nebula Plus window > Mods, or --detect-mods). The model, animation and voice mods are skipped; the game runs with Galaxy's own.");
                var none = new ModBuilder.Result();
                none.Skipped.Add("model, animation and voice mods: no input files set");
                Directory.CreateDirectory(Path.Combine(staging, "modwork", "mods"));
                return none;
            }
            foreach (var pair in config.ModInputs)
            {
                var input = ModInput.ByKey(pair.Key);
                if (input != null && !input.Present(pair.Value)) Log("Mod input " + input.Label + " not found at " + pair.Value + ".");
            }
            var python = PythonEnv.Find(layout, Log);
            if (python == null)
            {
                Log("Mods: no Python 3.9+ found. Install Python from https://www.python.org (\"py\" launcher) and run Nebula Plus again; the installed mods are kept.");
                return null;
            }
            Log("Mods: " + python.Description + (python.HasPackages ? "" : ", missing " + python.Missing));
            if (!python.HasPackages)
            {
                bool allowed = request.AllowPythonVenv || config.PythonVenvAllowed || progress.Ask(
                    "The mod builders need the Python packages " + python.Missing + " (numpy, Pillow and soundfile: about 60 MB from PyPI). " +
                    "Install them into Nebula Plus's own Python environment (" + layout.Python + ")? Your Python installation is not changed.");
                if (!allowed) { Log("Mods: Python packages not installed; the installed mods are kept."); return null; }
                StepProgress("Installing the Python packages", -1);
                python = PythonEnv.CreateVenv(python, layout, Log, cancel);
            }
            var builder = new ModBuilder(python, src, Log, cancel);
            return builder.Build(content, Path.Combine(staging, "modwork"), config.ModInputs, delegate(string text, double f) { StepProgress(text, f); });
        }

        /// <summary>The new mods replace content\mods; the old ones stay as content\mods.previous for rollback.</summary>
        private void SwapMods(string newMods)
        {
            string previous = layout.Mods + ".previous";
            if (Directory.Exists(previous)) FileUtil.DeleteTree(previous);
            if (Directory.Exists(layout.Mods)) FileUtil.MoveDirectoryInPlace(layout.Mods, previous);
            try { FileUtil.MoveDirectoryInPlace(newMods, layout.Mods); }
            catch (Exception)
            {
                if (Directory.Exists(previous) && !Directory.Exists(layout.Mods)) FileUtil.MoveDirectoryInPlace(previous, layout.Mods);
                throw;
            }
        }

        /// <summary>Content of the mod folders (reports left out): part of the online build identity.</summary>
        internal static string ModsDigest(string mods)
        {
            if (!Directory.Exists(mods)) return FileUtil.Sha256Text("no mods");
            var text = new System.Text.StringBuilder();
            string root = Path.GetFullPath(mods).TrimEnd('\\');
            foreach (var file in Directory.GetFiles(root, "*", SearchOption.AllDirectories).OrderBy(f => f, StringComparer.OrdinalIgnoreCase))
            {
                string name = Path.GetFileName(file).ToLowerInvariant();
                if (name.EndsWith("report.txt") || name == "plus-mods.json") continue;
                text.Append(file.Substring(root.Length + 1).Replace('\\', '/').ToLowerInvariant()).Append(' ').Append(FileUtil.Sha256File(file)).Append('\n');
            }
            return FileUtil.Sha256Text(text.ToString());
        }

        /// <summary>The game reads the content and mods: it must be closed for the switch. Plus never closes it.</summary>
        private void WaitUntilClosed()
        {
            while (true)
            {
                string running = Running.Find(layout.Root, "NebulaRuntime");
                if (running == null) return;
                Log("Waiting for " + running + " to close.");
                if (!progress.Ask("Nebula Plus is running (" + running + "). Close the game, then choose Yes to finish the update (No cancels it)."))
                    throw new InvalidOperationException("The game is running from " + layout.Root + ". Close it and try again; nothing was changed.");
            }
        }

        // ---- process helpers -----------------------------------------------------

        private void RunRecomp(string recomp, IList<string> arguments)
        {
            int code = RunTool(recomp, arguments, null, delegate(string line)
            {
                var match = Regex.Match(line, @"^PROGRESS (\d+) (\d+)$");
                if (match.Success) { StepProgress(null, double.Parse(match.Groups[1].Value) / Math.Max(1, double.Parse(match.Groups[2].Value))); return; }
                if (line.StartsWith("STEP ")) StepProgress(line.Substring(line.IndexOf(' ', 5) + 1), -1);
                Log(line);
            });
            if (code != 0) throw new InvalidOperationException("nebula-recomp " + arguments[0] + " failed (exit " + code + "). See " + LogPath + ".");
        }

        private static readonly Regex NinjaProgress = new Regex(@"^\[(\d+)/(\d+)\]");

        private void CMake(Toolchain toolchain, IList<string> arguments, Dictionary<string, string> env)
        {
            string last = null;
            int code = RunTool(toolchain.CMake, arguments, env, delegate(string line)
            {
                var match = NinjaProgress.Match(line);
                if (match.Success)
                {
                    StepProgress(null, double.Parse(match.Groups[1].Value) / Math.Max(1, double.Parse(match.Groups[2].Value)));
                    WriteLog(line);
                    return;
                }
                if (line.Contains("error") || line.Contains("FAILED")) last = line;
                Log(line);
            });
            if (code != 0) throw new InvalidOperationException("Compiling failed" + (last != null ? ": " + last : ".") + " See " + LogPath + ".");
        }

        private int RunTool(string file, IList<string> arguments, Dictionary<string, string> env, Action<string> onLine)
        {
            Log("> " + Path.GetFileName(file) + " " + ProcessRunner.JoinArguments(arguments));
            using (var runner = new ProcessRunner())
                return runner.Run(file, arguments, staging, env, onLine, cancel);
        }

        // ---- progress ------------------------------------------------------------

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

        private readonly object logLock = new object();

        private void WriteLog(string line)
        {
            lock (logLock) logFile.WriteLine(DateTime.Now.ToString("HH:mm:ss.fff") + " " + line);
        }

        private void Log(string line)
        {
            WriteLog(line);
            progress.Log(line);
        }
    }
}
