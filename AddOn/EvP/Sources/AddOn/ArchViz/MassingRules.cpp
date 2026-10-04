#include "ArchViz/MassingRules.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <map>
#include <sstream>

namespace geomsrv::archviz::massingrules {
namespace {
namespace meta = metadata;

// Python's round uses ties-to-even, unlike std::round. Do not depend on the
// process floating-point rounding mode: Archicad and its other add-ons own it.
int64_t Quantize (double value)
{
    const double scaled = value / 1e-4;
    const double low = std::floor (scaled), fraction = scaled - low;
    const int64_t integer = int64_t (low);
    return integer + (fraction > 0.5 || (fraction == 0.5 && integer % 2 != 0) ? 1 : 0);
}

uint32_t Rotate (uint32_t value, int bits)
{
    return (value << bits) | (value >> (32 - bits));
}

// SHA-1 is an existing on-element identity format, NOT a security primitive.
// Keep this tiny portable encoder under cross-language fixtures: changing the
// algorithm would invalidate previously authored Python segment assignments.
std::string Hash (const std::string& key)
{
    std::vector<uint8_t> bytes (key.begin (), key.end ());
    const uint64_t bits = uint64_t (bytes.size ()) * 8;
    bytes.push_back (0x80);
    while (bytes.size () % 64 != 56)
        bytes.push_back (0);
    for (int i = 7; i >= 0; --i)
        bytes.push_back (uint8_t (bits >> (i * 8)));
    std::array<uint32_t, 5> hash { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    for (size_t offset = 0; offset < bytes.size (); offset += 64) {
        uint32_t words[80] {};
        for (int i = 0; i < 16; ++i)
            for (int j = 0; j < 4; ++j)
                words[i] = (words[i] << 8) | bytes[offset + i * 4 + j];
        for (int i = 16; i < 80; ++i)
            words[i] = Rotate (words[i - 3] ^ words[i - 8] ^ words[i - 14] ^ words[i - 16], 1);
        auto a = hash[0], b = hash[1], c = hash[2], d = hash[3], e = hash[4];
        for (int i = 0; i < 80; ++i) {
            const uint32_t f = i < 20   ? (b & c) | (~b & d)
                               : i < 40 ? b ^ c ^ d
                               : i < 60 ? (b & c) | (b & d) | (c & d)
                                        : b ^ c ^ d;
            const uint32_t k = i < 20 ? 0x5A827999 : i < 40 ? 0x6ED9EBA1 : i < 60 ? 0x8F1BBCDC : 0xCA62C1D6;
            const uint32_t next = Rotate (a, 5) + f + e + k + words[i];
            e = d;
            d = c;
            c = Rotate (b, 30);
            b = a;
            a = next;
        }
        hash[0] += a;
        hash[1] += b;
        hash[2] += c;
        hash[3] += d;
        hash[4] += e;
    }
    std::ostringstream out;
    out << std::hex << std::setfill ('0') << std::setw (8) << hash[0] << std::setw (8) << hash[1];
    return out.str ();
}

struct Record {
    std::string fingerprint;
    Assignment assignment;
};
bool Records (const meta::Value& value, std::vector<Record>& records)
{
    if (value.type != meta::ValueType::List || value.elementType != meta::ValueType::Object || value.list.size () > 256)
        return false;
    for (const auto& entry : value.list) {
        if (entry.type != meta::ValueType::Object)
            return false;
        const auto& fields = entry.fields;
        if (!fields.count ("segmentIndex") || !fields.count ("mode") || !fields.count ("distance") ||
            !fields.count ("geometryFingerprint"))
            return false;
        const auto& index = fields.at ("segmentIndex");
        const auto& mode = fields.at ("mode");
        const auto& distance = fields.at ("distance");
        const auto& fingerprint = fields.at ("geometryFingerprint");
        if (index.type != meta::ValueType::Int || index.i < 0 || mode.type != meta::ValueType::String ||
            distance.type != meta::ValueType::Length || !std::isfinite (distance.d) || distance.d < 0 ||
            distance.d > 1000 || fingerprint.type != meta::ValueType::String || fingerprint.s.empty ())
            return false;
        int choice = -1;
        for (int m = 0; m < 3; ++m)
            if (mode.s == ModeName (Mode (m)))
                choice = m;
        if (choice < 0 || (choice == 2 && distance.d != 0))
            return false;
        records.push_back ({ fingerprint.s, { Mode (choice), distance.d, false } });
    }
    return true;
}
} // namespace

const char* ModeName (Mode mode)
{
    switch (mode) {
        case Mode::Default:
            return "Default";
        case Mode::Custom:
            return "Custom";
        case Mode::None:
            return "None";
    }
    return "";
}

std::string Fingerprint (const Edge& edge)
{
    for (double value : { edge.ax, edge.ay, edge.bx, edge.by, edge.arcAngle })
        if (!std::isfinite (value) || std::abs (value) > 1e9)
            return {};
    std::pair<int64_t, int64_t> a { Quantize (edge.ax), Quantize (edge.ay) };
    std::pair<int64_t, int64_t> b { Quantize (edge.bx), Quantize (edge.by) };
    double angle = edge.arcAngle;
    if (a > b) {
        std::swap (a, b);
        angle = -angle;
    }
    return Hash (std::to_string (a.first) + "," + std::to_string (a.second) + "|" + std::to_string (b.first) + "," +
                 std::to_string (b.second) + "|" + std::to_string (Quantize (angle)));
}

bool ValidEdges (const std::vector<Edge>& edges)
{
    if (edges.size () < 3 || edges.size () > 256)
        return false;
    for (size_t i = 0; i < edges.size (); ++i) {
        const auto& edge = edges[i];
        const auto& next = edges[(i + 1) % edges.size ()];
        if (Fingerprint (edge).empty () || std::hypot (edge.bx - edge.ax, edge.by - edge.ay) < 1e-7 ||
            std::abs (edge.arcAngle) >= 6.283185307179586 || std::hypot (edge.bx - next.ax, edge.by - next.ay) > 1e-7)
            return false;
    }
    return true;
}

bool SameGeometry (const std::vector<Edge>& a, const std::vector<Edge>& b)
{
    if (a.size () != b.size ())
        return false;
    for (size_t i = 0; i < a.size (); ++i)
        if (a[i].ax != b[i].ax || a[i].ay != b[i].ay || a[i].bx != b[i].bx || a[i].by != b[i].by ||
            a[i].arcAngle != b[i].arcAngle)
            return false;
    return true;
}

bool Restore (Page& page, const meta::EntityMetadata& entity)
{
    page.known = false;
    page.assignments.clear ();
    page.note.clear ();
    const auto* property = meta::FindProperty (entity, "setback.segments");
    page.hasStored = property != nullptr;
    page.stored = property == nullptr ? meta::Value {} : property->value;
    if (!ValidEdges (page.edges)) {
        page.note = "Property line needs a closed 3..256 edge ring with valid geometry.";
        return false;
    }
    std::vector<Record> records;
    if (property != nullptr && !Records (property->value, records)) {
        page.note = "Invalid saved setback.segments; no metadata will be overwritten.";
        return false;
    }
    std::map<std::string, size_t> currentCounts, savedCounts;
    for (const auto& edge : page.edges)
        ++currentCounts[Fingerprint (edge)];
    for (const auto& record : records)
        ++savedCounts[record.fingerprint];
    size_t matched = 0;
    for (const auto& edge : page.edges) {
        const auto fingerprint = Fingerprint (edge);
        Assignment assignment;
        if (property != nullptr) {
            assignment.review = true;
            if (currentCounts[fingerprint] == 1 && savedCounts[fingerprint] == 1)
                for (const auto& record : records)
                    if (record.fingerprint == fingerprint) {
                        assignment = record.assignment;
                        ++matched;
                        break;
                    }
        }
        page.assignments.push_back (assignment);
    }
    if (matched != records.size ())
        for (auto& assignment : page.assignments)
            assignment.review = true;
    if (std::any_of (page.assignments.begin (), page.assignments.end (), [] (const Assignment& a) { return a.review; }))
        page.note = "Geometry changed or is ambiguous. Review every marked assignment before saving.";
    page.known = true;
    return true;
}

bool CheckSource (const Edit& edit, const std::vector<Edge>& current, const meta::EntityMetadata& entity,
                  std::string& error)
{
    error.clear ();
    if (!edit.before.known || edit.before.guid.empty ()) {
        error = "No valid property-line snapshot was captured for Save.";
        return false;
    }
    if (!SameGeometry (current, edit.before.edges)) {
        error = "Property line changed before Save; reread and review the current assignments.";
        return false;
    }
    const auto* role = meta::FindProperty (entity, "tapioca.role");
    const auto* old = meta::FindProperty (entity, "setback.segments");
    if (role == nullptr || role->value.type != meta::ValueType::String || role->value.s != "PropertyLine" ||
        (old != nullptr) != edit.before.hasStored || (old != nullptr && old->value != edit.before.stored)) {
        error = "Property-line role or saved assignments changed before Save; reread before editing.";
        return false;
    }
    return true;
}

bool Encode (const std::vector<Edge>& edges, const std::vector<Assignment>& assignments, meta::Property& property,
             std::string& error)
{
    error.clear ();
    if (!ValidEdges (edges) || edges.size () != assignments.size ()) {
        error = "Invalid property-line geometry or assignment count.";
        return false;
    }
    meta::Property result;
    result.key = "setback.segments";
    result.value.type = meta::ValueType::List;
    result.value.elementType = meta::ValueType::Object;
    for (size_t i = 0; i < edges.size (); ++i) {
        const auto& assignment = assignments[i];
        if (assignment.review || *ModeName (assignment.mode) == '\0' || !std::isfinite (assignment.distance) ||
            assignment.distance < 0 || assignment.distance > 1000 ||
            (assignment.mode == Mode::None && assignment.distance != 0)) {
            error = "Review assignments and use finite offsets from 0 to 1000 m before saving.";
            return false;
        }
        meta::Value row;
        row.type = meta::ValueType::Object;
        row.fields = { { "segmentIndex", meta::Value::Integer (int64_t (i)) },
                       { "mode", meta::Value::Text (ModeName (assignment.mode)) },
                       { "distance", meta::Value::Number (assignment.distance, meta::ValueType::Length) },
                       { "geometryFingerprint", meta::Value::Text (Fingerprint (edges[i])) } };
        result.value.list.push_back (std::move (row));
    }
    property = std::move (result);
    return true;
}
} // namespace geomsrv::archviz::massingrules
