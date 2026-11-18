// Frame Dock PC helper: SteamVR's frame timing and the PC's own load, for "GET /v2/state".
//
// SteamVR: attached as a background application (it never starts SteamVR and doesn't keep it open) through
// openvr_api.dll from the installed runtime (%LOCALAPPDATA%\openvr\openvrpaths.vrpath), with only the few entries
// this needs, declared by hand: VR_InitInternal2 / VR_ShutdownInternal / VR_GetGenericInterface, and from the C
// function tables IVRCompositor_029's GetFrameTimings, IVRSystem_026's GetFloatTrackedDeviceProperty and
// PollNextEvent (their places in the tables are the methods' order in openvr.h 2.15.6). Checked 2026-11-15 against
// fpsVR during a stream: GPU 6.6-7.3 ms here, 6.5 there; CPU about 8 ms here, 7.3 there (this adds the compositor's
// CPU time, as Frame Dock does on the headset).
//
// Only while the headset asks: attached on the first /v2/state, released a minute after the last one, and at once
// when SteamVR says it quits.
using System;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Text.RegularExpressions;

namespace FrameDockPc
{
    /// <summary>One second of frames, counted like Frame Dock counts them on the headset.</summary>
    public sealed class FrameWindow
    {
        public int Frames;        // new frames in the window
        public double Fps;        // frames shown per second
        public double GpuMs;      // mean m_flTotalRenderGpuMs
        public double GpuMaxMs;
        public double CpuMs;      // mean app CPU (poses ready to second Submit) + compositor CPU
        public double CpuMaxMs;
        public int Dropped;       // sum of m_nNumDroppedFrames
    }

    /// <summary>Compositor_FrameTiming as bytes (Windows layout, 192 bytes with m_flTransferLatencyMs).</summary>
    public static class FrameMath
    {
        public const int TimingSize = 192;
        public const int MaxTimings = 128;
        const int FrameIndex = 4, Presents = 8, DroppedFrames = 16, SystemTime = 24, TotalRenderGpuMs = 40,
                  CompositorRenderCpuMs = 48, NewPosesReadyMs = 76, NewFrameReadyMs = 80;

        /// <summary>
        /// The frames newer than lastIndex, oldest first in data. The newest record is left for the next call (its
        /// presents aren't final yet). The host of a stream reports every frame as presented once, so "reprojected"
        /// can't be told here: what was missed is the display rate minus Fps.
        /// lastTime: the time of the last frame counted before (0 = none), so a stall between two reads lowers Fps.
        /// </summary>
        public static FrameWindow Aggregate(byte[] data, int count, ref uint lastIndex, ref double lastTime)
        {
            var w = new FrameWindow();
            int shown = 0;
            double first = 0, end = 0, gpu = 0, cpu = 0;
            // Frame numbers that start again below the last one (the compositor restarted): everything is new
            uint highest = 0;
            for (int i = 0; i < count; ++i) highest = Math.Max(highest, BitConverter.ToUInt32(data, i * TimingSize + FrameIndex));
            if (count > 0 && highest < lastIndex)
            {
                lastIndex = 0;
                lastTime = 0;
            }
            uint newest = lastIndex;
            for (int i = 0; i + 1 < count; ++i)
            {
                int t = i * TimingSize;
                uint index = BitConverter.ToUInt32(data, t + FrameIndex);
                if (lastIndex != 0 && index <= lastIndex) continue;
                ++w.Frames;
                w.Dropped += (int)BitConverter.ToUInt32(data, t + DroppedFrames);
                double g = BitConverter.ToSingle(data, t + TotalRenderGpuMs);
                double c = Math.Max(0.0, BitConverter.ToSingle(data, t + NewFrameReadyMs) -
                                             BitConverter.ToSingle(data, t + NewPosesReadyMs)) +
                           BitConverter.ToSingle(data, t + CompositorRenderCpuMs);
                gpu += g;
                cpu += c;
                w.GpuMaxMs = Math.Max(w.GpuMaxMs, g);
                w.CpuMaxMs = Math.Max(w.CpuMaxMs, c);
                if (BitConverter.ToUInt32(data, t + Presents) > 0)
                {
                    double time = BitConverter.ToDouble(data, t + SystemTime);
                    if (shown == 0) first = time;
                    end = time;
                    ++shown;
                }
                if (index > newest) newest = index;
            }
            lastIndex = newest;
            if (w.Frames > 0)
            {
                w.GpuMs = gpu / w.Frames;
                w.CpuMs = cpu / w.Frames;
            }
            // From the last frame counted before to the last one now; without one (or after a long gap), within the window
            if (shown > 0 && lastTime > 0 && end > lastTime && end - lastTime < 5.0) w.Fps = shown / (end - lastTime);
            else w.Fps = shown >= 2 && end > first ? (shown - 1) / (end - first) : 0;
            if (shown > 0) lastTime = end;
            return w;
        }
    }

    /// <summary>The connection to SteamVR. One thread uses it (the helper's loop).</summary>
    sealed class SteamVr
    {
        [DllImport("kernel32", SetLastError = true, CharSet = CharSet.Unicode)]
        static extern IntPtr LoadLibraryW(string path);
        [DllImport("kernel32", SetLastError = true, CharSet = CharSet.Ansi, BestFitMapping = false)]
        static extern IntPtr GetProcAddress(IntPtr module, string name);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        delegate uint InitInternal2(ref int error, int applicationType, [MarshalAs(UnmanagedType.LPStr)] string startupInfo);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        delegate void ShutdownInternal();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        delegate IntPtr GetGenericInterface([MarshalAs(UnmanagedType.LPStr)] string version, ref int error);
        [UnmanagedFunctionPointer(CallingConvention.StdCall)]
        delegate uint GetFrameTimingsFn(IntPtr timings, uint frames);
        [UnmanagedFunctionPointer(CallingConvention.StdCall)]
        [return: MarshalAs(UnmanagedType.I1)]
        delegate bool PollNextEventFn(IntPtr ev, uint size);
        [UnmanagedFunctionPointer(CallingConvention.StdCall)]
        delegate float GetFloatPropFn(uint device, int prop, ref int error);

        const int VRApplication_Background = 3;
        const int VREvent_Quit = 700;
        const int Prop_DisplayFrequency_Float = 2002;
        const int CompositorGetFrameTimings = 11;
        const int SystemGetFloatProp = 23;
        const int SystemPollNextEvent = 30;
        const int EventBytes = 128;  // more than sizeof(VREvent_t)

        IntPtr module;
        InitInternal2 init;
        ShutdownInternal shutdown;
        GetGenericInterface generic;
        GetFrameTimingsFn getTimings;
        PollNextEventFn pollEvent;
        GetFloatPropFn floatProp;
        IntPtr native = IntPtr.Zero;
        IntPtr ev = IntPtr.Zero;
        readonly byte[] managed = new byte[FrameMath.TimingSize * FrameMath.MaxTimings];
        uint lastIndex;
        double lastTime;
        bool primed;      // the history from before the attach was skipped
        int serverPid;    // the vrserver.exe this is attached to
        int emptyReads;   // Sample() calls in a row that got nothing
        DateTime notBefore = DateTime.MinValue;
        DateTime nextServerCheck = DateTime.MinValue;
        DateTime lastWanted = DateTime.MinValue;
        int samplesSinceHz;

        public bool Attached { get; private set; }
        public double DisplayHz { get; private set; }

        /// <summary>vrserver.exe's process id (0 = not running, or the process list couldn't be read).</summary>
        static int ServerPid()
        {
            try
            {
                Process[] found = Process.GetProcessesByName("vrserver");
                int pid = found.Length > 0 ? found[0].Id : 0;
                foreach (Process p in found) p.Dispose();
                return pid;
            }
            catch (InvalidOperationException) { return 0; }
            catch (System.ComponentModel.Win32Exception) { return 0; }
        }

        static string RuntimeDll()
        {
            string paths = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                                        @"openvr\openvrpaths.vrpath");
            Match m = Regex.Match(File.ReadAllText(paths), "\"runtime\"\\s*:\\s*\\[\\s*\"((?:[^\"\\\\]|\\\\.)*)\"");
            if (!m.Success) return null;
            return Path.Combine(Regex.Unescape(m.Groups[1].Value), @"bin\win64\openvr_api.dll");
        }

        T Export<T>(string name) where T : class
        {
            IntPtr p = GetProcAddress(module, name);
            return p == IntPtr.Zero ? null : (T)(object)Marshal.GetDelegateForFunctionPointer(p, typeof(T));
        }

        static T Slot<T>(IntPtr table, int index) where T : class
        {
            IntPtr p = Marshal.ReadIntPtr(table, index * IntPtr.Size);
            return p == IntPtr.Zero ? null : (T)(object)Marshal.GetDelegateForFunctionPointer(p, typeof(T));
        }

        bool Load()
        {
            if (module != IntPtr.Zero) return init != null;
            if (IntPtr.Size != 8) return false;  // the tables' layout here is the 64-bit one
            string dll;
            try { dll = RuntimeDll(); }
            catch (IOException) { return false; }
            catch (UnauthorizedAccessException) { return false; }
            catch (ArgumentException) { return false; }  // a path in the file that isn't one
            if (dll == null || !File.Exists(dll)) return false;
            module = LoadLibraryW(dll);
            if (module == IntPtr.Zero) return false;
            init = Export<InitInternal2>("VR_InitInternal2");
            shutdown = Export<ShutdownInternal>("VR_ShutdownInternal");
            generic = Export<GetGenericInterface>("VR_GetGenericInterface");
            if (init == null || shutdown == null || generic == null) init = null;
            return init != null;
        }

        void TryAttach(DateTime now)
        {
            notBefore = now.AddSeconds(5);  // a failed try isn't repeated at once
            int pid = ServerPid();
            if (pid == 0 || !Load()) return;
            int error = 0;
            init(ref error, VRApplication_Background, "");
            if (error != 0) return;
            IntPtr compositor = generic("FnTable:IVRCompositor_029", ref error);
            IntPtr system = error == 0 ? generic("FnTable:IVRSystem_026", ref error) : IntPtr.Zero;
            if (compositor == IntPtr.Zero || system == IntPtr.Zero)
            {
                // A SteamVR without these versions, or one whose compositor isn't up yet: asked again in a while
                shutdown();
                notBefore = now.AddSeconds(15);
                return;
            }
            getTimings = Slot<GetFrameTimingsFn>(compositor, CompositorGetFrameTimings);
            floatProp = Slot<GetFloatPropFn>(system, SystemGetFloatProp);
            pollEvent = Slot<PollNextEventFn>(system, SystemPollNextEvent);
            if (getTimings == null || floatProp == null || pollEvent == null)
            {
                shutdown();
                notBefore = now.AddSeconds(15);
                return;
            }
            if (native == IntPtr.Zero) native = Marshal.AllocHGlobal(managed.Length);
            if (ev == IntPtr.Zero) ev = Marshal.AllocHGlobal(EventBytes);
            lastIndex = 0;
            lastTime = 0;
            primed = false;
            emptyReads = 0;
            serverPid = pid;
            samplesSinceHz = 0;
            DisplayHz = 0;
            Attached = true;
        }

        void Detach(DateTime now, double waitSeconds)
        {
            if (!Attached) return;
            Attached = false;
            getTimings = null;
            floatProp = null;
            pollEvent = null;
            shutdown();
            notBefore = now.AddSeconds(waitSeconds);
        }

        /// <summary>Called often (every 100 ms): follows SteamVR coming and going. wanted = the headset asked lately.</summary>
        public void Tick(bool wanted)
        {
            DateTime now = DateTime.UtcNow;
            if (wanted) lastWanted = now;
            if (!Attached)
            {
                if (wanted && now >= notBefore) TryAttach(now);
                return;
            }
            bool quit = false;
            while (pollEvent(ev, EventBytes))
            {
                if (Marshal.ReadInt32(ev, 0) == VREvent_Quit) quit = true;
            }
            if (quit)
            {
                Detach(now, 15);  // SteamVR is closing: out of its way at once, and not back while it does
                return;
            }
            if ((now - lastWanted).TotalSeconds > 60)
            {
                Detach(now, 0);
                return;
            }
            if (now >= nextServerCheck)
            {
                nextServerCheck = now.AddSeconds(2);
                // Gone without saying so, or another one in its place; or nothing came for 10 reads (a dead connection)
                if (ServerPid() != serverPid || emptyReads >= 10) Detach(now, 5);
            }
        }

        /// <summary>The frames since the last call; null while not attached or when SteamVR has none.</summary>
        public FrameWindow Sample()
        {
            if (!Attached) return null;
            if (samplesSinceHz-- <= 0)
            {
                int propError = 0;
                float hz = floatProp(0, Prop_DisplayFrequency_Float, ref propError);
                DisplayHz = propError == 0 && hz > 1 ? hz : 0;
                samplesSinceHz = 10;
            }
            Array.Clear(managed, 0, FrameMath.TimingSize);
            Marshal.Copy(managed, 0, native, FrameMath.TimingSize);
            Marshal.WriteInt32(native, 0, FrameMath.TimingSize);
            uint count = getTimings(native, FrameMath.MaxTimings);
            if (count == 0 || count > FrameMath.MaxTimings)
            {
                ++emptyReads;
                return null;
            }
            emptyReads = 0;
            Marshal.Copy(native, managed, 0, (int)count * FrameMath.TimingSize);
            FrameWindow window = FrameMath.Aggregate(managed, (int)count, ref lastIndex, ref lastTime);
            if (!primed)
            {
                primed = true;  // SteamVR's history from before the attach: only where it ends is kept
                return null;
            }
            return window;
        }
    }

    /// <summary>The PC's own load: CPU from GetSystemTimes, the GPU from NVIDIA's nvml.dll when it is there.</summary>
    sealed class PcLoad
    {
        [DllImport("kernel32")]
        [return: MarshalAs(UnmanagedType.Bool)]
        static extern bool GetSystemTimes(out long idle, out long kernel, out long user);

        [StructLayout(LayoutKind.Sequential)]
        struct NvmlUtilization { public uint gpu; public uint memory; }
        [StructLayout(LayoutKind.Sequential)]
        struct NvmlMemory { public ulong total; public ulong free; public ulong used; }
        [DllImport("nvml.dll", EntryPoint = "nvmlInit_v2")]
        static extern int NvmlInit();
        [DllImport("nvml.dll", EntryPoint = "nvmlDeviceGetHandleByIndex_v2")]
        static extern int NvmlHandle(uint index, out IntPtr device);
        [DllImport("nvml.dll", EntryPoint = "nvmlDeviceGetUtilizationRates")]
        static extern int NvmlUtilizationRates(IntPtr device, out NvmlUtilization utilization);
        [DllImport("nvml.dll", EntryPoint = "nvmlDeviceGetTemperature")]
        static extern int NvmlTemperature(IntPtr device, int sensor, out uint celsius);
        [DllImport("nvml.dll", EntryPoint = "nvmlDeviceGetMemoryInfo")]
        static extern int NvmlMemoryInfo(IntPtr device, out NvmlMemory memory);

        [DllImport("kernel32", SetLastError = true, CharSet = CharSet.Unicode)]
        static extern IntPtr LoadLibraryW(string path);

        /// <summary>Seconds the CPU number covers: a burst of a second or two (a background task) doesn't show as 100 %,
        /// a load that stays does.</summary>
        public const int CpuWindowSec = 5;
        // The last readings, one a second (Sample), oldest first; the number is from the oldest to the newest
        readonly System.Collections.Generic.Queue<long[]> cpuTimes = new System.Collections.Generic.Queue<long[]>();
        int nvml;  // 0 = not tried, 1 = working, -1 = not there
        bool noTemperature;
        IntPtr device;

        /// <summary>Nobody asks any more: the next CPU number starts from then, not from now back to the last one.</summary>
        public void Idle() { cpuTimes.Clear(); }

        /// <summary>The CPU in use between two readings of GetSystemTimes (idle, total = kernel + user), 0-100; null when
        /// no time passed between them.</summary>
        public static double? CpuBetween(long idle0, long total0, long idle1, long total1)
        {
            if (total1 <= total0) return null;
            return Math.Max(0.0, Math.Min(100.0, 100.0 * (1.0 - (double)(idle1 - idle0) / (total1 - total0))));
        }

        public double? CpuPct { get; private set; }
        public double? GpuPct { get; private set; }
        public double? GpuTempC { get; private set; }
        public double? VramUsedMb { get; private set; }
        public double? VramTotalMb { get; private set; }

        /// <summary>The CPU over the last CpuWindowSec readings (the first call gives no CPU number yet).</summary>
        public void Sample()
        {
            long idle, kernel, user;
            CpuPct = null;
            if (GetSystemTimes(out idle, out kernel, out user))
            {
                long total = kernel + user;  // kernel time includes idle time
                cpuTimes.Enqueue(new long[] { idle, total });
                while (cpuTimes.Count > CpuWindowSec + 1) cpuTimes.Dequeue();
                if (cpuTimes.Count > 1)
                {
                    long[] oldest = cpuTimes.Peek();
                    CpuPct = CpuBetween(oldest[0], oldest[1], idle, total);
                }
            }
            GpuPct = GpuTempC = VramUsedMb = VramTotalMb = null;
            if (nvml < 0) return;
            try
            {
                if (nvml == 0)
                {
                    // The driver's own copy in System32, by its full path (not one lying next to this program). The
                    // first GPU: the one a single-GPU gaming PC renders and encodes on
                    if (LoadLibraryW(Path.Combine(Environment.SystemDirectory, "nvml.dll")) == IntPtr.Zero) nvml = -1;
                    else nvml = NvmlInit() == 0 && NvmlHandle(0, out device) == 0 ? 1 : -1;
                    if (nvml < 0) return;
                }
                NvmlUtilization u;
                if (NvmlUtilizationRates(device, out u) == 0) GpuPct = u.gpu;
                if (!noTemperature)
                {
                    try
                    {
                        uint celsius;
                        if (NvmlTemperature(device, 0, out celsius) == 0) GpuTempC = celsius;
                    }
                    catch (EntryPointNotFoundException) { noTemperature = true; }  // a driver without it: the rest stays
                }
                NvmlMemory memory;
                if (NvmlMemoryInfo(device, out memory) == 0)
                {
                    VramUsedMb = memory.used / 1048576.0;
                    VramTotalMb = memory.total / 1048576.0;
                }
            }
            catch (DllNotFoundException) { nvml = -1; }          // no NVIDIA driver
            catch (EntryPointNotFoundException) { nvml = -1; }   // an old one
            catch (BadImageFormatException) { nvml = -1; }
        }
    }

    static class Json
    {
        public static string Num(double? value, string format)
        {
            if (!value.HasValue || double.IsNaN(value.Value) || double.IsInfinity(value.Value)) return "null";
            return value.Value.ToString(format, CultureInfo.InvariantCulture);
        }
    }
}
