#ifndef GEOMETRYSERVER_SURFACECRITERIA_HPP
#define GEOMETRYSERVER_SURFACECRITERIA_HPP
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace geomsrv::surfacecriteria {
enum class Field { ElementType, Layer, HomeStory, ElementId, Surface, Facing };
enum class Condition { Is, IsNot, Contains, StartsWith, EndsWith };
struct Element {
    std::string guid, type, layer, story, id;
    bool detailsAvailable = false;
};
struct Rule {
    int32_t id = 0; // draft row identity, retained when rows move
    Field field = Field::ElementType;
    Condition condition = Condition::Is;
    std::string value;
    bool orNext = false;
    uint32_t open = 0, close = 0;
};
constexpr size_t kMaxRules = 16;
const std::vector<std::string>& Fields ();
const std::vector<std::string>& Conditions ();
std::string TypeName (int32_t modelerType);
std::string Validate (const std::vector<Rule>& rules);
// AND binds before OR; parentheses override precedence. Missing metadata never
// satisfies a predicate (including Is not). Empty rules mean every captured target.
bool Matches (const std::vector<Rule>& rules, const Element& element, const std::string& surface,
              const std::string& facing);
} // namespace geomsrv::surfacecriteria
#endif
