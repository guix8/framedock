// Frame Dock PC helper: the summary of the last stream session, for "GET /v2/state"'s "last".
//
// When a stream ends, SteamVR writes Steam\logs\vrlink_pa_<yyyy-MM-dd-HH-mm-ss>.json: how many frames the session
// had, how many of them went wrong in which way ("outcomes"), and what it blames ("cause_by_outcome"). Version 1:
//   {"version":1,"frames":64605,
//    "outcomes":{"DisplayedLate":453,"NotDisplayed":122,"NotDecoded":20,"NotReceived":21,"NotEncoded":4946,
//                "Mispredicted":454,"DegateSkipped":814},
//    "causes":{"DecodeSlow":22867,...},
//    "cause_by_outcome":{"DisplayedLate":{"DecodeSlow":321,"ClientRxLoss":200,...},...}}
// The format isn't documented; these numbers are counters only (no application name, nothing of the user's). The
// top-level "causes" also counts frames that were fine, so it says nothing about what went wrong and isn't used.
//
// Only the newest file is kept, looked for while the headset asks (the folder is listed every few seconds at most,
// and a file is parsed once).
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;

namespace FrameDockPc
{
    /// <summary>
    /// A small JSON reader, enough for SteamVR's session file: objects (Dictionary), arrays (List), strings, numbers
    /// (double), true / false (bool) and null. Anything that isn't JSON is a FormatException.
    /// </summary>
    sealed class JsonReader
    {
        const int MaxDepth = 8;  // the file has three levels
        readonly string s;
        int p;
        int depth;

        JsonReader(string text) { s = text; }

        public static object Read(string text)
        {
            var reader = new JsonReader(text);
            object value = reader.Value();
            reader.Space();
            if (reader.p != text.Length) throw new FormatException();
            return value;
        }

        void Space()
        {
            while (p < s.Length && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n')) p++;
        }

        char Peek()
        {
            Space();
            if (p >= s.Length) throw new FormatException();
            return s[p];
        }

        object Value()
        {
            char c = Peek();
            if (c == '{')
            {
                if (++depth > MaxDepth) throw new FormatException();
                p++;
                var members = new Dictionary<string, object>();
                if (Peek() == '}') p++;
                else
                {
                    while (true)
                    {
                        if (Peek() != '"') throw new FormatException();
                        string key = Text();
                        if (Peek() != ':') throw new FormatException();
                        p++;
                        members[key] = Value();
                        char next = Peek();
                        p++;
                        if (next == '}') break;
                        if (next != ',') throw new FormatException();
                    }
                }
                depth--;
                return members;
            }
            if (c == '[')
            {
                if (++depth > MaxDepth) throw new FormatException();
                p++;
                var items = new List<object>();
                if (Peek() == ']') p++;
                else
                {
                    while (true)
                    {
                        items.Add(Value());
                        char next = Peek();
                        p++;
                        if (next == ']') break;
                        if (next != ',') throw new FormatException();
                    }
                }
                depth--;
                return items;
            }
            if (c == '"') return Text();
            int start = p;
            while (p < s.Length && ((s[p] >= '0' && s[p] <= '9') || (s[p] >= 'a' && s[p] <= 'z') || s[p] == 'E' ||
                                    s[p] == '+' || s[p] == '-' || s[p] == '.')) p++;
            string token = s.Substring(start, p - start);
            if (token == "true") return true;
            if (token == "false") return false;
            if (token == "null") return null;
            double number;
            if (!double.TryParse(token, NumberStyles.AllowLeadingSign | NumberStyles.AllowDecimalPoint | NumberStyles.AllowExponent,
                                 CultureInfo.InvariantCulture, out number)) throw new FormatException();
            return number;
        }

        /// <summary>A string; p is at its opening quote.</summary>
        string Text()
        {
            p++;
            var text = new StringBuilder();
            while (true)
            {
                if (p >= s.Length) throw new FormatException();
                char c = s[p++];
                if (c == '"') return text.ToString();
                if (c != '\\')
                {
                    text.Append(c);
                    continue;
                }
                if (p >= s.Length) throw new FormatException();
                char e = s[p++];
                if (e == '"' || e == '\\' || e == '/') text.Append(e);
                else if (e == 'b') text.Append('\b');
                else if (e == 'f') text.Append('\f');
                else if (e == 'n') text.Append('\n');
                else if (e == 'r') text.Append('\r');
                else if (e == 't') text.Append('\t');
                else if (e == 'u')
                {
                    int code;
                    if (p + 4 > s.Length || !int.TryParse(s.Substring(p, 4), NumberStyles.AllowHexSpecifier,
                                                         CultureInfo.InvariantCulture, out code)) throw new FormatException();
                    text.Append((char)code);
                    p += 4;
                }
                else throw new FormatException();
            }
        }
    }

    /// <summary>One stream session, summed up from its vrlink_pa_*.json.</summary>
    public sealed class SessionSummary
    {
        /// <summary>What the viewer saw late or not at all: the link's side of a bad frame.</summary>
        static readonly string[] LinkOutcomes = { "DisplayedLate", "NotDisplayed", "NotDecoded", "NotReceived" };
        const int MaxCauses = 3;

        public long EndedUnix;      // the file's last write time, UTC
        public long Frames;
        public double LinkBadPct;   // frames with one of LinkOutcomes, of all
        public double PcMissedPct;  // "NotEncoded": the PC had no new frame to send
        /// <summary>What SteamVR blames for the LinkOutcomes frames, the most frequent first (MaxCauses at most).</summary>
        public readonly List<string> Causes = new List<string>();

        static Dictionary<string, object> Child(Dictionary<string, object> parent, string key)
        {
            object value;
            return parent.TryGetValue(key, out value) ? value as Dictionary<string, object> : null;
        }

        /// <summary>A counter: 0 when it is missing, not a number, or negative.</summary>
        static double Count(Dictionary<string, object> parent, string key)
        {
            object value;
            if (!parent.TryGetValue(key, out value) || !(value is double)) return 0;
            double n = (double)value;
            return n > 0 && !double.IsInfinity(n) ? n : 0;
        }

        /// <summary>A name as SteamVR writes them ("DecodeSlow"): letters and digits, 31 at most (it is shown on the headset).</summary>
        static bool PlainName(string name)
        {
            if (name.Length == 0 || name.Length > 31) return false;
            foreach (char c in name)
                if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return false;
            return true;
        }

        /// <summary>The file's text as a summary; null when it isn't a version 1 file with frames.</summary>
        public static SessionSummary Parse(string text, long endedUnix)
        {
            Dictionary<string, object> root;
            try { root = JsonReader.Read(text) as Dictionary<string, object>; }
            catch (FormatException) { return null; }
            if (root == null) return null;
            object version;
            if (!root.TryGetValue("version", out version) || !(version is double) || (double)version != 1) return null;  // another format: not guessed at
            double frames = Count(root, "frames");
            Dictionary<string, object> outcomes = Child(root, "outcomes");
            if (frames < 1 || frames > 1e12 || frames != Math.Floor(frames) || outcomes == null) return null;

            var summary = new SessionSummary();
            summary.EndedUnix = endedUnix;
            summary.Frames = (long)frames;
            double linkBad = 0;
            foreach (string outcome in LinkOutcomes) linkBad += Count(outcomes, outcome);
            summary.LinkBadPct = Math.Min(100.0, 100.0 * linkBad / frames);
            summary.PcMissedPct = Math.Min(100.0, 100.0 * Count(outcomes, "NotEncoded") / frames);

            Dictionary<string, object> byOutcome = Child(root, "cause_by_outcome");
            var totals = new Dictionary<string, double>();
            foreach (string outcome in LinkOutcomes)
            {
                Dictionary<string, object> causes = byOutcome != null ? Child(byOutcome, outcome) : null;
                if (causes == null) continue;
                foreach (string cause in causes.Keys)
                {
                    double count = Count(causes, cause);
                    if (count <= 0 || !PlainName(cause)) continue;
                    double sum;
                    totals.TryGetValue(cause, out sum);
                    totals[cause] = sum + count;
                }
            }
            var ranked = new List<KeyValuePair<string, double>>(totals);
            ranked.Sort(delegate(KeyValuePair<string, double> a, KeyValuePair<string, double> b)
            {
                return a.Value != b.Value ? b.Value.CompareTo(a.Value) : string.CompareOrdinal(a.Key, b.Key);
            });
            for (int i = 0; i < ranked.Count && i < MaxCauses; ++i) summary.Causes.Add(ranked[i].Key);
            return summary;
        }

        /// <summary>/v2/state's "last" (null: no session known).</summary>
        public static string ToJson(SessionSummary s)
        {
            if (s == null) return "null";
            var o = new StringBuilder("{\"endedUnix\":");
            o.Append(s.EndedUnix.ToString(CultureInfo.InvariantCulture));
            o.Append(",\"frames\":").Append(s.Frames.ToString(CultureInfo.InvariantCulture));
            o.Append(",\"linkBadPct\":").Append(Json.Num(s.LinkBadPct, "0.00"));
            o.Append(",\"pcMissedPct\":").Append(Json.Num(s.PcMissedPct, "0.00"));
            o.Append(",\"causes\":[");
            for (int i = 0; i < s.Causes.Count; ++i) o.Append(i > 0 ? "," : "").Append('"').Append(s.Causes[i]).Append('"');
            return o.Append("]}").ToString();
        }
    }

    /// <summary>The newest vrlink_pa_*.json of a folder, as a summary. One thread uses it (the helper's loop).</summary>
    sealed class SessionWatch
    {
        const double LookEverySeconds = 3;
        const long MaxFileBytes = 256 * 1024;  // SteamVR's are under 6 KB
        static readonly DateTime UnixEpoch = new DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind.Utc);

        readonly string folder;
        DateTime nextLook = DateTime.MinValue;
        string seenPath;        // the file Last was read from (also one that couldn't be understood)
        DateTime seenWritten;
        long seenLength;

        public SessionWatch(string folder) { this.folder = folder; }

        /// <summary>The last session; null while there is none, or its file is another version.</summary>
        public SessionSummary Last { get; private set; }

        /// <summary>Looks for a newer file, LookEverySeconds after the last look at the earliest.</summary>
        public void Poll()
        {
            DateTime now = DateTime.UtcNow;
            if (now < nextLook) return;
            nextLook = now.AddSeconds(LookEverySeconds);
            var directory = new DirectoryInfo(folder);
            if (!directory.Exists) return;
            FileInfo newest = null;
            foreach (FileInfo file in directory.GetFiles("vrlink_pa_*.json"))
                if (newest == null || file.LastWriteTimeUtc > newest.LastWriteTimeUtc) newest = file;
            if (newest == null)
            {
                Last = null;
                seenPath = null;
                return;
            }
            if (newest.FullName == seenPath && newest.LastWriteTimeUtc == seenWritten && newest.Length == seenLength) return;
            string text = "";
            if (newest.Length <= MaxFileBytes)
            {
                // An IOException here (SteamVR still writing it) leaves it for the next look
                using (var stream = new FileStream(newest.FullName, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
                using (var reader = new StreamReader(stream, Encoding.UTF8))
                    text = reader.ReadToEnd();
            }
            // A file caught half written doesn't parse; it is read again once its size or time has moved
            seenPath = newest.FullName;
            seenWritten = newest.LastWriteTimeUtc;
            seenLength = newest.Length;
            Last = SessionSummary.Parse(text, (long)(newest.LastWriteTimeUtc - UnixEpoch).TotalSeconds);
        }
    }
}
