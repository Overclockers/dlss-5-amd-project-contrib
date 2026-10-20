# Shared Original/Small weight validation and model-granular installation.
function Assert-LmxxfModelPath([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path)
    $at = $full
    while ($at) {
        if (Test-Path -LiteralPath $at) {
            if ([IO.File]::GetAttributes($at) -band [IO.FileAttributes]::ReparsePoint) { throw "Linked model path: $at" }
        }
        $at = [IO.Path]::GetDirectoryName($at)
    }
    return $full
}

function Get-LmxxfAssetHash([string]$Path) {
    $hash = [Security.Cryptography.SHA256]::Create()
    $stream = $null
    try {
        $stream = [IO.File]::OpenRead($Path)
        return ([BitConverter]::ToString($hash.ComputeHash($stream))).Replace('-', '').ToLowerInvariant()
    } finally { if ($stream) { $stream.Dispose() }; $hash.Dispose() }
}

function Get-LmxxfOriginalLayout {
    # Logical element counts match hip_reference::Network::WeightElements.
    $layout = @{}
    for ($b = 0; $b -lt 70; $b++) {
        $c = if ($b -le 4) {32} elseif ($b -le 8) {64} elseif ($b -le 14) {128} elseif ($b -le 22) {256} elseif ($b -le 30) {512} elseif ($b -le 38) {1024} elseif ($b -le 47) {512} elseif ($b -le 55) {256} elseif ($b -le 61) {128} elseif ($b -le 65) {64} else {32}
        if ($b -eq 39) { $layout['decoder39-weights'] = 524800; continue }
        if ($b -ge 31 -and $b -le 38) {
            foreach ($pair in @(@('expand',4194304),@('contract',4195328),@('qkv',3145760),@('projection',1049600))) { $layout["block$b-$($pair[0])"] = $pair[1] }
        } else {
            $layout["block$b-attention"] = if ($c -eq 32) {8225} else {4*$c*$c+($c/32)*4096+$c/32+$c}
            if ($c -eq 512) { $layout["block$b-ffwd"] = 524288; $layout["block$b-ffwd-projection"] = 262656 }
            else { $layout["block$b-ffn"] = if ($c -eq 32) {8736} else {9*$c*$c+$c} }
        }
        if ($b -in @(4,8,14,22)) { $layout["block$b-ds"] = 2*$c*$c }
        if ($b -in @(48,56,62,66)) { $layout["block$b-weights"] = 2*$c*$c+$c }
    }
    # Original history uses the host's embedded row (NativePostWeights.h). The
    # separate post70-history-head asset belongs to Small, not the Original layout.
    foreach ($pair in @(@('head-matrix',524288),@('post70-scales',64),@('post70-head',96),@('post70-ffn',8736),@('post70-attention',8225))) { $layout[$pair[0]] = $pair[1] }
    return $layout
}

function Get-LmxxfModelFiles {
    param([string]$Directory, [ValidateSet('original','small')][string]$Model, [switch]$Quick)
    $root = Assert-LmxxfModelPath $Directory
    if (-not (Test-Path -LiteralPath $root -PathType Container)) { throw "Missing lmxxf $Model weights: $root" }
    if ($Model -eq 'original') {
        $layout = Get-LmxxfOriginalLayout
        foreach ($name in @($layout.Keys | Sort-Object)) {
            $file = Join-Path $root ($name + '.f32'); $bytes = [long]$layout[$name]*4
            if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { $file = Join-Path $root ($name + '.f16'); $bytes = [long]$layout[$name]*2 }
            $file = Assert-LmxxfModelPath $file
            if (-not (Test-Path -LiteralPath $file -PathType Leaf) -or (Get-Item -LiteralPath $file).Length -ne $bytes) { throw "Missing or invalid lmxxf Original tensor: $file (expected $bytes bytes)" }
            [pscustomobject]@{ Name = [IO.Path]::GetFileName($file); Path = $file }
        }
        foreach ($name in @('noise.f32','normalized-output.f32')) {
            $file = Join-Path $root $name
            if (Test-Path -LiteralPath $file -PathType Leaf) { $null = Assert-LmxxfModelPath $file; [pscustomobject]@{Name=$name; Path=$file} }
        }
    } else {
        $manifest = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'lmxxf-small-manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json
        $metadataPath = Assert-LmxxfModelPath (Join-Path $root 'model-manifest.json')
        $text = Get-Content -LiteralPath $metadataPath -Raw -Encoding UTF8
        $metadata = $text | ConvertFrom-Json
        if ($metadata.model -ne 'small' -or $metadata.source_dll_sha256 -ne $manifest.source_dll_sha256 -or -not $text.Contains('"model": "small"')) { throw "Invalid lmxxf Small model manifest: $metadataPath" }
        foreach ($entry in $manifest.files.PSObject.Properties) {
            $file = Assert-LmxxfModelPath (Join-Path $root $entry.Name)
            if ([IO.Path]::GetExtension($file) -eq '.f16' -and (Test-Path -LiteralPath ([IO.Path]::ChangeExtension($file,'.f32')))) { throw "Unverified f32 tensor shadows lmxxf Small: $file" }
            if (-not (Test-Path -LiteralPath $file -PathType Leaf) -or (Get-Item -LiteralPath $file).Length -ne $entry.Value.bytes) { throw "Missing or invalid lmxxf Small tensor: $file" }
            if (-not $Quick -and (Get-LmxxfAssetHash $file) -ne $entry.Value.sha256) { throw "lmxxf Small tensor hash mismatch: $file" }
            [pscustomobject]@{Name=$entry.Name; Path=$file}
        }
        [pscustomobject]@{Name='model-manifest.json'; Path=$metadataPath}
    }
}

function Test-LmxxfModel {
    param([string]$Directory, [ValidateSet('original','small')][string]$Model, [switch]$Quick)
    try { $null = @(Get-LmxxfModelFiles $Directory $Model -Quick:$Quick); return $true } catch { return $false }
}

function Copy-LmxxfModel {
    param([string]$Source, [string]$Destination, [ValidateSet('original','small')][string]$Model)
    $Source = Assert-LmxxfModelPath $Source; $Destination = Assert-LmxxfModelPath $Destination
    if (Test-LmxxfModel $Destination $Model) { return }
    $files = @(Get-LmxxfModelFiles $Source $Model)
    if ($Source -ieq $Destination) { throw "Invalid lmxxf $Model destination is also the source." }
    $parent = [IO.Path]::GetDirectoryName($Destination)
    [void][IO.Directory]::CreateDirectory($parent)
    $stage = Join-Path $parent ('.lmxxf-model-' + [guid]::NewGuid().ToString('N'))
    [void][IO.Directory]::CreateDirectory($stage)
    try {
        foreach ($file in $files) {
            $target = Join-Path $stage $file.Name
            Copy-Item -LiteralPath $file.Path -Destination $target
            if ((Get-LmxxfAssetHash $file.Path) -ne (Get-LmxxfAssetHash $target)) { throw "Model copy mismatch: $($file.Name)" }
        }
        if (-not (Test-LmxxfModel $stage $Model)) { throw "Staged lmxxf $Model weights failed validation." }
        [void][IO.Directory]::CreateDirectory($Destination)
        foreach ($file in $files) {
            $target = Assert-LmxxfModelPath (Join-Path $Destination $file.Name)
            if ([IO.Path]::GetExtension($target) -eq '.f16') {
                $alternate = [IO.Path]::ChangeExtension($target,'.f32')
                if (Test-Path -LiteralPath $alternate) { $null = Assert-LmxxfModelPath $alternate; Move-Item -LiteralPath $alternate -Destination ($alternate + '.mismatch-' + [guid]::NewGuid().ToString('N')) }
            }
            if ((Test-Path -LiteralPath $target -PathType Leaf) -and (Get-LmxxfAssetHash $target) -eq (Get-LmxxfAssetHash (Join-Path $stage $file.Name))) { continue }
            if (Test-Path -LiteralPath $target) { Move-Item -LiteralPath $target -Destination ($target + '.mismatch-' + [guid]::NewGuid().ToString('N')) }
            Move-Item -LiteralPath (Join-Path $stage $file.Name) -Destination $target
        }
        if (-not (Test-LmxxfModel $Destination $Model)) { throw "Installed lmxxf $Model weights failed validation." }
    } finally {
        if (Test-Path -LiteralPath $stage) {
            $clean = Assert-LmxxfModelPath $stage
            if ([IO.Path]::GetDirectoryName($clean) -ine $parent -or [IO.Path]::GetFileName($clean) -notmatch '^\.lmxxf-model-[0-9a-f]{32}$') { throw "Unexpected model staging directory: $clean" }
            Remove-Item -LiteralPath $clean -Recurse -Force
        }
    }
}

# Small and Original assets can be prepared natively without Python.
function Ensure-NativeModelExtractor {
    if ('OptScaler.NativeModelExtractor' -as [type]) { return }
    $csharp = @'
using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Security.Cryptography;
using System.Text;

namespace OptScaler
{
    public static class NativeModelExtractor
    {
        public const string Dll3108Sha256 = "e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e";
        public const string Dll3109Sha256 = "2bc19bc09a7b89dd44f73e2ac0108fcc586e2154bdd0830f242f4f040e28789f";
        public const string OriginalPackSha256 = "2b41c888cf4155b8958c665ba64018ab0bd25c85fc71a2b6db86d0d04d1f7fbd";
        public const string SmallPackSha256 = "7e966e2639dbec6596aa0bac3b8761137d2c40ca8cb4aca30df7ca10043c6e33";
        public const int OriginalEntriesCount = 599;
        public const int SmallEntriesCount = 255;
        public const long OriginalPackSize = 147756560L;
        public const long SmallPackSize = 113917248L;

        private static readonly float[] E4M3 = BuildE4M3Table();
        private static readonly int[] C32Order = new int[] {
            0,1,4,5,8,9,12,13,2,3,6,7,10,11,14,15,16,17,20,21,24,25,28,29,18,19,22,23,26,27,30,31
        };
        private static readonly int[] SmallLogicalToLegacyRole = new int[] {
            0, 1, 4, 5, 8, 9, 14, 15, 22, 23, 30,
            31, 32, 33, 34, 35, 36, 37, 38, 39,
            40, 47, 48, 55, 56, 61, 62, 65, 66, 69, 70
        };

        private static float[] BuildE4M3Table()
        {
            float[] table = new float[256];
            for (int b = 0; b < 256; b++)
            {
                float s = (b & 0x80) != 0 ? -1.0f : 1.0f;
                int e = (b >> 3) & 0x0F;
                int m = b & 0x07;
                if (e == 0x0F && m == 0x07)
                    table[b] = float.NaN;
                else if (e == 0)
                    table[b] = (float)(s * m * Math.Pow(2.0, -9));
                else
                    table[b] = (float)(s * (1.0 + m / 8.0) * Math.Pow(2.0, e - 7));
            }
            return table;
        }

        public static bool CanExtractMochizuki(string sourcePath)
        {
            if (string.IsNullOrEmpty(sourcePath) || !File.Exists(sourcePath)) return false;
            try
            {
                FileInfo fi = new FileInfo(sourcePath);
                if (fi.Length < 65536) return false;
                using (FileStream fs = File.OpenRead(sourcePath))
                {
                    int b0 = fs.ReadByte();
                    int b1 = fs.ReadByte();
                    return (b0 == 'M' && b1 == 'Z') || (b0 == 'P' && b1 == 'K');
                }
            }
            catch { return false; }
        }

        public static string ComputeSha256(byte[] data, int offset, int count)
        {
            using (SHA256 sha = SHA256.Create())
            {
                byte[] hash = sha.ComputeHash(data, offset, count);
                return ToHex(hash);
            }
        }

        public static string ComputeSha256(byte[] data)
        {
            return ComputeSha256(data, 0, data.Length);
        }

        public static string ComputeFileSha256(string path)
        {
            using (SHA256 sha = SHA256.Create())
            using (FileStream fs = File.OpenRead(path))
            {
                return ToHex(sha.ComputeHash(fs));
            }
        }

        private static string ToHex(byte[] bytes)
        {
            char[] c = new char[bytes.Length * 2];
            for (int i = 0; i < bytes.Length; i++)
            {
                int b = bytes[i] >> 4;
                c[i * 2] = (char)(b > 9 ? b + 87 : b + 48);
                b = bytes[i] & 0x0F;
                c[i * 2 + 1] = (char)(b > 9 ? b + 87 : b + 48);
            }
            return new string(c);
        }

        public sealed class WeightRecord
        {
            public string Name;
            public int PayloadOffset;
            public int PayloadSize;
        }

        private sealed class PeSection
        {
            public uint Rva;
            public uint VirtualSize;
            public uint FileOffset;
            public uint FileSize;
        }

        public sealed class PeResource
        {
            public string[] Path;
            public int FileOffset;
            public int Size;
        }

        private static void CheckBounds(byte[] data, long offset, long size)
        {
            if (offset < 0 || size < 0 || offset + size > data.Length)
                throw new InvalidDataException(string.Format("Out of bounds: offset=0x{0:X}, size={1}, buffer={2}", offset, size, data.Length));
        }

        private static ushort ReadU16(byte[] d, int o) { CheckBounds(d, o, 2); return BitConverter.ToUInt16(d, o); }
        private static uint ReadU32(byte[] d, int o) { CheckBounds(d, o, 4); return BitConverter.ToUInt32(d, o); }
        private static ulong ReadU64(byte[] d, int o) { CheckBounds(d, o, 8); return BitConverter.ToUInt64(d, o); }

        public static List<PeResource> ParsePeResources(byte[] data)
        {
            if (data.Length < 64 || data[0] != (byte)'M' || data[1] != (byte)'Z')
                throw new InvalidDataException("Not a PE file");
            int pe = (int)ReadU32(data, 0x3C);
            CheckBounds(data, pe, 24);
            if (data[pe] != (byte)'P' || data[pe + 1] != (byte)'E' || data[pe + 2] != 0 || data[pe + 3] != 0)
                throw new InvalidDataException("Missing PE signature");
            int count = ReadU16(data, pe + 6);
            int optSize = ReadU16(data, pe + 20);
            int opt = pe + 24;
            ushort magic = ReadU16(data, opt);
            if (magic != 0x20B || optSize < 144)
                throw new InvalidDataException("Expected PE32+ optional header");
            uint resRva = ReadU32(data, opt + 128);
            uint resSize = ReadU32(data, opt + 132);
            List<PeSection> sections = new List<PeSection>(count);
            for (int i = 0; i < count; i++)
            {
                int o = opt + optSize + 40 * i;
                uint vs = ReadU32(data, o + 8);
                uint va = ReadU32(data, o + 12);
                uint sz = ReadU32(data, o + 16);
                uint raw = ReadU32(data, o + 20);
                CheckBounds(data, raw, sz);
                sections.Add(new PeSection { Rva = va, VirtualSize = vs, FileOffset = raw, FileSize = sz });
            }
            List<PeResource> result = new List<PeResource>();
            if (resRva == 0) return result;
            int root = RvaToOffset(sections, resRva, resSize);
            CheckBounds(data, root, resSize);
            HashSet<int> active = new HashSet<int>();
            WalkResourceTree(data, root, (int)resSize, sections, 0, new List<string>(), active, result);
            return result;
        }

        private static int RvaToOffset(List<PeSection> sections, uint rva, uint size)
        {
            for (int i = 0; i < sections.Count; i++)
            {
                PeSection s = sections[i];
                if (rva >= s.Rva)
                {
                    ulong delta = (ulong)(rva - s.Rva);
                    if (delta + size <= s.FileSize)
                        return (int)(s.FileOffset + delta);
                }
            }
            throw new InvalidDataException(string.Format("RVA not backed by file: 0x{0:X}, size={1}", rva, size));
        }

        private static void WalkResourceTree(
            byte[] data, int treeRoot, int treeSize, List<PeSection> sections,
            int rel, List<string> path, HashSet<int> active, List<PeResource> result)
        {
            if (active.Contains(rel) || path.Count > 8)
                throw new InvalidDataException("Cyclic or excessively deep resource tree");
            active.Add(rel);
            int named = ReadU16(data, treeRoot + rel + 12);
            int ids = ReadU16(data, treeRoot + rel + 14);
            for (int i = 0; i < named + ids; i++)
            {
                uint name = ReadU32(data, treeRoot + rel + 16 + 8 * i);
                uint child = ReadU32(data, treeRoot + rel + 16 + 8 * i + 4);
                string label;
                if ((name & 0x80000000u) != 0)
                {
                    int o = (int)(name & 0x7FFFFFFFu);
                    int n = ReadU16(data, treeRoot + o);
                    CheckBounds(data, treeRoot + o + 2, n * 2);
                    label = Encoding.Unicode.GetString(data, treeRoot + o + 2, n * 2);
                }
                else
                {
                    label = name.ToString();
                }
                List<string> nextPath = new List<string>(path);
                nextPath.Add(label);
                if ((child & 0x80000000u) != 0)
                {
                    WalkResourceTree(data, treeRoot, treeSize, sections, (int)(child & 0x7FFFFFFFu), nextPath, active, result);
                }
                else
                {
                    int c = (int)child;
                    uint rva = ReadU32(data, treeRoot + c);
                    uint size = ReadU32(data, treeRoot + c + 4);
                    int offset = RvaToOffset(sections, rva, size);
                    CheckBounds(data, offset, size);
                    result.Add(new PeResource { Path = nextPath.ToArray(), FileOffset = offset, Size = (int)size });
                }
            }
            active.Remove(rel);
        }

        public static List<WeightRecord> ParseWeightResource(byte[] dllData, string resourceName)
        {
            List<PeResource> resources = ParsePeResources(dllData);
            PeResource match = null;
            for (int i = 0; i < resources.Count; i++)
            {
                if (Array.IndexOf(resources[i].Path, resourceName) >= 0)
                {
                    if (match != null)
                        throw new InvalidDataException("Expected exactly one " + resourceName + " resource");
                    match = resources[i];
                }
            }
            if (match == null)
                throw new InvalidDataException("Expected resource " + resourceName + " not found");

            int baseOff = match.FileOffset;
            int totalLen = match.Size;
            ulong total = ReadU64(dllData, baseOff);
            if (total != (ulong)totalLen)
                throw new InvalidDataException("Weight map size mismatch");

            int p = 8;
            List<WeightRecord> records = new List<WeightRecord>();
            HashSet<string> names = new HashSet<string>(StringComparer.Ordinal);
            while (p < totalLen)
            {
                ulong nameLenU = ReadU64(dllData, baseOff + p);
                p += 8;
                if (nameLenU == 0 || nameLenU > 4096)
                    throw new InvalidDataException("Invalid weight name length");
                int nameLen = (int)nameLenU;
                CheckBounds(dllData, baseOff + p, nameLen);
                string name = Encoding.UTF8.GetString(dllData, baseOff + p, nameLen);
                if (!names.Add(name))
                    throw new InvalidDataException("Duplicate weight name: " + name);
                p += nameLen;
                ulong outerSizeU = ReadU64(dllData, baseOff + p);
                p += 8;
                int outerSize = (int)outerSizeU;
                CheckBounds(dllData, baseOff + p, outerSize);
                ulong innerSize = ReadU64(dllData, baseOff + p);
                ulong rawSize = ReadU64(dllData, baseOff + p + 8);
                uint device = ReadU32(dllData, baseOff + p + 16);
                if (innerSize != outerSizeU || rawSize == 0 || (device != 0 && device != 1))
                    throw new InvalidDataException("Invalid weight envelope for " + name);
                ulong ndim = ReadU64(dllData, baseOff + p + 20 + (int)rawSize + 8);
                if (ndim > 32 || 36UL + rawSize + 4UL * ndim != innerSize)
                    throw new InvalidDataException("Invalid weight metadata for " + name);
                records.Add(new WeightRecord
                {
                    Name = name,
                    PayloadOffset = baseOff + p + 20,
                    PayloadSize = (int)rawSize
                });
                p += outerSize;
            }
            if (p != totalLen)
                throw new InvalidDataException("Weight map did not end at resource boundary");
            return records;
        }

        public static string ExtractDanielRuntime(string setupExePath, string outputDllPath)
        {
            byte[] exe = File.ReadAllBytes(setupExePath);
            HashSet<string> knownDanielHashes = new HashSet<string>(StringComparer.OrdinalIgnoreCase)
            {
                "8321cae728d28cb7632d0d58d3d913e91132bf7645c126505698fbe4cd5a0138",
                "b108d6407eb7f094a4f9111edd778eee7b978b648d413a9fc7aeedfdd914c154",
                "b92f7481bc03fa41f443b1e1e54b502789df2bbbcefe48c680df7bb02a33fc1e",
                "907b30a61644a6d7e43e58a43a9d97a04a24b1a764a88bdef3954ac807e8d112",
                "d62be3d8b9fbb3c6c81982c4ddb3dfa00eb9662e3206925cbe5b7e1bc6798b80",
                "823063eb4c76b1334fd1800c41798873ae61d4016af0406f1f0b9dce57b1d376",
                "8aa2dcc5b6596aca97995dbfd4e0a9790d8c15108495e0ed154dd15dbb5b465a",
                "d1e320862a8763ac39e7ce194536d4b6c55ba61bae9e8a92753cec32df67a457",
                "cddfb09e019347957bf7b96c95c0e900e8d3062dfaed697a8a96b0a039aec31a",
                "493b4a3b80a21f7255109172ab7bb01ba08d35f2941718f441768f1abfc48acd",
                "195c4a891b6eac4c1cb7671e10ff62bbbe2b17f1dfae1344dc5a6714e4775721"
            };

            for (int pos = 0x1000; pos < exe.Length - 0x1000; pos++)
            {
                if (exe[pos] != (byte)'M' || exe[pos + 1] != (byte)'Z') continue;
                int peRel = BitConverter.ToInt32(exe, pos + 0x3C);
                if (peRel < 0x40 || peRel > 0x400 || pos + peRel + 264 >= exe.Length) continue;
                int peOff = pos + peRel;
                if (exe[peOff] != (byte)'P' || exe[peOff + 1] != (byte)'E' || exe[peOff + 2] != 0 || exe[peOff + 3] != 0) continue;
                ushort machine = BitConverter.ToUInt16(exe, peOff + 4);
                ushort numSec = BitConverter.ToUInt16(exe, peOff + 6);
                ushort optSize = BitConverter.ToUInt16(exe, peOff + 20);
                ushort chars = BitConverter.ToUInt16(exe, peOff + 22);
                if (machine != 0x8664 || (chars & 0x2000) == 0 || numSec == 0 || numSec > 32) continue;
                int secTable = peOff + 24 + optSize;
                if (secTable + numSec * 40 > exe.Length) continue;
                uint maxEnd = 0;
                bool validSec = true;
                for (int s = 0; s < numSec; s++)
                {
                    int so = secTable + s * 40;
                    uint rawSize = BitConverter.ToUInt32(exe, so + 16);
                    uint rawPtr = BitConverter.ToUInt32(exe, so + 20);
                    if (rawPtr + rawSize > (uint)(exe.Length - pos)) { validSec = false; break; }
                    if (rawPtr + rawSize > maxEnd) maxEnd = rawPtr + rawSize;
                }
                if (!validSec || maxEnd < 0x100000) continue;
                string sha = ComputeSha256(exe, pos, (int)maxEnd);
                if (knownDanielHashes.Contains(sha))
                {
                    WriteBytesAtomically(outputDllPath, exe, pos, (int)maxEnd);
                    return sha;
                }
            }
            throw new InvalidDataException("No supported Daniel runtime DLL found inside setup executable.");
        }

        public static void ExtractDanielWeights(string dllOrZipPath, string outputWeightsPath)
        {
            byte[] dllData = LoadSupportedDllBytes(dllOrZipPath, "original");
            List<WeightRecord> records = ParseWeightResource(dllData, "WEIGHTS_HT");

            using (MemoryStream indexMs = new MemoryStream())
            {
                using (BinaryWriter idxWriter = new BinaryWriter(indexMs, Encoding.UTF8, true))
                {
                    ulong currentDataRelOffset = 0;
                    for (int i = 0; i < records.Count; i++)
                    {
                        WeightRecord r = records[i];
                        byte[] nb = Encoding.UTF8.GetBytes(r.Name);
                        idxWriter.Write((byte)nb.Length);
                        idxWriter.Write(nb);
                        idxWriter.Write(currentDataRelOffset);
                        idxWriter.Write((ulong)r.PayloadSize);
                        currentDataRelOffset += (ulong)r.PayloadSize;
                    }
                }
                byte[] indexBytes = indexMs.ToArray();
                uint dataStartOffset = (uint)(16 + indexBytes.Length);

                string dir = Path.GetDirectoryName(Path.GetFullPath(outputWeightsPath));
                if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
                string tempPath = Path.Combine(dir, ".daniel-weights-" + Guid.NewGuid().ToString("N") + ".tmp");
                try
                {
                    using (FileStream fs = new FileStream(tempPath, FileMode.Create, FileAccess.Write, FileShare.None, 65536))
                    using (BinaryWriter bw = new BinaryWriter(fs))
                    {
                        bw.Write(Encoding.ASCII.GetBytes("DLSSNRW1"));
                        bw.Write((uint)records.Count);
                        bw.Write(dataStartOffset);
                        bw.Write(indexBytes);
                        for (int i = 0; i < records.Count; i++)
                        {
                            bw.Write(dllData, records[i].PayloadOffset, records[i].PayloadSize);
                        }
                    }
                    ReplaceFileAtomically(tempPath, outputWeightsPath);
                }
                finally
                {
                    if (File.Exists(tempPath)) File.Delete(tempPath);
                }
            }
        }

        public static byte[] LoadSupportedDllBytes(string sourcePath, string models)
        {
            byte[] raw = File.ReadAllBytes(sourcePath);
            if (raw.Length >= 4 && raw[0] == 0x50 && raw[1] == 0x4B && raw[2] == 0x03 && raw[3] == 0x04)
            {
                using (MemoryStream ms = new MemoryStream(raw))
                using (ZipArchive archive = new ZipArchive(ms, ZipArchiveMode.Read))
                {
                    if (archive.Entries.Count > 10000)
                        throw new InvalidDataException("ZIP contains too many entries (limit: 10000)");
                    long totalUncompressed = 0;
                    List<ZipArchiveEntry> matches = new List<ZipArchiveEntry>();
                    foreach (ZipArchiveEntry e in archive.Entries)
                    {
                        if (e.FullName.Contains("..") || e.FullName.StartsWith("/") || e.FullName.StartsWith("\\"))
                            throw new InvalidDataException("Unsafe path traversal in ZIP archive");
                        totalUncompressed += e.Length;
                        if (totalUncompressed > 2L * 1024 * 1024 * 1024)
                            throw new InvalidDataException("Total uncompressed ZIP size exceeds 2 GiB limit");
                        if (string.Equals(Path.GetFileName(e.FullName), "nvngx_dlssnr.dll", StringComparison.OrdinalIgnoreCase))
                        {
                            if (e.Length > 1024L * 1024 * 1024)
                                throw new InvalidDataException("DLL exceeds 1 GiB inspection limit");
                            matches.Add(e);
                        }
                    }
                    if (matches.Count == 0)
                        throw new InvalidDataException("expected at least one nvngx_dlssnr.dll ZIP member");

                    List<Tuple<string, byte[], string>> candidates = new List<Tuple<string, byte[], string>>();
                    HashSet<string> seenShas = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                    foreach (ZipArchiveEntry m in matches)
                    {
                        byte[] b = new byte[m.Length];
                        using (Stream es = m.Open())
                        {
                            int read = 0;
                            while (read < b.Length)
                            {
                                int r = es.Read(b, read, b.Length - read);
                                if (r <= 0) break;
                                read += r;
                            }
                        }
                        string h = ComputeSha256(b);
                        if (seenShas.Add(h))
                            candidates.Add(Tuple.Create(m.FullName, b, h));
                    }

                    List<Tuple<string, byte[], string>> qualifying = new List<Tuple<string, byte[], string>>();
                    foreach (var c in candidates)
                    {
                        if (c.Item3 == Dll3109Sha256)
                            qualifying.Add(c);
                        else if (c.Item3 == Dll3108Sha256 && models == "original")
                            qualifying.Add(c);
                    }
                    if (qualifying.Count == 0)
                    {
                        foreach (var c in candidates)
                        {
                            if (c.Item3 == Dll3108Sha256 && (models == "both" || models == "small"))
                                throw new InvalidDataException(
                                    "Original model only: this source cannot provide the new model.\n" +
                                    "To use the new model, provide nvngx_dlssnr.dll 310.9.16.0.\n" +
                                    "You may continue with the original model by explicitly selecting it.");
                        }
                        throw new InvalidDataException("nvngx_dlssnr.dll 310.9.16 or 310.8.0 is required (SHA256 mismatch)");
                    }
                    if (qualifying.Count == 1)
                        return qualifying[0].Item2;
                    if (models == "original")
                    {
                        foreach (var q in qualifying)
                            if (q.Item3 == Dll3109Sha256) return q.Item2;
                    }
                    throw new InvalidDataException("ambiguous ZIP members matching requested models");
                }
            }

            string sha = ComputeSha256(raw);
            if (sha != Dll3108Sha256 && sha != Dll3109Sha256)
                throw new InvalidDataException("nvngx_dlssnr.dll 310.9.16 or 310.8.0 is required (SHA256 mismatch)");
            if (sha == Dll3108Sha256 && (models == "both" || models == "small"))
            {
                throw new InvalidDataException(
                    "Original model only: this source cannot provide the new model.\n" +
                    "To use the new model, provide nvngx_dlssnr.dll 310.9.16.0.\n" +
                    "You may continue with the original model by explicitly selecting it.");
            }
            return raw;
        }

        private static byte[] DeswizzleSwin(byte[] blob, int baseOff, int N, int K, int inner)
        {
            byte[] output = new byte[N * K];
            int kg = K / 32;
            int stK = 512 * inner;
            int stO = stK * kg;
            for (int t = 0; t < N / 16; t++)
            {
                for (int g = 0; g < kg; g++)
                {
                    int off = baseOff + (t % inner) * 512 + g * stK + (t / inner) * stO;
                    for (int half = 0; half < 2; half++)
                    {
                        for (int n = 0; n < 8; n++)
                        {
                            for (int k = 0; k < 32; k++)
                            {
                                int lane = 4 * n + (k % 8) / 2;
                                int b = 2 * (k / 8) + (k % 2);
                                int src = off + lane * 16 + half * 8 + b;
                                int row = t * 16 + half * 8 + n;
                                output[row * K + g * 32 + k] = blob[src];
                            }
                        }
                    }
                }
            }
            return output;
        }

        private static byte[] DeswizzleSplitSwin(byte[] blob, int baseOff, int N, int K)
        {
            byte[] output = new byte[N * K];
            int ngrp = N / 16;
            int kgrp = K / 32;
            for (int s = 0; s < ngrp * kgrp; s++)
            {
                int ng = s % ngrp;
                int kg = s / ngrp;
                int off = baseOff + s * 512;
                for (int half = 0; half < 2; half++)
                {
                    for (int lane = 0; lane < 32; lane++)
                    {
                        int n = ng * 16 + half * 8 + lane / 4;
                        int src = off + lane * 16 + half * 8;
                        for (int b = 0; b < 8; b++)
                        {
                            int k = 8 * (b >> 1) + 2 * (lane % 4) + (b & 1);
                            output[n * K + kg * 32 + k] = blob[src + b];
                        }
                    }
                }
            }
            return output;
        }

        private static byte[] SliceBytes(byte[] blob, int offset, int length)
        {
            byte[] res = new byte[length];
            Buffer.BlockCopy(blob, offset, res, 0, length);
            return res;
        }

        public static Dictionary<string, byte[]> BuildMochizukiEntries(byte[] dllData, bool isSmall)
        {
            string resourceName = isSmall ? "WEIGHTS_CC_SMALL_FP8" : "WEIGHTS_HT";
            List<WeightRecord> records = ParseWeightResource(dllData, resourceName);
            Dictionary<string, WeightRecord> recordMap = new Dictionary<string, WeightRecord>(StringComparer.Ordinal);
            for (int i = 0; i < records.Count; i++)
                recordMap[records[i].Name] = records[i];

            Dictionary<string, byte[]> entries = new Dictionary<string, byte[]>(StringComparer.Ordinal);
            int blockCount = isSmall ? 31 : 71;
            for (int block = 0; block < blockCount; block++)
            {
                int role = isSmall ? SmallLogicalToLegacyRole[block] : block;
                if (role == 0)
                {
                    WeightRecord r = recordMap[string.Format("block{0}.layer0.layer", block)];
                    int o = r.PayloadOffset;
                    string pfx = string.Format("unpacked-preblock/block{0}.layer0.layer.", block);
                    entries[pfx + "mlp_expand.bin"] = DeswizzleSwin(dllData, o + 0, 128, 32, 8);
                    entries[pfx + "mlp_contract.bin"] = DeswizzleSwin(dllData, o + 4096, 32, 128, 2);
                    entries[pfx + "input_lift.bin"] = SliceBytes(dllData, o + 8208, 1024);
                    entries[pfx + "residual_scale.bin"] = SliceBytes(dllData, o + 9232, 80);
                    entries[pfx + "qkv.bin"] = DeswizzleSwin(dllData, o + 9312, 96, 32, 6);
                    entries[pfx + "attn_pos_bias.bin"] = SliceBytes(dllData, o + 12384, 8192);
                    entries[pfx + "scalars_b.bin"] = SliceBytes(dllData, o + 20576, 16);
                    entries[pfx + "attn_out_proj.bin"] = DeswizzleSwin(dllData, o + 20592, 32, 32, 2);
                    entries[pfx + "attn_residual_scale.bin"] = SliceBytes(dllData, o + 21616, 80);
                }
                else if (role == 70)
                {
                    WeightRecord r = recordMap[string.Format("block{0}.layer0.layer", block)];
                    int o = r.PayloadOffset;
                    string pfx = string.Format("unpacked-postblock/block{0}.layer0.layer.", block);
                    entries[pfx + "mlp_expand.bin"] = DeswizzleSwin(dllData, o + 0, 128, 32, 8);
                    entries[pfx + "mlp_contract.bin"] = DeswizzleSwin(dllData, o + 4096, 32, 128, 2);
                    entries[pfx + "residual_scale.bin"] = SliceBytes(dllData, o + 8208, 64);
                    entries[pfx + "main_gain.bin"] = SliceBytes(dllData, o + 8272, 64);
                    entries[pfx + "skip_gain.bin"] = SliceBytes(dllData, o + 8336, 64);
                    entries[pfx + "qkv.bin"] = DeswizzleSwin(dllData, o + 8400, 96, 32, 6);
                    entries[pfx + "attn_pos_bias.bin"] = SliceBytes(dllData, o + 11472, 8192);
                    entries[pfx + "scalars_b.bin"] = SliceBytes(dllData, o + 19664, 16);
                    entries[pfx + "attn_out_proj.bin"] = DeswizzleSwin(dllData, o + 19680, 32, 32, 2);
                    entries[pfx + "attn_residual_scale.bin"] = SliceBytes(dllData, o + 20704, 80);
                    byte[] canon = new byte[1024];
                    int rawOff = o + 20784;
                    for (int n = 0; n < 16; n++)
                    {
                        for (int k = 0; k < 32; k++)
                        {
                            int idx = (k / 16) * 256 + 32 * (n % 8) + 8 * ((k % 8) / 2) + 4 * (n / 8) + (k % 2) + 2 * ((k % 16) / 8);
                            canon[2 * (n * 32 + k)] = dllData[rawOff + 2 * idx];
                            canon[2 * (n * 32 + k) + 1] = dllData[rawOff + 2 * idx + 1];
                        }
                    }
                    entries[pfx + "out_project.bin"] = canon;
                }
                else if ((role >= 1 && role <= 22) || (role >= 48 && role <= 69))
                {
                    int c = role <= 4 ? 32 : role <= 8 ? 64 : role <= 14 ? 128 : role <= 22 ? 256 :
                            role <= 55 ? 256 : role <= 61 ? 128 : role <= 65 ? 64 : 32;
                    bool ds = (role == 4 || role == 8 || role == 14 || role == 22);
                    bool up = (role == 48 || role == 56 || role == 62 || role == 66);
                    int cIn = up ? 2 * c : c;
                    int cOut = ds ? 2 * c : c;
                    int heads = c / 32;
                    int H = 4 * c;
                    WeightRecord r = recordMap[string.Format("block{0}.layer0.layer", block)];
                    int o = r.PayloadOffset;
                    int off = 0;
                    string pfx = string.Format("unpacked/block{0}.layer0.layer.", block);
                    if (heads == 1)
                    {
                        entries[pfx + "mlp_expand.bin"] = DeswizzleSwin(dllData, o + off, H, c, H / 16);
                        off += c * H;
                        entries[pfx + "mlp_contract.bin"] = DeswizzleSwin(dllData, o + off, c, H, c / 16);
                        off += c * H;
                    }
                    else
                    {
                        entries[pfx + "mlp_expand.bin"] = DeswizzleSwin(dllData, o + off, H, c, (H / heads) / 16);
                        off += c * H;
                        entries[pfx + "mlp_mid.bin"] = DeswizzleSwin(dllData, o + off, c, H / heads, (c / heads) / 16);
                        off += c * H / heads;
                        entries[pfx + "mlp_contract.bin"] = DeswizzleSwin(dllData, o + off, c, c, c / 16);
                        off += c * c;
                    }
                    if (up)
                    {
                        entries[pfx + "resample.bin"] = DeswizzleSwin(dllData, o + off, cOut, cIn, cOut / 16);
                        off += cIn * cOut;
                    }
                    entries[pfx + "residual_scale.bin"] = SliceBytes(dllData, o + off, 2 * c + 32);
                    off += 2 * c + 32;
                    if (up)
                    {
                        int ugLen = c == 32 ? 2 * c : 2 * c - 32;
                        entries[pfx + "upsample_gain.bin"] = SliceBytes(dllData, o + off, ugLen);
                        off += ugLen;
                    }
                    entries[pfx + "qkv.bin"] = DeswizzleSwin(dllData, o + off, 3 * c, c, (3 * c) / 16);
                    off += 3 * c * c;
                    entries[pfx + "attn_pos_bias.bin"] = SliceBytes(dllData, o + off, heads * 8192);
                    off += heads * 8192;
                    int sbLen = Math.Max(16, 4 * heads);
                    entries[pfx + "scalars_b.bin"] = SliceBytes(dllData, o + off, sbLen);
                    off += sbLen;
                    entries[pfx + "attn_out_proj.bin"] = DeswizzleSwin(dllData, o + off, c, c, c / 16);
                    off += c * c;
                    if (ds)
                    {
                        entries[pfx + "attn_residual_scale.bin"] = SliceBytes(dllData, o + off, 2 * c);
                        off += 2 * c;
                        entries[pfx + "resample.bin"] = DeswizzleSwin(dllData, o + off, cOut, cIn, cOut / 16);
                        off += cIn * cOut;
                    }
                    else
                    {
                        entries[pfx + "attn_residual_scale.bin"] = SliceBytes(dllData, o + off, 2 * c + 16);
                        off += 2 * c + 16;
                    }
                }
                else if ((role >= 23 && role <= 30) || (role >= 40 && role <= 47))
                {
                    WeightRecord r0 = recordMap[string.Format("block{0}.layer0.layer", block)];
                    entries[string.Format("inventory/weights/block{0}.layer0.layer.bin", block)] = SliceBytes(dllData, r0.PayloadOffset, r0.PayloadSize);

                    WeightRecord r1 = recordMap[string.Format("block{0}.layer1.layer", block)];
                    entries[string.Format("unpacked-splitswin/block{0}.layer1.layer.weight.bin", block)] = DeswizzleSplitSwin(dllData, r1.PayloadOffset, 512, 512);
                    entries[string.Format("unpacked-splitswin/block{0}.layer1.layer.skip_weight.bin", block)] = SliceBytes(dllData, r1.PayloadOffset + 262144, 1024);

                    WeightRecord r2 = recordMap[string.Format("block{0}.layer2.layer", block)];
                    entries[string.Format("unpacked-splitswin/block{0}.layer2.layer.qkv.bin", block)] = DeswizzleSplitSwin(dllData, r2.PayloadOffset, 1536, 512);
                    entries[string.Format("unpacked-splitswin/block{0}.layer2.layer.attn_pos_bias.bin", block)] = SliceBytes(dllData, r2.PayloadOffset + 786432, 131072);
                    entries[string.Format("unpacked-splitswin/block{0}.layer2.layer.tail.bin", block)] = SliceBytes(dllData, r2.PayloadOffset + 917504, 64);

                    WeightRecord r3 = recordMap[string.Format("block{0}.layer3.layer", block)];
                    entries[string.Format("unpacked-splitswin/block{0}.layer3.layer.weight.bin", block)] = DeswizzleSplitSwin(dllData, r3.PayloadOffset, 512, 512);
                    entries[string.Format("unpacked-splitswin/block{0}.layer3.layer.skip_weight.bin", block)] = SliceBytes(dllData, r3.PayloadOffset + 262144, 1024);

                    if (role == 30)
                    {
                        WeightRecord r4 = recordMap[string.Format("block{0}.layer4.layer", block)];
                        entries[string.Format("unpacked-splitswin/block{0}.layer4.layer.weight.bin", block)] = DeswizzleSplitSwin(dllData, r4.PayloadOffset, 1024, 512);
                    }
                }
                else if (role >= 31 && role <= 38)
                {
                    WeightRecord r0 = recordMap[string.Format("block{0}.layer0.layer", block)];
                    entries[string.Format("unpacked-vit/block{0}.layer0.layer.weight.bin", block)] = DeswizzleSplitSwin(dllData, r0.PayloadOffset, 4096, 1024);

                    WeightRecord r1 = recordMap[string.Format("block{0}.layer1.layer", block)];
                    entries[string.Format("unpacked-vit/block{0}.layer1.layer.weight.bin", block)] = DeswizzleSplitSwin(dllData, r1.PayloadOffset, 1024, 4096);
                    entries[string.Format("unpacked-vit/block{0}.layer1.layer.skip_weight.bin", block)] = SliceBytes(dllData, r1.PayloadOffset + 4194304, 2048);

                    WeightRecord r2 = recordMap[string.Format("block{0}.layer2.layer", block)];
                    entries[string.Format("inventory/weights/block{0}.layer2.layer.bin", block)] = SliceBytes(dllData, r2.PayloadOffset, r2.PayloadSize);

                    WeightRecord r4 = recordMap[string.Format("block{0}.layer4.layer", block)];
                    entries[string.Format("unpacked-vit/block{0}.layer4.layer.weight.bin", block)] = DeswizzleSplitSwin(dllData, r4.PayloadOffset, 1024, 1024);
                    entries[string.Format("unpacked-vit/block{0}.layer4.layer.skip_weight.bin", block)] = SliceBytes(dllData, r4.PayloadOffset + 1048576, 2048);
                }
                else if (role == 39)
                {
                    WeightRecord r0 = recordMap[string.Format("block{0}.layer0.layer", block)];
                    entries[string.Format("unpacked-splitswin/block{0}.layer0.layer.weight.bin", block)] = DeswizzleSplitSwin(dllData, r0.PayloadOffset, 512, 1024);
                    entries[string.Format("unpacked-splitswin/block{0}.layer0.layer.skip_weight.bin", block)] = SliceBytes(dllData, r0.PayloadOffset + 524288, 1024);
                }
            }
            return entries;
        }

        private static int ExtractBlockIndex(string entryName)
        {
            int idx = entryName.IndexOf("/block", StringComparison.Ordinal);
            if (idx < 0) return 0;
            idx += 6;
            int end = entryName.IndexOf('.', idx);
            if (end < 0) return 0;
            int val;
            return int.TryParse(entryName.Substring(idx, end - idx), out val) ? val : 0;
        }

        public static byte[] BuildMochizukiPackBytes(byte[] dllData, bool isSmall)
        {
            Dictionary<string, byte[]> entries = BuildMochizukiEntries(dllData, isSmall);
            List<string> names = new List<string>(entries.Keys);
            if (isSmall)
            {
                names.Sort((a, b) =>
                {
                    int ba = ExtractBlockIndex(a);
                    int bb = ExtractBlockIndex(b);
                    if (ba != bb) return ba.CompareTo(bb);
                    return string.CompareOrdinal(a, b);
                });
            }
            else
            {
                names.Sort(StringComparer.Ordinal);
            }

            int headerLen = 16;
            for (int i = 0; i < names.Count; i++)
                headerLen += 4 + Encoding.UTF8.GetByteCount(names[i]) + 16;
            long off = ((headerLen + 255L) / 256L) * 256L;
            long[] offsets = new long[names.Count];
            for (int i = 0; i < names.Count; i++)
            {
                offsets[i] = off;
                int bLen = entries[names[i]].Length;
                off = ((off + bLen + 255L) / 256L) * 256L;
            }
            long totalSize = names.Count > 0 ? offsets[names.Count - 1] + entries[names[names.Count - 1]].Length : headerLen;
            byte[] pack = new byte[totalSize];
            Encoding.ASCII.GetBytes("NRMODEL1", 0, 8, pack, 0);
            BitConverter.GetBytes((uint)names.Count).CopyTo(pack, 8);
            int p = 16;
            for (int i = 0; i < names.Count; i++)
            {
                byte[] nb = Encoding.UTF8.GetBytes(names[i]);
                BitConverter.GetBytes((uint)nb.Length).CopyTo(pack, p); p += 4;
                Buffer.BlockCopy(nb, 0, pack, p, nb.Length); p += nb.Length;
                BitConverter.GetBytes((ulong)offsets[i]).CopyTo(pack, p); p += 8;
                BitConverter.GetBytes((ulong)entries[names[i]].Length).CopyTo(pack, p); p += 8;
            }
            for (int i = 0; i < names.Count; i++)
            {
                byte[] blob = entries[names[i]];
                Buffer.BlockCopy(blob, 0, pack, (int)offsets[i], blob.Length);
            }
            string expectedSha = isSmall ? SmallPackSha256 : OriginalPackSha256;
            if (ComputeSha256(pack) != expectedSha)
                throw new InvalidDataException("Packed NRMODEL1 SHA256 mismatch for " + (isSmall ? "small" : "original"));
            return pack;
        }

        public static void ExtractMochizukiModels(string sourcePath, string outputPath, string models)
        {
            if (models != "both" && models != "small" && models != "original")
                throw new ArgumentException("Invalid models mode: " + models);
            byte[] dllData = LoadSupportedDllBytes(sourcePath, models);
            string fullOut = Path.GetFullPath(outputPath);
            string dir = Path.GetDirectoryName(fullOut);
            if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
            if (models == "both" || models == "original")
            {
                byte[] origPack = BuildMochizukiPackBytes(dllData, false);
                WriteBytesAtomically(fullOut, origPack, 0, origPack.Length);
                Console.WriteLine(string.Format("Wrote original model ({0} entries, {1} bytes) to {2}", OriginalEntriesCount, origPack.Length, fullOut));
            }
            if (models == "both" || models == "small")
            {
                string smallOut = Path.Combine(dir ?? "", "dlssnr-small.bin");
                byte[] smallPack = BuildMochizukiPackBytes(dllData, true);
                WriteBytesAtomically(smallOut, smallPack, 0, smallPack.Length);
                Console.WriteLine(string.Format("Wrote small model ({0} entries, {1} bytes) to {2}", SmallEntriesCount, smallPack.Length, smallOut));
            }
        }

        public static bool VerifyMochizukiPack(string packPath, string family)
        {
            try
            {
                if (string.IsNullOrEmpty(packPath) || !File.Exists(packPath)) return false;
                bool isSmall = string.Equals(family, "small", StringComparison.OrdinalIgnoreCase);
                long expectedSize = isSmall ? SmallPackSize : OriginalPackSize;
                FileInfo fi = new FileInfo(packPath);
                if (fi.Length != expectedSize) return false;
                string sha = ComputeFileSha256(packPath);
                string expectedSha = isSmall ? SmallPackSha256 : OriginalPackSha256;
                return string.Equals(sha, expectedSha, StringComparison.OrdinalIgnoreCase);
            }
            catch { return false; }
        }

        public static float HalfBitsToFloat(ushort h)
        {
            float s = (h & 0x8000) != 0 ? -1.0f : 1.0f;
            int e = (h >> 10) & 0x1F;
            int m = h & 0x3FF;
            if (e == 0) return (float)(s * m * Math.Pow(2.0, -24));
            if (e == 31) return m == 0 ? (s * float.PositiveInfinity) : float.NaN;
            return (float)(s * (1.0 + m / 1024.0) * Math.Pow(2.0, e - 15));
        }

        public static ushort FloatToHalfBits(float v)
        {
            uint f = BitConverter.ToUInt32(BitConverter.GetBytes(v), 0);
            uint sign = (f >> 16) & 0x8000u;
            int exp = (int)((f >> 23) & 0xFFu) - 127 + 15;
            uint mant = f & 0x7FFFFFu;
            if (exp <= 0)
            {
                if (exp < -10) return (ushort)sign;
                mant |= 0x800000u;
                int shift = 14 - exp;
                uint halfMant = mant >> shift;
                uint rem = mant & ((1u << shift) - 1u);
                uint halfway = 1u << (shift - 1);
                if (rem > halfway || (rem == halfway && (halfMant & 1u) != 0))
                    halfMant++;
                return (ushort)(sign | halfMant);
            }
            if (exp >= 31)
            {
                if (((f >> 23) & 0xFFu) == 0xFFu && mant != 0)
                    return (ushort)(sign | 0x7E00u);
                return (ushort)(sign | 0x7C00u);
            }
            uint hMant = mant >> 13;
            uint hRem = mant & 0x1FFFu;
            if (hRem > 0x1000u || (hRem == 0x1000u && (hMant & 1u) != 0))
            {
                hMant++;
                if (hMant == 0x400u)
                {
                    hMant = 0;
                    exp++;
                    if (exp >= 31) return (ushort)(sign | 0x7C00u);
                }
            }
            return (ushort)(sign | ((uint)exp << 10) | hMant);
        }

        private static float[] ReadHalfFloats(byte[] raw)
        {
            int count = raw.Length / 2;
            float[] res = new float[count];
            for (int i = 0; i < count; i++)
                res[i] = HalfBitsToFloat(BitConverter.ToUInt16(raw, i * 2));
            return res;
        }

        private static float[] ReadSingleFloats(byte[] raw)
        {
            int count = raw.Length / 4;
            float[] res = new float[count];
            for (int i = 0; i < count; i++)
                res[i] = BitConverter.ToSingle(raw, i * 4);
            return res;
        }

        private static float[] ConcatFloats(params float[][] parts)
        {
            int total = 0;
            for (int i = 0; i < parts.Length; i++) total += parts[i].Length;
            float[] res = new float[total];
            int pos = 0;
            for (int i = 0; i < parts.Length; i++)
            {
                Array.Copy(parts[i], 0, res, pos, parts[i].Length);
                pos += parts[i].Length;
            }
            return res;
        }

        private static float[] SliceFloats(float[] src, int start, int count)
        {
            float[] res = new float[count];
            Array.Copy(src, start, res, 0, count);
            return res;
        }

        private static int AxisA(int i) { return (i / 16) * 16 + (i % 8) * 2 + (i % 16) / 8; }
        private static int AxisB(int i) { return (i / 32) * 32 + C32Order[i % 32]; }
        private static int VitAxis(int i) { return (i & ~31) | (i & 1) | ((i & 6) << 2) | ((i & 24) >> 2); }

        private static float[] ReorderFloats(float[] raw, int[] order)
        {
            float[] res = new float[order.Length];
            for (int i = 0; i < order.Length; i++) res[order[i]] = raw[i];
            return res;
        }

        private static float[] SkipFloats(float[] raw, int c)
        {
            if (c == 32) return ReorderFloats(raw, C32Order);
            int[] order = new int[c];
            for (int i = 0; i < c; i++) order[i] = AxisA(i);
            return ReorderFloats(raw, order);
        }

        private static float[] MatrixFp8(byte[] raw, int rows, int cols, Func<int, int> rowMap, Func<int, int> colMap)
        {
            float[] res = new float[rows * cols];
            int[] co = new int[cols];
            for (int c = 0; c < cols; c++) co[c] = colMap(c);
            for (int r = 0; r < rows; r++)
            {
                int dr = rowMap(r) * cols;
                int sr = r * cols;
                for (int c = 0; c < cols; c++)
                    res[dr + co[c]] = E4M3[raw[sr + c]];
            }
            return res;
        }

        private static byte[] UnswizzleInverse(byte[] canonical, int rows, int cols)
        {
            byte[] raw = new byte[rows * cols];
            int rowsDiv16 = rows / 16;
            for (int r = 0; r < rows; r++)
            {
                for (int c = 0; c < cols; c++)
                {
                    int lane = 4 * (r % 8) + (c % 8) / 2;
                    int b = 2 * ((c % 32) / 8) + (c % 2);
                    int off = (r / 16) * 512 + (c / 32) * 512 * rowsDiv16 + lane * 16 + ((r % 16) / 8) * 8 + b;
                    raw[off] = canonical[r * cols + c];
                }
            }
            return raw;
        }

        private static int ExtractBits(int val, int[] positions)
        {
            int acc = 0;
            for (int b = 0; b < positions.Length; b++)
                acc |= ((val >> positions[b]) & 1) << b;
            return acc;
        }

        private static float[] ScatterFp8(byte[] raw, int offset, int length, int[] rows, int[] cols)
        {
            float[] res = new float[length];
            int width = 1 << cols.Length;
            int lowLen = Math.Min(1024, length);
            int[] low = new int[lowLen];
            for (int i = 0; i < lowLen; i++)
                low[i] = ExtractBits(i, rows) * width + ExtractBits(i, cols);
            int highLen = (length + 1023) / 1024;
            int[] high = new int[highLen];
            for (int i = 0; i < highLen; i++)
                high[i] = ExtractBits(i * 1024, rows) * width + ExtractBits(i * 1024, cols);
            for (int i = 0; i < length; i++)
                res[low[i & 1023] + high[i >> 10]] = E4M3[raw[offset + i]];
            return res;
        }

        private static int[] ConcatIntArrays(int[] a, int start, int endExclusive)
        {
            int count = Math.Max(0, endExclusive - start);
            int[] res = new int[a.Length + count];
            Array.Copy(a, 0, res, 0, a.Length);
            for (int i = 0; i < count; i++) res[a.Length + i] = start + i;
            return res;
        }

        private static float[] QkvFp8(byte[] raw, int c)
        {
            byte[] packed = UnswizzleInverse(raw, 3 * c, c);
            int d = 0;
            int tmp = c;
            while (tmp > 1) { tmp >>= 1; d++; }
            int partLen = packed.Length / 3;
            byte[][] parts = new byte[][] { new byte[partLen], new byte[partLen], new byte[partLen] };
            int dstOff = 0;
            for (int i = 0; i < packed.Length; i += 3072)
            {
                for (int p = 0; p < 3; p++)
                    Buffer.BlockCopy(packed, i + p * 1024, parts[p], dstOff, 1024);
                dstOff += 1024;
            }
            int[] rows = ConcatIntArrays(new int[] { 3 }, 6, d + 5);
            int[] cols = ConcatIntArrays(new int[] { 1, 0, 4, 5, 2 }, d + 5, 2 * d);
            return ConcatFloats(
                ScatterFp8(parts[0], 0, partLen, rows, cols),
                ScatterFp8(parts[1], 0, partLen, rows, cols),
                ScatterFp8(parts[2], 0, partLen, rows, cols));
        }

        private static float[] BiasFloats(byte[] raw)
        {
            float[] src = ReadHalfFloats(raw);
            float[] res = new float[src.Length];
            int[] rPos = new int[] { 5, 6, 10, 7, 1, 11 };
            int[] cPos = new int[] { 0, 3, 8, 4, 2, 9 };
            int[] perm = new int[4096];
            for (int i = 0; i < 4096; i++)
                perm[i] = ExtractBits(i, rPos) * 64 + ExtractBits(i, cPos);
            for (int i = 0; i < src.Length; i++)
                res[(i / 4096) * 4096 + perm[i % 4096]] = src[i];
            return res;
        }

        public static Dictionary<string, byte[]> ReadMochizukiPackEntries(byte[] packBytes)
        {
            if (packBytes.Length < 16 || Encoding.ASCII.GetString(packBytes, 0, 8) != "NRMODEL1")
                throw new InvalidDataException("Invalid NRMODEL1 header");
            uint count = BitConverter.ToUInt32(packBytes, 8);
            uint reserved = BitConverter.ToUInt32(packBytes, 12);
            if (reserved != 0) throw new InvalidDataException("Invalid NRMODEL1 reserved field");
            Dictionary<string, byte[]> entries = new Dictionary<string, byte[]>((int)count, StringComparer.Ordinal);
            int pos = 16;
            for (uint i = 0; i < count; i++)
            {
                int n = (int)BitConverter.ToUInt32(packBytes, pos); pos += 4;
                string name = Encoding.UTF8.GetString(packBytes, pos, n); pos += n;
                long off = (long)BitConverter.ToUInt64(packBytes, pos); pos += 8;
                long sz = (long)BitConverter.ToUInt64(packBytes, pos); pos += 8;
                entries[name] = SliceBytes(packBytes, (int)off, (int)sz);
            }
            return entries;
        }

        public static Dictionary<string, byte[]> BuildLmxxfTensors(Dictionary<string, byte[]> entries, bool isSmall)
        {
            Dictionary<string, byte[]> result = new Dictionary<string, byte[]>(StringComparer.Ordinal);
            int blockCount = isSmall ? 31 : 71;

            Action<string, float[], bool> save = (name, values, isF32) =>
            {
                string filename = name + (isF32 ? ".f32" : ".f16");
                if (isF32)
                {
                    byte[] raw = new byte[values.Length * 4];
                    Buffer.BlockCopy(values, 0, raw, 0, raw.Length);
                    result[filename] = raw;
                }
                else
                {
                    byte[] raw = new byte[values.Length * 2];
                    for (int i = 0; i < values.Length; i++)
                    {
                        ushort h = FloatToHalfBits(values[i]);
                        raw[i * 2] = (byte)(h & 0xFF);
                        raw[i * 2 + 1] = (byte)(h >> 8);
                    }
                    result[filename] = raw;
                }
            };

            Func<int, string, string, int, byte[]> get = (b, nm, grp, lyr) =>
                entries[string.Format("{0}/block{1}.layer{2}.layer.{3}.bin", grp, b, lyr, nm)];

            for (int block = 0; block < blockCount; block++)
            {
                int role = isSmall ? SmallLogicalToLegacyRole[block] : block;
                string rName = role == 70 ? "post70" : ("block" + role);
                if (role == 0 || role == 70 || (role >= 1 && role <= 22) || (role >= 48 && role <= 69))
                {
                    int c = role <= 4 ? 32 : role <= 8 ? 64 : role <= 14 ? 128 : role <= 22 ? 256 :
                            role <= 55 ? 256 : role <= 61 ? 128 : role <= 65 ? 64 : 32;
                    bool up = (role == 48 || role == 56 || role == 62 || role == 66);
                    string grp = role == 0 ? "unpacked-preblock" : role == 70 ? "unpacked-postblock" : "unpacked";
                    float[] residualFull = ReadHalfFloats(get(block, "residual_scale", grp, 0));
                    float[] residual = ((up && c != 32) || role == 0 || role == 70)
                        ? SliceFloats(residualFull, 0, c)
                        : SliceFloats(residualFull, 8, c);
                    Func<int, int> colMap = c == 32 ? (Func<int, int>)AxisB : AxisA;
                    float[] w1 = MatrixFp8(get(block, "mlp_expand", grp, 0), 4 * c, c, AxisA, colMap);
                    if (c == 32)
                    {
                        float[] w2 = MatrixFp8(get(block, "mlp_contract", grp, 0), c, 4 * c, AxisB, AxisA);
                        float[] first = new float[512];
                        if (role == 0)
                        {
                            float[] lift = ReadHalfFloats(get(block, "input_lift", grp, 0));
                            for (int s = 0; s < lift.Length; s++)
                            {
                                int channel = (s / 64) * 4 + ((s / 32) % 2) + ((s / 4) % 2) * 2;
                                int feature = ((s / 8) % 4) * 4 + (s % 4);
                                first[channel * 16 + feature] = lift[s];
                            }
                        }
                        save(rName + "-ffn", ConcatFloats(first, w1, w2, SkipFloats(residual, c)), false);
                    }
                    else
                    {
                        float[] w2 = new float[4 * c * c];
                        float[] mid = MatrixFp8(get(block, "mlp_mid", grp, 0), c, 128, AxisA, AxisA);
                        for (int r = 0; r < c; r++)
                        {
                            int baseIdx = r * 4 * c + (r / 32) * 128;
                            Array.Copy(mid, r * 128, w2, baseIdx, 128);
                        }
                        save("block" + role + "-ffn", ConcatFloats(w1, w2, MatrixFp8(get(block, "mlp_contract", grp, 0), c, c, AxisA, AxisA), SkipFloats(residual, c)), false);
                    }
                    float[] qkv = c == 32 ? MatrixFp8(get(block, "qkv", grp, 0), 3 * c, c, AxisA, AxisB) : QkvFp8(get(block, "qkv", grp, 0), c);
                    float[] attnProj = MatrixFp8(get(block, "attn_out_proj", grp, 0), c, c, c == 32 ? (Func<int, int>)AxisB : AxisA, AxisA);
                    float[] posBias = BiasFloats(get(block, "attn_pos_bias", grp, 0));
                    float[] scalarsB = SliceFloats(ReadSingleFloats(get(block, "scalars_b", grp, 0)), 0, c / 32);
                    float[] attnRes = SkipFloats(SliceFloats(ReadHalfFloats(get(block, "attn_residual_scale", grp, 0)), 0, c), c);
                    save(rName + "-attention", ConcatFloats(qkv, attnProj, posBias, scalarsB, attnRes), true);

                    if (role == 4 || role == 8 || role == 14 || role == 22)
                        save("block" + role + "-ds", MatrixFp8(get(block, "resample", grp, 0), 2 * c, c, AxisA, c == 32 ? (Func<int, int>)AxisB : AxisA), false);
                    if (up)
                    {
                        float[] resMat = MatrixFp8(get(block, "resample", grp, 0), c, 2 * c, c == 32 ? (Func<int, int>)AxisB : AxisA, AxisA);
                        float[] upGainRaw = ReadHalfFloats(get(block, "upsample_gain", grp, 0));
                        float[] upGain = c == 32
                            ? SliceFloats(upGainRaw, 0, c)
                            : ConcatFloats(SliceFloats(residualFull, c, residualFull.Length - c), upGainRaw);
                        save("block" + role + "-weights", ConcatFloats(resMat, SkipFloats(upGain, c)), false);
                    }
                    if (role == 70)
                    {
                        save("post70-scales", ConcatFloats(
                            SkipFloats(ReadHalfFloats(get(block, "main_gain", grp, 0)), 32),
                            SkipFloats(ReadHalfFloats(get(block, "skip_gain", grp, 0)), 32)), false);
                        float[] src = ReadHalfFloats(get(block, "out_project", grp, 0));
                        float[] head = new float[512];
                        for (int r = 0; r < 16; r++)
                            for (int col = 0; col < 32; col++)
                                head[AxisA(r) * 32 + AxisB(col)] = src[r * 32 + col];
                        save("post70-head", ConcatFloats(
                            SliceFloats(head, 0, 32),
                            SliceFloats(head, 64, 32),
                            SliceFloats(head, 128, 32)), false);
                        if (isSmall)
                            save("post70-history-head", SliceFloats(head, 192, 32), false);
                    }
                }
                else if ((role >= 23 && role <= 30) || (role >= 40 && role <= 47))
                {
                    byte[] raw = entries[string.Format("inventory/weights/block{0}.layer0.layer.bin", block)];
                    float[] pre = ScatterFp8(raw, 0, 262144, new int[] { 3, 6, 7, 8, 9, 10, 11, 12, 13 }, new int[] { 1, 0, 4, 5, 2, 14, 15, 16, 17 });
                    float[][] expParts = new float[8][];
                    float[][] conParts = new float[8][];
                    int[] expR = new int[] { 3, 6, 7, 8, 9, 10, 11, 12 };
                    int[] expC = new int[] { 1, 0, 4, 5, 2, 13 };
                    int[] conR = new int[] { 3, 6, 7, 8, 9, 10 };
                    int[] conC = new int[] { 1, 0, 4, 5, 2, 11, 12, 13 };
                    for (int g = 0; g < 8; g++)
                    {
                        expParts[g] = ScatterFp8(raw, 0x40000 + g * 16384, 16384, expR, expC);
                        conParts[g] = ScatterFp8(raw, 0x60000 + g * 16384, 16384, conR, conC);
                    }
                    save("block" + role + "-ffwd", ConcatFloats(pre, ConcatFloats(expParts), ConcatFloats(conParts)), false);
                    save("block" + role + "-ffwd-projection", ConcatFloats(
                        MatrixFp8(get(block, "weight", "unpacked-splitswin", 1), 512, 512, AxisA, AxisA),
                        SkipFloats(ReadHalfFloats(get(block, "skip_weight", "unpacked-splitswin", 1)), 512)), false);
                    save("block" + role + "-attention", ConcatFloats(
                        QkvFp8(get(block, "qkv", "unpacked-splitswin", 2), 512),
                        MatrixFp8(get(block, "weight", "unpacked-splitswin", 3), 512, 512, AxisA, AxisA),
                        BiasFloats(get(block, "attn_pos_bias", "unpacked-splitswin", 2)),
                        ReadSingleFloats(get(block, "tail", "unpacked-splitswin", 2)),
                        SkipFloats(ReadHalfFloats(get(block, "skip_weight", "unpacked-splitswin", 3)), 512)), true);
                    if (role == 30)
                        save("head-matrix", MatrixFp8(get(block, "weight", "unpacked-splitswin", 4), 1024, 512, AxisA, AxisA), false);
                }
                else if (role >= 31 && role <= 38)
                {
                    save("block" + role + "-expand", MatrixFp8(get(block, "weight", "unpacked-vit", 0), 4096, 1024, VitAxis, VitAxis), false);
                    int[] vitOrder = new int[1024];
                    for (int i = 0; i < 1024; i++) vitOrder[i] = VitAxis(i);
                    save("block" + role + "-contract", ConcatFloats(
                        MatrixFp8(get(block, "weight", "unpacked-vit", 1), 1024, 4096, VitAxis, VitAxis),
                        ReorderFloats(ReadHalfFloats(get(block, "skip_weight", "unpacked-vit", 1)), vitOrder)), false);
                    save("block" + role + "-projection", ConcatFloats(
                        MatrixFp8(get(block, "weight", "unpacked-vit", 4), 1024, 1024, VitAxis, VitAxis),
                        ReorderFloats(ReadHalfFloats(get(block, "skip_weight", "unpacked-vit", 4)), vitOrder)), false);
                    byte[] raw = entries[string.Format("inventory/weights/block{0}.layer2.layer.bin", block)];
                    int partLen = (raw.Length - 128) / 3;
                    byte[][] parts = new byte[][] { new byte[partLen], new byte[partLen], new byte[partLen] };
                    int dstOff = 0;
                    for (int i = 128; i < raw.Length; i += 3072)
                    {
                        for (int p = 0; p < 3; p++)
                            Buffer.BlockCopy(raw, i + p * 1024, parts[p], dstOff, 1024);
                        dstOff += 1024;
                    }
                    int[] qkvR = new int[] { 6, 3, 9, 7, 8, 10, 11, 12, 13, 14 };
                    int[] qkvC = new int[] { 0, 1, 2, 4, 5, 15, 16, 17, 18, 19 };
                    byte[] head128 = SliceBytes(raw, 0, 128);
                    save("block" + role + "-qkv", ConcatFloats(
                        ScatterFp8(parts[0], 0, partLen, qkvR, qkvC),
                        ScatterFp8(parts[1], 0, partLen, qkvR, qkvC),
                        ScatterFp8(parts[2], 0, partLen, qkvR, qkvC),
                        ReadSingleFloats(head128)), false);
                }
                else if (role == 39)
                {
                    save("decoder39-weights", ConcatFloats(
                        MatrixFp8(get(block, "weight", "unpacked-splitswin", 0), 512, 1024, AxisA, AxisA),
                        SkipFloats(ReadHalfFloats(get(block, "skip_weight", "unpacked-splitswin", 0)), 512)), false);
                }
            }
            return result;
        }

        public static void ConvertLmxxfModel(string packPath, string dllPath, string outputDir, bool isSmall)
        {
            byte[] packBytes;
            if (!string.IsNullOrEmpty(packPath))
            {
                if (!File.Exists(packPath)) throw new FileNotFoundException("Model pack not found: " + packPath);
                packBytes = File.ReadAllBytes(packPath);
                string expectedSha = isSmall ? SmallPackSha256 : OriginalPackSha256;
                if (ComputeSha256(packBytes) != expectedSha)
                    throw new InvalidDataException("Mochizuki model pack SHA256 mismatch: " + packPath);
            }
            else if (!string.IsNullOrEmpty(dllPath))
            {
                byte[] dllData = LoadSupportedDllBytes(dllPath, isSmall ? "small" : "original");
                packBytes = BuildMochizukiPackBytes(dllData, isSmall);
            }
            else
            {
                throw new ArgumentException("Either packPath or dllPath must be provided.");
            }

            Dictionary<string, byte[]> entries = ReadMochizukiPackEntries(packBytes);
            Dictionary<string, byte[]> tensors = BuildLmxxfTensors(entries, isSmall);
            if (isSmall)
            {
                string manifestJson =
                    "{\n" +
                    "  \"format\": \"optscaler-lmxxf-model-v1\",\n" +
                    "  \"model\": \"small\",\n" +
                    "  \"source_dll_sha256\": \"" + Dll3109Sha256 + "\",\n" +
                    "  \"source_pack_sha256\": \"" + SmallPackSha256 + "\",\n" +
                    "  \"tensor_count\": 93\n" +
                    "}\n";
                tensors["model-manifest.json"] = Encoding.UTF8.GetBytes(manifestJson);
            }

            string fullOut = Path.GetFullPath(outputDir);
            string parent = Path.GetDirectoryName(fullOut);
            if (!string.IsNullOrEmpty(parent)) Directory.CreateDirectory(parent);
            string stage = Path.Combine(parent ?? "", ".lmxxf-convert-" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(stage);
            try
            {
                foreach (KeyValuePair<string, byte[]> kv in tensors)
                {
                    File.WriteAllBytes(Path.Combine(stage, kv.Key), kv.Value);
                }
                Directory.CreateDirectory(fullOut);
                foreach (KeyValuePair<string, byte[]> kv in tensors)
                {
                    string srcFile = Path.Combine(stage, kv.Key);
                    string dstFile = Path.Combine(fullOut, kv.Key);
                    if (Path.GetExtension(dstFile).Equals(".f16", StringComparison.OrdinalIgnoreCase))
                    {
                        string alt = Path.ChangeExtension(dstFile, ".f32");
                        if (File.Exists(alt)) File.Delete(alt);
                    }
                    ReplaceFileAtomically(srcFile, dstFile);
                }
            }
            finally
            {
                if (Directory.Exists(stage))
                {
                    try { Directory.Delete(stage, true); } catch { }
                }
            }
        }

        private static void WriteBytesAtomically(string path, byte[] buffer, int offset, int count)
        {
            string full = Path.GetFullPath(path);
            string dir = Path.GetDirectoryName(full);
            if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
            string temp = Path.Combine(dir, ".tmp-" + Guid.NewGuid().ToString("N"));
            try
            {
                using (FileStream fs = new FileStream(temp, FileMode.Create, FileAccess.Write, FileShare.None, 65536))
                {
                    fs.Write(buffer, offset, count);
                }
                ReplaceFileAtomically(temp, full);
            }
            finally
            {
                if (File.Exists(temp)) File.Delete(temp);
            }
        }

        private static void ReplaceFileAtomically(string stagedPath, string destPath)
        {
            if (File.Exists(destPath))
                File.Delete(destPath);
            File.Move(stagedPath, destPath);
        }
    }
}
'@
    Add-Type -TypeDefinition $csharp -ReferencedAssemblies 'System.IO.Compression', 'System.IO.Compression.FileSystem'
}

function Initialize-LmxxfSmall {
    param([string]$Pack, [string]$Dll,
          [Parameter(Mandatory=$true)][Alias('Output')][string]$ModelOutput,
          [string]$Python = '', [string]$Tool = (Join-Path $PSScriptRoot 'lmxxf-model.py'))
    if (Test-LmxxfModel $ModelOutput small) { Write-Host 'lmxxf Small assets verified.'; return }
    Write-Host 'Preparing lmxxf Small from your local model pack...' -ForegroundColor Cyan
    if (-not $Pack -and -not $Dll) { throw 'Supply a verified Small pack or nvngx_dlssnr.dll 310.9.16.' }
    try {
        Ensure-NativeModelExtractor
        [OptScaler.NativeModelExtractor]::ConvertLmxxfModel($Pack, $Dll, $ModelOutput, $true)
        if (-not (Test-LmxxfModel $ModelOutput small)) { throw 'Converted Small weights failed validation.' }
        Write-Host 'lmxxf Small assets verified.'
    } catch {
        throw "lmxxf Small conversion failed; previous model assets were preserved."
    }
}

function Initialize-LmxxfOriginal {
    param([string]$Pack, [string]$Dll,
          [Parameter(Mandatory=$true)][Alias('Output')][string]$ModelOutput)
    if (Test-LmxxfModel $ModelOutput original) { Write-Host 'lmxxf Original assets verified.'; return }
    Write-Host 'Preparing lmxxf Original from your local model pack...' -ForegroundColor Cyan
    if (-not $Pack -and -not $Dll) { throw 'Supply a verified Original pack or nvngx_dlssnr.dll 310.9.16 / 310.8.0.' }
    try {
        Ensure-NativeModelExtractor
        [OptScaler.NativeModelExtractor]::ConvertLmxxfModel($Pack, $Dll, $ModelOutput, $false)
        if (-not (Test-LmxxfModel $ModelOutput original)) { throw 'Converted Original weights failed validation.' }
        Write-Host 'lmxxf Original assets verified.'
    } catch {
        throw "lmxxf Original conversion failed; previous model assets were preserved."
    }
}
