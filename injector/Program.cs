// BO2 MVM launcher / injector.  Injects bo2mvm.dll into a running Plutonium t6zm.exe (Zombies; t6mp also works) that was started with -lan.
using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Management;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows.Forms;

namespace Bo2Mvm
{
    static class Native
    {
        public const uint PROCESS_ALL_ACCESS = 0x1F0FFF;
        public const uint MEM_COMMIT = 0x1000, MEM_RESERVE = 0x2000, MEM_RELEASE = 0x8000, PAGE_READWRITE = 0x04;

        [DllImport("kernel32.dll", SetLastError = true)] public static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
        [DllImport("kernel32.dll", SetLastError = true)] public static extern IntPtr VirtualAllocEx(IntPtr h, IntPtr addr, uint size, uint type, uint prot);
        [DllImport("kernel32.dll", SetLastError = true)] public static extern bool VirtualFreeEx(IntPtr h, IntPtr addr, uint size, uint type);
        [DllImport("kernel32.dll", SetLastError = true)] public static extern bool WriteProcessMemory(IntPtr h, IntPtr addr, byte[] buf, uint size, out UIntPtr written);
        [DllImport("kernel32.dll", SetLastError = true)] public static extern IntPtr CreateRemoteThread(IntPtr h, IntPtr attr, uint stack, IntPtr start, IntPtr param, uint flags, out uint id);
        [DllImport("kernel32.dll", CharSet = CharSet.Ansi, SetLastError = true)] public static extern IntPtr GetProcAddress(IntPtr mod, string name);
        [DllImport("kernel32.dll", CharSet = CharSet.Ansi, SetLastError = true)] public static extern IntPtr GetModuleHandleA(string name);
        [DllImport("kernel32.dll", SetLastError = true)] public static extern uint WaitForSingleObject(IntPtr h, uint ms);
        [DllImport("kernel32.dll", SetLastError = true)] public static extern bool GetExitCodeThread(IntPtr h, out uint code);
        [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
        [DllImport("kernel32.dll", SetLastError = true)] public static extern bool IsWow64Process(IntPtr h, out bool wow64);
    }

    static class Injector
    {
        // Returns null on success, otherwise an error message.
        public static string Inject(Process p, string dllPath)
        {
            IntPtr h = Native.OpenProcess(Native.PROCESS_ALL_ACCESS, false, p.Id);
            if (h == IntPtr.Zero) return "OpenProcess failed (" + Marshal.GetLastWin32Error() + "). Run this launcher as the same user as the game.";
            try
            {
                if (Environment.Is64BitProcess) return "Launcher is running as 64 bit. Use the x86 build.";
                byte[] path = Encoding.ASCII.GetBytes(dllPath + "\0");
                IntPtr mem = Native.VirtualAllocEx(h, IntPtr.Zero, (uint)path.Length, Native.MEM_COMMIT | Native.MEM_RESERVE, Native.PAGE_READWRITE);
                if (mem == IntPtr.Zero) return "VirtualAllocEx failed.";
                UIntPtr w;
                if (!Native.WriteProcessMemory(h, mem, path, (uint)path.Length, out w)) return "WriteProcessMemory failed.";
                IntPtr load = Native.GetProcAddress(Native.GetModuleHandleA("kernel32.dll"), "LoadLibraryA");
                uint tid;
                IntPtr th = Native.CreateRemoteThread(h, IntPtr.Zero, 0, load, mem, 0, out tid);
                if (th == IntPtr.Zero) return "CreateRemoteThread failed (" + Marshal.GetLastWin32Error() + ").";
                Native.WaitForSingleObject(th, 10000);
                uint code;
                Native.GetExitCodeThread(th, out code);
                Native.CloseHandle(th);
                Native.VirtualFreeEx(h, mem, 0, Native.MEM_RELEASE);
                return code == 0 ? "LoadLibrary returned NULL - missing DLL or wrong architecture (needs the x86 build)." : null;
            }
            finally { Native.CloseHandle(h); }
        }

        public static bool AlreadyLoaded(Process p, string dllName)
        {
            try { return p.Modules.Cast<ProcessModule>().Any(m => string.Equals(m.ModuleName, dllName, StringComparison.OrdinalIgnoreCase)); }
            catch { return false; }
        }

        public static string CommandLine(int pid)
        {
            try
            {
                using (var s = new ManagementObjectSearcher("SELECT CommandLine FROM Win32_Process WHERE ProcessId = " + pid))
                    foreach (ManagementObject o in s.Get()) return (o["CommandLine"] as string) ?? "";
            }
            catch { }
            return null;
        }
    }

    class MainForm : Form
    {
        readonly Label status = new Label();
        readonly Button inject = new Button();
        readonly CheckBox auto = new CheckBox();
        readonly Timer timer = new Timer();
        readonly string dll;
        int lastInjectedPid = -1;

        public MainForm()
        {
            dll = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "bo2mvm.dll");
            Text = "BO2 MVM";
            ClientSize = new Size(380, 190);
            FormBorderStyle = FormBorderStyle.FixedSingle;
            MaximizeBox = false;
            StartPosition = FormStartPosition.CenterScreen;
            BackColor = Color.FromArgb(18, 18, 18);
            ForeColor = Color.FromArgb(220, 220, 214);
            Font = new Font("Segoe UI", 9f);

            var title = new Label { Text = "BO2 MVM", Font = new Font("Segoe UI", 20f, FontStyle.Bold), AutoSize = true, Location = new Point(16, 10) };
            var sub = new Label { Text = "movie maker tool  -  Plutonium T6 LAN only", ForeColor = Color.FromArgb(130, 130, 126), AutoSize = true, Location = new Point(18, 50) };
            status.Location = new Point(18, 78); status.Size = new Size(346, 44); status.Text = "Waiting for t6zm.exe (BO2 Zombies) ...";
            inject.Text = "Inject"; inject.Location = new Point(18, 126); inject.Size = new Size(150, 36);
            inject.FlatStyle = FlatStyle.Flat; inject.BackColor = Color.FromArgb(40, 40, 40); inject.FlatAppearance.BorderColor = Color.FromArgb(110, 110, 105);
            inject.Click += (s, e) => DoInject(true);
            auto.Text = "Auto-inject when the game is ready"; auto.Checked = true; auto.AutoSize = true; auto.Location = new Point(184, 134);
            Controls.AddRange(new Control[] { title, sub, status, inject, auto });

            timer.Interval = 1500;
            timer.Tick += (s, e) => { if (auto.Checked) DoInject(false); else Refresh2(); };
            timer.Start();
            if (!File.Exists(dll)) status.Text = "bo2mvm.dll not found next to the launcher.";
        }

        Process FindGame()
        {
            // direct t6zm / t6mp process (if Plutonium ever runs it standalone)
            foreach (var n in new[] { "t6zm", "t6mp" })
            {
                var ps = Process.GetProcessesByName(n);
                if (ps.Length > 0) return ps[0];
            }
            // normal case: the game runs inside the Plutonium bootstrapper, command line has "t6zm" / "t6mp"
            Process mp = null;
            foreach (var p in Process.GetProcessesByName("plutonium-bootstrapper-win32"))
            {
                string cl = Injector.CommandLine(p.Id) ?? "";
                if (cl.IndexOf("t6zm", StringComparison.OrdinalIgnoreCase) >= 0) return p;
                if (cl.IndexOf("t6mp", StringComparison.OrdinalIgnoreCase) >= 0) mp = p;
            }
            return mp;
        }

        void Refresh2()
        {
            var p = FindGame();
            status.Text = p == null ? "Waiting for t6zm.exe (BO2 Zombies) ..." : "BO2 found (" + p.ProcessName + ", pid " + p.Id + ").";
        }

        void DoInject(bool manual)
        {
            if (!File.Exists(dll)) { status.Text = "bo2mvm.dll not found next to the launcher."; return; }
            var p = FindGame();
            if (p == null) { status.Text = "Waiting for t6zm.exe (BO2 Zombies) ..."; return; }
            if (!manual && p.Id == lastInjectedPid) return;
            if (Injector.AlreadyLoaded(p, "bo2mvm.dll")) { lastInjectedPid = p.Id; status.Text = "Already loaded in pid " + p.Id + ".  Press INSERT in game."; return; }

            // (the DLL itself verifies LAN mode and tells you if it refuses)
            string err = Injector.Inject(p, dll);
            try { File.AppendAllText(Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "bo2mvm_injector_log.txt"), DateTime.Now + "  pid " + p.Id + " (" + p.ProcessName + "): " + (err ?? "injected OK") + "\r\n"); } catch { }
            if (err != null) { status.Text = "Failed: " + err; return; }
            lastInjectedPid = p.Id;
            status.Text = "Injected into pid " + p.Id + ".  Press INSERT in game for the menu, F2 for freecam.";
        }
    }

    static class Program
    {
        [STAThread]
        static void Main()
        {
            Application.EnableVisualStyles();
            Application.Run(new MainForm());
        }
    }
}
