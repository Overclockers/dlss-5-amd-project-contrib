using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;

namespace OptScalerNR.Launcher
{
    // Pure codec: no launcher state or WPF dependencies.
    public static class MochizukiShareCode
    {
        public enum Kind { Float, Int, Bool }
        public sealed class Key
        {
            public readonly int Index;
            public readonly string IniKey;
            public readonly Kind Kind;
            public readonly decimal Default, Min, Max;
            public Key(int index, string key, Kind kind, decimal value, decimal min, decimal max)
            { Index = index; IniKey = key; Kind = kind; Default = value; Min = min; Max = max; }
        }
        // FROZEN compatibility contract: never reorder or renumber. Append new keys only.
        // MochizukiOptions.inc order, excluding installation/performance switches, then shared visual keys.
        public static readonly System.Collections.ObjectModel.ReadOnlyCollection<Key> Keys = Array.AsReadOnly(new Key[] {
            new Key(0, "MochizukiIntensity", Kind.Float, 1m, 0m, 2m),
            new Key(1, "MochizukiStyle", Kind.Int, 0m, 0m, 2m),
            new Key(2, "MochizukiLocalTone", Kind.Float, 1m, 0m, 2m),
            new Key(3, "MochizukiLocalStructure", Kind.Float, 1m, 0m, 2m),
            new Key(4, "MochizukiSkinStructure", Kind.Float, -1m, -1m, 2m),
            new Key(5, "MochizukiAutomaticMask", Kind.Bool, 1m, 0m, 1m),
            new Key(6, "MochizukiDetailStrength", Kind.Float, 1m, 0m, 2m),
            new Key(7, "MochizukiColourStrength", Kind.Float, 1m, 0m, 4m),
            new Key(8, "MochizukiMaxRatio", Kind.Float, 2m, 1m, 8m),
            new Key(9, "MochizukiEnlargeMode", Kind.Int, 0m, 0m, 2m),
            new Key(10, "MochizukiClassicScaler", Kind.Int, 1m, 0m, 3m),
            new Key(11, "MochizukiWhitePoint", Kind.Float, 1m, .01m, 100m),
            new Key(12, "MochizukiPasses", Kind.Int, 1m, 1m, 3m),
            new Key(13, "MochizukiTemporal", Kind.Bool, 1m, 0m, 1m),
            new Key(14, "MochizukiHistoryStrength", Kind.Float, 1m, 0m, 1m),
            new Key(15, "MochizukiPreprocess", Kind.Bool, 0m, 0m, 1m),
            new Key(16, "MochizukiPreprocessExposure", Kind.Int, 1m, 0m, 2m),
            new Key(17, "MochizukiPreprocessBiasEv", Kind.Float, 0m, -8m, 8m),
            new Key(18, "MochizukiPreprocessCurve", Kind.Int, 3m, 0m, 6m),
            new Key(19, "MochizukiPreprocessContrast", Kind.Float, 1m, .5m, 2m),
            new Key(20, "MochizukiPreprocessSaturation", Kind.Float, 1m, .05m, 2m),
            new Key(21, "MochizukiApplyModel", Kind.Bool, 1m, 0m, 1m),
            new Key(22, "MochizukiLinearInput", Kind.Int, 0m, 0m, 2m),
            new Key(23, "MochizukiPass2Override", Kind.Bool, 0m, 0m, 1m),
            new Key(24, "MochizukiPass2Style", Kind.Int, 0m, 0m, 2m),
            new Key(25, "MochizukiPass2Intensity", Kind.Float, 1m, 0m, 2m),
            new Key(26, "MochizukiPass2LocalTone", Kind.Float, 0m, 0m, 2m),
            new Key(27, "MochizukiPass2LocalStructure", Kind.Float, 1m, 0m, 2m),
            new Key(28, "MochizukiPass2SkinStructure", Kind.Float, -1m, -1m, 2m),
            new Key(29, "MochizukiPass2AutomaticMask", Kind.Bool, 1m, 0m, 1m),
            new Key(30, "MochizukiPass3Override", Kind.Bool, 0m, 0m, 1m),
            new Key(31, "MochizukiPass3Style", Kind.Int, 0m, 0m, 2m),
            new Key(32, "MochizukiPass3Intensity", Kind.Float, 1m, 0m, 2m),
            new Key(33, "MochizukiPass3LocalTone", Kind.Float, 0m, 0m, 2m),
            new Key(34, "MochizukiPass3LocalStructure", Kind.Float, 1m, 0m, 2m),
            new Key(35, "MochizukiPass3SkinStructure", Kind.Float, -1m, -1m, 2m),
            new Key(36, "MochizukiPass3AutomaticMask", Kind.Bool, 1m, 0m, 1m),
            new Key(37, "MochizukiRenoDX", Kind.Bool, 0m, 0m, 1m),
            new Key(38, "MochizukiNoiseEveryFrame", Kind.Bool, 0m, 0m, 1m),
            new Key(39, "MochizukiRenoNeuralUplift", Kind.Bool, 1m, 0m, 1m),
            new Key(40, "MochizukiRenoIntensity", Kind.Float, 1.0m, 0.0m, 2.0m),
            new Key(41, "MochizukiRenoStyle", Kind.Int, 0m, 0m, 2m),
            new Key(42, "MochizukiRenoLocalTone", Kind.Float, 1.0m, 0.0m, 2.0m),
            new Key(43, "MochizukiRenoLocalStructure", Kind.Float, 1.0m, 0.0m, 2.0m),
            new Key(44, "MochizukiRenoSkinStructure", Kind.Float, -1.0m, -1.0m, 1.0m),
            new Key(45, "MochizukiRenoAutoMask", Kind.Bool, 0m, 0m, 1m),
            new Key(46, "MochizukiRenoSkinIndependent", Kind.Bool, 1m, 0m, 1m),
            new Key(47, "MochizukiRenoPasses", Kind.Int, 1m, 1m, 30m),
            new Key(48, "MochizukiRenoUnlockPasses", Kind.Bool, 0m, 0m, 1m),
            new Key(49, "MochizukiRenoChainedHistory", Kind.Bool, 1m, 0m, 1m),
            new Key(50, "MochizukiRenoPass2Transfer", Kind.Float, 1.0m, 0.0m, 1.0m),
            new Key(51, "MochizukiRenoPass2Color", Kind.Float, 1.0m, 0.0m, 1.0m),
            new Key(52, "MochizukiRenoPass2Intensity", Kind.Float, 1.0m, 0.0m, 2.0m),
            new Key(53, "MochizukiRenoPass2FollowPass1", Kind.Bool, 1m, 0m, 1m),
            new Key(54, "MochizukiRenoPass2Style", Kind.Int, 0m, 0m, 2m),
            new Key(55, "MochizukiRenoPass2LocalTone", Kind.Float, 1.0m, 0.0m, 2.0m),
            new Key(56, "MochizukiRenoPass2Structure", Kind.Float, 1.0m, 0.0m, 2.0m),
            new Key(57, "MochizukiRenoPass2Skin", Kind.Float, -1.0m, -1.0m, 1.0m),
            new Key(58, "MochizukiRenoPass2AutoMask", Kind.Bool, 0m, 0m, 1m),
            new Key(59, "MochizukiRenoPass3Transfer", Kind.Float, 1.0m, 0.0m, 1.0m),
            new Key(60, "MochizukiRenoPass3Color", Kind.Float, 1.0m, 0.0m, 1.0m),
            new Key(61, "MochizukiRenoPass3Intensity", Kind.Float, 1.0m, 0.0m, 2.0m),
            new Key(62, "MochizukiRenoPass3FollowPass1", Kind.Bool, 1m, 0m, 1m),
            new Key(63, "MochizukiRenoPass3Style", Kind.Int, 0m, 0m, 2m),
            new Key(64, "MochizukiRenoPass3LocalTone", Kind.Float, 1.0m, 0.0m, 2.0m),
            new Key(65, "MochizukiRenoPass3Structure", Kind.Float, 1.0m, 0.0m, 2.0m),
            new Key(66, "MochizukiRenoPass3Skin", Kind.Float, -1.0m, -1.0m, 1.0m),
            new Key(67, "MochizukiRenoPass3AutoMask", Kind.Bool, 0m, 0m, 1m),
            new Key(68, "MochizukiRenoPass4Transfer", Kind.Float, 1.0m, 0.0m, 1.0m),
            new Key(69, "MochizukiRenoPass4Color", Kind.Float, 1.0m, 0.0m, 1.0m),
            new Key(70, "MochizukiRenoPass4Intensity", Kind.Float, 1.0m, 0.0m, 2.0m),
            new Key(71, "MochizukiRenoPass4FollowPass1", Kind.Bool, 1m, 0m, 1m),
            new Key(72, "MochizukiRenoPass4Style", Kind.Int, 0m, 0m, 2m),
            new Key(73, "MochizukiRenoPass4LocalTone", Kind.Float, 1.0m, 0.0m, 2.0m),
            new Key(74, "MochizukiRenoPass4Structure", Kind.Float, 1.0m, 0.0m, 2.0m),
            new Key(75, "MochizukiRenoPass4Skin", Kind.Float, -1.0m, -1.0m, 1.0m),
            new Key(76, "MochizukiRenoPass4AutoMask", Kind.Bool, 0m, 0m, 1m),
            new Key(77, "MochizukiRenoColorStrength", Kind.Float, 1.0m, 0.0m, 1.0m),
            new Key(78, "MochizukiRenoTransferStrength", Kind.Float, 1.0m, 0.0m, 1.0m),
            new Key(79, "MochizukiRenoChromaClamp", Kind.Float, 1.0m, 0.25m, 2.0m),
            new Key(80, "MochizukiRenoTransferMode", Kind.Int, 0m, 0m, 1m),
            new Key(81, "MochizukiRenoStyleHighlightGuard", Kind.Float, 0.0m, 0.0m, 2.0m),
            new Key(82, "MochizukiRenoDetailStability", Kind.Int, 0m, 0m, 2m),
            new Key(83, "MochizukiRenoResolutionScale", Kind.Float, 1.0m, 0.33m, 1.0m),
            new Key(84, "MochizukiRenoFollowInputRes", Kind.Bool, 0m, 0m, 1m),
            new Key(85, "MochizukiRenoLookUpsample", Kind.Int, 0m, 0m, 3m),
            new Key(86, "MochizukiRenoLookMode", Kind.Bool, 0m, 0m, 1m),
            new Key(87, "MochizukiRenoLookStrength", Kind.Float, 1.0m, 0.0m, 2.0m),
            new Key(88, "MochizukiRenoLookBrighten", Kind.Float, 1.0m, 0.0m, 2.0m),
            new Key(89, "MochizukiRenoLookDarken", Kind.Float, 1.0m, 0.0m, 2.0m),
            new Key(90, "MochizukiRenoLookColour", Kind.Float, 1.0m, 0.0m, 2.0m),
            new Key(91, "MochizukiRenoLookHue", Kind.Float, 1.0m, 0.0m, 2.0m),
            new Key(92, "MochizukiRenoLookShadows", Kind.Float, 1.0m, 0.0m, 2.0m),
            new Key(93, "MochizukiRenoLookMidtones", Kind.Float, 1.0m, 0.0m, 2.0m),
            new Key(94, "MochizukiRenoLookHighlights", Kind.Float, 1.0m, 0.0m, 2.0m),
            new Key(95, "MochizukiRenoLookMaxBrighten", Kind.Float, 0.0m, 0.0m, 4.0m),
            new Key(96, "MochizukiRenoLookMaxDarken", Kind.Float, 0.0m, 0.0m, 4.0m),
            new Key(97, "MochizukiRenoLookMaxColour", Kind.Float, 0.0m, 0.0m, 2.0m),
            new Key(98, "MochizukiRenoLookTone", Kind.Float, 1.0m, 0.0m, 2.0m),
            new Key(99, "MochizukiRenoLookDetail", Kind.Float, 1.0m, 0.0m, 2.0m),
            new Key(100, "MochizukiRenoLookDetailRadius", Kind.Float, 1.0m, 0.25m, 4.0m),
            new Key(101, "MochizukiRenoLookHalo", Kind.Float, 0.0m, 0.0m, 1.0m),
            new Key(102, "MochizukiRenoLookHaloBrightSide", Kind.Float, 0.0m, 0.0m, 1.0m),
            new Key(103, "MochizukiRenoLookStabilize", Kind.Int, 0m, 0m, 2m),
            new Key(104, "MochizukiRenoLookStabilizeMs", Kind.Float, 60.0m, 10.0m, 250.0m),
            new Key(105, "MochizukiRenoLookStabilizeDetail", Kind.Bool, 0m, 0m, 1m),
            new Key(106, "MochizukiRenoSdrBlackRestore", Kind.Int, 0m, 0m, 2m),
            new Key(107, "MochizukiRenoDisplayPedestal", Kind.Int, 0m, 0m, 2m),
            new Key(108, "MochizukiRenoCodecMode", Kind.Int, 2m, 0m, 3m),
            new Key(109, "MochizukiRenoNeuralFloorGuard", Kind.Bool, 1m, 0m, 1m),
            new Key(110, "MochizukiRenoSourceEncoding", Kind.Int, 0m, 0m, 3m),
            new Key(111, "MochizukiRenoLinearUnitNits", Kind.Float, 0.0m, 0.0m, 10000.0m),
            new Key(112, "MochizukiRenoProxyAnchor", Kind.Float, 4.0m, 0.5m, 32.0m),
            new Key(113, "MochizukiRenoPaperWhiteScale", Kind.Float, 2.5375m, 0.005m, 16.0m),
            new Key(114, "MochizukiRenoDiffuseWhiteNits", Kind.Float, 203.0m, 80.0m, 1000.0m),
            new Key(115, "MochizukiRenoPQCalibration", Kind.Float, 1.0m, 0.005m, 16.0m),
            new Key(116, "MochizukiRenoFeedMode", Kind.Int, 0m, 0m, 2m),
            new Key(117, "MochizukiRenoNormGovernor", Kind.Int, 2m, 0m, 2m),
            new Key(118, "MochizukiRenoNormCutAware", Kind.Bool, 1m, 0m, 1m),
            new Key(119, "MochizukiRenoNormAttackStops", Kind.Float, 4.0m, 0.1m, 64.0m),
            new Key(120, "MochizukiRenoNormReleaseStops", Kind.Float, 0.5m, 0.05m, 16.0m),
            new Key(121, "MochizukiRenoNormSlewStops", Kind.Float, 0.02m, 0.0m, 0.5m),
            new Key(122, "MochizukiRenoMVecScaleX", Kind.Float, 1.0m, -2.0m, 2.0m),
            new Key(123, "MochizukiRenoMVecScaleY", Kind.Float, 1.0m, -2.0m, 2.0m),
            new Key(124, "NrOverallIntensity", Kind.Float, 1m, 0m, 2m),
            new Key(125, "NrResidualLowGain", Kind.Float, 1m, 0m, 2m),
            new Key(126, "NrResidualDetailGain", Kind.Float, 1m, 0m, 2m),
            new Key(127, "NrResidualSkinProtection", Kind.Float, 0m, 0m, 1m),
            new Key(128, "NrResidualEdgeProtection", Kind.Float, 0m, 0m, 1m),
            new Key(129, "NrStabilizerEnabled", Kind.Bool, 0m, 0m, 1m),
            new Key(130, "NrStabilizerAlpha", Kind.Float, .8m, 0m, .95m),
            new Key(131, "NrStabilizerThreshold", Kind.Float, 4m, 0m, 16m),
            new Key(132, "NrPersonPartition", Kind.Bool, 0m, 0m, 1m),
            new Key(133, "NrPersonDetailGain", Kind.Float, 1m, 0m, 1m),
            new Key(134, "NrPersonStrength", Kind.Float, 1m, 0m, 1m),
            new Key(135, "MochizukiWhitePointSource", Kind.Int, 0m, 0m, 3m),
        });
        public sealed class CodeException : Exception
        {
            public readonly string Reason;
            public CodeException(string reason) : base(reason) { Reason = reason; }
        }
        public sealed class DecodedCode
        {
            public readonly SortedDictionary<int, decimal> Values;
            public readonly string GameTag;
            public readonly string Note;
            internal DecodedCode(SortedDictionary<int, decimal> values, string gameTag, string note = "")
            { Values = values; GameTag = gameTag; Note = note ?? ""; }
        }
        public static string NormalizeGameTag(string game)
        {
            if (string.IsNullOrEmpty(game)) return "";
            var name = Path.GetFileName(game);
            return name.EndsWith(".exe", StringComparison.OrdinalIgnoreCase) ? name.Substring(0, name.Length - 4) : name;
        }
        public static string SanitizeNote(string note)
        {
            if (string.IsNullOrEmpty(note)) return "";
            var clean = new StringBuilder(note.Length);
            foreach (var ch in note.Trim())
            {
                if (ch == ';' || ch == '|' || ch == '\r' || ch == '\n' || char.IsControl(ch)) continue;
                clean.Append(ch);
                if (clean.Length >= 60) break;
            }
            return clean.ToString().Trim();
        }
        public static bool GameTagMatches(string tag, string executable)
        { return string.Equals(NormalizeGameTag(tag), NormalizeGameTag(executable), StringComparison.OrdinalIgnoreCase); }
        static readonly CultureInfo Invariant = CultureInfo.InvariantCulture;
        static readonly Regex Header = new Regex(@"^\s*\[([^\]]+)\]\s*(?:[;#].*)?$");
        static readonly Regex Assignment = new Regex(@"^(\s*)([^=;#]+?)(\s*=\s*)([^;#]*)(.*)$");
        static Key Find(string name)
        { foreach (var key in Keys) if (string.Equals(key.IniKey, name, StringComparison.OrdinalIgnoreCase)) return key; return null; }
        public static ushort Crc16(string payload)
        {
            ushort crc = 0xffff;
            foreach (byte b in Encoding.UTF8.GetBytes(payload)) {
                crc ^= (ushort)(b << 8);
                for (int i = 0; i < 8; i++) crc = (ushort)((crc & 0x8000) != 0 ? (crc << 1) ^ 0x1021 : crc << 1);
            }
            return crc;
        }
        static decimal ParseIni(Key key, string text)
        {
            text = text.Trim();
            if (string.Equals(text, "auto", StringComparison.OrdinalIgnoreCase)) return key.Default;
            if (key.Kind == Kind.Bool) {
                if (string.Equals(text, "true", StringComparison.OrdinalIgnoreCase) || text == "1") return 1;
                if (string.Equals(text, "false", StringComparison.OrdinalIgnoreCase) || text == "0") return 0;
                throw new CodeException("value");
            }
            decimal value;
            if (!decimal.TryParse(text, NumberStyles.Float, Invariant, out value) ||
                (key.Kind == Kind.Int && decimal.Truncate(value) != value)) throw new CodeException("value");
            if (value < key.Min || value > key.Max) throw new CodeException("range");
            return value;
        }
        static SortedDictionary<int, decimal> ReadValues(byte[] ini)
        {
            var document = Document.Read(ini);
            var values = new SortedDictionary<int, decimal>();
            bool active = false;
            foreach (Match line in Regex.Matches(document.Text, @"[^\r\n]*(?:\r\n|\r|\n|$)")) {
                string text = line.Value.TrimEnd('\r', '\n');
                var header = Header.Match(text);
                if (header.Success) { active = string.Equals(header.Groups[1].Value.Trim(), "DlssNr", StringComparison.OrdinalIgnoreCase); continue; }
                if (!active) continue;
                var assignment = Assignment.Match(text);
                if (!assignment.Success) continue;
                var key = Find(assignment.Groups[2].Value.Trim());
                if (key != null) values[key.Index] = ParseIni(key, assignment.Groups[4].Value);
            }
            return values;
        }
        public static string Encode(byte[] ini, string game = null, string note = null)
        { return EncodeValues(ReadValues(ini), NormalizeGameTag(game), SanitizeNote(note)); }
        static string EncodeValues(IDictionary<int, decimal> values, string game, string note = null)
        {
            var pairs = new List<string>();
            foreach (var key in Keys) {
                decimal value;
                if (!values.TryGetValue(key.Index, out value) || value == key.Default) continue;
                decimal raw = key.Kind == Kind.Float ? decimal.Round(value * 100, 0, MidpointRounding.AwayFromZero) : value;
                // Quantized defaults must stay sparse and round-trip stable.
                if (key.Kind == Kind.Float && raw / 100 == key.Default) continue;
                pairs.Add(key.Index.ToString(Invariant) + "=" + raw.ToString("0", Invariant));
            }
            string payload = string.Join(",", pairs.ToArray());
            if (!string.IsNullOrEmpty(note))
            {
                // MZ3 carries an author note after the game tag; the CRC covers the whole tagged payload
                string noted = payload + "|" + game + "|" + note;
                return "MZ3:" + noted + ";" + Crc16(noted).ToString("X4", Invariant);
            }
            if (game.Length == 0) return "MZ1:" + payload + ";" + Crc16(payload).ToString("X4", Invariant);
            string tagged = payload + "|" + game;
            return "MZ2:" + tagged + ";" + Crc16(tagged).ToString("X4", Invariant);
        }
        public static DecodedCode Decode(string code)
        {
            if (code == null || (!code.StartsWith("MZ1:", StringComparison.Ordinal) && !code.StartsWith("MZ2:", StringComparison.Ordinal) &&
                                 !code.StartsWith("MZ3:", StringComparison.Ordinal))) throw new CodeException("prefix");
            bool tagged = code.StartsWith("MZ2:", StringComparison.Ordinal);
            bool noted = code.StartsWith("MZ3:", StringComparison.Ordinal);
            int end = code.LastIndexOf(';');
            ushort crc;
            if (end < 4 || code.Length != end + 5 || !ushort.TryParse(code.Substring(end + 1), NumberStyles.AllowHexSpecifier, Invariant, out crc)) throw new CodeException("format");
            string payload = code.Substring(4, end - 4);
            if (Crc16(payload) != crc) throw new CodeException("crc");
            string game = "";
            string note = "";
            if (noted) {
                // pairs|game|note — the game segment may be empty, the note may not
                int first = payload.IndexOf('|');
                int second = first < 0 ? -1 : payload.IndexOf('|', first + 1);
                if (first < 0 || second < 0 || payload.IndexOf('|', second + 1) >= 0) throw new CodeException("format");
                note = payload.Substring(second + 1);
                game = payload.Substring(first + 1, second - first - 1);
                payload = payload.Substring(0, first);
                if (note.Length == 0) throw new CodeException("format");
            }
            else if (tagged) {
                int separator = payload.IndexOf('|');
                if (separator < 0 || separator == payload.Length - 1 || payload.IndexOf('|', separator + 1) >= 0) throw new CodeException("format");
                game = payload.Substring(separator + 1);
                payload = payload.Substring(0, separator);
            }
            var result = new SortedDictionary<int, decimal>();
            if (payload.Length == 0) return new DecodedCode(result, game, note);
            foreach (string pair in payload.Split(',')) {
                var match = Regex.Match(pair, @"^(\d+)=(-?\d+)$");
                int index; decimal raw;
                if (!match.Success || !int.TryParse(match.Groups[1].Value, NumberStyles.None, Invariant, out index) ||
                    !decimal.TryParse(match.Groups[2].Value, NumberStyles.AllowLeadingSign, Invariant, out raw)) throw new CodeException("format");
                if (index < 0 || index >= Keys.Count) throw new CodeException("index");
                if (result.ContainsKey(index)) throw new CodeException("duplicate");
                var key = Keys[index];
                decimal value = key.Kind == Kind.Float ? raw / 100 : raw;
                decimal tolerance = key.Kind == Kind.Float ? .01m : 0;
                if (value < key.Min - tolerance || value > key.Max + tolerance) throw new CodeException("range");
                value = Math.Max(key.Min, Math.Min(key.Max, value));
                result.Add(index, value);
            }
            return new DecodedCode(result, game, note);
        }
        public static string Summary(DecodedCode code, bool chinese)
        {
            var values = code.Values;
            decimal style = 0, reno = 0; int count = 0;
            foreach (var key in Keys) {
                decimal value; if (!values.TryGetValue(key.Index, out value)) value = key.Default;
                if (value != key.Default) count++;
                if (key.IniKey == "MochizukiStyle") style = value;
                if (key.IniKey == "MochizukiRenoDX") reno = value;
            }
            string[] styles = chinese ? new[] { "标准", "自然", "电影感" } : new[] { "Standard", "Natural", "Cinematic" };
            return "Mochizuki · " + (chinese ? "风格 " : "Style ") + styles[(int)style] + " · RenoDX " +
                (chinese ? (reno == 1 ? "开" : "关") : (reno == 1 ? "On" : "Off")) + " · " +
                (chinese ? "修改 " + count + " 项" : count + " non-default settings") +
                (code.GameTag.Length == 0 ? "" : (chinese ? " · 来自 " : " · from ") + code.GameTag);
        }
        public static int NonDefaultCount(DecodedCode code)
        {
            int count = 0;
            foreach (var key in Keys) {
                decimal value;
                if (code.Values.TryGetValue(key.Index, out value) && value != key.Default) count++;
            }
            return count;
        }
        public static int DifferenceCount(byte[] ini, string code)
        {
            var before = ReadValues(ini); var after = Decode(code).Values; int count = 0;
            foreach (var key in Keys) {
                decimal a, b;
                if (!before.TryGetValue(key.Index, out a)) a = key.Default;
                if (!after.TryGetValue(key.Index, out b)) b = key.Default;
                if (a != b) count++;
            }
            return count;
        }
        static string Native(Key key, decimal value)
        { return key.Kind == Kind.Bool ? (value == 1 ? "true" : "false") : value.ToString(key.Kind == Kind.Float ? "0.######" : "0", Invariant); }
        public static byte[] Apply(byte[] ini, string code)
        {
            var values = Decode(code).Values;
            var document = Document.Read(ini);
            string newline = document.Text.Contains("\r\n") ? "\r\n" : document.Text.Contains("\n") ? "\n" : document.Text.Contains("\r") ? "\r" : Environment.NewLine;
            var output = new StringBuilder(); bool active = false, inserted = false;
            foreach (Match line in Regex.Matches(document.Text, @"[^\r\n]*(?:\r\n|\r|\n|$)")) {
                string full = line.Value;
                if (full.Length == 0) continue;
                string text = full.TrimEnd('\r', '\n');
                string ending = full.Substring(text.Length);
                var header = Header.Match(text);
                if (header.Success) {
                    active = string.Equals(header.Groups[1].Value.Trim(), "DlssNr", StringComparison.OrdinalIgnoreCase);
                    output.Append(full);
                    if (active && !inserted) {
                        if (ending.Length == 0) output.Append(newline);
                        foreach (var key in Keys) { decimal value; if (values.TryGetValue(key.Index, out value) && value != key.Default) output.Append(key.IniKey).Append('=').Append(Native(key, value)).Append(ending.Length == 0 ? newline : ending); }
                        inserted = true;
                    }
                    continue;
                }
                var assignment = active ? Assignment.Match(text) : Match.Empty;
                if (assignment.Success && Find(assignment.Groups[2].Value.Trim()) != null) {
                    // Neutralize, rather than delete: preserve the entire original line, including comments.
                    output.Append(';').Append(full);
                } else output.Append(full);
            }
            if (!inserted && values.Count > 0) {
                if (output.Length > 0 && output[output.Length - 1] != '\n' && output[output.Length - 1] != '\r') output.Append(newline);
                output.Append("[DlssNr]").Append(newline);
                foreach (var key in Keys) { decimal value; if (values.TryGetValue(key.Index, out value) && value != key.Default) output.Append(key.IniKey).Append('=').Append(Native(key, value)).Append(newline); }
            }
            return document.Bytes(output.ToString());
        }
        // Mirror IniProfiles.Document: strict UTF-8 or BOM-marked UTF-16, preserving the preamble.
        sealed class Document
        {
            public string Text; Encoding encoding; byte[] preamble;
            public static Document Read(byte[] bytes)
            {
                Encoding encoding = new UTF8Encoding(false, true); int offset = 0;
                if (bytes.Length >= 3 && bytes[0] == 0xef && bytes[1] == 0xbb && bytes[2] == 0xbf) offset = 3;
                else if (bytes.Length >= 2 && bytes[0] == 0xff && bytes[1] == 0xfe) { encoding = new UnicodeEncoding(false, false, true); offset = 2; }
                else if (bytes.Length >= 2 && bytes[0] == 0xfe && bytes[1] == 0xff) { encoding = new UnicodeEncoding(true, false, true); offset = 2; }
                var preamble = new byte[offset]; Buffer.BlockCopy(bytes, 0, preamble, 0, offset);
                try { return new Document { Text = encoding.GetString(bytes, offset, bytes.Length - offset), encoding = encoding, preamble = preamble }; }
                catch (DecoderFallbackException) { throw new CodeException("encoding"); }
            }
            public byte[] Bytes(string text)
            {
                byte[] content = encoding.GetBytes(text), result = new byte[preamble.Length + encoding.GetByteCount(text)];
                Buffer.BlockCopy(preamble, 0, result, 0, preamble.Length); Buffer.BlockCopy(content, 0, result, preamble.Length, content.Length); return result;
            }
        }
    }
}
