using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text;
using System.Text.Json;

namespace Tapioca.Grasshopper
{
    /// <summary>
    /// Reads the <c>data</c> payload of a Tapioca reply, and writes the one
    /// request shape every element command takes.
    /// </summary>
    /// <remarks>
    /// <para>
    /// <see cref="Envelope"/> deliberately stops at the envelope and says so:
    /// "components that need to look inside it should use a real parser". This is
    /// that parser's front door, so no component hand-scans JSON.
    /// </para>
    /// <para>
    /// ⚠️ System.Text.Json, NOT Newtonsoft. Envelope's remark points at
    /// Newtonsoft because Rhino ships a copy — but depending on the version
    /// Rhino happens to load is a binding problem waiting to happen in a .gha
    /// that also loads into a worker. System.Text.Json is part of the .NET shared
    /// framework this package targets, so it is present by definition and has no
    /// version to negotiate.
    /// </para>
    /// <para>
    /// ⚠️ EVERY READER RETURNS A DEFAULT RATHER THAN THROWING. These run inside
    /// SolveInstance, where an exception is a red component and a lost solve; a
    /// missing field is reported as absent and the component says so in words.
    /// </para>
    /// <para>
    /// ⚠️ ONE RECORD PER INPUT, POSITIONALLY ALIGNED, is the shape the native
    /// commands promise ("a short list would silently shift every later element's
    /// data onto the wrong guid" — ElementReadCommands.cpp). The list outputs
    /// here preserve that: a miss emits its row with found=false rather than
    /// being dropped, so a caller can zip against its own guids.
    /// </para>
    /// </remarks>
    internal static class Reply
    {
        /// <summary>
        /// The <c>{"elements":[{"elementId":{"guid":…}}]}</c> parameter object
        /// that every element command takes.
        /// </summary>
        internal static string ElementsRequest(IList<string> guids)
        {
            StringBuilder json = new StringBuilder();
            json.Append("{\"elements\":[");
            int written = 0;
            for (int index = 0; index < guids.Count; index++)
            {
                string guid = guids[index];
                if (string.IsNullOrWhiteSpace(guid))
                {
                    continue;
                }

                json.Append(written == 0 ? string.Empty : ",");
                json.Append("{\"elementId\":{\"guid\":");
                json.Append(JsonSerializer.Serialize(guid.Trim()));
                json.Append("}}");
                written++;
            }

            json.Append("]}");
            return json.ToString();
        }

        /// <summary>
        /// The records of a named array in the payload, or an empty list.
        /// </summary>
        internal static IList<JsonElement> Records(string dataJson, string arrayName)
        {
            List<JsonElement> records = new List<JsonElement>();
            try
            {
                using (JsonDocument document = JsonDocument.Parse(dataJson))
                {
                    JsonElement array;
                    if (!document.RootElement.TryGetProperty(arrayName, out array)
                        || array.ValueKind != JsonValueKind.Array)
                    {
                        return records;
                    }

                    foreach (JsonElement item in array.EnumerateArray())
                    {
                        // Cloned: the records outlive the JsonDocument, whose
                        // buffer is returned to the pool when it is disposed.
                        records.Add(item.Clone());
                    }
                }
            }
            catch (JsonException)
            {
                return records;
            }

            return records;
        }

        /// <summary>
        /// The <c>elementId.guid</c> of a record, or empty.
        /// </summary>
        internal static string GuidOf(JsonElement record)
        {
            JsonElement elementId;
            if (record.ValueKind != JsonValueKind.Object || !record.TryGetProperty("elementId", out elementId))
            {
                return string.Empty;
            }

            return Text(elementId, "guid");
        }

        internal static string Text(JsonElement record, string name)
        {
            JsonElement value;
            if (record.ValueKind != JsonValueKind.Object || !record.TryGetProperty(name, out value))
            {
                return string.Empty;
            }

            switch (value.ValueKind)
            {
                case JsonValueKind.String:
                    return value.GetString();
                case JsonValueKind.Number:
                case JsonValueKind.True:
                case JsonValueKind.False:
                    return value.ToString();
                default:
                    return string.Empty;
            }
        }

        internal static bool Flag(JsonElement record, string name)
        {
            JsonElement value;
            if (record.ValueKind != JsonValueKind.Object || !record.TryGetProperty(name, out value))
            {
                return false;
            }

            return value.ValueKind == JsonValueKind.True;
        }

        internal static int Whole(JsonElement record, string name)
        {
            JsonElement value;
            int number;
            if (record.ValueKind != JsonValueKind.Object
                || !record.TryGetProperty(name, out value)
                || value.ValueKind != JsonValueKind.Number
                || !value.TryGetInt32(out number))
            {
                return 0;
            }

            return number;
        }

        internal static double Real(JsonElement record, string name)
        {
            JsonElement value;
            double number;
            if (record.ValueKind != JsonValueKind.Object
                || !record.TryGetProperty(name, out value)
                || value.ValueKind != JsonValueKind.Number
                || !value.TryGetDouble(out number))
            {
                return 0.0;
            }

            return number;
        }

        /// <summary>
        /// A top-level integer of the payload, such as a count.
        /// </summary>
        internal static int Count(string dataJson, string name)
        {
            try
            {
                using (JsonDocument document = JsonDocument.Parse(dataJson))
                {
                    return Whole(document.RootElement, name);
                }
            }
            catch (JsonException)
            {
                return 0;
            }
        }

        /// <summary>
        /// "3 of 5" style summary for a command that reports per-element hits.
        /// </summary>
        internal static string Summarise(int found, int asked)
        {
            return found.ToString(CultureInfo.InvariantCulture) + " of "
                   + asked.ToString(CultureInfo.InvariantCulture);
        }
    }
}
