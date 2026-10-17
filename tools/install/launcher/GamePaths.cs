using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using Microsoft.Win32;

namespace OptScalerNR.Launcher
{
    static class GamePaths
    {
        const uint InvalidAttributes = 0xFFFFFFFF;
        const uint DirectoryFlag = 0x10;
        const uint ReparseFlag = 0x400;
        const int ErrorAccessDenied = 5;

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern uint GetFileAttributesW(string lpFileName);

        public static bool CanUse(string path)
        {
            if (string.IsNullOrEmpty(path)) return false;
            uint attr = GetFileAttributesW(path);
            if (attr != InvalidAttributes) return (attr & DirectoryFlag) == 0;
            return Marshal.GetLastWin32Error() == ErrorAccessDenied;
        }

        public static string Normalize(string path)
        {
            if (string.IsNullOrEmpty(path)) return path;
            try { path = Path.GetFullPath(path); }
            catch { return path; }
            if (path.IndexOf("\\WindowsApps\\", StringComparison.OrdinalIgnoreCase) < 0)
                return path;
            var name = Path.GetFileName(path);
            if (string.IsNullOrEmpty(name)) return path;
            var mapped = FindUnderXboxGames(name);
            return mapped ?? path;
        }

        public static string PickExe()
        {
            var dlg = new OpenFileDialog();
            dlg.Filter = "Game executable (*.exe)|*.exe";
            dlg.CheckFileExists = false;
            dlg.CheckPathExists = true;
            dlg.DereferenceLinks = false;
            dlg.AddExtension = false;
            dlg.ValidateNames = false;
            if (Directory.Exists(@"C:\XboxGames"))
                dlg.InitialDirectory = @"C:\XboxGames";
            if (dlg.ShowDialog() != true) return null;
            return Normalize(dlg.FileName);
        }

        static string FindUnderXboxGames(string fileName)
        {
            var hits = new List<string>();
            foreach (var drive in Directory.GetLogicalDrives())
            {
                var root = Path.Combine(drive, "XboxGames");
                uint attr = GetFileAttributesW(root);
                if (attr == InvalidAttributes || (attr & DirectoryFlag) == 0) continue;
                Walk(root, fileName, 0, hits);
                if (hits.Count >= 32) break;
            }
            if (hits.Count == 0) return null;
            hits.Sort(CompareXboxHits);
            return hits[0];
        }

        static int CompareXboxHits(string a, string b)
        {
            int ra = XboxRank(a).CompareTo(XboxRank(b));
            if (ra != 0) return ra;
            return a.Length.CompareTo(b.Length);
        }

        static int XboxRank(string path)
        {
            if (path.IndexOf("\\WinGDK\\", StringComparison.OrdinalIgnoreCase) >= 0) return 0;
            if (path.IndexOf("\\Win64\\", StringComparison.OrdinalIgnoreCase) >= 0) return 1;
            if (path.IndexOf("\\Binaries\\", StringComparison.OrdinalIgnoreCase) >= 0) return 2;
            return 3;
        }

        static void Walk(string dir, string fileName, int depth, List<string> hits)
        {
            if (depth > 8 || hits.Count >= 32) return;
            string[] entries;
            try { entries = Directory.GetFileSystemEntries(dir); }
            catch { return; }
            foreach (var entry in entries)
            {
                uint attr = GetFileAttributesW(entry);
                if (attr == InvalidAttributes) continue;
                bool isDir = (attr & DirectoryFlag) != 0;
                bool reparse = (attr & ReparseFlag) != 0;
                if (isDir)
                {
                    if (reparse) continue;
                    Walk(entry, fileName, depth + 1, hits);
                    continue;
                }
                if (string.Equals(Path.GetFileName(entry), fileName, StringComparison.OrdinalIgnoreCase))
                    hits.Add(entry);
            }
        }
    }
}
