# Print the visible text of another process's console window.
# Usage: pwsh -NoProfile -File peek-console.ps1 -ProcessId <pid>
param([int]$ProcessId)

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class ConPeek {
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool FreeConsole();
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool AttachConsole(uint pid);
    [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    static extern IntPtr CreateFileW(string name, uint access, uint share, IntPtr sec, uint disp, uint flags, IntPtr tmpl);
    [StructLayout(LayoutKind.Sequential)] struct COORD { public short X; public short Y; }
    [StructLayout(LayoutKind.Sequential)] struct SMALL_RECT { public short L, T, R, B; }
    [StructLayout(LayoutKind.Sequential)] struct CSBI { public COORD size; public COORD cursor; public ushort attr; public SMALL_RECT win; public COORD max; }
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool GetConsoleScreenBufferInfo(IntPtr h, out CSBI info);
    [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    static extern bool ReadConsoleOutputCharacterW(IntPtr h, StringBuilder buf, uint len, COORD at, out uint read);
    public static string Read(uint pid) {
        FreeConsole();
        if (!AttachConsole(pid)) return "AttachConsole failed: " + Marshal.GetLastWin32Error();
        IntPtr h = CreateFileW("CONOUT$", 0xC0000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
        CSBI info;
        if (!GetConsoleScreenBufferInfo(h, out info)) return "GetConsoleScreenBufferInfo failed: " + Marshal.GetLastWin32Error();
        var sb = new StringBuilder();
        for (short y = info.win.T; y <= info.win.B; y++) {
            int w = info.win.R - info.win.L + 1;
            var line = new StringBuilder(w);
            uint read;
            ReadConsoleOutputCharacterW(h, line, (uint)w, new COORD { X = info.win.L, Y = y }, out read);
            sb.AppendLine(line.ToString(0, (int)read).TrimEnd());
        }
        FreeConsole();
        return sb.ToString();
    }
}
"@
[ConPeek]::Read([uint32]$ProcessId)
