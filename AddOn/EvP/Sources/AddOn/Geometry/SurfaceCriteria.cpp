#include "SurfaceCriteria.hpp"
#include <algorithm>
#include <functional>
#include <set>

namespace geomsrv::surfacecriteria {
const std::vector<std::string>& Fields ()
{
    static const std::vector<std::string> fields { "Element Type", "Layer",   "Home Story",
                                                   "Element ID",   "Surface", "Face direction" };
    return fields;
}
const std::vector<std::string>& Conditions ()
{
    static const std::vector<std::string> conditions { "is", "is not", "contains", "starts with", "ends with" };
    return conditions;
}
std::string TypeName (int32_t type)
{
    switch (type) {
        case 1:
            return "Wall";
        case 2:
            return "Slab";
        case 17:
            return "Column";
        case 19:
            return "Beam";
        default:
            return "Modeler type " + std::to_string (type);
    }
}
namespace {
enum class Token { Value, And, Or, Open, Close };
struct Term {
    Token kind;
    bool value = false;
};
bool Evaluate (const std::vector<Rule>& rules, const std::function<bool (const Rule&)>& predicate, bool& valid)
{
    if (rules.empty ()) {
        valid = true;
        return true;
    }
    std::vector<Term> terms;
    for (size_t i = 0; i < rules.size (); ++i) {
        for (uint32_t n = 0; n < rules[i].open; ++n)
            terms.push_back ({ Token::Open });
        terms.push_back ({ Token::Value, predicate (rules[i]) });
        for (uint32_t n = 0; n < rules[i].close; ++n)
            terms.push_back ({ Token::Close });
        if (i + 1 < rules.size ())
            terms.push_back ({ rules[i].orNext ? Token::Or : Token::And });
    }
    size_t at = 0;
    valid = true;
    std::function<bool ()> expression, conjunction, atom;
    atom = [&] {
        if (at == terms.size ()) {
            valid = false;
            return false;
        }
        if (terms[at].kind == Token::Value)
            return terms[at++].value;
        if (terms[at++].kind != Token::Open) {
            valid = false;
            return false;
        }
        const bool value = expression ();
        if (at == terms.size () || terms[at++].kind != Token::Close)
            valid = false;
        return value;
    };
    conjunction = [&] {
        bool value = atom ();
        while (at < terms.size () && terms[at].kind == Token::And) {
            ++at;
            const bool rhs = atom ();
            value = value && rhs;
        }
        return value;
    };
    expression = [&] {
        bool value = conjunction ();
        while (at < terms.size () && terms[at].kind == Token::Or) {
            ++at;
            const bool rhs = conjunction ();
            value = value || rhs;
        }
        return value;
    };
    const bool result = expression ();
    valid &= at == terms.size ();
    return result;
}
std::string Fold (std::string text)
{
    for (char& c : text)
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
    return text;
}
} // namespace
std::string Validate (const std::vector<Rule>& rules)
{
    if (rules.size () > kMaxRules)
        return "At most 16 criteria rows.";
    std::set<int32_t> ids;
    for (const auto& rule : rules) {
        if (!ids.insert (rule.id).second || int (rule.field) < 0 || size_t (rule.field) >= Fields ().size () ||
            int (rule.condition) < 0 || size_t (rule.condition) >= Conditions ().size () || rule.value.size () > 512 ||
            rule.open > 3 || rule.close > 3)
            return "Invalid criterion field, condition, value or row identity.";
    }
    bool valid = false;
    Evaluate (rules, [] (const Rule&) { return true; }, valid);
    return valid ? "" : "Criteria invalid: check opening and closing parentheses. Last applied rules are retained.";
}
bool Matches (const std::vector<Rule>& rules, const Element& element, const std::string& surface,
              const std::string& facing)
{
    if (!Validate (rules).empty ())
        return false;
    bool valid = false;
    return Evaluate (
               rules,
               [&] (const Rule& rule) {
                   std::optional<std::string> actual;
                   switch (rule.field) {
                       case Field::ElementType:
                           actual = element.type;
                           break;
                       case Field::Layer:
                           if (element.detailsAvailable)
                               actual = element.layer;
                           break;
                       case Field::HomeStory:
                           if (element.detailsAvailable)
                               actual = element.story;
                           break;
                       case Field::ElementId:
                           if (element.detailsAvailable)
                               actual = element.id;
                           break;
                       case Field::Surface:
                           actual = surface;
                           break;
                       case Field::Facing:
                           actual = facing;
                           break;
                   }
                   if (!actual)
                       return false;
                   const auto a = Fold (*actual), b = Fold (rule.value);
                   switch (rule.condition) {
                       case Condition::Is:
                           return a == b;
                       case Condition::IsNot:
                           return a != b;
                       case Condition::Contains:
                           return a.find (b) != std::string::npos;
                       case Condition::StartsWith:
                           return a.starts_with (b);
                       case Condition::EndsWith:
                           return a.ends_with (b);
                   }
                   return false;
               },
               valid) &&
           valid;
}
} // namespace geomsrv::surfacecriteria
