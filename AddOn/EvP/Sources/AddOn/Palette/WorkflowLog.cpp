#include "WorkflowLog.hpp"

#include <sstream>

namespace evp {

namespace {

using grasshopper::protocol::DiagnosticLevel;
using grasshopper::protocol::SessionDiagnostic;
using grasshopper::protocol::SessionOutputValue;

// A value is trimmed here rather than at the widget, because the widget wraps
// and a single 200-kilobyte item would push everything else off the visible
// tail. The number is deliberately generous: a curve's description is long and
// still worth reading.
constexpr size_t MaxValueChars = 240;

// The path column in a multi-item group. "{0;12;3}" fits; a deeper one simply
// pushes its own value right rather than breaking the block.
constexpr size_t PathColumn = 10;

std::string Clip (const std::string& text)
{
    if (text.size () <= MaxValueChars)
        return text;

    // ⚠️ NOT AT AN ARBITRARY BYTE. The value is UTF-8 and a cut inside a
    // multi-byte sequence produces a broken character in a read-only box the
    // user cannot fix. Back up over continuation bytes (0b10xxxxxx) first --
    // the same rule the worker's own Clip keeps on the way out.
    size_t cut = MaxValueChars;
    while (cut > 0 && (static_cast<unsigned char> (text[cut]) & 0xC0) == 0x80)
        --cut;
    return text.substr (0, cut) + "...";
}

// Newlines are flattened INTO the line rather than allowed through. A printed
// value containing a line break would otherwise silently become two transcript
// lines, one of which has no id in front of it and reads as a separate output.
std::string OneLine (const std::string& text)
{
    std::string flat;
    flat.reserve (text.size ());
    for (const char character : text) {
        if (character == '\n' || character == '\r')
            flat += " / ";
        else
            flat += character;
    }
    return flat;
}

std::string Pad (const std::string& text, size_t column)
{
    // ⚠️ MEASURED IN BYTES, WHICH IS THE HONEST APPROXIMATION HERE. The column
    // is cosmetic and the box is proportional-font anyway, so a non-ASCII id
    // lands a little off rather than wrong. Counting characters would suggest a
    // precision this alignment does not have.
    if (text.size () >= column)
        return text + " ";
    return text + std::string (column - text.size (), ' ');
}

// The value as it is shown: clipped, flattened, and never blank -- an empty
// cell reads as a formatting fault, and "this output produced an empty string"
// is a real and different answer from "this output produced nothing".
std::string Shown (const std::string& value)
{
    if (value.empty ())
        return "(empty)";
    return OneLine (Clip (value));
}

// ⚠️ THE ROOT PATH IS NOT SHOWN. Almost every value has "{0}", so a column
// carrying it on every line distinguishes nothing and costs the width the value
// needs. A value in any other branch keeps its path, because there the path is
// the only thing telling two values apart.
bool WorthShowing (const std::string& path)
{
    return !path.empty () && path != "{0}";
}

// The component's readable half. The wire carries "Filter {c03d286e-...}"
// because a GUID is what identifies the object on the canvas -- but a reader
// looking at a message wants the nickname, and 38 characters of GUID in front
// of every error is most of the reason the first transcript was unreadable.
std::string ComponentName (const std::string& component)
{
    const size_t brace = component.find (" {");
    if (brace == std::string::npos)
        return component;
    return component.substr (0, brace);
}

const char* HeadingFor (DiagnosticLevel level)
{
    switch (level) {
        case DiagnosticLevel::Error:
            return "errors";
        case DiagnosticLevel::Warning:
            return "warnings";
        case DiagnosticLevel::Remark:
        default:
            return "remarks";
    }
}

} // namespace

std::vector<std::string> FormatOutputBlock (const std::vector<SessionOutputValue>& outputs)
{
    std::vector<std::string> lines;
    if (outputs.empty ())
        return lines;

    // First-appearance order, kept by walking the values rather than sorting
    // them: the order the definition published them in is the author's own, and
    // an alphabetical panel would reorder a report they laid out deliberately.
    std::vector<std::string> ids;
    std::vector<std::vector<const SessionOutputValue*>> groups;
    for (const SessionOutputValue& output : outputs) {
        size_t index = 0;
        while (index < ids.size () && ids[index] != output.id)
            ++index;
        if (index == ids.size ()) {
            ids.push_back (output.id);
            groups.push_back ({});
        }
        groups[index].push_back (&output);
    }

    size_t written = 0;
    for (size_t index = 0; index < ids.size (); ++index) {
        const std::vector<const SessionOutputValue*>& group = groups[index];

        if (written >= MaxWorkflowLogOutputs) {
            std::ostringstream more;
            more << "  ... " << (ids.size () - index) << " more outputs not listed";
            lines.push_back (more.str ());
            break;
        }

        if (group.size () == 1) {
            const SessionOutputValue& only = *group.front ();
            std::string line = "  " + Pad (ids[index], WorkflowLogIdColumn);
            if (WorthShowing (only.path))
                line += Pad (only.path, PathColumn);
            lines.push_back (line + Shown (only.value));
            ++written;
            continue;
        }

        std::ostringstream header;
        header << "  " << Pad (ids[index], WorkflowLogIdColumn) << group.size () << " items";
        lines.push_back (header.str ());
        ++written;

        for (size_t item = 0; item < group.size (); ++item) {
            if (item >= MaxWorkflowLogItemsPerOutput) {
                std::ostringstream more;
                more << "      ... " << (group.size () - MaxWorkflowLogItemsPerOutput) << " more";
                lines.push_back (more.str ());
                break;
            }

            const SessionOutputValue& value = *group[item];
            lines.push_back ("      " + Pad (WorthShowing (value.path) ? value.path : std::string (), PathColumn) +
                             Shown (value.value));
            ++written;
        }
    }

    return lines;
}

std::vector<std::string> FormatDiagnosticBlock (const std::vector<SessionDiagnostic>& list)
{
    std::vector<std::string> lines;
    if (list.empty ())
        return lines;

    // Errors, then warnings, then remarks -- three passes over a short list, so
    // that the severity a reader is looking for is always in the same place and
    // the one that matters is never below the ones that do not.
    const DiagnosticLevel order[] = { DiagnosticLevel::Error, DiagnosticLevel::Warning, DiagnosticLevel::Remark };

    for (const DiagnosticLevel level : order) {
        bool heading = false;
        for (const SessionDiagnostic& diagnostic : list) {
            if (diagnostic.level != level)
                continue;

            if (!heading) {
                lines.push_back (std::string ("  ") + HeadingFor (level));
                heading = true;
            }

            const std::string name = ComponentName (diagnostic.component);
            lines.push_back ("      " + (name.empty () ? std::string () : name + " - ") +
                             OneLine (Clip (diagnostic.text)));
        }
    }

    return lines;
}

std::vector<std::string> FormatSolutionLines (const grasshopper::StoredSolution& solution, const std::string& stamp)
{
    std::vector<std::string> lines;

    std::ostringstream header;
    if (!stamp.empty ())
        header << stamp << "  ";
    // The request it answers, so a transcript read after the fact can be lined
    // up against the input change that caused it. Two counters, never merged --
    // §7's rule, and the reason a stale solution is recognisable at all.
    header << "solution " << solution.solutionRevision << "  (request " << solution.requestRevision << ")  "
           << solution.elapsedMs << " ms";
    lines.push_back (header.str ());

    if (solution.outputs.empty () && solution.diagnostics.empty ()) {
        // Said out loud, because a definition with no Tapioca Data Output, no
        // Context Print and no RH_OUT group solves perfectly and publishes
        // nothing -- which looks exactly like a solve that failed silently.
        lines.push_back ("  (this definition publishes no outputs)");
        return lines;
    }

    if (!solution.outputs.empty ()) {
        lines.push_back ("  outputs");
        const std::vector<std::string> block = FormatOutputBlock (solution.outputs);
        lines.insert (lines.end (), block.begin (), block.end ());
    }

    const std::vector<std::string> messages = FormatDiagnosticBlock (solution.diagnostics);
    lines.insert (lines.end (), messages.begin (), messages.end ());

    return lines;
}

void WorkflowLog::Push (const std::string& line)
{
    lines.push_back (line);
    if (lines.size () > MaxWorkflowLogLines) {
        // The tail is what the band shows, so the head is what goes. Erasing
        // one at a time keeps the buffer at its cap without a periodic
        // compaction that would drop a visible block all at once.
        lines.erase (lines.begin (), lines.begin () + (lines.size () - MaxWorkflowLogLines));
    }
    ++revision;
}

void WorkflowLog::Note (const std::string& line)
{
    Push (line);
}

bool WorkflowLog::Record (const grasshopper::StoredSolution& solution, const std::string& stamp)
{
    // ⚠️ COMPARED WITH <=, NOT !=. The band polls, and a != would re-log a
    // solution that simply had not changed on every idle tick. A session that
    // was closed and reopened starts its counter again, which Forget() is what
    // resets this for -- Clear() deliberately does not.
    if (solution.solutionRevision == 0 || solution.solutionRevision <= loggedSolution)
        return false;

    loggedSolution = solution.solutionRevision;

    // ⚠️ THE TRANSCRIPT IS REPLACED, NOT APPENDED TO, AND THAT IS THE WHOLE
    // POINT OF IT. Appending made a session's history: ten solves of the same
    // definition, nine of them answers to inputs that have since changed, with
    // the current one at the bottom off the visible end of the box. What a
    // panel is asked is "what does it say NOW", and every earlier block was a
    // different answer to that question. The notes above the block go with it:
    // "Loading ..." belongs to the load, not to the solve that followed it.
    lines.clear ();

    for (const std::string& line : FormatSolutionLines (solution, stamp))
        Push (line);
    return true;
}

void WorkflowLog::Clear ()
{
    lines.clear ();
    ++revision;
}

void WorkflowLog::Forget ()
{
    lines.clear ();
    loggedSolution = 0;
    ++revision;
}

std::string WorkflowLog::Text () const
{
    std::string text;
    for (size_t index = 0; index < lines.size (); ++index) {
        if (index > 0)
            text += "\n";
        text += lines[index];
    }
    return text;
}

} // namespace evp
