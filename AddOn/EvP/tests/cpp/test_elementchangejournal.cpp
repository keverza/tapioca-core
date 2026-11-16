#include "Notify/ElementChangeJournal.hpp"
#include <gtest/gtest.h>

using tapioca::sync::ElementChangeJournal;

TEST (ElementChangeJournal, WakeDoesNotConsumeAndCanBeDetachedBeforeOwnerShutdown)
{
    ElementChangeJournal journal;
    int wakes = 0;
    journal.changed = [&] { ++wakes; };
    journal.SetEpoch (1);
    journal.SetActive (true);
    journal.Record ({ "slab" }, ElementChangeJournal::Unknown);
    EXPECT_EQ (wakes, 3);
    EXPECT_EQ (journal.Read (1, 1, { "slab" }, 1).changes.size (), 1u);
    EXPECT_EQ (wakes, 3);
    journal.changed = {};
    journal.SetActive (false);
    EXPECT_EQ (wakes, 3);
}

TEST (ElementChangeJournal, ScopedReadersDoNotConsumeEachOther)
{
    ElementChangeJournal journal;
    journal.SetEpoch (7);
    journal.SetActive (true);
    journal.Record ({ "slab", "wall" }, ElementChangeJournal::Geometry);
    const auto slab = journal.Read (7, 1, { "slab" }, 1);
    const auto wall = journal.Read (7, 1, { "wall" }, 1);
    ASSERT_FALSE (slab.resyncRequired);
    ASSERT_EQ (slab.changes.size (), 1u);
    EXPECT_EQ (slab.changes[0].guid, "slab");
    ASSERT_EQ (wall.changes.size (), 1u);
    EXPECT_EQ (wall.changes[0].guid, "wall");
    EXPECT_TRUE (journal.Read (7, slab.revision, { "slab" }, 1).changes.empty ());
    EXPECT_TRUE (journal.Read (7, 1, { "unrelated" }, 1).changes.empty ());
}

TEST (ElementChangeJournal, FieldFilteringCoalescingAndUnknownInvalidation)
{
    ElementChangeJournal journal;
    journal.SetEpoch (1);
    journal.SetActive (true);
    journal.Record ({ "a" }, ElementChangeJournal::Properties);
    EXPECT_TRUE (journal.Read (1, 1, { "a" }, ElementChangeJournal::Geometry).changes.empty ());
    journal.Record ({ "a" }, ElementChangeJournal::Geometry);
    auto delta = journal.Read (1, 1, { "a" }, 31);
    ASSERT_EQ (delta.changes.size (), 1u);
    EXPECT_EQ (delta.changes[0].mask, 25u);
    EXPECT_EQ (delta.changes[0].revision, delta.revision);
    journal.Record ({ "a" }, ElementChangeJournal::Unknown);
    EXPECT_EQ (journal.Read (1, delta.revision, { "a" }, ElementChangeJournal::Properties).changes.size (), 1u);
}

TEST (ElementChangeJournal, MembershipRefreshesOwnersForNewMovedAndRemovedTargets)
{
    ElementChangeJournal journal;
    journal.SetEpoch (1);
    journal.SetActive (true);
    uint64_t cursor = 1;
    for (const auto* target : { "new-child", "moved-child", "removed-child" }) {
        journal.Record ({ target }, ElementChangeJournal::Unknown);
        const auto delta = journal.Read (1, cursor, { "owner" }, ElementChangeJournal::Membership);
        ASSERT_FALSE (delta.resyncRequired);
        ASSERT_EQ (delta.changes.size (), 1u);
        EXPECT_EQ (delta.changes[0].guid, "owner");
        EXPECT_EQ (delta.changes[0].mask, ElementChangeJournal::Membership);
        EXPECT_EQ (delta.changes[0].revision, delta.revision);
        EXPECT_TRUE (journal.Read (1, cursor, { "owner" }, ElementChangeJournal::Geometry).changes.empty ());
        EXPECT_TRUE (journal.Read (1, cursor, { "owner" }, ElementChangeJournal::Properties).changes.empty ());
        cursor = delta.revision;
        EXPECT_TRUE (journal.Read (1, cursor, { "owner" }, ElementChangeJournal::Membership).changes.empty ());
    }
}

TEST (ElementChangeJournal, MembershipIsBoundedToRequestedOwnersAndIndependentCursors)
{
    ElementChangeJournal journal;
    journal.SetEpoch (1);
    journal.SetActive (true);
    journal.Record ({ "external-a" }, ElementChangeJournal::Geometry);
    journal.Record ({ "owner-a" }, ElementChangeJournal::Properties);
    journal.Record ({ "external-b" }, ElementChangeJournal::Transform);
    const auto delta = journal.Read (1, 1, { "owner-a", "owner-b" },
                                     ElementChangeJournal::Membership | ElementChangeJournal::Properties);
    ASSERT_EQ (delta.changes.size (), 2u);
    EXPECT_EQ (delta.changes[0].guid, "owner-a");
    EXPECT_EQ (delta.changes[0].mask, 24u);
    EXPECT_EQ (delta.changes[1].guid, "owner-b");
    EXPECT_EQ (delta.changes[1].mask, ElementChangeJournal::Membership);
    EXPECT_EQ (delta.changes[0].revision, delta.revision);
    EXPECT_EQ (delta.changes[1].revision, delta.revision);
    EXPECT_EQ (journal.Read (1, 1, { "other-owner" }, ElementChangeJournal::Membership).changes.size (), 1u);
    EXPECT_TRUE (journal.Read (1, 1, {}, ElementChangeJournal::Membership).changes.empty ());
}

TEST (ElementChangeJournal, SourceGapsProjectReplacementAndFutureCursorsRequireRebase)
{
    ElementChangeJournal journal;
    journal.SetEpoch (1);
    EXPECT_TRUE (journal.Read (1, 0, {}, 31).resyncRequired);
    journal.SetActive (true);
    auto baseline = journal.Read (1, 0, {}, 31);
    EXPECT_TRUE (baseline.resyncRequired);
    EXPECT_TRUE (baseline.sourceActive);
    EXPECT_FALSE (journal.Read (1, baseline.revision, {}, 31).resyncRequired);
    journal.SetActive (false);
    journal.SetActive (true);
    EXPECT_TRUE (journal.Read (1, baseline.revision, {}, 31).resyncRequired);
    EXPECT_TRUE (journal.Read (1, 999, {}, 31).resyncRequired);
    journal.SetEpoch (2);
    journal.SetActive (true);
    EXPECT_TRUE (journal.Read (1, 1, {}, 31).resyncRequired);
}

TEST (ElementChangeJournal, OverflowNeverReturnsAPartialBatchAsComplete)
{
    ElementChangeJournal journal;
    journal.SetEpoch (1);
    journal.SetActive (true);
    std::set<std::string> large;
    for (size_t i = 0; i < ElementChangeJournal::Capacity; ++i)
        large.insert (std::to_string (i));
    journal.Record (large, 1);
    journal.Record ({ "new" }, 1);
    EXPECT_TRUE (journal.Read (1, 1, { "0", "new" }, 1).resyncRequired);
    auto delta = journal.Read (1, 2, { "new" }, 1);
    ASSERT_FALSE (delta.resyncRequired);
    ASSERT_EQ (delta.changes.size (), 1u);
    large.insert ("overflow");
    journal.Record (large, 1);
    delta = journal.Read (1, delta.revision, { "new" }, 1);
    EXPECT_TRUE (delta.resyncRequired);
    EXPECT_TRUE (delta.changes.empty ());
}
