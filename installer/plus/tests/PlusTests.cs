using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;

namespace Nebula.Plus
{
    /// <summary>
    /// Nebula Plus checks that need no game, network or install: release selection, version order, the launchers,
    /// the mod key, and the saves copy with its NTFS streams. NEBULA_DATA_ROOT points the "official" data root at a
    /// scratch folder, so the user's files are never read or written.
    /// </summary>
    internal static class PlusTests
    {
        private static int failures;

        private static void Check(bool condition, string name)
        {
            Console.WriteLine((condition ? "ok   " : "FAIL ") + name);
            if (!condition) failures++;
        }

        private static int Main(string[] args)
        {
            string scratch = args.Length > 0 ? args[0] : Path.Combine(Path.GetTempPath(), "plus-tests-" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(scratch);
            Environment.SetEnvironmentVariable("NEBULA_DATA_ROOT", Path.Combine(scratch, "official-data"));

            // Versions and release selection.
            Check(PlusUpdater.CompareVersions("1.20261024.105", "1.20261024.99") > 0, "versions compare numerically");
            Check(PlusUpdater.CompareVersions("1.0.0", "1.0.0-test.1") > 0, "a release sorts after its test builds");
            string releases = "[" +
                "{\"tag_name\":\"plus2-v1.0.1\",\"draft\":false,\"body\":\"b\",\"assets\":[{\"name\":\"NebulaPlus-1.0.1.zip\",\"browser_download_url\":\"https://x/z\",\"size\":5,\"digest\":\"sha256:ABCD\"},{\"name\":\"NebulaPlus-1.0.1.zip.sig\",\"browser_download_url\":\"https://x/s\"}]}," +
                "{\"tag_name\":\"plus2-v1.0.2\",\"draft\":true,\"assets\":[{\"name\":\"NebulaPlus-1.0.2.zip\"},{\"name\":\"NebulaPlus-1.0.2.zip.sig\"}]}," +
                "{\"tag_name\":\"plus2-v1.0.3\",\"draft\":false,\"assets\":[{\"name\":\"NebulaPlus-1.0.3.zip\"}]}," +
                "{\"tag_name\":\"plus-v9.0.0\",\"draft\":false,\"assets\":[{\"name\":\"Nebula-Setup.exe\"},{\"name\":\"Nebula-Setup.exe.sig\"}]}]";
            var picked = PlusUpdater.SelectRelease(releases, "1.0.0");
            Check(picked != null && picked.Version == "1.0.1", "the newest signed, published plus-v release is offered (drafts, unsigned, official and Nebula 1 Plus releases are not)");
            Check(picked != null && picked.PackageSha256 == "abcd", "GitHub's asset digest is kept for the download check");
            Check(PlusUpdater.SelectRelease(releases, "1.0.1") == null, "nothing is offered when the installed version is the newest");

            // Launchers.
            var layout = new PlusLayout(Path.Combine(scratch, "Plus Folder"));
            Directory.CreateDirectory(layout.Root);
            Launchers.WriteCurrent(layout, "1.0.1", "1.0.1", "abc");
            Launchers.WriteAll(layout, new PlusConfig());
            string play = File.ReadAllText(Path.Combine(layout.Root, "Play Nebula Plus.cmd"));
            Check(play.Contains("set \"LOCALAPPDATA=%PLUS_DATA%\"") && play.Contains("--nand \"%PLUS_SAVES%\"") && play.Contains("PLUS_SAVES=%PLUS_DATA%\\saves") &&
                play.Contains("--pak \"%~dp0content\\game.pak\""), "the play launcher keeps the game's data, saves and settings in data\\");
            string coop = File.ReadAllText(Path.Combine(layout.Root, "Play co-op.cmd"));
            Check(coop.Contains("GALAXY_TWO_PLAYERS=1"), "the co-op launcher starts two players");
            string joinCmd = File.ReadAllText(Path.Combine(layout.Root, "Join online game.cmd"));
            Check(joinCmd.Contains("GALAXY_NET_JOIN=%HOST%") && joinCmd.Contains("data-join") && joinCmd.Contains("/MIR"),
                "the join launcher keeps its own data and mirrors the host-side mods");
            var shared = new PlusConfig { SavesMode = "shared" };
            Launchers.WriteAll(layout, shared);
            Check(File.ReadAllText(Path.Combine(layout.Root, "Host online game.cmd")).Contains("PLUS_SAVES=" + OfficialNebula.Saves),
                "shared saves point the launchers at the official saves folder");
            Check(File.ReadAllText(layout.CurrentCmd).Contains("PLUS_APP=%~dp0versions\\1.0.1"), "current.cmd names the installed version");

            // Mod key: stable, and it follows the input files.
            string inputs = Path.Combine(scratch, "inputs");
            Directory.CreateDirectory(Path.Combine(inputs, "voice"));
            File.WriteAllText(Path.Combine(inputs, "voice", "(Mario) Jump 1.ogg"), "a");
            var mods = new Dictionary<string, string> { { "voiceClips", Path.Combine(inputs, "voice") } };
            string marker = Path.Combine(scratch, "content.json");
            File.WriteAllText(marker, "{\"formatVersion\":1}");
            string key1 = ModBuilder.Key("tools", mods, marker), key2 = ModBuilder.Key("tools", mods, marker);
            File.WriteAllText(Path.Combine(inputs, "voice", "(Mario) Jump 2.ogg"), "b");
            string key3 = ModBuilder.Key("tools", mods, marker);
            Check(key1 == key2, "the mod key is stable");
            Check(key1 != key3, "a new file in an input folder changes the mod key");
            Check(ModBuilder.Key("tools2", mods, marker) != key3, "new builders change the mod key");

            // Saves copy: streams kept, official files untouched, Plus's own saves never overwritten.
            string official = OfficialNebula.Saves;
            string title = Path.Combine(official, "title", "00010000", "524d4745", "data");
            Directory.CreateDirectory(title);
            string save = Path.Combine(title, "GameData.bin");
            File.WriteAllBytes(save, new byte[] { 1, 2, 3 });
            FileUtil.WriteNamedStream(save, "galaxy.isfs.meta", new byte[] { 9, 9 });
            FileUtil.WriteNamedStream(title, "galaxy.isfs.meta", new byte[] { 7 });
            var before = FileUtil.StreamDigests(save);
            string target = SavesImport.ImportSaves(layout, delegate(string line) { });
            string copied = Path.Combine(layout.Saves, "title", "00010000", "524d4745", "data", "GameData.bin");
            Check(target == layout.Saves && File.Exists(copied), "the official saves are copied into data\\saves");
            Check(FileUtil.StreamDigests(copied).SequenceEqual(before), "a save file keeps its galaxy.isfs.meta stream");
            Check(FileUtil.StreamDigests(Path.GetDirectoryName(copied)).Count == 1, "a save folder keeps its stream");
            Check(FileUtil.StreamDigests(save).SequenceEqual(before) && File.Exists(save), "the official save is unchanged");
            File.WriteAllBytes(copied, new byte[] { 4 });
            string second = SavesImport.ImportSaves(layout, delegate(string line) { });
            Check(second != layout.Saves && File.ReadAllBytes(copied)[0] == 4, "a second import goes beside Plus's saves and overwrites nothing");

            try { FileUtil.DeleteTree(scratch); } catch (Exception) { }
            Console.WriteLine(failures == 0 ? "PASS: Nebula Plus checks" : failures + " Nebula Plus checks FAILED");
            return failures == 0 ? 0 : 1;
        }
    }
}
