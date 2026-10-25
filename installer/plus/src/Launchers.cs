using System;
using System.IO;
using System.Text;

namespace Nebula.Plus
{
    /// <summary>
    /// The .cmd launchers in the Plus folder (batch files, since PowerShell scripts may be blocked). Each points the
    /// runtime at Plus's own content, saves, settings, shader cache, crash dumps and netplay cache, and logs to
    /// data\logs. The installed version comes from current.cmd, which an update rewrites last, in one step.
    /// </summary>
    internal static class Launchers
    {
        public static void WriteCurrent(PlusLayout layout, string versionDir, string version, string commit, string buildId)
        {
            var text = new StringBuilder();
            text.Append("@rem Written by Nebula Plus at each install or update: the version the launchers start.\r\n");
            text.Append("set \"PLUS_VERSION=").Append(version).Append("\"\r\n");
            text.Append("set \"PLUS_COMMIT=").Append(commit).Append("\"\r\n");
            text.Append("set \"PLUS_APP=%~dp0versions\\").Append(versionDir).Append("\"\r\n");
            // Online play matches builds by content, not by file times: two PCs that built the same source match.
            text.Append("set \"GALAXY_NET_BUILD_ID=").Append(buildId).Append("\"\r\n");
            FileUtil.WriteAllTextAtomic(layout.CurrentCmd, text.ToString());
        }

        public static void WriteAll(PlusLayout layout, PlusConfig config)
        {
            string saves = config.SavesMode == "shared" ? OfficialNebula.Saves : null;
            Write(layout, "Play Nebula Plus.cmd", "Play. Co-op (Multiplayer) and the Odyssey moves are switched on the save-select screen;\r\n" +
                "rem the model, animations and voice in F1 > General.", saves, "nebula-plus", "shadercache", "", "run");
            Write(layout, "Host online game.cmd", "Online co-op, host (Mario). Your friend runs \"Join online game.cmd\" with this PC's address.\r\n" +
                "rem Usage: \"Host online game.cmd\" [port, default 7777 UDP] [input delay in frames, default 3]",
                saves, "nebula-plus-host", "shadercache-host",
                "set \"PORT=%~1\"\r\nif \"%PORT%\"==\"\" set \"PORT=7777\"\r\nset \"DELAY=%~2\"\r\nif \"%DELAY%\"==\"\" set \"DELAY=3\"\r\n" +
                "set \"GALAXY_NET_HOST=%PORT%\"\r\nset \"GALAXY_NET_DELAY=%DELAY%\"\r\n" +
                "echo Hosting on UDP port %PORT%. The game waits at the start until your friend connects.\r\n", "host");
            Write(layout, "Join online game.cmd", "Online co-op, join (Luigi). Plays the host's save, downloaded into data\\netplay (your saves are not touched).\r\n" +
                "rem Usage: \"Join online game.cmd\" <host address>[:port] [input delay, default 3, the same as the host's]",
                saves, "nebula-plus-join", "shadercache-join",
                "set \"HOST=%~1\"\r\nif \"%HOST%\"==\"\" set /p \"HOST=Host address (name or IP, optionally :port): \"\r\n" +
                "if \"%HOST%\"==\"\" exit /b 1\r\necho %HOST% | find \":\" >nul || set \"HOST=%HOST%:7777\"\r\n" +
                "set \"DELAY=%~2\"\r\nif \"%DELAY%\"==\"\" set \"DELAY=3\"\r\n" +
                "set \"GALAXY_NET_JOIN=%HOST%\"\r\nset \"GALAXY_NET_DELAY=%DELAY%\"\r\n", "join");
            WriteTool(layout, "Check for updates.cmd", "Downloads and installs the newest Nebula Plus build. Saves and settings are not touched.", "update");
            WriteTool(layout, "Nebula Plus.cmd", "Opens the Nebula Plus window: play, update, roll back, mods and saves.", "");
        }

        private static void Write(PlusLayout layout, string name, string about, string sharedSaves, string lease, string cache,
            string extra, string logPrefix)
        {
            var text = new StringBuilder();
            text.Append("@echo off\r\nrem Nebula Plus: ").Append(about).Append("\r\n");
            text.Append("rem Written by Nebula Plus (regenerated at each update); put your own GALAXY_* settings in data\\launch-env.cmd.\r\n");
            text.Append("setlocal\r\ncall \"%~dp0current.cmd\"\r\n");
            text.Append("if not exist \"%PLUS_APP%\\NebulaRuntime.exe\" goto :missing\r\n");
            text.Append("set \"PLUS_DATA=%~dp0data\"\r\n");
            text.Append(sharedSaves != null
                ? "rem Shared saves: the official Nebula's own folder, used in place (Nebula Plus window > Saves).\r\nset \"GALAXY_NAND_ROOT=" + sharedSaves + "\"\r\n"
                : "set \"GALAXY_NAND_ROOT=%PLUS_DATA%\\saves\"\r\n");
            text.Append("set \"GALAXY_RUNTIME_SETTINGS_PATH=%PLUS_DATA%\\runtime_settings.ini\"\r\n");
            text.Append("set \"GALAXY_RENDER_CONFIG_PATH=%PLUS_DATA%\\config.ini\"\r\n");
            text.Append("set \"GALAXY_SHADER_CACHE_DIR=%PLUS_DATA%\\").Append(cache).Append("\"\r\n");
            text.Append("set \"GALAXY_CRASH_DUMP_DIR=%PLUS_DATA%\\crashdumps\"\r\n");
            text.Append("set \"GALAXY_NETPLAY_DIR=%PLUS_DATA%\\netplay\"\r\n");
            text.Append("set \"GALAXY_INSTANCE_LEASE=").Append(lease).Append("\"\r\n");
            text.Append(extra);
            text.Append("if exist \"%PLUS_DATA%\\launch-env.cmd\" call \"%PLUS_DATA%\\launch-env.cmd\"\r\n");
            text.Append("for %%d in (\"%GALAXY_NAND_ROOT%\" \"%PLUS_DATA%\\logs\" \"%GALAXY_CRASH_DUMP_DIR%\") do if not exist \"%%~d\" mkdir \"%%~d\"\r\n");
            text.Append("cd /d \"%PLUS_APP%\"\r\n");
            text.Append("for /f %%i in ('powershell -NoProfile -Command \"Get-Date -Format yyyyMMdd-HHmmss\"') do set \"STAMP=%%i\"\r\n");
            text.Append("if \"%STAMP%\"==\"\" set \"STAMP=latest\"\r\n");
            text.Append("set \"LOG=%PLUS_DATA%\\logs\\").Append(logPrefix).Append("-%STAMP%.log\"\r\n");
            text.Append("\"%PLUS_APP%\\NebulaRuntime.exe\" \"%~dp0content\" \"%PLUS_APP%\\RMGE01_game.dll\" 1> \"%PLUS_DATA%\\logs\\")
                .Append(logPrefix).Append("-out-%STAMP%.log\" 2> \"%LOG%\"\r\n");
            text.Append("if errorlevel 1 goto :failed\r\nexit /b 0\r\n:failed\r\necho The game stopped with an error. Its log: \"%LOG%\"\r\npause\r\nexit /b 1\r\n");
            text.Append(":missing\r\necho Nebula Plus is not installed in \"%~dp0\". Run \"Nebula Plus.cmd\" or NebulaPlus.exe.\r\npause\r\nexit /b 1\r\n");
            FileUtil.WriteAllTextAtomic(Path.Combine(layout.Root, name), text.ToString());
        }

        private static void WriteTool(PlusLayout layout, string name, string about, string command)
        {
            string text = "@echo off\r\nrem Nebula Plus: " + about + "\r\nrem Written by Nebula Plus (regenerated at each update).\r\n" +
                "setlocal\r\ncall \"%~dp0current.cmd\"\r\n" +
                "if not exist \"%PLUS_APP%\\NebulaPlus.exe\" goto :missing\r\n" +
                "start \"\" \"%PLUS_APP%\\NebulaPlus.exe\" --root \"%~dp0.\"" + (command.Length != 0 ? " --" + command : "") + "\r\nexit /b 0\r\n:missing\r\necho Nebula Plus is not installed in \"%~dp0\".\r\npause\r\nexit /b 1\r\n";
            FileUtil.WriteAllTextAtomic(Path.Combine(layout.Root, name), text);
        }
    }
}
