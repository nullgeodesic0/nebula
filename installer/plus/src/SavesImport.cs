using System;
using System.Collections.Generic;
using System.IO;

namespace Nebula.Plus
{
    /// <summary>
    /// Copies the official Nebula's saves and settings into Plus's own data folder. Copies only, never moves: the
    /// official files are read, not changed. Each save file and folder keeps its NTFS streams (galaxy.isfs.meta holds
    /// the Wii file metadata; a plain copy drops it), and every stream is compared after the copy.
    /// </summary>
    internal static class SavesImport
    {
        /// <summary>The folder the saves went to (data\saves, or a dated folder beside it when Plus has saves already); null when there was nothing to copy.</summary>
        public static string ImportSaves(PlusLayout layout, Action<string> log)
        {
            string source = OfficialNebula.Saves;
            if (!Directory.Exists(source) || Directory.GetFileSystemEntries(source).Length == 0)
            {
                log("The official Nebula has no saves at " + source + ".");
                return null;
            }
            if (FileUtil.IsReparsePoint(source)) source = FileUtil.FinalPath(source);
            Directory.CreateDirectory(layout.Data);
            string target = layout.Saves;
            if (Directory.Exists(target) && Directory.GetFileSystemEntries(target).Length != 0)
            {
                // Plus's own saves are never overwritten: the copy goes beside them for the user to choose.
                target = Path.Combine(layout.Data, "saves-imported-" + DateTime.Now.ToString("yyyyMMdd-HHmmss"));
                log("Nebula Plus has saves already; the official saves are copied to " + target + " instead.");
            }
            string partial = target + ".partial";
            if (Directory.Exists(partial)) FileUtil.DeleteTree(partial);
            int count = 0;
            FileUtil.CopyTreeWithStreams(source, partial, delegate(string file) { count++; });
            Verify(source, partial);
            if (Directory.Exists(target)) Directory.Delete(target); // empty: checked above
            FileUtil.MoveDirectoryInPlace(partial, target);
            log("Copied " + count + " save files from " + source + " to " + target + " (with their metadata streams).");
            return target;
        }

        /// <summary>Every file and folder of the copy has the same streams with the same contents.</summary>
        private static void Verify(string source, string copy)
        {
            var pending = new Stack<KeyValuePair<string, string>>();
            pending.Push(new KeyValuePair<string, string>(source, copy));
            while (pending.Count != 0)
            {
                var pair = pending.Pop();
                Compare(pair.Key, pair.Value);
                foreach (var file in Directory.GetFiles(pair.Key))
                    Compare(file, Path.Combine(pair.Value, Path.GetFileName(file)));
                foreach (var dir in Directory.GetDirectories(pair.Key))
                    pending.Push(new KeyValuePair<string, string>(dir, Path.Combine(pair.Value, Path.GetFileName(dir))));
            }
        }

        private static void Compare(string a, string b)
        {
            var x = FileUtil.StreamDigests(a);
            var y = FileUtil.StreamDigests(b);
            if (x.Count != y.Count) throw new IOException("The copy of " + a + " lost a stream; the import was stopped.");
            foreach (var pair in x)
            {
                string other;
                if (!y.TryGetValue(pair.Key, out other) || other != pair.Value)
                    throw new IOException("The copy of " + a + pair.Key + " differs; the import was stopped.");
            }
        }

        /// <summary>The official runtime_settings.ini and config.ini, each only when Plus has none yet.</summary>
        public static List<string> ImportSettings(PlusLayout layout, Action<string> log)
        {
            var copied = new List<string>();
            Directory.CreateDirectory(layout.Data);
            foreach (var pair in new[]
            {
                new KeyValuePair<string, string>(OfficialNebula.RuntimeSettings, layout.RuntimeSettings),
                new KeyValuePair<string, string>(OfficialNebula.RenderConfig, layout.RenderConfig)
            })
            {
                if (!File.Exists(pair.Key)) continue;
                if (File.Exists(pair.Value)) { log("Kept Nebula Plus's own " + Path.GetFileName(pair.Value) + "."); continue; }
                File.Copy(pair.Key, pair.Value);
                copied.Add(Path.GetFileName(pair.Value));
                log("Copied " + pair.Key + " to " + pair.Value + ".");
            }
            return copied;
        }
    }
}
