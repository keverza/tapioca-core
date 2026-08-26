using System;

namespace Tapioca.Grasshopper
{
    /// <summary>
    /// Reads the <c>{"ok":…,"data":…,"error":…}</c> envelope every Tapioca call
    /// returns, without pulling in a JSON library.
    /// </summary>
    /// <remarks>
    /// A deliberate three-case reader, not a parser. The envelope is produced by
    /// one function in <c>GrasshopperBridge.cpp</c> and has exactly this shape,
    /// so a parser would be answering questions nobody asks. The <c>data</c>
    /// payload is handed on as raw JSON: components that need to look inside it
    /// should use a real parser, and Rhino ships Newtonsoft.Json for that.
    /// </remarks>
    internal static class Envelope
    {
        internal static bool IsOk(string json)
        {
            return json != null
                && json.IndexOf("\"ok\":true", StringComparison.OrdinalIgnoreCase) >= 0;
        }

        /// <summary>
        /// The <c>error</c> string, or a readable stand-in when the reply is not
        /// an envelope at all.
        /// </summary>
        internal static string ErrorOf(string json)
        {
            if (json == null)
            {
                return "No reply.";
            }

            string error = StringAfter(json, "\"error\"");
            return error ?? ("Unrecognised reply: " + Clip(json));
        }

        /// <summary>
        /// The raw JSON of the <c>data</c> member, braces included, or an empty
        /// object when there is none.
        /// </summary>
        internal static string DataOf(string json)
        {
            if (json == null)
            {
                return "{}";
            }

            int at = json.IndexOf("\"data\"", StringComparison.OrdinalIgnoreCase);
            if (at < 0)
            {
                return "{}";
            }

            int colon = json.IndexOf(':', at);
            if (colon < 0)
            {
                return "{}";
            }

            int start = colon + 1;
            while (start < json.Length && char.IsWhiteSpace(json[start]))
            {
                start++;
            }

            if (start >= json.Length)
            {
                return "{}";
            }

            // Scan to the matching brace rather than to the next one: the data
            // member is a whole object and nesting is normal. Strings are
            // tracked so a brace inside a value cannot end the scan early.
            char opening = json[start];
            if (opening != '{' && opening != '[')
            {
                int end = start;
                while (end < json.Length && json[end] != ',' && json[end] != '}')
                {
                    end++;
                }

                return json.Substring(start, end - start).Trim();
            }

            char closing = opening == '{' ? '}' : ']';
            int depth = 0;
            bool inString = false;
            bool escaped = false;
            for (int index = start; index < json.Length; index++)
            {
                char current = json[index];
                if (inString)
                {
                    if (escaped)
                    {
                        escaped = false;
                    }
                    else if (current == '\\')
                    {
                        escaped = true;
                    }
                    else if (current == '"')
                    {
                        inString = false;
                    }

                    continue;
                }

                if (current == '"')
                {
                    inString = true;
                }
                else if (current == opening)
                {
                    depth++;
                }
                else if (current == closing)
                {
                    depth--;
                    if (depth == 0)
                    {
                        return json.Substring(start, index - start + 1);
                    }
                }
            }

            return "{}";
        }

        private static string StringAfter(string json, string key)
        {
            int at = json.IndexOf(key, StringComparison.OrdinalIgnoreCase);
            if (at < 0)
            {
                return null;
            }

            int colon = json.IndexOf(':', at);
            if (colon < 0)
            {
                return null;
            }

            int open = json.IndexOf('"', colon + 1);
            if (open < 0)
            {
                return null;
            }

            // Honour escapes so a message containing a quote is not truncated.
            System.Text.StringBuilder text = new System.Text.StringBuilder();
            for (int index = open + 1; index < json.Length; index++)
            {
                char current = json[index];
                if (current == '\\' && index + 1 < json.Length)
                {
                    char next = json[++index];
                    text.Append(next == 'n' ? '\n' : next);
                    continue;
                }

                if (current == '"')
                {
                    return text.ToString();
                }

                text.Append(current);
            }

            return text.ToString();
        }

        private static string Clip(string text)
        {
            return text.Length <= 300 ? text : text.Substring(0, 300) + "...";
        }
    }
}
