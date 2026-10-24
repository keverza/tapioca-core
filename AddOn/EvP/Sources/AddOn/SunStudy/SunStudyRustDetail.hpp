#ifndef EVP_SUNSTUDY_SUNSTUDYRUSTDETAIL_HPP
#define EVP_SUNSTUDY_SUNSTUDYRUSTDETAIL_HPP

// SunStudy/SunStudyRustDetail -- helpers shared by the SunStudyRust*.cpp
// wrappers: the error buffer and call envelope, and POD conversions. Not part
// of the wrapper API; include only from those translation units.

#include "SunStudy/SunStudyRust.hpp"
#include "SunStudy/SunStudyRustFfi.h"

#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace evp::sunstudy::rust::detail {

inline constexpr size_t kErrorCapacity = 256;
using ErrorBuffer = std::array<char, kErrorCapacity>;

inline std::string FailureMessage (TapiocaStatus status, const ErrorBuffer& buffer)
{
    size_t length = 0;
    while (length < buffer.size () && buffer[length] != '\0')
        ++length;
    if (length != 0)
        return std::string (buffer.data (), length);
    // A failed status with no diagnostic is still a failure (overlap refusals
    // deliberately write nothing); never turn it into success.
    return "Tapioca Rust sun-study call failed with status " + std::to_string (status);
}

// Runs one export with a fresh error buffer; true on TAPIOCA_OK.
template <typename Call> inline bool Invoke (Call&& call, std::string& error)
{
    ErrorBuffer buffer {};
    const TapiocaStatus status = call (buffer.data (), buffer.size ());
    if (status != TAPIOCA_OK) {
        error = FailureMessage (status, buffer);
        return false;
    }
    error.clear ();
    return true;
}

inline void Copy3 (const Vec3& from, double (&to)[3])
{
    std::copy (from.begin (), from.end (), to);
}

inline Vec3 ToVec3 (const double (&from)[3])
{
    return { from[0], from[1], from[2] };
}

inline Mat4 ToMat4 (const double (&from)[16])
{
    Mat4 m {};
    std::copy (from, from + 16, m.begin ());
    return m;
}

inline TapiocaSunStep ToPod (const SunStep& step)
{
    TapiocaSunStep pod {};
    pod.hour = step.time.hour;
    pod.minute = step.time.minute;
    std::copy (step.direction, step.direction + 3, pod.direction);
    pod.altitudeDegrees = step.altitudeDegrees;
    return pod;
}

inline SunStep FromPod (const TapiocaSunStep& pod)
{
    SunStep step;
    step.time = TimeOfDay { pod.hour, pod.minute };
    std::copy (pod.direction, pod.direction + 3, step.direction);
    step.altitudeDegrees = pod.altitudeDegrees;
    return step;
}

// Borrowed text views over a string list; valid while `strings` lives.
inline std::vector<TapiocaTextView> TextViews (const std::vector<std::string>& strings)
{
    std::vector<TapiocaTextView> views;
    views.reserve (strings.size ());
    for (const std::string& s : strings)
        views.push_back (TapiocaTextView { s.data (), s.size () });
    return views;
}

inline TapiocaTextArrayView ArrayView (const std::vector<TapiocaTextView>& views)
{
    return TapiocaTextArrayView { views.data (), views.size () };
}

} // namespace evp::sunstudy::rust::detail

#endif
