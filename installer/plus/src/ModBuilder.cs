using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

namespace Nebula.Plus
{
    /// <summary>One of the user's own mod input files, which the repository's builders convert on this PC.</summary>
    internal sealed class ModInput
    {
        public string Key, Label, Hint;
        public bool Folder;

        public static readonly ModInput[] All =
        {
            new ModInput { Key = "marioModelBlend", Label = "Odyssey Mario model (.blend)", Hint = "mario-model-FK.blend" },
            new ModInput { Key = "marioTextures", Label = "Odyssey Mario textures (folder)", Hint = "the folder with MarioBody_alb.png", Folder = true },
            new ModInput { Key = "animationBlend", Label = "Animations (.blend)", Hint = "the Odyssey rig's action file, e.g. \"64  24.blend\"" },
            new ModInput { Key = "faceAnimations", Label = "Face animations (.szs)", Hint = "PlayerAnimationFace.szs" },
            new ModInput { Key = "faceSkeleton", Label = "Face skeleton (.fbx)", Hint = "Custom Bone vis Armature.fbx" },
            new ModInput { Key = "luigiGltf", Label = "Odyssey Luigi model (.gltf)", Hint = "Luigi.gltf, with its textures folder beside it" },
            new ModInput { Key = "voiceClips", Label = "Mario voice clips (folder)", Hint = "the folder with \"(Mario) Jump 1.ogg\"", Folder = true },
            new ModInput { Key = "marioModelDae", Label = "Other Mario model (.dae, instead of the Odyssey one)", Hint = "a Smash Ultimate style mario.dae" }
        };

        public static ModInput ByKey(string key) { return All.FirstOrDefault(i => i.Key == key); }

        public bool Present(string path)
        {
            return !string.IsNullOrEmpty(path) && (Folder ? Directory.Exists(path) : File.Exists(path));
        }

        /// <summary>
        /// Looks for the inputs in the usual places (Downloads, Documents, Desktop, OneDrive Documents), by the names
        /// the builders were written for. Only paths are returned; nothing is read or copied.
        /// </summary>
        public static Dictionary<string, string> Detect()
        {
            var found = new Dictionary<string, string>(StringComparer.Ordinal);
            string home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
            var roots = new[]
            {
                Path.Combine(home, "Downloads"), Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments),
                Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory), Path.Combine(home, "OneDrive", "Documents"),
                Path.Combine(home, "OneDrive", "Desktop")
            }.Where(Directory.Exists).Distinct(StringComparer.OrdinalIgnoreCase).ToList();
            var files = new List<FileInfo>();
            var dirs = new List<DirectoryInfo>();
            foreach (var root in roots) Walk(new DirectoryInfo(root), 0, files, dirs);
            Func<Func<FileInfo, bool>, string> file = test =>
            {
                var match = files.Where(test).OrderByDescending(f => f.LastWriteTimeUtc).FirstOrDefault();
                return match == null ? null : match.FullName;
            };
            Action<string, string> put = (key, value) => { if (value != null) found[key] = value; };
            put("marioModelBlend", file(f => f.Name.Equals("mario-model-FK.blend", StringComparison.OrdinalIgnoreCase)));
            put("animationBlend", file(f => f.Extension.Equals(".blend", StringComparison.OrdinalIgnoreCase) && f.Length > (512L << 20)));
            put("faceAnimations", file(f => f.Name.Equals("PlayerAnimationFace.szs", StringComparison.OrdinalIgnoreCase)));
            put("faceSkeleton", file(f => f.Extension.Equals(".fbx", StringComparison.OrdinalIgnoreCase) &&
                f.Name.IndexOf("Armature", StringComparison.OrdinalIgnoreCase) >= 0));
            put("luigiGltf", file(f => f.Name.Equals("Luigi.gltf", StringComparison.OrdinalIgnoreCase)));
            var textures = dirs.FirstOrDefault(d => File.Exists(Path.Combine(d.FullName, "MarioBody_alb.png")) &&
                File.Exists(Path.Combine(d.FullName, "MarioFace_alb_2.png")));
            if (textures != null) found["marioTextures"] = textures.FullName;
            var voice = dirs.FirstOrDefault(d => File.Exists(Path.Combine(d.FullName, "(Mario) Jump 1.ogg")));
            if (voice != null) found["voiceClips"] = voice.FullName;
            if (!found.ContainsKey("marioModelBlend"))
                put("marioModelDae", file(f => f.Name.Equals("mario.dae", StringComparison.OrdinalIgnoreCase)));
            return found;
        }

        private static void Walk(DirectoryInfo dir, int depth, List<FileInfo> files, List<DirectoryInfo> dirs)
        {
            if (depth > 4) return;
            try
            {
                if ((dir.Attributes & FileAttributes.ReparsePoint) != 0 && depth > 0) return;
                dirs.Add(dir);
                foreach (var f in dir.GetFiles()) files.Add(f);
                foreach (var d in dir.GetDirectories())
                {
                    // Game folders and repacks hold thousands of files and never the inputs.
                    if (d.Name.StartsWith(".") || d.Name.Equals("node_modules", StringComparison.OrdinalIgnoreCase)) continue;
                    Walk(d, depth + 1, files, dirs);
                }
            }
            catch (UnauthorizedAccessException) { }
            catch (IOException) { }
        }
    }

    /// <summary>A Python 3 with the builders' packages (numpy, Pillow, soundfile).</summary>
    internal sealed class PythonEnv
    {
        public static readonly string[] Packages = { "numpy", "pillow", "soundfile" };

        public string Executable { get; private set; }
        public string[] PrefixArgs { get; private set; }
        public string Description { get; private set; }
        public bool HasPackages { get; private set; }
        public string Missing { get; private set; }

        /// <summary>Plus's own environment first, then the py launcher, then python on PATH; null without any Python 3.</summary>
        public static PythonEnv Find(PlusLayout layout, Action<string> log)
        {
            var candidates = new List<KeyValuePair<string, string[]>>();
            string venv = Path.Combine(layout.Python, "Scripts", "python.exe");
            if (File.Exists(venv)) candidates.Add(new KeyValuePair<string, string[]>(venv, new string[0]));
            foreach (var py in new[] { Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Windows), "py.exe"),
                Path.Combine(NebulaPaths.LocalAppData, "Programs", "Python", "Launcher", "py.exe"), Which("py.exe") })
                if (py != null && File.Exists(py)) { candidates.Add(new KeyValuePair<string, string[]>(py, new[] { "-3" })); break; }
            string python = Which("python.exe");
            // The Microsoft Store alias opens the Store instead of running anything.
            if (python != null && python.IndexOf("WindowsApps", StringComparison.OrdinalIgnoreCase) < 0)
                candidates.Add(new KeyValuePair<string, string[]>(python, new string[0]));
            PythonEnv first = null;
            foreach (var candidate in candidates)
            {
                var env = Probe(candidate.Key, candidate.Value, log);
                if (env == null) continue;
                if (env.HasPackages) return env;
                if (first == null) first = env;
            }
            return first;
        }

        private static PythonEnv Probe(string exe, string[] prefix, Action<string> log)
        {
            var lines = new List<string>();
            const string script = "import sys\nprint('PY', sys.version_info[0], sys.version_info[1], sys.executable)\nmissing=[]\n" +
                "for m in ('numpy','PIL','soundfile'):\n    try: __import__(m)\n    except Exception: missing.append(m)\nprint('MISSING', ' '.join(missing))\n";
            int code = Run(exe, prefix.Concat(new[] { "-c", script }).ToList(), null, lines.Add, CancellationToken.None);
            var version = lines.FirstOrDefault(l => l.StartsWith("PY "));
            if (code != 0 || version == null) return null;
            var parts = version.Split(new[] { ' ' }, 4);
            int major, minor;
            if (!int.TryParse(parts[1], out major) || !int.TryParse(parts[2], out minor) || major != 3 || minor < 9)
            {
                log("Python at " + exe + " is " + parts[1] + "." + parts[2] + "; the mod builders need 3.9 or newer.");
                return null;
            }
            string missing = (lines.FirstOrDefault(l => l.StartsWith("MISSING")) ?? "MISSING").Substring(7).Trim();
            return new PythonEnv
            {
                Executable = exe, PrefixArgs = prefix, Description = "Python " + major + "." + minor + " (" + parts[3] + ")",
                HasPackages = missing.Length == 0, Missing = missing.Replace("PIL", "Pillow")
            };
        }

        /// <summary>
        /// Plus's own virtual environment (layout\python) with the packages from PyPI, about 60 MB. Only called when
        /// the user agreed; the system Python and its packages are left as they are.
        /// </summary>
        public static PythonEnv CreateVenv(PythonEnv basePython, PlusLayout layout, Action<string> log, CancellationToken cancel)
        {
            if (Directory.Exists(layout.Python)) FileUtil.DeleteTree(layout.Python);
            int code = Run(basePython.Executable, basePython.PrefixArgs.Concat(new[] { "-m", "venv", layout.Python }).ToList(), null, log, cancel);
            if (code != 0) throw new InvalidOperationException("Could not create a Python environment in " + layout.Python + ".");
            string python = Path.Combine(layout.Python, "Scripts", "python.exe");
            code = Run(python, new[] { "-m", "pip", "install", "--disable-pip-version-check", "--no-input" }.Concat(Packages).ToList(), null, log, cancel);
            if (code != 0) throw new InvalidOperationException("pip could not install " + string.Join(", ", Packages) + ". See the log.");
            var env = Probe(python, new string[0], log);
            if (env == null || !env.HasPackages) throw new InvalidOperationException("The new Python environment is missing " + (env == null ? "Python" : env.Missing) + ".");
            return env;
        }

        public int RunScript(string script, IList<string> arguments, string workingDirectory, Action<string> log, CancellationToken cancel)
        {
            return Run(Executable, PrefixArgs.Concat(new[] { "-u", script }).Concat(arguments).ToList(), workingDirectory, log, cancel);
        }

        private static int Run(string exe, IList<string> arguments, string workingDirectory, Action<string> log, CancellationToken cancel)
        {
            var env = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            foreach (System.Collections.DictionaryEntry pair in Environment.GetEnvironmentVariables())
                env[(string)pair.Key] = (string)pair.Value;
            env.Remove("PYTHONPATH");
            env.Remove("PYTHONHOME");
            env["PYTHONIOENCODING"] = "utf-8";
            env["PYTHONDONTWRITEBYTECODE"] = "1";
            try
            {
                return Proc.Run(exe, arguments, workingDirectory ?? Path.GetTempPath(), env, log, cancel);
            }
            catch (System.ComponentModel.Win32Exception) { return -1; }
        }

        private static string Which(string name)
        {
            foreach (var dir in (Environment.GetEnvironmentVariable("PATH") ?? "").Split(';'))
            {
                if (dir.Length == 0) continue;
                try { string path = Path.Combine(dir.Trim('"'), name); if (File.Exists(path)) return path; }
                catch (ArgumentException) { }
            }
            return null;
        }
    }

    /// <summary>
    /// Builds the model, Luigi, animation and voice mods with tools/animations from the user's own files, into a
    /// work folder beside the content. Each mod whose inputs are missing is skipped with the reason.
    /// </summary>
    internal sealed class ModBuilder
    {
        public sealed class Result
        {
            public readonly List<string> Built = new List<string>();
            public readonly List<string> Skipped = new List<string>();
        }

        /// <summary>
        /// Identity of a mod build: the builders' sources, each input's path, size and write time (every file of a
        /// folder input), and the content it reads. Equal keys mean the existing mods are current.
        /// </summary>
        public static string Key(string modToolsKey, IDictionary<string, string> inputs, string contentMarker)
        {
            var text = new StringBuilder();
            text.Append("tools ").Append(modToolsKey).Append('\n');
            text.Append("content ").Append(File.Exists(contentMarker) ? FileUtil.Sha256File(contentMarker) : "-").Append('\n');
            foreach (var input in ModInput.All)
            {
                string path;
                if (!inputs.TryGetValue(input.Key, out path) || !input.Present(path)) { text.Append(input.Key).Append(" -\n"); continue; }
                text.Append(input.Key).Append(' ').Append(Path.GetFullPath(path).ToLowerInvariant()).Append('\n');
                IEnumerable<string> members = input.Folder
                    ? Directory.GetFiles(path, "*", SearchOption.AllDirectories).OrderBy(p => p, StringComparer.OrdinalIgnoreCase)
                    : (IEnumerable<string>)new[] { path };
                foreach (var member in members)
                {
                    var info = new FileInfo(member);
                    text.Append("  ").Append(member.Substring(Math.Min(member.Length, path.Length)).ToLowerInvariant())
                        .Append(' ').Append(info.Length).Append(' ').Append(info.LastWriteTimeUtc.Ticks).Append('\n');
                }
            }
            return PathUtil.Sha256Text(text.ToString());
        }

        private readonly PythonEnv python;
        private readonly string tools;
        private readonly Action<string> log;
        private readonly CancellationToken cancel;

        public ModBuilder(PythonEnv python, string sourceDir, Action<string> log, CancellationToken cancel)
        {
            this.python = python;
            tools = Path.Combine(sourceDir, "tools", "animations");
            this.log = log;
            this.cancel = cancel;
        }

        /// <summary>
        /// Builds into <paramref name="work"/>\mods. <paramref name="work"/> gets a hard link to the content's
        /// game.pak (the builders only read it), so nothing is written to the content until the caller swaps the
        /// finished mods in.
        /// </summary>
        public Result Build(string content, string work, IDictionary<string, string> inputs, Action<string, double> progress)
        {
            var result = new Result();
            if (Directory.Exists(work)) FileUtil.DeleteTree(work);
            Directory.CreateDirectory(work);
            string pak = Path.Combine(work, "game.pak");
            if (!CreateHardLink(pak, Path.Combine(content, "game.pak"), IntPtr.Zero))
            {
                log("Could not link game.pak (error " + Marshal.GetLastWin32Error() + "); copying it for the mod build.");
                File.Copy(Path.Combine(content, "game.pak"), pak);
            }
            Func<string, string> input = key =>
            {
                string path;
                return inputs.TryGetValue(key, out path) && ModInput.ByKey(key).Present(path) ? path : null;
            };
            string modelBlend = input("marioModelBlend"), textures = input("marioTextures"), animBlend = input("animationBlend");
            string face = input("faceAnimations"), faceSkeleton = input("faceSkeleton"), luigi = input("luigiGltf");
            string voice = input("voiceClips"), dae = input("marioModelDae");
            string model = Path.Combine(work, "mods", "model");

            progress("Building the Mario model", 0.0);
            if (modelBlend != null && textures != null && animBlend != null)
                Step(result, "Odyssey Mario model", "build_odyssey_model_mod.py",
                    new[] { "--blend", modelBlend, "--textures", textures, "--anim-blend", animBlend, "--content", work });
            else if (dae != null)
                Step(result, "Mario model (" + Path.GetFileName(dae) + ")", "build_model_mod.py", new[] { "--dae", dae, "--content", work });
            else
                result.Skipped.Add("Odyssey Mario model: needs the model .blend, its textures folder and the animations .blend" +
                    Missing(new[] { "marioModelBlend", "marioTextures", "animationBlend" }, input));
            bool haveModel = File.Exists(Path.Combine(model, "manifest.txt"));

            progress("Building the Luigi model", 0.25);
            if (luigi != null && haveModel)
                Step(result, "Odyssey Luigi model (co-op)", "build_odyssey_luigi_mod.py", new[] { "--gltf", luigi, "--content", work });
            else
                result.Skipped.Add("Odyssey Luigi model: " + (luigi == null ? "no Luigi.gltf" : "needs the Mario model built first"));

            progress("Converting the animations", 0.45);
            if (animBlend != null)
            {
                var args = new List<string> { "--blend", animBlend, "--content", work, "--luigi-originals" };
                if (haveModel) { args.Add("--model"); args.Add(model); }
                if (haveModel && face != null && faceSkeleton != null)
                {
                    args.AddRange(new[] { "--face-anims", face, "--face-skeleton", faceSkeleton });
                }
                else if (haveModel)
                    log("Face animations skipped: needs PlayerAnimationFace.szs and the face skeleton .fbx.");
                Step(result, "Odyssey animations" + (haveModel && face != null && faceSkeleton != null ? " with face animation" : ""),
                    "build_animation_mod.py", args);
            }
            else result.Skipped.Add("Odyssey animations: no animations .blend");

            progress("Converting the voice clips", 0.85);
            if (voice != null)
                Step(result, "Mario voice", "build_voice_mod.py", new[] { "--clips", voice, "--content", work });
            else result.Skipped.Add("Mario voice: no voice clips folder");
            progress("Mods built", 1.0);
            return result;
        }

        /// <summary>
        /// Packs what <see cref="Build"/> wrote under <paramref name="work"/>\mods into one mod folder of Nebula's mods
        /// system (tools/animations/nebula_mod.py: the archives as files\ in the disc's layout, the voice clips, Luigi's
        /// animation table). The model's animations are the ones made for it when the model was built. Returns false when
        /// nothing was built.
        /// </summary>
        public bool Pack(string work, string modFolder)
        {
            string mods = Path.Combine(work, "mods");
            var inputs = new List<string>();
            Action<string, string> add = (folder, marker) =>
            {
                string path = Path.Combine(mods, folder);
                if (File.Exists(Path.Combine(path, marker))) inputs.Add(path);
            };
            add("model", "manifest.txt");
            bool model = inputs.Count != 0;
            add(model ? "animations-model" : "animations", "manifest.txt");
            add("voice", "voices.txt");
            if (inputs.Count == 0) return false;
            if (Directory.Exists(modFolder)) FileUtil.DeleteTree(modFolder);
            var arguments = new List<string> { "--out", modFolder };
            arguments.AddRange(inputs);
            log("== Packing the mod: nebula_mod.py " + ProcessRunner.JoinArguments(arguments));
            int code = python.RunScript(Path.Combine(tools, "nebula_mod.py"), arguments, tools, log, cancel);
            if (code != 0) throw new InvalidOperationException("Packing the mod failed (nebula_mod.py exit " + code + "). See the log.");
            return true;
        }

        private static string Missing(string[] keys, Func<string, string> input)
        {
            var missing = keys.Where(k => input(k) == null).Select(k => ModInput.ByKey(k).Label).ToList();
            return missing.Count == 0 ? "" : " (missing: " + string.Join(", ", missing) + ")";
        }

        private void Step(Result result, string name, string script, IList<string> arguments)
        {
            cancel.ThrowIfCancellationRequested();
            log("== " + name + ": " + script + " " + ProcessRunner.JoinArguments(arguments));
            int code = python.RunScript(Path.Combine(tools, script), arguments, tools, log, cancel);
            // One failed mod does not stop the others or the install; the game runs without it.
            if (code != 0) { result.Skipped.Add(name + ": FAILED (" + script + " exit " + code + "; see the log)"); return; }
            result.Built.Add(name);
        }

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern bool CreateHardLink(string link, string existing, IntPtr security);
    }
}
