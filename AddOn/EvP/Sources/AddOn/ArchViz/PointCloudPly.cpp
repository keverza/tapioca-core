#include "ArchViz/PointCloudPly.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>

namespace geomsrv {
namespace archviz {

namespace {

enum class ScalarType : uint8_t { Int8, UInt8, Int16, UInt16, Int32, UInt32, Float32, Float64 };

struct Property {
    ScalarType type = ScalarType::Float32;
    std::string name;
};

bool ParseType (const std::string& text, ScalarType& type, size_t& bytes)
{
    if (text == "char" || text == "int8") {
        type = ScalarType::Int8;
        bytes = 1;
    }
    else if (text == "uchar" || text == "uint8") {
        type = ScalarType::UInt8;
        bytes = 1;
    }
    else if (text == "short" || text == "int16") {
        type = ScalarType::Int16;
        bytes = 2;
    }
    else if (text == "ushort" || text == "uint16") {
        type = ScalarType::UInt16;
        bytes = 2;
    }
    else if (text == "int" || text == "int32") {
        type = ScalarType::Int32;
        bytes = 4;
    }
    else if (text == "uint" || text == "uint32") {
        type = ScalarType::UInt32;
        bytes = 4;
    }
    else if (text == "float" || text == "float32") {
        type = ScalarType::Float32;
        bytes = 4;
    }
    else if (text == "double" || text == "float64") {
        type = ScalarType::Float64;
        bytes = 8;
    }
    else {
        return false;
    }
    return true;
}

size_t TypeSize (ScalarType type)
{
    switch (type) {
        case ScalarType::Int8:
        case ScalarType::UInt8:
            return 1;
        case ScalarType::Int16:
        case ScalarType::UInt16:
            return 2;
        case ScalarType::Int32:
        case ScalarType::UInt32:
        case ScalarType::Float32:
            return 4;
        case ScalarType::Float64:
            return 8;
    }
    return 0;
}

bool ReadScalar (std::istream& input, ScalarType type, double& value)
{
    const size_t size = TypeSize (type);
    unsigned char bytes[8] = {};
    if (!input.read (reinterpret_cast<char*> (bytes), std::streamsize (size)))
        return false;

    uint64_t bits = 0;
    for (size_t i = 0; i < size; ++i)
        bits |= uint64_t (bytes[i]) << (i * 8);

    switch (type) {
        case ScalarType::Int8:
            value = int8_t (bits);
            break;
        case ScalarType::UInt8:
            value = uint8_t (bits);
            break;
        case ScalarType::Int16:
            value = int16_t (bits);
            break;
        case ScalarType::UInt16:
            value = uint16_t (bits);
            break;
        case ScalarType::Int32:
            value = int32_t (bits);
            break;
        case ScalarType::UInt32:
            value = uint32_t (bits);
            break;
        case ScalarType::Float32: {
            const uint32_t raw = uint32_t (bits);
            float decoded = 0.0f;
            std::memcpy (&decoded, &raw, sizeof (decoded));
            value = decoded;
            break;
        }
        case ScalarType::Float64: {
            double decoded = 0.0;
            std::memcpy (&decoded, &bits, sizeof (decoded));
            value = decoded;
            break;
        }
    }
    return true;
}

uint8_t ColourByte (double value)
{
    return uint8_t (std::clamp (std::lround (value), 0l, 255l));
}

std::string ScalarName (const std::string& name)
{
    constexpr char prefix[] = "scalar_";
    if (name.rfind (prefix, 0) == 0)
        return name.substr (sizeof (prefix) - 1);
    return name;
}

} // namespace

size_t PointCloudData::Bytes () const
{
    size_t bytes = vertices.capacity () * sizeof (PointCloudVertex);
    for (const std::string& field : scalarFields)
        bytes += field.capacity ();
    return bytes;
}

bool LoadPointCloudPly (std::istream& input, PointCloudData& output, std::string& error)
{
    output = {};
    error.clear ();

    std::string line;
    if (!std::getline (input, line) || (line != "ply" && line != "ply\r")) {
        error = "PLY header does not start with 'ply'.";
        return false;
    }

    bool formatSeen = false;
    bool endSeen = false;
    bool inVertex = false;
    bool vertexSeen = false;
    size_t vertexCount = 0;
    size_t recordBytes = 0;
    size_t headerBytes = line.size () + 1;
    bool shiftSeen[3] = {};
    std::vector<Property> properties;

    while (std::getline (input, line)) {
        headerBytes += line.size () + 1;
        if (headerBytes > 1024 * 1024) {
            error = "PLY header exceeds 1 MiB.";
            return false;
        }
        if (!line.empty () && line.back () == '\r')
            line.pop_back ();
        if (line == "end_header") {
            endSeen = true;
            break;
        }

        std::istringstream words (line);
        std::string keyword;
        words >> keyword;
        if (keyword == "format") {
            std::string format;
            std::string version;
            words >> format >> version;
            if (format != "binary_little_endian" || version != "1.0") {
                error = "PLY must use format binary_little_endian 1.0.";
                return false;
            }
            formatSeen = true;
        }
        else if (keyword == "element") {
            std::string name;
            size_t count = 0;
            if (!(words >> name >> count)) {
                error = "Malformed PLY element declaration.";
                return false;
            }
            inVertex = name == "vertex";
            if (inVertex) {
                if (vertexSeen) {
                    error = "PLY declares the vertex element more than once.";
                    return false;
                }
                vertexSeen = true;
                vertexCount = count;
            }
            else if (!vertexSeen && count != 0) {
                error = "PLY vertex data must be the first non-empty element.";
                return false;
            }
        }
        else if (keyword == "property" && inVertex) {
            std::string typeName;
            words >> typeName;
            if (typeName == "list") {
                error = "PLY list properties are not supported on vertices.";
                return false;
            }
            Property property;
            size_t bytes = 0;
            if (!ParseType (typeName, property.type, bytes) || !(words >> property.name)) {
                error = "Unsupported or malformed PLY vertex property.";
                return false;
            }
            if (recordBytes > std::numeric_limits<size_t>::max () - bytes) {
                error = "PLY vertex record size overflows this process.";
                return false;
            }
            recordBytes += bytes;
            properties.push_back (std::move (property));
        }
        else if (keyword == "comment") {
            std::string name;
            double value = 0.0;
            if (words >> name >> value) {
                const char* names[3] = { "global_shift_x", "global_shift_y", "global_shift_z" };
                for (size_t axis = 0; axis < 3; ++axis) {
                    if (name == names[axis]) {
                        output.reportedGlobalShift[axis] = value;
                        shiftSeen[axis] = true;
                    }
                }
            }
        }
    }

    if (!endSeen || !formatSeen || !vertexSeen) {
        error = "PLY header is missing format, vertex element, or end_header.";
        return false;
    }
    if (recordBytes == 0 || vertexCount > std::numeric_limits<size_t>::max () / recordBytes) {
        error = "PLY vertex body size is invalid or overflows this process.";
        return false;
    }

    bool haveX = false, haveY = false, haveZ = false;
    bool haveNx = false, haveNy = false, haveNz = false;
    bool haveRed = false, haveGreen = false, haveBlue = false;
    for (const Property& property : properties) {
        haveX |= property.name == "x";
        haveY |= property.name == "y";
        haveZ |= property.name == "z";
        haveNx |= property.name == "nx";
        haveNy |= property.name == "ny";
        haveNz |= property.name == "nz";
        haveRed |= property.name == "red";
        haveGreen |= property.name == "green";
        haveBlue |= property.name == "blue";
        if (property.name.rfind ("scalar_", 0) == 0)
            output.scalarFields.push_back (ScalarName (property.name));
    }
    if (!haveX || !haveY || !haveZ) {
        error = "PLY vertex element must contain x, y, and z properties.";
        return false;
    }
    output.hasColours = haveRed && haveGreen && haveBlue;
    output.hasNormals = haveNx && haveNy && haveNz;
    output.hasReportedGlobalShift = shiftSeen[0] && shiftSeen[1] && shiftSeen[2];
    output.vertices.reserve (vertexCount);

    for (size_t pointIndex = 0; pointIndex < vertexCount; ++pointIndex) {
        double source[3] = {};
        double normal[3] = { 0.0, 0.0, 1.0 };
        uint8_t colour[4] = { 255, 255, 255, 255 };
        for (const Property& property : properties) {
            double value = 0.0;
            if (!ReadScalar (input, property.type, value)) {
                error = "PLY vertex body is truncated at point " + std::to_string (pointIndex) + ".";
                output = {};
                return false;
            }
            if (property.name == "x")
                source[0] = value;
            else if (property.name == "y")
                source[1] = value;
            else if (property.name == "z")
                source[2] = value;
            else if (property.name == "nx")
                normal[0] = value;
            else if (property.name == "ny")
                normal[1] = value;
            else if (property.name == "nz")
                normal[2] = value;
            else if (property.name == "red")
                colour[0] = ColourByte (value);
            else if (property.name == "green")
                colour[1] = ColourByte (value);
            else if (property.name == "blue")
                colour[2] = ColourByte (value);
            else if (property.name == "alpha")
                colour[3] = ColourByte (value);
        }
        if (!std::isfinite (source[0]) || !std::isfinite (source[1]) || !std::isfinite (source[2])) {
            error = "PLY contains a non-finite coordinate at point " + std::to_string (pointIndex) + ".";
            output = {};
            return false;
        }
        if (pointIndex == 0) {
            std::copy (source, source + 3, output.origin);
            std::fill (output.boundsMin, output.boundsMin + 3, 0.0f);
            std::fill (output.boundsMax, output.boundsMax + 3, 0.0f);
        }

        PointCloudVertex vertex;
        for (size_t axis = 0; axis < 3; ++axis) {
            vertex.position[axis] = float (source[axis] - output.origin[axis]);
            vertex.normal[axis] = float (normal[axis]);
            output.boundsMin[axis] = std::min (output.boundsMin[axis], vertex.position[axis]);
            output.boundsMax[axis] = std::max (output.boundsMax[axis], vertex.position[axis]);
        }
        vertex.rgba = uint32_t (colour[0]) | (uint32_t (colour[1]) << 8) | (uint32_t (colour[2]) << 16) |
                      (uint32_t (colour[3]) << 24);
        output.vertices.push_back (vertex);
    }
    return true;
}

bool PlacePointCloudInProject (PointCloudData& cloud, const double projectToSurvey[12], std::string& error)
{
    error.clear ();
    if (projectToSurvey == nullptr) {
        error = "Project-to-survey transformation is missing.";
        return false;
    }

    const double a = projectToSurvey[0], b = projectToSurvey[1], c = projectToSurvey[2];
    const double d = projectToSurvey[4], e = projectToSurvey[5], f = projectToSurvey[6];
    const double g = projectToSurvey[8], h = projectToSurvey[9], i = projectToSurvey[10];
    const double determinant = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (!std::isfinite (determinant) || std::abs (determinant) < 1.0e-12) {
        error = "Archicad survey-point transformation is singular.";
        return false;
    }

    const double inverse[9] = {
        (e * i - f * h) / determinant, (c * h - b * i) / determinant, (b * f - c * e) / determinant,
        (f * g - d * i) / determinant, (a * i - c * g) / determinant, (c * d - a * f) / determinant,
        (d * h - e * g) / determinant, (b * g - a * h) / determinant, (a * e - b * d) / determinant,
    };
    const double translatedOrigin[3] = {
        cloud.origin[0] - projectToSurvey[3],
        cloud.origin[1] - projectToSurvey[7],
        cloud.origin[2] - projectToSurvey[11],
    };
    double projectOrigin[3] = {};
    for (size_t row = 0; row < 3; ++row) {
        projectOrigin[row] = inverse[row * 3] * translatedOrigin[0] + inverse[row * 3 + 1] * translatedOrigin[1] +
                             inverse[row * 3 + 2] * translatedOrigin[2];
    }

    bool first = true;
    for (PointCloudVertex& vertex : cloud.vertices) {
        const float source[3] = { vertex.position[0], vertex.position[1], vertex.position[2] };
        const float normal[3] = { vertex.normal[0], vertex.normal[1], vertex.normal[2] };
        for (size_t row = 0; row < 3; ++row) {
            vertex.position[row] = float (inverse[row * 3] * source[0] + inverse[row * 3 + 1] * source[1] +
                                          inverse[row * 3 + 2] * source[2]);
            vertex.normal[row] = float (inverse[row * 3] * normal[0] + inverse[row * 3 + 1] * normal[1] +
                                        inverse[row * 3 + 2] * normal[2]);
            if (first)
                cloud.boundsMin[row] = cloud.boundsMax[row] = vertex.position[row];
            else {
                cloud.boundsMin[row] = std::min (cloud.boundsMin[row], vertex.position[row]);
                cloud.boundsMax[row] = std::max (cloud.boundsMax[row], vertex.position[row]);
            }
        }
        first = false;
    }
    std::copy (projectOrigin, projectOrigin + 3, cloud.origin);
    return true;
}

} // namespace archviz
} // namespace geomsrv
