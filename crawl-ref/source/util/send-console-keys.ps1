# Type keys into another process's console window (Windows console builds).
# Together with peek-console.ps1 this lets a script drive menus that run
# before any game-side Lua can (e.g. a party member's character choice).
#
# Usage: pwsh -NoProfile -File send-console-keys.ps1 -ProcessId <pid> -Keys "ab"
#   "{ENTER}" and "{ESC}" in -Keys send Enter / Escape.
param([int]$ProcessId, [string]$Keys)

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class ConKeys {
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool FreeConsole();
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool AttachConsole(uint pid);
    [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    static extern IntPtr CreateFileW(string name, uint access, uint share, IntPtr sec, uint disp, uint flags, IntPtr tmpl);
    [StructLayout(LayoutKind.Explicit, CharSet=CharSet.Unicode)]
    struct KEY_EVENT_RECORD {
        [FieldOffset(0)] public int bKeyDown;
        [FieldOffset(4)] public ushort wRepeatCount;
        [FieldOffset(6)] public ushort wVirtualKeyCode;
        [FieldOffset(8)] public ushort wVirtualScanCode;
        [FieldOffset(10)] public char UnicodeChar;
        [FieldOffset(12)] public uint dwControlKeyState;
    }
    [StructLayout(LayoutKind.Explicit)]
    struct INPUT_RECORD {
        [FieldOffset(0)] public ushort EventType;
        [FieldOffset(4)] public KEY_EVENT_RECORD KeyEvent;
    }
    [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    static extern bool WriteConsoleInputW(IntPtr h, INPUT_RECORD[] recs, uint n, out uint written);
    [DllImport("user32.dll")] static extern short VkKeyScanW(char ch);

    public static string Send(uint pid, string keys) {
        FreeConsole();
        if (!AttachConsole(pid)) return "AttachConsole failed: " + Marshal.GetLastWin32Error();
        IntPtr h = CreateFileW("CONIN$", 0xC0000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
        var recs = new System.Collections.Generic.List<INPUT_RECORD>();
        foreach (char ch in keys) {
            ushort vk = ch == '\r' ? (ushort)0x0D : ch == (char)27 ? (ushort)0x1B
                      : (ushort)(VkKeyScanW(ch) & 0xff);
            foreach (int down in new[] { 1, 0 }) {
                var r = new INPUT_RECORD { EventType = 1 };
                r.KeyEvent.bKeyDown = down;
                r.KeyEvent.wRepeatCount = 1;
                r.KeyEvent.wVirtualKeyCode = vk;
                r.KeyEvent.UnicodeChar = ch;
                recs.Add(r);
            }
        }
        uint written;
        bool ok = WriteConsoleInputW(h, recs.ToArray(), (uint)recs.Count, out written);
        FreeConsole();
        return ok ? "sent " + keys.Length + " key(s)" : "WriteConsoleInput failed: " + Marshal.GetLastWin32Error();
    }
}
"@
$k = $Keys.Replace("{ENTER}", "`r").Replace("{ESC}", [string][char]27)
[ConKeys]::Send([uint32]$ProcessId, $k)
