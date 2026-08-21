#include "CloudCompareCommand.hpp"

#include <iomanip>
#include <limits>
#include <sstream>

namespace evp {

namespace {

std::wstring QuoteArgument (const std::wstring& value)
{
    std::wstring quoted;
    quoted.reserve (value.size () + 2);
    quoted.push_back (L'"');

    size_t backslashes = 0;
    for (const wchar_t character : value) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }

        if (character == L'"')
            quoted.append (backslashes * 2 + 1, L'\\');
        else
            quoted.append (backslashes, L'\\');
        quoted.push_back (character);
        backslashes = 0;
    }

    // Backslashes before the closing quote must be doubled, otherwise the
    // Windows command-line parser treats them as escaping that quote.
    quoted.append (backslashes * 2, L'\\');
    quoted.push_back (L'"');
    return quoted;
}

std::wstring Number (double value)
{
    std::wostringstream stream;
    stream.imbue (std::locale::classic ());
    stream << std::setprecision (std::numeric_limits<double>::max_digits10) << value;
    return stream.str ();
}

void Append (std::wstring& command, const std::wstring& argument)
{
    if (!command.empty ())
        command.push_back (L' ');
    command += argument;
}

} // namespace

std::wstring BuildCloudCompareCommandLine (const CloudCompareCommandRequest& request)
{
    std::wstring command;
    Append (command, QuoteArgument (request.executablePath));
    Append (command, L"-SILENT");
    Append (command, L"-AUTO_SAVE");
    Append (command, L"OFF");
    Append (command, L"-LOG_FILE");
    Append (command, QuoteArgument (request.logPath));
    Append (command, L"-O");
    Append (command, L"-GLOBAL_SHIFT");
    Append (command, L"AUTO");
    Append (command, QuoteArgument (request.inputPath));

    if (!request.cropPolygon.empty ()) {
        Append (command, L"-CROP2D");
        Append (command, L"Z");
        Append (command, Number (static_cast<double> (request.cropPolygon.size ())));
        for (const CloudComparePoint& point : request.cropPolygon) {
            Append (command, Number (point.x));
            Append (command, Number (point.y));
        }
        if (request.keepOutside)
            Append (command, L"-OUTSIDE");
    }

    if (request.subsampleStep > 0.0) {
        Append (command, L"-SS");
        Append (command, L"SPATIAL");
        Append (command, Number (request.subsampleStep));
    }

    Append (command, L"-C_EXPORT_FMT");
    Append (command, L"PLY");
    Append (command, L"-PLY_EXPORT_FMT");
    Append (command, L"BINARY_LE");
    Append (command, L"-NO_TIMESTAMP");
    Append (command, L"-SAVE_CLOUDS");
    Append (command, L"FILE");
    Append (command, QuoteArgument (request.outputPath));
    return command;
}

} // namespace evp
