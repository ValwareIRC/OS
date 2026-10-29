// OperatingSystemSenses.exe: the hardware half of scripts\host-senses.mjs.
//
// Node cannot call COM objects or vendor DLLs, so this small helper does:
//   - CPU load and clock (Windows performance counters), CPU temperature
//     (AMD Ryzen Master SDK; only works when this process is elevated);
//   - NVIDIA GPU load, temperature, fans, power and memory (NVML);
//   - RGB lights through the vendors' own SDKs: Corsair iCUE (SDK v4),
//     MSI Mystic Light and ASUS Aura. It reads their colours and sets them.
//
// It speaks one JSON object per line: requests on stdin, answers on stdout.
// When stdin closes (the service has gone), every light is handed back to
// its vendor app before it exits, so nothing stays changed or held.
//
// host-senses.mjs builds it with the C# compiler that ships with Windows
// (.NET Framework csc.exe) into bin\OperatingSystemSenses.exe. The name is
// what iCUE shows if it asks whether to let this program light devices.
using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Security.Principal;
using System.Text;
using System.Threading;
using System.Web.Script.Serialization;
using Microsoft.Win32;

class Light
{
    public string Id, Name, Kind, Vendor, Effect;
    public int? Leds;
    public List<string> Colors = new List<string>();
    public bool Controllable;
    public Dictionary<string, object> ToJson()
    {
        return new Dictionary<string, object> {
            { "id", Id }, { "name", Name }, { "kind", Kind }, { "vendor", Vendor }, { "leds", Leds },
            { "colors", Colors }, { "controllable", Controllable }, { "effect", Effect } };
    }
}

interface IProvider
{
    void Start();
    List<Light> Read();
    // Shows one colour on every LED of the light. Returns null, or why not.
    string Set(string id, byte r, byte g, byte b);
    // A breathing or pulsing effect done by the device itself; false means
    // the caller should animate it with Set instead.
    bool HardwareEffect(string id, string effect, byte r, byte g, byte b);
    // Hands the light (or, with null, every light) back to the vendor app.
    void Restore(string id);
    bool Owns(string id);
    Dictionary<string, object> Status();
}

static class Util
{
    [DllImport("kernel32", CharSet = CharSet.Unicode, SetLastError = true)] public static extern IntPtr LoadLibraryW(string path);
    [DllImport("kernel32", CharSet = CharSet.Unicode)] public static extern bool SetDllDirectoryW(string path);
    [DllImport("kernel32", CharSet = CharSet.Ansi)] public static extern IntPtr GetProcAddress(IntPtr module, string name);

    public static string Hex(int r, int g, int b) { return "#" + (r & 255).ToString("x2") + (g & 255).ToString("x2") + (b & 255).ToString("x2"); }

    public static string Slug(string s)
    {
        var sb = new StringBuilder();
        foreach (char c in (s ?? "").ToLowerInvariant()) sb.Append(char.IsLetterOrDigit(c) && c < 128 ? c : '-');
        string o = sb.ToString();
        while (o.Contains("--")) o = o.Replace("--", "-");
        o = o.Trim('-');
        return o.Length == 0 ? "light" : (o.Length > 40 ? o.Substring(0, 40).Trim('-') : o);
    }

    public static T VFn<T>(IntPtr obj, int slot) where T : class
    {
        IntPtr vtable = Marshal.ReadIntPtr(obj);
        return Marshal.GetDelegateForFunctionPointer(Marshal.ReadIntPtr(vtable, slot * IntPtr.Size), typeof(T)) as T;
    }
}

// ---------------------------------------------------------------- CPU

class Cpu
{
    PerformanceCounter utility, performance;
    double baseMhz;
    public string Model;
    public Cpu()
    {
        using (var k = Registry.LocalMachine.OpenSubKey(@"HARDWARE\DESCRIPTION\System\CentralProcessor\0"))
        {
            if (k != null)
            {
                Model = ((k.GetValue("ProcessorNameString") as string) ?? "").Trim();
                object mhz = k.GetValue("~MHz");
                if (mhz is int) baseMhz = (int)mhz;
            }
        }
        try
        {
            utility = new PerformanceCounter("Processor Information", "% Processor Utility", "_Total", true);
            performance = new PerformanceCounter("Processor Information", "% Processor Performance", "_Total", true);
            utility.NextValue(); performance.NextValue();
        }
        catch (Exception) { utility = null; performance = null; }
    }
    public double? Load()
    {
        if (utility == null) return null;
        try { return Math.Round(Math.Min(100.0, Math.Max(0.0, utility.NextValue())), 1); } catch (Exception) { return null; }
    }
    public double? ClockMhz()
    {
        if (performance == null || baseMhz <= 0) return null;
        try { return Math.Round(baseMhz * performance.NextValue() / 100.0); } catch (Exception) { return null; }
    }
}

// AMD's own SDK reads the temperature through its signed driver (already
// installed here as AMDRyzenMasterDriverV29); it refuses without elevation.
// The SDK is C++ classes, so the calls go through their virtual tables.
class Ryzen
{
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate IntPtr GetPlatformFn();
    [UnmanagedFunctionPointer(CallingConvention.ThisCall)] [return: MarshalAs(UnmanagedType.I1)]
    delegate bool InitFn(IntPtr self, IntPtr vendor, [MarshalAs(UnmanagedType.I1)] bool cpuOnly);
    [UnmanagedFunctionPointer(CallingConvention.ThisCall)] delegate IntPtr GetPtrFn(IntPtr self);
    [UnmanagedFunctionPointer(CallingConvention.ThisCall)] delegate IntPtr GetDeviceFn(IntPtr self, int type, uint index);
    [UnmanagedFunctionPointer(CallingConvention.ThisCall)] delegate int GetIntFn(IntPtr self);
    [UnmanagedFunctionPointer(CallingConvention.ThisCall)] delegate int GetDoubleFn(IntPtr self, out double value);

    // Slots in the SDK headers' declaration order (IPlatform, IDeviceManager,
    // IDevice then ICPUEx). MSVC puts overloads together in reverse order, so
    // GetDevice(type, index) may sit at 3 or 2; the device's type tells.
    const int PlatformInit = 0, PlatformManager = 2, DeviceName = 2, DeviceType = 7, CpuTemperature = 44;
    IntPtr cpu;
    GetDoubleFn temperature;
    public string Error = "not-started";

    public void Start(bool admin)
    {
        string dir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), @"AMD\RyzenMasterSDK\bin");
        if (!File.Exists(Path.Combine(dir, "Platform.dll"))) { Error = "not-installed"; return; }
        // Without elevation the SDK cannot open its driver; don't even try.
        if (!admin) { Error = "needs-admin"; return; }
        try
        {
            Util.SetDllDirectoryW(dir);
            IntPtr module = Util.LoadLibraryW(Path.Combine(dir, "Platform.dll"));
            Util.SetDllDirectoryW(null);
            if (module == IntPtr.Zero) { Error = "load-failed"; return; }
            IntPtr fn = Util.GetProcAddress(module, "GetPlatform");
            if (fn == IntPtr.Zero) { Error = "no-entry"; return; }
            IntPtr platform = ((GetPlatformFn)Marshal.GetDelegateForFunctionPointer(fn, typeof(GetPlatformFn)))();
            if (!Util.VFn<InitFn>(platform, PlatformInit)(platform, IntPtr.Zero, false)) { Error = "init-failed"; return; }
            IntPtr manager = Util.VFn<GetPtrFn>(platform, PlatformManager)(platform);
            foreach (int slot in new[] { 3, 2 })
            {
                IntPtr d = Util.VFn<GetDeviceFn>(manager, slot)(manager, 0 /* dtCPU */, 0);
                if (d != IntPtr.Zero && Util.VFn<GetIntFn>(d, DeviceType)(d) == 0) { cpu = d; break; }
            }
            if (cpu == IntPtr.Zero) { Error = "no-cpu"; return; }
            string name = Marshal.PtrToStringUni(Util.VFn<GetPtrFn>(cpu, DeviceName)(cpu)) ?? "";
            if (name.IndexOf("AMD", StringComparison.OrdinalIgnoreCase) < 0) { Error = "unexpected-device"; cpu = IntPtr.Zero; return; }
            temperature = Util.VFn<GetDoubleFn>(cpu, CpuTemperature);
            Error = null;
        }
        catch (Exception e) { Error = "failed: " + e.Message; cpu = IntPtr.Zero; }
    }

    public double? Read()
    {
        if (cpu == IntPtr.Zero) return null;
        try
        {
            double t;
            if (temperature(cpu, out t) != 0 || t < 5 || t > 125) return null;
            return Math.Round(t, 1);
        }
        catch (Exception e) { Error = "failed: " + e.Message; cpu = IntPtr.Zero; return null; }
    }
}

// ---------------------------------------------------------------- GPU (NVML)

class Nvml
{
    [StructLayout(LayoutKind.Sequential)] struct Util2 { public uint gpu, memory; }
    [StructLayout(LayoutKind.Sequential)] struct Mem { public ulong total, free, used; }
    const string D = "nvml.dll";
    [DllImport(D)] static extern int nvmlInit_v2();
    [DllImport(D)] static extern int nvmlDeviceGetCount_v2(out uint n);
    [DllImport(D)] static extern int nvmlDeviceGetHandleByIndex_v2(uint i, out IntPtr d);
    [DllImport(D, CharSet = CharSet.Ansi)] static extern int nvmlDeviceGetName(IntPtr d, StringBuilder name, uint length);
    [DllImport(D)] static extern int nvmlDeviceGetUtilizationRates(IntPtr d, out Util2 u);
    [DllImport(D)] static extern int nvmlDeviceGetTemperature(IntPtr d, int sensor, out uint t);
    [DllImport(D)] static extern int nvmlDeviceGetNumFans(IntPtr d, out uint n);
    [DllImport(D)] static extern int nvmlDeviceGetFanSpeed_v2(IntPtr d, uint fan, out uint pct);
    [DllImport(D)] static extern int nvmlDeviceGetPowerUsage(IntPtr d, out uint milliwatts);
    [DllImport(D)] static extern int nvmlDeviceGetMemoryInfo(IntPtr d, out Mem m);

    List<IntPtr> devices = new List<IntPtr>();
    List<string> names = new List<string>();
    public string Error = "not-started";

    public void Start()
    {
        try
        {
            int rc = nvmlInit_v2();
            if (rc != 0) { Error = "init " + rc; return; }
            uint n; nvmlDeviceGetCount_v2(out n);
            for (uint i = 0; i < n; i++)
            {
                IntPtr d; if (nvmlDeviceGetHandleByIndex_v2(i, out d) != 0) continue;
                var sb = new StringBuilder(96); nvmlDeviceGetName(d, sb, 96);
                devices.Add(d); names.Add(sb.ToString());
            }
            Error = null;
        }
        catch (DllNotFoundException) { Error = "no-nvml"; }
        catch (Exception e) { Error = "failed: " + e.Message; }
    }

    public List<object> Read()
    {
        var o = new List<object>();
        for (int i = 0; i < devices.Count; i++)
        {
            IntPtr d = devices[i];
            Util2 u; uint t, w, nf; Mem m;
            var fans = new List<object>();
            if (nvmlDeviceGetNumFans(d, out nf) == 0)
                for (uint f = 0; f < nf && f < 8; f++) { uint p; if (nvmlDeviceGetFanSpeed_v2(d, f, out p) == 0) fans.Add((int)p); }
            bool hasMem = nvmlDeviceGetMemoryInfo(d, out m) == 0;
            o.Add(new Dictionary<string, object> {
                { "name", names[i] },
                { "load", nvmlDeviceGetUtilizationRates(d, out u) == 0 ? (object)(int)u.gpu : null },
                { "temp_c", nvmlDeviceGetTemperature(d, 0, out t) == 0 ? (object)(int)t : null },
                { "fans_pct", fans },
                { "power_w", nvmlDeviceGetPowerUsage(d, out w) == 0 ? (object)Math.Round(w / 1000.0, 1) : null },
                { "vram_used_mb", hasMem ? (object)(long)(m.used / 1048576) : null },
                { "vram_total_mb", hasMem ? (object)(long)(m.total / 1048576) : null } });
        }
        return o;
    }
}

// ---------------------------------------------------------------- Corsair iCUE SDK v4

class Corsair : IProvider
{
    const string D = "iCUESDK.x64_2019.dll";
    [StructLayout(LayoutKind.Sequential)] struct Version { public int major, minor, patch; }
    [StructLayout(LayoutKind.Sequential)] struct SessionStateChanged { public int state; public Version client, server, host; }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
    struct DeviceInfo
    {
        public int type;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string id;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string serial;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string model;
        public int ledCount, channelCount;
    }
    [StructLayout(LayoutKind.Sequential)] struct LedPosition { public uint id; public double cx, cy; }
    [StructLayout(LayoutKind.Sequential)] public struct LedColor { public uint id; public byte r, g, b, a; }
    [StructLayout(LayoutKind.Sequential)] struct DeviceFilter { public int mask; }
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate void StateHandler(IntPtr context, IntPtr data);

    [DllImport(D, CallingConvention = CallingConvention.Cdecl)] static extern int CorsairConnect(StateHandler handler, IntPtr context);
    [DllImport(D, CallingConvention = CallingConvention.Cdecl)] static extern int CorsairDisconnect();
    [DllImport(D, CallingConvention = CallingConvention.Cdecl)] static extern int CorsairGetDevices(ref DeviceFilter filter, int sizeMax, [Out] DeviceInfo[] devices, out int size);
    [DllImport(D, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)] static extern int CorsairGetLedPositions(string id, int sizeMax, [Out] LedPosition[] positions, out int size);
    [DllImport(D, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)] static extern int CorsairGetLedColors(string id, int size, [In, Out] LedColor[] colors);
    [DllImport(D, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)] static extern int CorsairSetLedColors(string id, int size, [In] LedColor[] colors);
    [DllImport(D, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)] static extern int CorsairRequestControl(string id, int accessLevel);
    [DllImport(D, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)] static extern int CorsairReleaseControl(string id);

    static readonly string[] StateNames = { "invalid", "closed", "connecting", "timeout", "refused", "lost", "connected" };
    class Dev { public string LightId, SdkId, Model; public int Type; public LedColor[] Leds; public bool Held; }

    StateHandler handler;  // kept alive: the SDK calls it from its own thread
    volatile int state;
    volatile bool changed;
    string error, iCueVersion;
    bool loaded;
    DateTime enumerated = DateTime.MinValue;
    Dictionary<string, Dev> devs = new Dictionary<string, Dev>();
    readonly string dir;
    public Corsair(string dir) { this.dir = dir; }

    public void Start()
    {
        string path = Path.Combine(dir, D);
        if (!File.Exists(path)) { error = "no-sdk-dll"; return; }
        if (Util.LoadLibraryW(path) == IntPtr.Zero) { error = "load-failed"; return; }
        loaded = true;
        Connect();
    }

    void Connect()
    {
        handler = OnState;
        int e = CorsairConnect(handler, IntPtr.Zero);
        if (e != 0) error = "connect-error-" + e;
    }

    void OnState(IntPtr context, IntPtr data)
    {
        var s = (SessionStateChanged)Marshal.PtrToStructure(data, typeof(SessionStateChanged));
        state = s.state;
        if (s.host.major > 0) iCueVersion = s.host.major + "." + s.host.minor + "." + s.host.patch;
        changed = true;
    }

    static string KindOf(int t)
    {
        switch (t)
        {
            case 0x1: return "keyboard";
            case 0x2: return "mouse";
            case 0x8: return "headset";
            case 0x20: return "fan";
            case 0x40: return "strip";
            case 0x80: return "ram";
            case 0x100: return "cooler";
            case 0x200: return "motherboard";
            case 0x400: return "gpu";
            default: return "other";
        }
    }

    void Enumerate()
    {
        var filter = new DeviceFilter { mask = unchecked((int)0xFFFFFFFF) };
        var infos = new DeviceInfo[64];
        int n;
        if (CorsairGetDevices(ref filter, infos.Length, infos, out n) != 0) return;
        var fresh = new Dictionary<string, Dev>();
        for (int i = 0; i < n; i++)
        {
            var info = infos[i];
            var pos = new LedPosition[512];
            int count;
            if (CorsairGetLedPositions(info.id, pos.Length, pos, out count) != 0) count = 0;
            // Left to right, then top to bottom: samples read across the device.
            var leds = pos.Take(count).OrderBy(p => Math.Round(p.cx / 10)).ThenBy(p => p.cy)
                          .Select(p => new LedColor { id = p.id }).ToArray();
            string baseId = "corsair:" + Util.Slug(info.model), lightId = baseId;
            for (int k = 2; fresh.ContainsKey(lightId); k++) lightId = baseId + "-" + k;
            Dev old;
            bool held = devs.TryGetValue(lightId, out old) && old.SdkId == info.id && old.Held;
            fresh[lightId] = new Dev { LightId = lightId, SdkId = info.id, Model = info.model, Type = info.type, Leds = leds, Held = held };
        }
        devs = fresh;
        enumerated = DateTime.UtcNow;
    }

    public List<Light> Read()
    {
        var o = new List<Light>();
        if (state != 6) return o;
        // Devices come and go (a wireless mouse sleeps), so look again now and then.
        if (changed || (DateTime.UtcNow - enumerated).TotalSeconds > 15) { changed = false; Enumerate(); }
        foreach (var d in devs.Values)
        {
            var l = new Light { Id = d.LightId, Name = d.Model, Kind = KindOf(d.Type), Vendor = "corsair", Leds = d.Leds.Length, Controllable = d.Leds.Length > 0 };
            if (d.Leds.Length > 0)
            {
                var c = (LedColor[])d.Leds.Clone();
                if (CorsairGetLedColors(d.SdkId, c.Length, c) == 0)
                    foreach (var x in c) l.Colors.Add(Util.Hex(x.r, x.g, x.b));
            }
            o.Add(l);
        }
        return o;
    }

    public bool Owns(string id) { return devs.ContainsKey(id); }

    public string Set(string id, byte r, byte g, byte b)
    {
        Dev d;
        if (state != 6 || !devs.TryGetValue(id, out d)) return "not connected to iCUE";
        if (d.Leds.Length == 0) return "no controllable LEDs";
        if (!d.Held)
        {
            int e = CorsairRequestControl(d.SdkId, 1 /* CAL_ExclusiveLightingControl */);
            if (e != 0) return "iCUE refused control (error " + e + ")";
            d.Held = true;
        }
        var c = new LedColor[d.Leds.Length];
        for (int i = 0; i < c.Length; i++) c[i] = new LedColor { id = d.Leds[i].id, r = r, g = g, b = b, a = 255 };
        int err = CorsairSetLedColors(d.SdkId, c.Length, c);
        return err == 0 ? null : "iCUE error " + err;
    }

    public bool HardwareEffect(string id, string effect, byte r, byte g, byte b) { return false; }

    public void Restore(string id)
    {
        if (!loaded) return;
        bool any = false;
        foreach (var d in devs.Values)
        {
            if (id != null && d.LightId != id) continue;
            if (!d.Held) continue;
            // A transparent layer first, so iCUE's own lighting shows through
            // even before the control is gone.
            var c = new LedColor[d.Leds.Length];
            for (int i = 0; i < c.Length; i++) c[i] = new LedColor { id = d.Leds[i].id, a = 0 };
            CorsairSetLedColors(d.SdkId, c.Length, c);
            CorsairReleaseControl(d.SdkId);
            d.Held = false;
            any = true;
        }
        if (any && !Holding())
        {
            // Leaving the session drops anything this client still had in iCUE.
            CorsairDisconnect();
            state = 1;
            Thread.Sleep(150);
            Connect();
        }
    }

    public bool Holding() { return devs.Values.Any(d => d.Held); }

    public Dictionary<string, object> Status()
    {
        return new Dictionary<string, object> {
            { "sdk", loaded }, { "state", state < StateNames.Length ? StateNames[state] : state.ToString() },
            { "error", error }, { "icue", iCueVersion }, { "devices", devs.Values.Select(d => d.Model).ToList() } };
    }
}

// ---------------------------------------------------------------- MSI Mystic Light SDK

// A vendor SDK that can take its time (seconds, or over a minute) gets a
// thread of its own; the rest of the helper never waits on it for long.
abstract class Worker
{
    protected readonly BlockingCollection<Action> work = new BlockingCollection<Action>();
    protected volatile bool busy;

    protected void Begin(ApartmentState apartment, Action first)
    {
        var t = new Thread(() => { foreach (var job in work.GetConsumingEnumerable()) { busy = true; try { job(); } catch (Exception) { } busy = false; } });
        t.IsBackground = true;
        t.SetApartmentState(apartment);
        t.Start();
        work.Add(first);
    }

    // Runs f on the worker; stops waiting after ms (f still finishes there).
    protected T Call<T>(Func<T> f, int ms, T late)
    {
        if (busy && work.Count > 0) return late;
        T result = late;
        var done = new ManualResetEventSlim();
        work.Add(() => { result = f(); done.Set(); });
        return done.Wait(ms) ? result : late;
    }

    protected void Post(Action a) { if (!busy && work.Count == 0) work.Add(a); }
}

class Msi : Worker, IProvider
{
    const string D = "MysticLight_SDK_x64.dll";
    [DllImport(D)] static extern int MLAPI_Initialize();
    [DllImport(D)] static extern int MLAPI_Release();
    [DllImport(D)] static extern int MLAPI_GetDeviceInfo(
        [MarshalAs(UnmanagedType.SafeArray, SafeArraySubType = VarEnum.VT_BSTR)] out string[] types,
        [MarshalAs(UnmanagedType.SafeArray, SafeArraySubType = VarEnum.VT_BSTR)] out string[] ledCounts);
    [DllImport(D)] static extern int MLAPI_GetDeviceNameEx([MarshalAs(UnmanagedType.BStr)] string type, uint index, [MarshalAs(UnmanagedType.BStr)] out string name);
    [DllImport(D)] static extern int MLAPI_GetLedInfo([MarshalAs(UnmanagedType.BStr)] string type, uint index, [MarshalAs(UnmanagedType.BStr)] out string name,
        [MarshalAs(UnmanagedType.SafeArray, SafeArraySubType = VarEnum.VT_BSTR)] out string[] styles);
    [DllImport(D)] static extern int MLAPI_GetLedColor([MarshalAs(UnmanagedType.BStr)] string type, uint index, out uint r, out uint g, out uint b);
    [DllImport(D)] static extern int MLAPI_GetLedStyle([MarshalAs(UnmanagedType.BStr)] string type, uint index, [MarshalAs(UnmanagedType.BStr)] out string style);
    [DllImport(D)] static extern int MLAPI_GetLedBright([MarshalAs(UnmanagedType.BStr)] string type, uint index, out uint level);
    [DllImport(D)] static extern int MLAPI_GetLedSpeed([MarshalAs(UnmanagedType.BStr)] string type, uint index, out uint level);
    [DllImport(D)] static extern int MLAPI_SetLedColor([MarshalAs(UnmanagedType.BStr)] string type, uint index, uint r, uint g, uint b);
    [DllImport(D)] static extern int MLAPI_SetLedStyle([MarshalAs(UnmanagedType.BStr)] string type, uint index, [MarshalAs(UnmanagedType.BStr)] string style);
    [DllImport(D)] static extern int MLAPI_SetLedBright([MarshalAs(UnmanagedType.BStr)] string type, uint index, uint level);
    [DllImport(D)] static extern int MLAPI_SetLedSpeed([MarshalAs(UnmanagedType.BStr)] string type, uint index, uint level);

    class Area
    {
        public string LightId, Type, Name, Device; public uint Index; public string[] Styles;
        public bool Saved; public string Style; public uint R, G, B, Bright, Speed; public bool HasBright, HasSpeed;
    }
    Dictionary<string, Area> areas = new Dictionary<string, Area>();
    volatile Dictionary<string, Light> cache = new Dictionary<string, Light>();
    DateTime readAt = DateTime.MinValue;
    volatile string error = "starting"; int initCode = int.MinValue; volatile bool ready;
    public List<string> Skip = new List<string>();

    // MSI's SDK can block for over a minute (without elevation it waits 78 s
    // and then times out), so it runs on a thread of its own.
    public void Start() { Begin(ApartmentState.MTA, StartHere); }

    void StartHere()
    {
        // MSI's SDK DLL as iCUE ships it; it talks to MSI Center's LEDKeeper2.
        string[] places = {
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), @"Corsair\Corsair iCUE5 Software\plugins\MSI\" + D),
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86), @"MSI\MSI Center\Mystic Light\" + D) };
        string path = places.FirstOrDefault(File.Exists);
        if (path == null) { error = "no-sdk-dll"; return; }
        if (Util.LoadLibraryW(path) == IntPtr.Zero) { error = "load-failed"; return; }
        try
        {
            initCode = MLAPI_Initialize();
            if (initCode != 0) { error = "init-" + initCode; return; }
            error = "listing";
            string[] types, counts;
            int rc = MLAPI_GetDeviceInfo(out types, out counts);
            if (rc == -2) { error = "timeout"; return; }
            if (rc != 0 || types == null) { error = "device-info-" + rc; return; }
            for (int i = 0; i < types.Length; i++)
            {
                uint n; uint.TryParse(counts != null && i < counts.Length ? counts[i] : "0", out n);
                for (uint k = 0; k < n && k < 32; k++)
                {
                    string name, dev = null; string[] styles;
                    if (MLAPI_GetLedInfo(types[i], k, out name, out styles) != 0) { name = types[i] + " " + k; styles = new string[0]; }
                    MLAPI_GetDeviceNameEx(types[i], k, out dev);
                    string baseId = "msi:" + Util.Slug(types[i].Replace("MSI_", "")) + "-" + Util.Slug(name), id = baseId;
                    for (int j = 2; areas.ContainsKey(id); j++) id = baseId + "-" + j;
                    areas[id] = new Area { LightId = id, Type = types[i], Index = k, Name = name, Device = dev, Styles = styles ?? new string[0] };
                }
            }
            error = null;
            ready = true;
            Refresh();
        }
        catch (Exception e) { error = "failed: " + e.Message; }
    }

    static string KindOf(Area a)
    {
        string t = a.Type.ToUpperInvariant(), n = (a.Name ?? "").ToUpperInvariant();
        if (n.Contains("FAN")) return "fan";
        if (n.Contains("JRAINBOW") || n.Contains("JARGB") || n.Contains("JRGB") || n.Contains("STRIP")) return "strip";
        if (t.Contains("MB")) return "motherboard";
        if (t.Contains("VGA")) return "gpu";
        if (t.Contains("KEYBOARD")) return "keyboard";
        if (t.Contains("MOUSE")) return "mouse";
        if (t.Contains("HEADSET")) return "headset";
        if (t.Contains("COOLER")) return "cooler";
        if (t.Contains("MEMORY") || t.Contains("DRAM")) return "ram";
        if (t.Contains("SSD") || t.Contains("STORAGE")) return "ssd";
        return "other";
    }

    // On MSI's thread: the current style and colour of every area.
    void Refresh()
    {
        var fresh = new Dictionary<string, Light>();
        foreach (var a in areas.Values)
        {
            if (Skip.Any(s => a.Type.IndexOf(s, StringComparison.OrdinalIgnoreCase) >= 0)) continue;
            uint r, g, b; string style;
            var l = new Light { Id = a.LightId, Name = string.IsNullOrEmpty(a.Device) || a.Device == a.Name ? a.Name : a.Device + " " + a.Name,
                                Kind = KindOf(a), Vendor = "msi", Leds = null, Controllable = true };
            if (MLAPI_GetLedStyle(a.Type, a.Index, out style) == 0) l.Effect = style;
            if (MLAPI_GetLedColor(a.Type, a.Index, out r, out g, out b) == 0)
                l.Colors.Add(string.Equals(style, "Off", StringComparison.OrdinalIgnoreCase) ? "#000000" : Util.Hex((int)r, (int)g, (int)b));
            fresh[a.LightId] = l;
        }
        cache = fresh;
        readAt = DateTime.UtcNow;
    }

    public List<Light> Read()
    {
        if (!ready) return new List<Light>();
        // Its state only changes when someone changes it: a look every 10 s is plenty.
        if ((DateTime.UtcNow - readAt).TotalSeconds >= 10) Post(Refresh);
        return cache.Values.ToList();
    }

    public bool Owns(string id) { return ready && areas.ContainsKey(id); }

    volatile int held;
    void Save(Area a)
    {
        if (a.Saved) return;
        MLAPI_GetLedStyle(a.Type, a.Index, out a.Style);
        MLAPI_GetLedColor(a.Type, a.Index, out a.R, out a.G, out a.B);
        a.HasBright = MLAPI_GetLedBright(a.Type, a.Index, out a.Bright) == 0;
        a.HasSpeed = MLAPI_GetLedSpeed(a.Type, a.Index, out a.Speed) == 0;
        a.Saved = true;
        held++;
    }

    string PickStyle(Area a, params string[] wanted)
    {
        foreach (var w in wanted)
            foreach (var s in a.Styles) if (string.Equals(s, w, StringComparison.OrdinalIgnoreCase)) return s;
        return null;
    }

    public string Set(string id, byte r, byte g, byte b)
    {
        Area a;
        if (!ready || !areas.TryGetValue(id, out a)) return "not available";
        return Call(() => {
            Save(a);
            string steady = PickStyle(a, "Steady", "Static", "Always On"), now;
            if (steady != null && (MLAPI_GetLedStyle(a.Type, a.Index, out now) != 0 || !string.Equals(now, steady, StringComparison.OrdinalIgnoreCase)))
                MLAPI_SetLedStyle(a.Type, a.Index, steady);
            int rc = MLAPI_SetLedColor(a.Type, a.Index, r, g, b);
            Refresh();
            return rc == 0 ? null : "Mystic Light error " + rc;
        }, 5000, "Mystic Light did not answer");
    }

    public bool HardwareEffect(string id, string effect, byte r, byte g, byte b)
    {
        Area a;
        if (!ready || !areas.TryGetValue(id, out a)) return false;
        string style = effect == "breathe" ? PickStyle(a, "Breathing") : effect == "pulse" ? PickStyle(a, "Flashing", "Double Flashing") : null;
        if (style == null) return false;
        return Call(() => {
            Save(a);
            MLAPI_SetLedStyle(a.Type, a.Index, style);
            MLAPI_SetLedColor(a.Type, a.Index, r, g, b);
            Refresh();
            return true;
        }, 5000, false);
    }

    public void Restore(string id)
    {
        if (!ready || held == 0) return;
        Call(() => {
            foreach (var a in areas.Values)
            {
                if ((id != null && a.LightId != id) || !a.Saved) continue;
                if (a.Style != null) MLAPI_SetLedStyle(a.Type, a.Index, a.Style);
                MLAPI_SetLedColor(a.Type, a.Index, a.R, a.G, a.B);
                if (a.HasBright) MLAPI_SetLedBright(a.Type, a.Index, a.Bright);
                if (a.HasSpeed) MLAPI_SetLedSpeed(a.Type, a.Index, a.Speed);
                a.Saved = false;
                held--;
            }
            Refresh();
            return true;
        }, 8000, false);
    }

    public bool Holding() { return held > 0; }

    public void Close() { if (ready) Call(() => { MLAPI_Release(); ready = false; return true; }, 3000, false); }

    public Dictionary<string, object> Status()
    {
        return new Dictionary<string, object> {
            { "ready", ready }, { "error", error }, { "init", initCode == int.MinValue ? null : (object)initCode },
            { "areas", ready ? areas.Values.Select(a => a.Type + "/" + a.Name + " [" + string.Join(",", a.Styles) + "]").ToList() : new List<string>() } };
    }
}

// ---------------------------------------------------------------- ASUS Aura SDK (COM)


// The Aura SDK is an in-process COM object that asks ASUS's LightingService
// for devices; listing them takes about 24 s, so it has its own STA thread.
class Aura : Worker, IProvider
{
    dynamic sdk;
    volatile bool held, ready;
    volatile string error = "starting";
    int count = -1;
    Dictionary<string, dynamic> devs = new Dictionary<string, dynamic>();
    Dictionary<string, int> types = new Dictionary<string, int>();
    volatile List<Light> cache = new List<Light>();
    DateTime readAt = DateTime.MinValue;

    public void Start() { Begin(ApartmentState.STA, StartHere); }

    void StartHere()
    {
        try
        {
            Type t = Type.GetTypeFromProgID("aura.sdk.1");
            if (t == null) { error = "not-installed"; return; }
            sdk = Activator.CreateInstance(t);
            Enumerate();
            error = null;
            ready = true;
            Refresh();
        }
        catch (Exception e) { error = "failed: " + e.Message; sdk = null; }
    }

    void Enumerate()
    {
        var fresh = new Dictionary<string, dynamic>();
        var kinds = new Dictionary<string, int>();
        dynamic list = sdk.Enumerate(0);
        count = (int)list.Count;
        foreach (dynamic d in list)
        {
            string name = (string)d.Name, id = "aura:" + Util.Slug(name), baseId = id;
            for (int k = 2; fresh.ContainsKey(id); k++) id = baseId + "-" + k;
            fresh[id] = d; kinds[id] = (int)(uint)d.Type;
        }
        types = kinds; devs = fresh;
    }

    static string KindOf(int type, string name)
    {
        string n = name.ToUpperInvariant();
        if (n.Contains("DRAM") || n.Contains("MEMORY")) return "ram";
        if (n.Contains("HEADSET")) return "headset";
        if (n.Contains("KEYBOARD")) return "keyboard";
        if (n.Contains("MOUSE")) return "mouse";
        if (n.Contains("FAN")) return "fan";
        switch ((type >> 16) & 0xFF)
        {
            case 0x01: return (type & 0xF000) != 0 ? "strip" : "motherboard";
            case 0x02: return "gpu";
            case 0x04: return "headset";
            case 0x07: return "ram";
            case 0x08: return "keyboard";
            case 0x09: return "mouse";
            default: return "other";
        }
    }

    void Refresh()
    {
        var o = new List<Light>();
        foreach (var kv in devs)
        {
            dynamic d = kv.Value;
            var l = new Light { Id = kv.Key, Name = (string)d.Name, Kind = KindOf(types[kv.Key], (string)d.Name), Vendor = "asus", Controllable = true };
            try
            {
                dynamic lights = d.Lights;
                l.Leds = (int)lights.Count;
                // The SDK only sees colours it set itself, so they are only
                // reported while this helper is the one lighting the device.
                if (held) foreach (dynamic x in lights) { uint c = (uint)x.Color; l.Colors.Add(Util.Hex((int)(c & 255), (int)((c >> 8) & 255), (int)((c >> 16) & 255))); }
            }
            catch (Exception) { }
            o.Add(l);
        }
        cache = o;
        readAt = DateTime.UtcNow;
    }

    public List<Light> Read()
    {
        if (!ready) return new List<Light>();
        if ((DateTime.UtcNow - readAt).TotalSeconds >= 10) Post(Refresh);
        return cache.ToList();
    }

    public bool Owns(string id) { return ready && devs.ContainsKey(id); }

    public string Set(string id, byte r, byte g, byte b)
    {
        if (!ready || !devs.ContainsKey(id)) return "not available";
        return Call(() => {
            try
            {
                if (!held) { sdk.SwitchMode(); held = true; }
                dynamic d = devs[id];
                uint c = (uint)(r | (g << 8) | (b << 16));
                foreach (dynamic x in d.Lights) x.Color = c;
                d.Apply();
                return (string)null;
            }
            catch (Exception e) { return "Aura error: " + e.Message; }
        }, 5000, "Aura did not answer");
    }

    public bool HardwareEffect(string id, string effect, byte r, byte g, byte b) { return false; }

    public void Restore(string id)
    {
        // Aura hands control back as a whole (Armoury Crate repaints every device).
        if (!ready || !held) return;
        Call(() => { try { sdk.ReleaseControl(0); } catch (Exception) { } held = false; Refresh(); return true; }, 8000, false);
    }

    public bool Holding() { return held; }

    public Dictionary<string, object> Status()
    {
        return new Dictionary<string, object> { { "ready", ready }, { "error", error }, { "devices", count } };
    }
}


// ---------------------------------------------------------------- the helper

static class Program
{
    static readonly JavaScriptSerializer Json = new JavaScriptSerializer { MaxJsonLength = 1 << 24 };
    static readonly BlockingCollection<Action> Jobs = new BlockingCollection<Action>();
    static bool Admin;
    static Cpu cpu;
    static Ryzen ryzen = new Ryzen();
    static Nvml nvml = new Nvml();
    static Corsair corsair;
    static Msi msi = new Msi();
    static Aura aura = new Aura();
    static List<IProvider> providers;

    class Anim { public string Id, Effect; public byte R, G, B; public IProvider P; public DateTime Start; }
    static readonly Dictionary<string, Anim> anims = new Dictionary<string, Anim>();

    static void Main(string[] args)
    {
        Console.OutputEncoding = new UTF8Encoding(false);
        Console.InputEncoding = new UTF8Encoding(false);
        Admin = new WindowsPrincipal(WindowsIdentity.GetCurrent()).IsInRole(WindowsBuiltInRole.Administrator);
        string here = Path.GetDirectoryName(Path.GetDirectoryName(typeof(Program).Assembly.Location));
        corsair = new Corsair(here);
        providers = new List<IProvider> { corsair, msi, aura };

        // Every SDK call happens on this one STA thread, so COM and the
        // vendors' DLLs only ever see a single caller.
        var hardware = new Thread(HardwareLoop) { IsBackground = false };
        hardware.SetApartmentState(ApartmentState.STA);
        hardware.Start();
        Jobs.Add(() => {
            var clock = Stopwatch.StartNew();
            Action<string> step = s => Console.Error.WriteLine("start: " + s + " " + clock.ElapsedMilliseconds + " ms");
            cpu = new Cpu(); step("cpu");
            ryzen.Start(Admin); step("ryzen " + (ryzen.Error ?? "ok"));
            nvml.Start(); step("nvml " + (nvml.Error ?? "ok"));
            corsair.Start(); step("corsair");
            msi.Start(); step("msi");
            aura.Start(); step("aura");
            // Corsair devices show up in Mystic Light too (MSI syncs through
            // iCUE); they are listed once, from iCUE.
            msi.Skip.Add("CORSAIR");
        });

        string line;
        while ((line = Console.In.ReadLine()) != null)
        {
            Dictionary<string, object> q;
            try { q = Json.Deserialize<Dictionary<string, object>>(line); }
            catch (Exception) { Reply(null, new Dictionary<string, object> { { "error", "bad json" } }); continue; }
            if (q == null) continue;
            var req = q;
            Jobs.Add(() => {
                object id; req.TryGetValue("id", out id);
                Dictionary<string, object> r;
                try { r = Handle(req); }
                catch (Exception e) { r = new Dictionary<string, object> { { "error", e.Message } }; }
                Reply(id, r);
            });
        }
        // The service is gone: give everything back, then leave.
        var done = new ManualResetEventSlim();
        Jobs.Add(() => { RestoreAll(); msi.Close(); done.Set(); });
        done.Wait(8000);
        Environment.Exit(0);
    }

    static readonly object WriteLock = new object();
    static void Reply(object id, Dictionary<string, object> r)
    {
        r["id"] = id;
        string s = Json.Serialize(r);
        lock (WriteLock) { Console.Out.Write(s + "\n"); Console.Out.Flush(); }
    }

    static void HardwareLoop()
    {
        var last = DateTime.MinValue;
        while (true)
        {
            Action job;
            int wait = anims.Count > 0 ? 33 : Timeout.Infinite;
            if (Jobs.TryTake(out job, wait))
            {
                try { job(); } catch (Exception e) { Console.Error.WriteLine("job failed: " + e.Message); }
            }
            if (anims.Count > 0 && (DateTime.UtcNow - last).TotalMilliseconds >= 33) { last = DateTime.UtcNow; Frame(); }
        }
    }

    static void Frame()
    {
        foreach (var a in anims.Values.ToList())
        {
            double t = (DateTime.UtcNow - a.Start).TotalSeconds, f;
            if (a.Effect == "breathe") f = 0.08 + 0.92 * (0.5 - 0.5 * Math.Cos(2 * Math.PI * t / 4.0));
            else { double ph = t % 1.0; f = 0.06 + 0.94 * Math.Exp(-6.0 * ph); }  // pulse: a beat a second
            a.P.Set(a.Id, (byte)(a.R * f), (byte)(a.G * f), (byte)(a.B * f));
        }
    }

    static IProvider Find(string id) { return providers.FirstOrDefault(p => p.Owns(id)); }

    static void RestoreAll()
    {
        anims.Clear();
        foreach (var p in providers) { try { p.Restore(null); } catch (Exception) { } }
    }

    static Dictionary<string, object> Handle(Dictionary<string, object> q)
    {
        string op = q.ContainsKey("op") ? q["op"] as string : null;
        if (op == "read") return Read();
        if (op == "set") return Set(q);
        if (op == "restore") return Restore(q);
        return new Dictionary<string, object> { { "error", "unknown op" } };
    }

    static Dictionary<string, object> Read()
    {
        var lights = new List<object>();
        var errors = new List<string>();
        foreach (var p in providers)
        {
            try { foreach (var l in p.Read()) lights.Add(l.ToJson()); }
            catch (Exception e) { errors.Add(p.GetType().Name + ": " + e.Message); }
        }
        return new Dictionary<string, object> {
            { "admin", Admin },
            { "cpu", new Dictionary<string, object> {
                { "model", cpu == null ? null : cpu.Model }, { "load", cpu == null ? null : cpu.Load() },
                { "clock_mhz", cpu == null ? null : cpu.ClockMhz() }, { "temp_c", ryzen.Read() } } },
            { "gpu", nvml.Read() },
            { "lights", lights },
            { "holding", corsair.Holding() || msi.Holding() || aura.Holding() || anims.Count > 0 },
            { "status", new Dictionary<string, object> {
                { "ryzen", ryzen.Error }, { "nvml", nvml.Error }, { "corsair", corsair.Status() },
                { "msi", msi.Status() }, { "aura", aura.Status() }, { "errors", errors } } } };
    }

    static bool ParseColor(object v, out byte r, out byte g, out byte b)
    {
        r = g = b = 0;
        string s = v as string;
        if (s == null || s.Length != 7 || s[0] != '#') return false;
        int x;
        if (!int.TryParse(s.Substring(1), System.Globalization.NumberStyles.HexNumber, null, out x)) return false;
        r = (byte)(x >> 16); g = (byte)(x >> 8); b = (byte)x;
        return true;
    }

    static List<string> Ids(Dictionary<string, object> q)
    {
        var o = new List<string>();
        object v;
        if (q.TryGetValue("ids", out v) && v is System.Collections.IEnumerable)
            foreach (var x in (System.Collections.IEnumerable)v) { string s = x as string; if (s != null && s.Length < 100) o.Add(s); }
        return o;
    }

    static Dictionary<string, object> Set(Dictionary<string, object> q)
    {
        string effect = (q.ContainsKey("effect") ? q["effect"] as string : null) ?? "static";
        byte r, g, b;
        bool hasColor = ParseColor(q.ContainsKey("color") ? q["color"] : null, out r, out g, out b);
        if (effect == "off") { r = g = b = 0; hasColor = true; }
        if (!hasColor) return new Dictionary<string, object> { { "error", "a colour is needed" } };
        if (effect != "static" && effect != "off" && effect != "breathe" && effect != "pulse")
            return new Dictionary<string, object> { { "error", "unknown effect" } };
        var changed = new List<object>();
        var failed = new List<object>();
        foreach (string id in Ids(q))
        {
            IProvider p = Find(id);
            if (p == null) { failed.Add(new Dictionary<string, object> { { "id", id }, { "reason", "no such light" } }); continue; }
            anims.Remove(id);
            string why = null;
            if (effect == "breathe" || effect == "pulse")
            {
                if (!p.HardwareEffect(id, effect, r, g, b))
                {
                    why = p.Set(id, r, g, b);
                    if (why == null) anims[id] = new Anim { Id = id, Effect = effect, R = r, G = g, B = b, P = p, Start = DateTime.UtcNow };
                }
            }
            else why = p.Set(id, r, g, b);
            if (why == null) changed.Add(new Dictionary<string, object> { { "id", id }, { "effect", effect }, { "color", Util.Hex(r, g, b) } });
            else failed.Add(new Dictionary<string, object> { { "id", id }, { "reason", why } });
        }
        return new Dictionary<string, object> { { "changed", changed }, { "failed", failed } };
    }

    static Dictionary<string, object> Restore(Dictionary<string, object> q)
    {
        var ids = Ids(q);
        if (ids.Count == 0) { RestoreAll(); return new Dictionary<string, object> { { "restored", "all" } }; }
        var done = new List<object>();
        foreach (string id in ids)
        {
            anims.Remove(id);
            IProvider p = Find(id);
            if (p == null) continue;
            p.Restore(id);
            done.Add(id);
        }
        return new Dictionary<string, object> { { "restored", done } };
    }
}
