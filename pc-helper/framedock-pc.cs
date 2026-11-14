// Frame Dock PC helper (optional, Windows): tells Frame Dock on the Steam Frame the render size of the PC game being
// streamed to it. The headset only receives the video stream, so the size the game really renders at is known only to
// SteamVR on the PC. SteamVR writes it to its log when the game creates its eye textures ("vrlink: CreateSwapTextureSet:
// 3048x3048(..)" in Steam\logs\vrserver.txt, after the game's "StartSceneApplicationTransitionFromProcess( pid, ..."),
// and this helper reads that log, nothing else.
//
// It answers "GET /render" on TCP port 27463 with {"v":1,"app":"<app key>","width":W,"height":H}, or {"v":1,"app":null}
// while no streamed game runs (the game's process gone, or no size seen yet).
//
// "GET /v2/state" (Frame Dock 0.1.3 on) adds what SteamVR and the PC say about the stream's source, once a second while
// it is asked: the game's frame rate and its GPU / CPU frame times (steamvr.cs: SteamVR's frame timing, as fpsVR reads
// it), the PC's CPU and GPU load, and the link bandwidth SteamVR estimates ("Link BW: Est 350.0 Mbit/s" in the log).
// Its "last" sums up the stream session that ended last (session.cs: SteamVR's vrlink_pa_*.json, counters only), or
// is null. Values it doesn't have are null. Read-only: no other request, no other data. Only clients on the same
// private network are answered.
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
        static readonly Regex LinkBw = new Regex(@"Link BW: Est (\d+(?:\.\d+)?) Mbit/s", RegexOptions.Compiled);

        public int ScenePid { get; private set; }
        public string SceneKey { get; private set; }
        public int Width { get; private set; }
        public int Height { get; private set; }
        /// <summary>SteamVR's estimate of the link, Mbit/s (written every 30 s during a stream); 0 = none yet.</summary>
        public double LinkMbit { get; private set; }
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
            m = LinkBw.Match(line);
            if (m.Success)
            {
                double mbit;
                if (double.TryParse(m.Groups[1].Value, System.Globalization.NumberStyles.Float,
                                    System.Globalization.CultureInfo.InvariantCulture, out mbit)) LinkMbit = mbit;
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
            LinkMbit = 0;
        }
    }

    static class Program
    {
        const int DefaultPort = 27463;
        static int Port = DefaultPort;
        const string RunValue = "Frame Dock PC helper";

        [STAThread]
        static int Main(string[] args)
        {
            string arg = args.Length > 0 ? args[0].ToLowerInvariant() : "";
            if (arg == "--install") return Install();
            if (arg == "--uninstall") return Uninstall();
            if (arg == "--check") return Check();
            if (arg == "--self-test") return SelfTest();
            // For trying a build next to the installed one: FRAMEDOCK_PC_PORT=<port> serves there, as its own instance
            int port;
            if (int.TryParse(Environment.GetEnvironmentVariable("FRAMEDOCK_PC_PORT"), out port) && port > 1024 && port < 65536) Port = port;
            bool created;
            using (var single = new Mutex(true, @"Local\FrameDockPcHelper" + (Port == DefaultPort ? "" : "-" + Port), out created))
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

        /// <summary>What /v2/state answers with: the last second's readings (the helper's loop fills it).</summary>
        sealed class Readings
        {
            public readonly SteamVr Vr = new SteamVr();
            public readonly PcLoad Pc = new PcLoad();
            public FrameWindow Frame;
            public DateTime FrameAt = DateTime.MinValue;
            public DateTime AskedAt = DateTime.MinValue;  // the last /v2/state
            public bool PcSampled;
            public SessionWatch Sessions;  // Steam's log folder
        }

        /// <summary>
        /// Loopback, 10/8, 172.16/12, 192.168/16, link-local and 100.64/10 (carrier-grade NAT, which mesh VPNs use): the
        /// headset on the same network, no one else.
        /// </summary>
        public static bool PrivatePeer(IPAddress address)
        {
            if (address == null) return false;
            if (address.IsIPv4MappedToIPv6) address = address.MapToIPv4();
            if (IPAddress.IsLoopback(address)) return true;
            if (address.AddressFamily != AddressFamily.InterNetwork) return address.IsIPv6LinkLocal;
            byte[] b = address.GetAddressBytes();
            return b[0] == 10 || (b[0] == 172 && b[1] >= 16 && b[1] <= 31) || (b[0] == 192 && b[1] == 168) ||
                   (b[0] == 169 && b[1] == 254) || (b[0] == 100 && b[1] >= 64 && b[1] <= 127);
        }

        /// <summary>The /v2/state body. frame / pc / last may be null (not attached, nothing read yet, no session).</summary>
        public static string State(RenderLog log, bool vr, double displayHz, FrameWindow frame, double ageMs,
                                   double? cpuPct, double? gpuPct, double? gpuTempC, double? vramUsedMb, double? vramTotalMb,
                                   SessionSummary last)
        {
            var o = new StringBuilder("{\"v\":2,\"render\":");
            o.Append(Answer(log));
            o.Append(",\"vr\":").Append(vr ? "true" : "false");
            o.Append(",\"hz\":").Append(Json.Num(vr && displayHz > 0 ? displayHz : (double?)null, "0.0#"));
            bool have = vr && frame != null;
            // No new frame at all: the headset is idle (SteamVR keeps its old history), not a game at 0 fps
            double? missed = have && frame.Frames > 0 && displayHz > 0 ? Math.Max(0.0, Math.Min(100.0, 100.0 * (1.0 - frame.Fps / displayHz))) : (double?)null;
            o.Append(",\"fps\":").Append(Json.Num(have ? frame.Fps : (double?)null, "0.0"));
            o.Append(",\"missedPct\":").Append(Json.Num(missed, "0.0"));
            o.Append(",\"gpuMs\":").Append(Json.Num(have && frame.Frames > 0 ? frame.GpuMs : (double?)null, "0.00"));
            o.Append(",\"gpuMaxMs\":").Append(Json.Num(have && frame.Frames > 0 ? frame.GpuMaxMs : (double?)null, "0.00"));
            o.Append(",\"cpuMs\":").Append(Json.Num(have && frame.Frames > 0 ? frame.CpuMs : (double?)null, "0.00"));
            o.Append(",\"cpuMaxMs\":").Append(Json.Num(have && frame.Frames > 0 ? frame.CpuMaxMs : (double?)null, "0.00"));
            o.Append(",\"dropped\":").Append(Json.Num(have ? frame.Dropped : (double?)null, "0"));
            o.Append(",\"ageMs\":").Append(Json.Num(have ? ageMs : (double?)null, "0"));
            o.Append(",\"pcCpuPct\":").Append(Json.Num(cpuPct, "0.0"));
            o.Append(",\"pcGpuPct\":").Append(Json.Num(gpuPct, "0"));
            o.Append(",\"pcGpuTempC\":").Append(Json.Num(gpuTempC, "0"));
            o.Append(",\"pcVramUsedMb\":").Append(Json.Num(vramUsedMb, "0"));
            o.Append(",\"pcVramTotalMb\":").Append(Json.Num(vramTotalMb, "0"));
            o.Append(",\"linkMbit\":").Append(Json.Num(log.LinkMbit > 0 ? log.LinkMbit : (double?)null, "0.0"));
            o.Append(",\"last\":").Append(SessionSummary.ToJson(last));
            return o.Append('}').ToString();
        }

        static string State(RenderLog log, Readings r)
        {
            bool fresh = r.PcSampled && (DateTime.UtcNow - r.FrameAt).TotalSeconds < 5;
            // The last session, looked for now: the headset goes by which session an answer names (the one before its
            // stream, then the stream's own once it has ended), so an answer never names an older one than there is
            try { r.Sessions.Poll(); }
            catch (IOException) { } catch (UnauthorizedAccessException) { }
            catch (ArgumentException) { } catch (NotSupportedException) { }  // a Steam path that isn't one
            return State(log, r.Vr.Attached, r.Vr.DisplayHz, fresh ? r.Frame : null, (DateTime.UtcNow - r.FrameAt).TotalMilliseconds,
                         r.PcSampled ? r.Pc.CpuPct : null, r.PcSampled ? r.Pc.GpuPct : null, r.PcSampled ? r.Pc.GpuTempC : null,
                         r.PcSampled ? r.Pc.VramUsedMb : null, r.PcSampled ? r.Pc.VramTotalMb : null, r.Sessions.Last);
        }

        static void Serve()
        {
            var follower = new Follower(LogPath());
            var readings = new Readings();
            readings.Sessions = new SessionWatch(Path.GetDirectoryName(LogPath()));
            var listener = new TcpListener(IPAddress.Any, Port);
            try { listener.Start(); }
            catch (SocketException) { return; }  // the port is taken: nothing to serve with, and no error box at sign-in
            DateTime nextPoll = DateTime.MinValue;
            while (true)
            {
                // Asked in the last 5 s: SteamVR's timing and the PC's load are read once a second. Otherwise nothing is
                bool wanted = (DateTime.UtcNow - readings.AskedAt).TotalSeconds < 5;
                readings.Vr.Tick(wanted);
                if (DateTime.UtcNow >= nextPoll)
                {
                    try { follower.Poll(); }
                    catch (IOException) { } catch (UnauthorizedAccessException) { }
                    catch (ArgumentException) { } catch (NotSupportedException) { }
                    if (wanted)
                    {
                        readings.Pc.Sample();
                        readings.PcSampled = true;
                        FrameWindow frame = readings.Vr.Sample();
                        if (frame != null)
                        {
                            readings.Frame = frame;
                            readings.FrameAt = DateTime.UtcNow;
                        }
                    }
                    else
                    {
                        readings.PcSampled = false;  // the next first answer has no stale numbers
                        readings.Frame = null;
                        readings.Pc.Idle();
                    }
                    nextPoll = DateTime.UtcNow.AddSeconds(1);
                }
                if (!listener.Pending())
                {
                    Thread.Sleep(100);
                    continue;
                }
                using (TcpClient client = listener.AcceptTcpClient())
                {
                    try { Reply(client, follower.Log, readings); } catch (IOException) { } catch (SocketException) { }
                }
            }
        }

        static void Reply(TcpClient client, RenderLog log, Readings readings)
        {
            var peer = client.Client.RemoteEndPoint as IPEndPoint;
            if (peer == null || !PrivatePeer(peer.Address)) return;  // not from this network: closed unanswered
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
            bool state = head.StartsWith("GET /v2/state ") || head.StartsWith("GET /v2/state?");
            if (state) readings.AskedAt = DateTime.UtcNow;
            string body = render ? Answer(log) : state ? State(log, readings) : "not found";
            string status = render || state ? "200 OK" : "404 Not Found";
            string type = render || state ? "application/json" : "text/plain";
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
            log.Feed("Sun Nov 15 2026 02:26:12.496 [Info] - Link BW: Est 350.0 Mbit/s (auto: 0)");
            if (log.LinkMbit != 350.0) failures++;

            // Frame timing: 5 records 1/90 s apart (the 5th is left for the next read), then nothing new
            var data = new byte[FrameMath.TimingSize * 5];
            for (int i = 0; i < 5; ++i)
            {
                int t = i * FrameMath.TimingSize;
                BitConverter.GetBytes((uint)(100 + i)).CopyTo(data, t + 4);     // frame index
                BitConverter.GetBytes((uint)1).CopyTo(data, t + 8);             // presents
                BitConverter.GetBytes((uint)(i == 1 ? 2 : 0)).CopyTo(data, t + 16);  // dropped
                BitConverter.GetBytes(10.0 + i / 90.0).CopyTo(data, t + 24);    // system time
                BitConverter.GetBytes((float)(6.0 + i)).CopyTo(data, t + 40);   // total render GPU ms
                BitConverter.GetBytes(1.0f).CopyTo(data, t + 48);               // compositor CPU ms
                BitConverter.GetBytes(2.0f).CopyTo(data, t + 76);               // new poses ready
                BitConverter.GetBytes(9.0f).CopyTo(data, t + 80);               // new frame ready
            }
            uint last = 0;
            double lastTime = 0;
            FrameWindow w = FrameMath.Aggregate(data, 5, ref last, ref lastTime);
            if (w.Frames != 4 || last != 103 || w.Dropped != 2) failures++;
            if (Math.Abs(w.Fps - 90.0) > 0.01 || Math.Abs(w.GpuMs - 7.5) > 1e-6 || w.GpuMaxMs != 9.0) failures++;
            if (Math.Abs(w.CpuMs - 8.0) > 1e-6 || w.CpuMaxMs != 8.0) failures++;
            if (Math.Abs(lastTime - (10.0 + 3 / 90.0)) > 1e-9) failures++;
            w = FrameMath.Aggregate(data, 5, ref last, ref lastTime);
            if (w.Frames != 0 || w.Fps != 0 || last != 103) failures++;
            // One second later only two more frames came (a stall): 2 frames over the second since the last one, not 90
            for (int i = 0; i < 3; ++i)
            {
                int t = i * FrameMath.TimingSize;
                BitConverter.GetBytes((uint)(104 + i)).CopyTo(data, t + 4);
                BitConverter.GetBytes(10.0 + 3 / 90.0 + 1.0 - (1 - i) / 90.0).CopyTo(data, t + 24);
            }
            w = FrameMath.Aggregate(data, 3, ref last, ref lastTime);
            if (w.Frames != 2 || last != 105 || Math.Abs(w.Fps - 2.0) > 0.01) failures++;
            // A single record is the unfinished newest one: nothing yet
            BitConverter.GetBytes((uint)106).CopyTo(data, 4);
            w = FrameMath.Aggregate(data, 1, ref last, ref lastTime);
            if (w.Frames != 0 || last != 105) failures++;
            // Frame numbers from the start again (the compositor restarted): counted, not skipped for ever
            for (int i = 0; i < 3; ++i) BitConverter.GetBytes((uint)(1 + i)).CopyTo(data, i * FrameMath.TimingSize + 4);
            w = FrameMath.Aggregate(data, 3, ref last, ref lastTime);
            if (w.Frames != 2 || last != 2) failures++;

            // Who is answered
            if (!PrivatePeer(IPAddress.Parse("192.168.50.48")) || !PrivatePeer(IPAddress.Parse("10.1.2.3")) ||
                !PrivatePeer(IPAddress.Parse("172.31.0.1")) || !PrivatePeer(IPAddress.Loopback) ||
                !PrivatePeer(IPAddress.Parse("169.254.7.7"))) failures++;
            if (PrivatePeer(IPAddress.Parse("8.8.8.8")) || PrivatePeer(IPAddress.Parse("172.32.0.1")) ||
                PrivatePeer(IPAddress.Parse("192.169.1.1")) || PrivatePeer(IPAddress.Parse("2001:db8::1")) || PrivatePeer(null)) failures++;
            if (!PrivatePeer(IPAddress.Parse("::ffff:192.168.1.2")) || PrivatePeer(IPAddress.Parse("::ffff:8.8.8.8")) ||
                !PrivatePeer(IPAddress.Parse("100.100.1.1")) || PrivatePeer(IPAddress.Parse("100.128.0.1"))) failures++;

            // The answer: numbers with a point whatever the system's language (here one that writes 6,79), null for what
            // isn't known
            Thread.CurrentThread.CurrentCulture = new System.Globalization.CultureInfo("de-DE");
            var german = new RenderLog();
            german.Feed("Sun Nov 15 2026 02:26:12.496 [Info] - Link BW: Est 212.5 Mbit/s (auto: 1)");
            if (german.LinkMbit != 212.5) failures++;
            var one = new FrameWindow();
            one.Frames = 90; one.Fps = 45.0; one.GpuMs = 6.789; one.GpuMaxMs = 9.1; one.CpuMs = 8.0; one.CpuMaxMs = 21.7; one.Dropped = 3;
            string json = State(log, true, 90.0, one, 120, 12.34, 56, 61, 4096, 16384, null);
            string want = "{\"v\":2,\"render\":{\"v\":1,\"app\":null},\"vr\":true,\"hz\":90.0,\"fps\":45.0,\"missedPct\":50.0," +
                          "\"gpuMs\":6.79,\"gpuMaxMs\":9.10,\"cpuMs\":8.00,\"cpuMaxMs\":21.70,\"dropped\":3,\"ageMs\":120," +
                          "\"pcCpuPct\":12.3,\"pcGpuPct\":56,\"pcGpuTempC\":61,\"pcVramUsedMb\":4096,\"pcVramTotalMb\":16384,\"linkMbit\":350.0,\"last\":null}";
            if (json != want) failures++;
            if (!State(log, true, 90.0, new FrameWindow(), 0, null, null, null, null, null, null).Contains("\"fps\":0.0,\"missedPct\":null,")) failures++;
            json = State(new RenderLog(), false, 0, null, 0, null, null, null, null, null, null);
            if (json != "{\"v\":2,\"render\":{\"v\":1,\"app\":null},\"vr\":false,\"hz\":null,\"fps\":null,\"missedPct\":null,\"gpuMs\":null," +
                        "\"gpuMaxMs\":null,\"cpuMs\":null,\"cpuMaxMs\":null,\"dropped\":null,\"ageMs\":null,\"pcCpuPct\":null,\"pcGpuPct\":null," +
                        "\"pcGpuTempC\":null,\"pcVramUsedMb\":null,\"pcVramTotalMb\":null,\"linkMbit\":null,\"last\":null}") failures++;

            // A stream session's file, written the way SteamVR writes it (version 1, cut down to a few causes). Of 1000
            // frames the viewer saw 30 + 10 + 4 + 6 late or not at all and the PC had none to send for 77. The causes
            // are those of the four link outcomes only, summed: DecodeSlow 23, ClientRxLoss 15, NetworkSlow 15 (a tie:
            // by name), RetransmitRequested 6, AppSlow 5. Not the top-level "causes", which also counts good frames
            string session =
                "{\n   \"cause_by_outcome\" : {\n" +
                "      \"DegateSkipped\" : {\n         \"AppSlow\" : 12,\n         \"DecodeSlow\" : 0\n      },\n" +
                "      \"DisplayedLate\" : {\n         \"AppSlow\" : 5,\n         \"ClientRxLoss\" : 9,\n         \"DecodeSlow\" : 20,\n" +
                "         \"Holdoff\" : 0,\n         \"NetworkSlow\" : 8\n      },\n" +
                "      \"Mispredicted\" : {\n         \"DecodeSlow\" : 20\n      },\n" +
                "      \"NotDecoded\" : {\n         \"ClientRxLoss\" : 3,\n         \"NetworkSlow\" : 1\n      },\n" +
                "      \"NotDisplayed\" : {\n         \"ClientRxLoss\" : 2,\n         \"DecodeSlow\" : 3,\n         \"NetworkSlow\" : 6\n      },\n" +
                "      \"NotEncoded\" : {\n         \"AppSlow\" : 70,\n         \"Holdoff\" : 7\n      },\n" +
                "      \"NotReceived\" : {\n         \"ClientRxLoss\" : 1,\n         \"RetransmitRequested\" : 6\n      }\n   },\n" +
                "   \"causes\" : {\n      \"AppSlow\" : 999,\n      \"DecodeSlow\" : 23\n   },\n" +
                "   \"frames\" : 1000,\n" +
                "   \"outcomes\" : {\n      \"DegateSkipped\" : 12,\n      \"DisplayedLate\" : 30,\n      \"Mispredicted\" : 31,\n" +
                "      \"NotDecoded\" : 4,\n      \"NotDisplayed\" : 10,\n      \"NotEncoded\" : 77,\n      \"NotReceived\" : 6\n   },\n" +
                "   \"version\" : 1\n}\n";
            SessionSummary sum = SessionSummary.Parse(session, 1794680861);
            if (sum == null) failures++;
            else
            {
                if (sum.Frames != 1000 || sum.EndedUnix != 1794680861) failures++;
                if (Math.Abs(sum.LinkBadPct - 5.0) > 1e-9 || Math.Abs(sum.PcMissedPct - 7.7) > 1e-9) failures++;
                if (string.Join(",", sum.Causes.ToArray()) != "DecodeSlow,ClientRxLoss,NetworkSlow") failures++;
                // (the culture is still the one that writes 7,70)
                string lastJson = "{\"endedUnix\":1794680861,\"frames\":1000,\"linkBadPct\":5.00,\"pcMissedPct\":7.70," +
                                  "\"causes\":[\"DecodeSlow\",\"ClientRxLoss\",\"NetworkSlow\"]}";
                if (SessionSummary.ToJson(sum) != lastJson) failures++;
                if (!State(new RenderLog(), false, 0, null, 0, null, null, null, null, null, sum).EndsWith(",\"linkMbit\":null,\"last\":" + lastJson + "}")) failures++;
            }
            // Another version, no frames, a file caught half written, not JSON at all: no summary
            if (SessionSummary.Parse(session.Replace("\"version\" : 1", "\"version\" : 2"), 1) != null) failures++;
            if (SessionSummary.Parse(session.Replace("\"frames\" : 1000", "\"frames\" : 0"), 1) != null) failures++;
            if (SessionSummary.Parse(session.Substring(0, session.Length / 2), 1) != null) failures++;
            if (SessionSummary.Parse("", 1) != null || SessionSummary.Parse("[1]", 1) != null || SessionSummary.Parse("{\"version\":1}", 1) != null) failures++;
            if (SessionSummary.Parse("{\"version\":\"1\",\"frames\":10,\"outcomes\":{}}", 1) != null) failures++;
            // A clean session: no causes. Fewer than three; a name that isn't one, a count that isn't a number: left out
            sum = SessionSummary.Parse("{\"version\":1,\"frames\":90,\"outcomes\":{\"NotEncoded\":9}}", 5);
            if (sum == null || sum.LinkBadPct != 0 || Math.Abs(sum.PcMissedPct - 10.0) > 1e-9 || sum.Causes.Count != 0) failures++;
            if (sum != null && SessionSummary.ToJson(sum) != "{\"endedUnix\":5,\"frames\":90,\"linkBadPct\":0.00,\"pcMissedPct\":10.00,\"causes\":[]}") failures++;
            sum = SessionSummary.Parse("{\"version\":1,\"frames\":100,\"outcomes\":{\"NotReceived\":2,\"DisplayedLate\":\"x\"}," +
                                       "\"cause_by_outcome\":{\"NotReceived\":{\"PingSpike\":2,\"<b>\\u0041\":9,\"WgpGap\":null,\"HostRxLoss\":-1}," +
                                       "\"DisplayedLate\":[1,2]}}", 5);
            if (sum == null || Math.Abs(sum.LinkBadPct - 2.0) > 1e-9 || string.Join(",", sum.Causes.ToArray()) != "PingSpike") failures++;
            return failures;
        }
    }
}
