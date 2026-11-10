#ifndef TAPIOCA_PUBLICATION_ADMISSION_HPP
#define TAPIOCA_PUBLICATION_ADMISSION_HPP

#include <cstdint>
#include <map>
#include <set>
#include <string>

namespace tapioca::preview {

// Main-thread authority. A project transition invalidates every queued token,
// even when the replacement has the same filename or is another untitled file.
class PublicationAdmission {
  public:
    struct Token {
        uint64_t epoch = 0;
        std::string producer;
        std::string document;
        uint64_t revision = 0;
    };
    uint64_t Epoch () const
    {
        return epoch;
    }
    void ProjectChanged ()
    {
        ++epoch;
        accepted.clear ();
        retired.clear ();
    }
    bool Current (const Token& token) const
    {
        if (token.epoch != epoch || token.revision == 0 || token.document.size () != 32 || token.producer.empty ())
            return false;
        if (retired.size () >= 4096)
            return false;
        for (const auto& prefix : retired)
            if (token.producer.rfind (prefix, 0) == 0)
                return false;
        for (char c : token.document)
            if (!(c >= '0' && c <= '9') && !(c >= 'a' && c <= 'f'))
                return false;
        if (token.document == std::string (32, '0'))
            return false;
        const auto found = accepted.find (token.producer);
        if (found == accepted.end ())
            return accepted.size () < 4096;
        return found->second.document == token.document && found->second.revision < token.revision;
    }
    bool Adopt (const Token& token)
    {
        if (!Current (token))
            return false;
        accepted[token.producer] = token; // Clear retains the revision as a tombstone.
        return true;
    }
    void ReleasePrefix (const std::string& prefix)
    {
        if (retired.size () < 4096)
            retired.insert (prefix);
        for (auto it = accepted.begin (); it != accepted.end ();)
            if (it->first.rfind (prefix, 0) == 0)
                it = accepted.erase (it);
            else
                ++it;
    }

  private:
    uint64_t epoch = 1;
    std::map<std::string, Token> accepted;
    std::set<std::string> retired;
};

inline PublicationAdmission& PublicationState ()
{
    static PublicationAdmission state;
    return state;
}

} // namespace tapioca::preview
#endif
