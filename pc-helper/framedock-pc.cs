// Frame Dock PC helper (optional, Windows): tells Frame Dock on the Steam Frame the render size of the PC game being
// streamed to it. The headset only receives the video stream, so the size the game really renders at is known only to
// SteamVR on the PC. SteamVR writes it to its log when the game creates its eye textures ("vrlink: CreateSwapTextureSet:
// 3048x3048(..)" in Steam\logs\vrserver.txt, after the game's "StartSceneApplicationTransitionFromProcess( pid, ..."),
// and this helper reads that log, nothing else.
//
// It answers "GET /render" on TCP port 27463 with {"v":1,"app":"<app key>","width":W,"height":H}, or {"v":1,"app":null}
// while no streamed game runs (the game's process gone, or no size seen yet). Read-only: no other request, no other data.
//
//   framedock-pc.exe --install     copy to %LOCALAPPDATA%\FrameDock, start at sign-in (HKCU Run), start now
//   framedock-pc.exe --uninstall   stop it, remove the sign-in entry and the copy
//   framedock-pc.exe --check       show what it would answer now (a message box), for a quick test
//   framedock-pc.exe               run (a windowless background program, one at a time)
//
// Built with the C# compiler every Windows 10/11 has (.NET Framework 4.8): pc-helper\build.ps1. C# 5, no packages.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using Microsoft.Win32;

namespace FrameDockPc
{
    /// <summary>What vrserver.txt says about the streamed game, line by line.</summary>
    public sealed class RenderLog
    {
        static readonly Regex SceneStart = new Regex(
            @"StartSceneApplicationTransitionFromProcess\(\s*(\d+)\s*,\s*[^,]*,\s*([^\s,)]+)", RegexOptions.Compiled);
        static readonly Regex StreamViews = new Regex(@"vrlink: Stream views (\d+)x(\d+)", RegexOptions.Compiled);
        static readonly Regex SwapSet = new Regex(@"vrlink: CreateSwapTextureSet: (\d+)x(\d+)\(", RegexOptions.Compiled);

        public int ScenePid { get; private set; }
        public string SceneKey { get; private set; }
        public int Width { get; private set; }
        public int Height { get; private set; }
        int streamView;

        /// <summary>Takes one log line.</summary>
        public void Feed(string line)
        {
            Match m = SceneStart.Match(line);
            if (m.Success)
            {
                int pid;
                if (!int.TryParse(m.Groups[1].Value, out pid)) return;
                ScenePid = pid;
                SceneKey = m.Groups[2].Value;
                Width = Height = 0;  // the new game's textures come next
                return;
            }
            m = StreamViews.Match(line);
            if (m.Success)
            {
                int.TryParse(m.Groups[1].Value, out streamView);
                return;
            }
            m = SwapSet.Match(line);
            if (m.Success && ScenePid != 0)
            {
                int w, h;
                if (!int.TryParse(m.Groups[1].Value, out w) || !int.TryParse(m.Groups[2].Value, out h)) return;
                // Not the game's: SteamVR's tiny placeholders and the stream's own views
                if (w < 256 || h < 256 || w == streamView) return;
                Width = w;
                Height = h;  // the latest one: a game that changes its resolution makes new textures
            }
        }

        /// <summary>A log that starts again (SteamVR restarted): nothing is known.</summary>
        public void Reset()
        {
            ScenePid = 0;
            SceneKey = null;
            Width = Height = streamView = 0;
        }
    }

    static class Program
    {
        const int Port = 27463;
        const string RunValue = "Frame Dock PC helper";

        [STAThread]
        static int Main(string[] args)
        {
            string arg = args.Length > 0 ? args[0].ToLowerInvariant() : "";
            if (arg == "--install") return Install();
            if (arg == "--uninstall") return Uninstall();
            if (arg == "--check") return Check();
            if (arg == "--self-test") return SelfTest();
            bool created;
            using (var single = new Mutex(true, @"Local\FrameDockPcHelper", out created))
            {
                if (!created) return 0;  // already running
                Serve();
            }
            return 0;
        }

        static string LogPath()
        {
            string steam = Registry.GetValue(@"HKEY_CURRENT_USER\Software\Valve\Steam", "SteamPath", null) as string;
            if (string.IsNullOrEmpty(steam)) steam = @"C:\Program Files (x86)\Steam";
            return Path.Combine(steam.Replace('/', '\\'), "logs", "vrserver.txt");
        }

        /// <summary>Follows vrserver.txt: new lines as they come, from the start again when it is rewritten.</summary>
        sealed class Follower
        {
            readonly string path;
            long offset;
            DateTime created;
            string partial = "";
            public readonly RenderLog Log = new RenderLog();

            public Follower(string path) { this.path = path; }

            public void Poll()
            {
                FileInfo info = new FileInfo(path);
                if (!info.Exists) return;
                if (info.CreationTimeUtc != created || info.Length < offset)
                {
                    created = info.CreationTimeUtc;
                    offset = 0;
                    partial = "";
                    Log.Reset();
                }
                if (info.Length == offset) return;
                using (var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
                {
                    stream.Seek(offset, SeekOrigin.Begin);
                    var buffer = new byte[64 * 1024];
                    int read;
                    var text = new StringBuilder(partial);
                    while ((read = stream.Read(buffer, 0, buffer.Length)) > 0)
                    {
                        text.Append(Encoding.UTF8.GetString(buffer, 0, read));
                        offset += read;
                    }
                    string all = text.ToString();
                    int last = all.LastIndexOf('\n');
                    partial = last < 0 ? all : all.Substring(last + 1);
                    if (last < 0) return;
                    foreach (string line in all.Substring(0, last).Split('\n')) Log.Feed(line);
                }
            }
        }

        static bool Alive(int pid)
        {
            if (pid == 0) return false;
            try
            {
                using (Process p = Process.GetProcessById(pid)) return !p.HasExited;
            }
            catch (ArgumentException) { return false; }
            catch (InvalidOperationException) { return false; }
            catch (System.ComponentModel.Win32Exception) { return true; }  // there, but not ours to look into
        }

        static string Answer(RenderLog log)
        {
            if (log.Width == 0 || !Alive(log.ScenePid)) return "{\"v\":1,\"app\":null}";
            string key = log.SceneKey ?? "";
            var safe = new StringBuilder();
            foreach (char c in key) if (c >= 0x20 && c != '"' && c != '\\' && c < 0x7f) safe.Append(c);
            return "{\"v\":1,\"app\":\"" + safe + "\",\"width\":" + log.Width + ",\"height\":" + log.Height + "}";
        }

        static void Serve()
        {
            var follower = new Follower(LogPath());
            var listener = new TcpListener(IPAddress.Any, Port);
            try { listener.Start(); }
            catch (SocketException) { return; }  // the port is taken: nothing to serve with, and no error box at sign-in
            DateTime nextPoll = DateTime.MinValue;
            while (true)
            {
                if (DateTime.UtcNow >= nextPoll)
                {
                    try { follower.Poll(); } catch (IOException) { } catch (UnauthorizedAccessException) { }
                    nextPoll = DateTime.UtcNow.AddSeconds(1);
                }
                if (!listener.Pending())
                {
                    Thread.Sleep(100);
                    continue;
                }
                using (TcpClient client = listener.AcceptTcpClient())
                {
                    try { Reply(client, follower.Log); } catch (IOException) { } catch (SocketException) { }
                }
            }
        }

        static void Reply(TcpClient client, RenderLog log)
        {
            client.ReceiveTimeout = 1000;
            client.SendTimeout = 1000;
            NetworkStream stream = client.GetStream();
            var request = new byte[1024];
            int got = 0;
            // Only the request line matters: read until the end of the headers, 1 KB or 2 s in all (one slow client
            // must not hold up the others: this serves one connection at a time)
            DateTime deadline = DateTime.UtcNow.AddSeconds(2);
            while (got < request.Length && DateTime.UtcNow < deadline)
            {
                int n = stream.Read(request, got, request.Length - got);
                if (n <= 0) break;
                got += n;
                if (Encoding.ASCII.GetString(request, 0, got).Contains("\r\n\r\n")) break;
            }
            string head = Encoding.ASCII.GetString(request, 0, got);
            bool render = head.StartsWith("GET /render ") || head.StartsWith("GET /render?");
            string body = render ? Answer(log) : "not found";
            string status = render ? "200 OK" : "404 Not Found";
            string type = render ? "application/json" : "text/plain";
            byte[] bytes = Encoding.UTF8.GetBytes(body);
            byte[] header = Encoding.ASCII.GetBytes("HTTP/1.0 " + status + "\r\nContent-Type: " + type + "\r\nContent-Length: " +
                                                    bytes.Length + "\r\nConnection: close\r\n\r\n");
            stream.Write(header, 0, header.Length);
            stream.Write(bytes, 0, bytes.Length);
        }

        static string InstallPath()
        {
            return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "FrameDock",
                                "framedock-pc.exe");
        }

        static void StopRunning()
        {
            string target = InstallPath();
            foreach (Process p in Process.GetProcessesByName("framedock-pc"))
            {
                try
                {
                    if (p.Id != Process.GetCurrentProcess().Id &&
                        string.Equals(p.MainModule.FileName, target, StringComparison.OrdinalIgnoreCase))
                    {
                        p.Kill();
                        p.WaitForExit(3000);
                    }
                }
                catch (Exception) { }
                finally { p.Dispose(); }
            }
        }

        static int Install()
        {
            string target = InstallPath();
            string self = Process.GetCurrentProcess().MainModule.FileName;
            StopRunning();
            Directory.CreateDirectory(Path.GetDirectoryName(target));
            if (!string.Equals(self, target, StringComparison.OrdinalIgnoreCase)) File.Copy(self, target, true);
            using (RegistryKey run = Registry.CurrentUser.CreateSubKey(@"Software\Microsoft\Windows\CurrentVersion\Run"))
                run.SetValue(RunValue, "\"" + target + "\"");
            Process.Start(target);
            System.Windows.Forms.MessageBox.Show(
                "Frame Dock PC helper is installed and running.\n\nIt starts when you sign in. If Windows asks, allow it on " +
                "private networks so the headset can reach it.\n\nRemove it with: framedock-pc.exe --uninstall",
                "Frame Dock PC helper");
            return 0;
        }

        static int Uninstall()
        {
            StopRunning();
            using (RegistryKey run = Registry.CurrentUser.OpenSubKey(@"Software\Microsoft\Windows\CurrentVersion\Run", true))
                if (run != null) run.DeleteValue(RunValue, false);
            string target = InstallPath();
            string self = Process.GetCurrentProcess().MainModule.FileName;
            if (File.Exists(target) && !string.Equals(self, target, StringComparison.OrdinalIgnoreCase)) File.Delete(target);
            System.Windows.Forms.MessageBox.Show(
                string.Equals(self, target, StringComparison.OrdinalIgnoreCase)
                    ? "Frame Dock PC helper is stopped and won't start at sign-in. Delete " + target + " to remove the file."
                    : "Frame Dock PC helper is removed.",
                "Frame Dock PC helper");
            return 0;
        }

        static int Check()
        {
            var follower = new Follower(LogPath());
            follower.Poll();
            System.Windows.Forms.MessageBox.Show("Log: " + LogPath() + "\n\nAnswer now: " + Answer(follower.Log),
                                                 "Frame Dock PC helper");
            return 0;
        }

        /// <summary>The parser against lines like SteamVR's (exit code 0 when all pass).</summary>
        static int SelfTest()
        {
            var log = new RenderLog();
            string[] lines = {
                "Thu Nov 12 2026 20:53:58.736 [Info] - vrlink: CreateSwapTextureSet: 2472x2472(29) 1",
                "Thu Nov 12 2026 20:53:58.737 [Info] - vrlink: Stream views 1536x1536 (target square view size 1536, aspect ratio 1.00), atlas 1536x6144, mode 0",
                "Thu Nov 12 2026 20:53:58.768 [Info] - vrlink: CreateSwapTextureSet: 1536x1536(29) 1",
                "Thu Nov 12 2026 20:54:58.379 [Info] - StartSceneApplicationTransitionFromProcess( 37388, E:\\SteamLibrary\\steamapps\\common\\Titanfall2\\Titanfall2VRDevLauncher-f70da0e0356f.exe, system.generated.openxr.titanfall2vr.titanfall2vrdevlauncher-f70da0e0356f.exe )",
                "Thu Nov 12 2026 20:54:58.926 [Info] - vrlink: CreateSwapTextureSet: 3048x3048(27) 9",
                "Thu Nov 12 2026 21:05:11.047 [Info] - vrlink: CreateSwapTextureSet: 16x16(29) 1",
            };
            int failures = 0;
            log.Feed(lines[0]);
            if (log.Width != 0) failures++;  // before any game: SteamVR's own
            log.Feed(lines[1]);
            log.Feed(lines[2]);
            log.Feed(lines[3]);
            if (log.ScenePid != 37388 || log.SceneKey != "system.generated.openxr.titanfall2vr.titanfall2vrdevlauncher-f70da0e0356f.exe") failures++;
            log.Feed(lines[4]);
            if (log.Width != 3048 || log.Height != 3048) failures++;
            log.Feed(lines[5]);
            if (log.Width != 3048) failures++;  // placeholders don't count
            log.Feed(lines[2]);
            if (log.Width != 3048) failures++;  // nor the stream's views
            return failures;
        }
    }
}
