using System.IO;
using System.Text;

namespace Nebula.Plus
{
    /// <summary>
    /// The .cmd launchers in the Plus folder (batch files, since PowerShell scripts may be blocked). Each sets
    /// LOCALAPPDATA to data\ for the game, so its settings, mods folder, shader cache, crash reports and netplay cache
    /// are data\Nebula, and passes the packed game and the saves folder (data\saves). The installed version comes from
    /// current.cmd, which an install, update or rollback writes last.
    /// </summary>
    internal static class Launchers
    {
        public static void WriteCurrent(PlusLayout layout, string versionDir, string version, string commit)
        {
            var text = new StringBuilder();
            text.Append("@rem Written by Nebula Plus at each install, update or rollback: the version the launchers start.\r\n");
            text.Append("set \"PLUS_VERSION=").Append(version).Append("\"\r\n");
            text.Append("set \"PLUS_COMMIT=").Append(commit).Append("\"\r\n");
            text.Append("set \"PLUS_APP=%~dp0versions\\").Append(versionDir).Append("\"\r\n");
            FileUtil.WriteAllTextAtomic(layout.CurrentCmd, text.ToString());
        }

        public static void WriteAll(PlusLayout layout, PlusConfig config)
        {
            string saves = config.SavesMode == "shared" ? OfficialNebula.Saves : null;
            Write(layout, "Play Nebula Plus.cmd", "One player. Settings (Esc or the pause menu > Settings) and Settings > Mods switch the Odyssey\r\n" +
                "rem model and moves; \"Play co-op.cmd\" starts with two players.", saves, "", "run", false);
            Write(layout, "Play co-op.cmd", "Mario and Luigi on one PC, split screen. Luigi joins when a second controller presses +\r\n" +
                "rem (Start, Menu or Options).", saves, "set \"GALAXY_TWO_PLAYERS=1\"\r\necho Two players: Luigi joins when a second controller presses + (Start, Menu or Options).\r\n", "coop", false);
            Write(layout, "Host online game.cmd", "Online co-op, host (you play Mario, in your save). Your friend runs \"Join online game.cmd\"\r\n" +
                "rem with this PC's address. TCP port 7777 by default: forward it on your router for a friend outside your network.\r\n" +
                "rem Both PCs need the same Nebula Plus version and the same mods on.\r\n" +
                "rem Change settings and mods before hosting: in an online session the settings and HOME menus are locked.\r\n" +
                "rem Usage: \"Host online game.cmd\" [port] [input delay in frames] [live | rollback]\r\n" +
                "rem   live (default): the game starts at once and your friend can join at any time (Luigi drops in).\r\n" +
                "rem   rollback: the game waits at the start for your friend. Both find and repair any desync by themselves.",
                saves,
                "set \"PORT=%~1\"\r\nif \"%PORT%\"==\"\" set \"PORT=7777\"\r\nset \"MODE=%~3\"\r\nif \"%MODE%\"==\"\" set \"MODE=live\"\r\n" +
                "if /i not \"%MODE%\"==\"rollback\" set \"MODE=live\"\r\nset \"DELAY=%~2\"\r\nif \"%DELAY%\"==\"\" set \"DELAY=2\"\r\n" +
                "set \"GALAXY_NET_HOST=%PORT%\"\r\nset \"GALAXY_NET_DELAY=%DELAY%\"\r\n" +
                "if /i \"%MODE%\"==\"live\" set \"GALAXY_NET_LIVE=1\"\r\nif /i \"%MODE%\"==\"rollback\" set \"GALAXY_NET_ROLLBACK=1\"\r\n" +
                "echo Hosting on TCP port %PORT% (%MODE%). Give your friend this PC's address: on the same network one of these,\r\n" +
                "echo from elsewhere your public address (the router forwards TCP port %PORT% to this PC).\r\n" +
                "for /f \"tokens=2 delims=:\" %%a in ('ipconfig ^| findstr /c:\"IPv4\"') do echo    %%a\r\n", "host", false);
            Write(layout, "Join online game.cmd", "Online co-op, join (you play Luigi, in the host's save: it comes from the host at the start\r\n" +
                "rem and is kept in data-join\\Nebula\\netplay; your own saves are not touched).\r\n" +
                "rem Change settings and mods before joining: in an online session the settings and HOME menus are locked.\r\n" +
                "rem Usage: \"Join online game.cmd\" <host address>[:port]", null,
                "set \"HOST=%~1\"\r\nif \"%HOST%\"==\"\" set /p \"HOST=Host address (name or IP, optionally :port): \"\r\n" +
                "if \"%HOST%\"==\"\" exit /b 1\r\necho %HOST% | find \":\" >nul || set \"HOST=%HOST%:7777\"\r\n" +
                "set \"GALAXY_NET_JOIN=%HOST%\"\r\n", "join", true);
            WriteTool(layout, "Check for updates.cmd", "Downloads and installs the newest Nebula Plus. Saves and settings are not touched.", "update");
            WriteTool(layout, "Nebula Plus.cmd", "Opens the Nebula Plus window: install, update, roll back, mods and saves.", "");
        }

        private static void Write(PlusLayout layout, string name, string about, string sharedSaves, string extra, string logPrefix, bool joiner)
        {
            var text = new StringBuilder();
            text.Append("@echo off\r\nrem Nebula Plus: ").Append(about).Append("\r\n");
            text.Append("rem Written by Nebula Plus (rewritten at each update); put your own GALAXY_* settings in data\\launch-env.cmd.\r\n");
            text.Append("setlocal\r\ncall \"%~dp0current.cmd\"\r\n");
            text.Append("if not exist \"%PLUS_APP%\\NebulaNative.exe\" goto :missing\r\n");
            if (joiner)
            {
                // The joiner keeps its own settings and caches (data-join), with the same mods as data\ (mirrored
                // each start: both PCs must load the same mods) and its own, untouched saves folder.
                text.Append("set \"PLUS_DATA=%~dp0data-join\"\r\n");
                text.Append("robocopy \"%~dp0data\\Nebula\\mods\" \"%PLUS_DATA%\\Nebula\\mods\" /MIR /COPY:DAT /NJH /NJS /NFL /NDL /NP >nul\r\n");
                text.Append("if exist \"%~dp0data\\Nebula\\mods.ini\" copy /y \"%~dp0data\\Nebula\\mods.ini\" \"%PLUS_DATA%\\Nebula\\mods.ini\" >nul\r\n");
                text.Append("set \"PLUS_SAVES=%PLUS_DATA%\\saves\"\r\n");
            }
            else
            {
                text.Append("set \"PLUS_DATA=%~dp0data\"\r\n");
                text.Append(sharedSaves != null
                    ? "rem Shared saves: the official Nebula's own folder, used in place (Nebula Plus window > Saves).\r\nset \"PLUS_SAVES=" + sharedSaves + "\"\r\n"
                    : "set \"PLUS_SAVES=%PLUS_DATA%\\saves\"\r\n");
            }
            text.Append("set \"LOCALAPPDATA=%PLUS_DATA%\"\r\n");
            text.Append(extra);
            text.Append("if exist \"%~dp0data\\launch-env.cmd\" call \"%~dp0data\\launch-env.cmd\"\r\n");
            text.Append("for %%d in (\"%PLUS_SAVES%\" \"%~dp0data\\logs\" \"%PLUS_DATA%\\Nebula\") do if not exist \"%%~d\" mkdir \"%%~d\"\r\n");
            text.Append("cd /d \"%PLUS_APP%\"\r\n");
            text.Append("for /f %%i in ('powershell -NoProfile -Command \"Get-Date -Format yyyyMMdd-HHmmss\"') do set \"STAMP=%%i\"\r\n");
            text.Append("if \"%STAMP%\"==\"\" set \"STAMP=latest\"\r\n");
            text.Append("set \"LOG=%~dp0data\\logs\\").Append(logPrefix).Append("-%STAMP%.log\"\r\n");
            text.Append("\"%PLUS_APP%\\NebulaNative.exe\" --pak \"%~dp0content\\game.pak\" --nand \"%PLUS_SAVES%\" 1> \"%~dp0data\\logs\\")
                .Append(logPrefix).Append("-out-%STAMP%.log\" 2> \"%LOG%\"\r\n");
            text.Append("if errorlevel 1 goto :failed\r\nexit /b 0\r\n:failed\r\necho The game stopped with an error. Its log: \"%LOG%\"\r\npause\r\nexit /b 1\r\n");
            text.Append(":missing\r\necho Nebula Plus is not installed in \"%~dp0\". Run \"Nebula Plus.cmd\" or NebulaPlus.exe.\r\npause\r\nexit /b 1\r\n");
            FileUtil.WriteAllTextAtomic(Path.Combine(layout.Root, name), text.ToString());
        }

        private static void WriteTool(PlusLayout layout, string name, string about, string command)
        {
            string text = "@echo off\r\nrem Nebula Plus: " + about + "\r\nrem Written by Nebula Plus (rewritten at each update).\r\n" +
                "setlocal\r\ncall \"%~dp0current.cmd\"\r\n" +
                "if not exist \"%PLUS_APP%\\NebulaPlus.exe\" goto :missing\r\n" +
                "start \"\" \"%PLUS_APP%\\NebulaPlus.exe\" --root \"%~dp0.\"" + (command.Length != 0 ? " --" + command : "") + "\r\n" +
                "exit /b 0\r\n:missing\r\necho Nebula Plus is not installed in \"%~dp0\".\r\npause\r\nexit /b 1\r\n";
            FileUtil.WriteAllTextAtomic(Path.Combine(layout.Root, name), text);
        }
    }
}
