#ifndef TAPIOCA_ELEMENT_CHANGE_JOURNAL_HPP
#define TAPIOCA_ELEMENT_CHANGE_JOURNAL_HPP

#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace tapioca::sync {

// Main-thread, host-neutral fan-out. Readers own their cursors; reading cannot
// consume another definition's changes. Overflow/gaps require an explicit rebase.
class ElementChangeJournal {
  public:
    enum Mask : uint32_t { Geometry = 1, Transform = 2, Parameters = 4, Properties = 8, Membership = 16, Unknown = 32 };
    struct Change {
        std::string guid;
        uint64_t revision;
        uint32_t mask;
    };
    struct Delta {
        uint64_t epoch;
        uint64_t revision;
        bool sourceActive;
        bool resyncRequired;
        std::vector<Change> changes;
    };
    static constexpr size_t Capacity = 4096;

    void SetEpoch (uint64_t value)
    {
        if (epoch == value)
            return;
        epoch = value;
        entries.clear ();
        revision = floor = 0;
        active = false;
    }
    void SetActive (bool value)
    {
        if (active == value)
            return;
        Invalidate (); // no promise about changes during an unwatched interval
        active = value;
    }
    void Invalidate ()
    {
        entries.clear ();
        floor = ++revision;
    }
    void Record (const std::set<std::string>& guids, uint32_t mask)
    {
        if (guids.empty () || mask == 0)
            return;
        const uint64_t next = ++revision;
        if (guids.size () > Capacity) {
            entries.clear ();
            floor = next;
            return;
        }
        while (entries.size () + guids.size () > Capacity) {
            // Evict a whole atomic source batch, never part of its revision.
            const uint64_t lost = entries.front ().revision;
            floor = lost;
            while (!entries.empty () && entries.front ().revision == lost)
                entries.pop_front ();
        }
        for (const auto& guid : guids)
            entries.push_back ({ guid, next, mask });
    }
    Delta Read (uint64_t expectedEpoch, uint64_t since, const std::set<std::string>& guids,
                uint32_t fields) const
    {
        Delta result { epoch, revision, active,
                       expectedEpoch != epoch || !active || since < floor || since > revision, {} };
        if (result.resyncRequired)
            return result;
        std::map<std::string, Change> merged;
        for (const auto& entry : entries) {
            if (entry.revision <= since || guids.count (entry.guid) == 0 ||
                (entry.mask & (fields | Unknown)) == 0)
                continue;
            auto [it, inserted] = merged.emplace (entry.guid, entry);
            if (!inserted) {
                it->second.revision = entry.revision;
                it->second.mask |= entry.mask;
            }
        }
        for (auto& [guid, change] : merged)
            result.changes.push_back (std::move (change));
        return result;
    }

  private:
    uint64_t epoch = 0, revision = 0, floor = 0;
    bool active = false;
    std::deque<Change> entries;
};

inline ElementChangeJournal& ElementChanges ()
{
    static ElementChangeJournal journal;
    return journal;
}

} // namespace tapioca::sync
#endif
