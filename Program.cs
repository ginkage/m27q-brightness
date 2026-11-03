using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Threading;

namespace Brightness
{
    internal static class Native
    {
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        public struct PHYSICAL_MONITOR
        {
            public IntPtr hPhysicalMonitor;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)]
            public string szDescription;
        }

        public delegate bool MonitorEnumProc(IntPtr hMonitor, IntPtr hdc, IntPtr lprcClip, IntPtr dwData);

        [DllImport("user32.dll")]
        public static extern bool EnumDisplayMonitors(IntPtr hdc, IntPtr lprcClip, MonitorEnumProc cb, IntPtr data);

        [DllImport("dxva2.dll")]
        public static extern bool GetNumberOfPhysicalMonitorsFromHMONITOR(IntPtr hMonitor, ref uint count);

        [DllImport("dxva2.dll")]
        public static extern bool GetPhysicalMonitorsFromHMONITOR(IntPtr hMonitor, uint count, [Out] PHYSICAL_MONITOR[] arr);

        [DllImport("dxva2.dll")]
        public static extern bool DestroyPhysicalMonitors(uint count, [In] PHYSICAL_MONITOR[] arr);

        [DllImport("dxva2.dll")]
        public static extern bool SetVCPFeature(IntPtr hMonitor, byte code, uint value);

        [DllImport("dxva2.dll")]
        public static extern bool GetVCPFeatureAndVCPFeatureReply(
            IntPtr hMonitor, byte code, ref uint type, ref uint current, ref uint max);
    }

    /// <summary>
    /// One DDC/CI-capable display. The M27Q drops roughly every other DDC/CI
    /// transaction when they are issued back to back, so every call here is
    /// paced by SettleMs and retried up to MaxAttempts times.
    /// </summary>
    internal sealed class Display
    {
        public const byte VcpLuminance = 0x10;

        private const int SettleMs = 60;
        private const int MaxAttempts = 8;

        private readonly IntPtr _handle;

        public string Name { get; private set; }

        internal Display(IntPtr handle, string name)
        {
            _handle = handle;
            Name = name;
        }

        /// <summary>Reads a VCP code. Returns false if every attempt failed.</summary>
        public bool TryRead(byte code, out uint current, out uint max)
        {
            for (int attempt = 0; attempt < MaxAttempts; attempt++)
            {
                uint type = 0, cur = 0, mx = 0;
                if (Native.GetVCPFeatureAndVCPFeatureReply(_handle, code, ref type, ref cur, ref mx))
                {
                    current = cur;
                    max = mx;
                    Thread.Sleep(SettleMs);
                    return true;
                }
                Thread.Sleep(SettleMs);
            }
            current = 0;
            max = 0;
            return false;
        }

        /// <summary>
        /// Writes a VCP code and confirms it by reading back, since a successful
        /// SetVCPFeature return value is not by itself reliable on this monitor.
        /// </summary>
        public bool TryWrite(byte code, uint value)
        {
            for (int attempt = 0; attempt < MaxAttempts; attempt++)
            {
                Native.SetVCPFeature(_handle, code, value);
                Thread.Sleep(SettleMs);

                uint current, max;
                if (TryRead(code, out current, out max) && current == value)
                    return true;
            }
            return false;
        }
    }

    /// <summary>
    /// Owns the PHYSICAL_MONITOR arrays handed out by the OS. Each array must go
    /// back to DestroyPhysicalMonitors exactly once even when it describes several
    /// displays, so ownership lives here rather than on individual Display objects.
    /// </summary>
    internal sealed class DisplaySet : IDisposable
    {
        private readonly List<Native.PHYSICAL_MONITOR[]> _arrays = new List<Native.PHYSICAL_MONITOR[]>();
        private readonly List<Display> _displays = new List<Display>();
        private bool _disposed;

        public int Count { get { return _displays.Count; } }

        public Display this[int index] { get { return _displays[index]; } }

        public static DisplaySet Open()
        {
            var hmonitors = new List<IntPtr>();
            Native.MonitorEnumProc cb = delegate(IntPtr h, IntPtr hdc, IntPtr rc, IntPtr data)
            {
                hmonitors.Add(h);
                return true;
            };
            Native.EnumDisplayMonitors(IntPtr.Zero, IntPtr.Zero, cb, IntPtr.Zero);
            GC.KeepAlive(cb);

            var set = new DisplaySet();
            foreach (IntPtr hmon in hmonitors)
            {
                uint count = 0;
                if (!Native.GetNumberOfPhysicalMonitorsFromHMONITOR(hmon, ref count) || count == 0)
                    continue;

                var arr = new Native.PHYSICAL_MONITOR[count];
                if (!Native.GetPhysicalMonitorsFromHMONITOR(hmon, count, arr))
                    continue;

                set._arrays.Add(arr);
                for (int i = 0; i < count; i++)
                    set._displays.Add(new Display(arr[i].hPhysicalMonitor, arr[i].szDescription));
            }
            return set;
        }

        public void Dispose()
        {
            if (_disposed)
                return;
            _disposed = true;

            foreach (Native.PHYSICAL_MONITOR[] arr in _arrays)
                Native.DestroyPhysicalMonitors((uint)arr.Length, arr);

            _arrays.Clear();
            _displays.Clear();
        }
    }

    internal static class Program
    {
        private static int Main(string[] args)
        {
            using (DisplaySet displays = DisplaySet.Open())
            {
                if (displays.Count == 0)
                {
                    Console.Error.WriteLine("No DDC/CI-capable display found.");
                    return 1;
                }

                if (args.Length == 1 && (args[0] == "-l" || args[0] == "--list"))
                    return List(displays);

                // Optional "--monitor N" prefix; default to the first display.
                int index = 0;
                var rest = new List<string>(args);
                if (rest.Count >= 2 && (rest[0] == "-m" || rest[0] == "--monitor"))
                {
                    if (!int.TryParse(rest[1], out index) || index < 0 || index >= displays.Count)
                    {
                        Console.Error.WriteLine("Bad monitor index. Use --list to see valid ones.");
                        return 2;
                    }
                    rest.RemoveRange(0, 2);
                }

                Display display = displays[index];

                uint current, max;
                if (!display.TryRead(Display.VcpLuminance, out current, out max))
                {
                    Console.Error.WriteLine("Could not read brightness from " + display.Name +
                                            ". Is DDC/CI enabled in the monitor OSD menu?");
                    return 3;
                }

                if (rest.Count == 0)
                {
                    Console.WriteLine(current + "   (0.." + max + " on " + display.Name + ")");
                    return 0;
                }

                uint target;
                if (!TryResolveTarget(rest[0], current, max, out target))
                {
                    Usage();
                    return 2;
                }

                if (!display.TryWrite(Display.VcpLuminance, target))
                {
                    Console.Error.WriteLine("Failed to set brightness to " + target + " after repeated attempts.");
                    return 4;
                }

                Console.WriteLine(current + " -> " + target);
                return 0;
            }
        }

        /// <summary>Parses an absolute ("70") or relative ("+10", "-10") argument, clamped to 0..max.</summary>
        private static bool TryResolveTarget(string arg, uint current, uint max, out uint target)
        {
            target = 0;
            if (string.IsNullOrEmpty(arg))
                return false;

            bool relative = arg[0] == '+' || arg[0] == '-';
            int parsed;
            if (!int.TryParse(arg, System.Globalization.NumberStyles.AllowLeadingSign,
                              System.Globalization.CultureInfo.InvariantCulture, out parsed))
                return false;

            long value = relative ? (long)current + parsed : parsed;
            if (value < 0) value = 0;
            if (value > max) value = max;

            target = (uint)value;
            return true;
        }

        private static int List(DisplaySet displays)
        {
            for (int i = 0; i < displays.Count; i++)
            {
                uint current, max;
                string state = displays[i].TryRead(Display.VcpLuminance, out current, out max)
                    ? current + "/" + max
                    : "no DDC/CI";
                Console.WriteLine(i + ": " + displays[i].Name + "   [" + state + "]");
            }
            return 0;
        }

        private static void Usage()
        {
            Console.Error.WriteLine("usage: bright [--monitor N] [value|+delta|-delta]");
            Console.Error.WriteLine("       bright --list");
        }
    }
}
