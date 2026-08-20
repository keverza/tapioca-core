#include "MetadataExtractor.hpp"

#include "APIEnvir.h"
#include "ACAPinc.h"

#include <algorithm>
#include <unordered_map>

namespace geomsrv {

namespace {

std::string Utf8 (const GS::UniString& u)
{
    return std::string (u.ToCStr (0, GS::MaxUSize, CC_UTF8).Get ());
}

// All stories, sorted bottom -> top, with elevation (world Z) and derived height.
std::vector<Story> BuildStories ()
{
    std::vector<Story> stories;
    API_StoryInfo si = {};
    if (ACAPI_ProjectSetting_GetStorySettings (&si) != NoError || si.data == nullptr)
        return stories;

    const short n = si.lastStory - si.firstStory + 1;
    for (short i = 0; i < n; ++i) {
        const API_StoryType& s = (*si.data)[i];
        Story st;
        st.index = s.index;
        st.name = Utf8 (GS::UniString (s.uName));
        st.elevation = s.level;          // project level == world Z (meters)
        stories.push_back (std::move (st));
    }
    BMKillHandle (reinterpret_cast<GSHandle*> (&si.data));

    std::sort (stories.begin (), stories.end (),
               [] (const Story& a, const Story& b) { return a.index < b.index; });

    // Height = distance to the story above. The topmost has none — leave it unset
    // (serialized as null) rather than inventing a value.
    for (size_t i = 0; i + 1 < stories.size (); ++i) {
        stories[i].height = stories[i + 1].elevation - stories[i].elevation;
        stories[i].hasHeight = true;
    }
    return stories;
}

std::string LayerName (const API_AttributeIndex& layerIdx)
{
    API_Attribute attr = {};
    attr.header.typeID = API_LayerID;
    attr.header.index  = layerIdx;
    GS::UniString name;
    attr.header.uniStringNamePtr = &name;
    if (ACAPI_Attribute_Get (&attr) != NoError)
        return {};
    return Utf8 (name);
}

void FillClassifications (const API_Guid& g, ElementMeta& m)
{
    GS::Array<GS::Pair<API_Guid, API_Guid>> pairs;   // (system, item)
    if (ACAPI_Element_GetClassificationItems (g, pairs) != NoError)
        return;
    for (const auto& p : pairs) {
        API_ClassificationItem item = {};
        item.guid = p.second;
        if (ACAPI_Classification_GetClassificationItem (item) == NoError)
            m.classifications.emplace_back (Utf8 (item.id), Utf8 (item.name));
    }
}

void FillProperties (const API_Guid& g, ElementMeta& m)
{
    GS::Array<API_PropertyDefinition> defs;
    if (ACAPI_Element_GetPropertyDefinitions (g, API_PropertyDefinitionFilter_UserDefined, defs) != NoError)
        return;
    GS::Array<API_Property> props;
    if (ACAPI_Element_GetPropertyValues (g, defs, props) != NoError)
        return;
    const USize n = props.GetSize ();
    for (USize i = 0; i < n; ++i) {
        GS::UniString vs;
        if (ACAPI_Property_GetPropertyValueString (props[i], &vs) == NoError)
            m.properties.emplace_back (Utf8 (defs[i].name), Utf8 (vs));
    }
}

} // namespace

std::shared_ptr<const MetaSet> ExtractMetadataFor (const std::vector<std::string>& guids,
                                                   MetaLevel level,
                                                   const CancelFn& shouldCancel,
                                                   const ProgressFn& onProgress)
{
    if (level == MetaLevel::None)
        return nullptr;

    auto set = std::make_shared<MetaSet> ();
    set->stories = BuildStories ();          // cheap: one call, a handful of rows
    set->elems.reserve (guids.size ());

    std::unordered_map<short, std::string> storyName;
    for (const auto& s : set->stories)
        storyName[static_cast<short> (s.index)] = s.name;

    const size_t total = guids.size ();
    size_t done = 0;

    for (const std::string& gs : guids) {
        // Poll cancel / report progress every so often — not per element, or the
        // polling itself becomes the bottleneck on a big model.
        if ((done & 0x3F) == 0) {
            if (shouldCancel && shouldCancel ())
                return nullptr;                    // user hit Cancel
            if (onProgress)
                onProgress (done, total);
        }
        ++done;

        API_Guid g = APIGuidFromString (gs.c_str ());
        if (g == APINULLGuid)
            continue;

        API_Elem_Head head = {};
        head.guid = g;
        if (ACAPI_Element_GetHeader (&head) != NoError)
            continue;      // not an API element (composite sub-part) — expected

        ElementMeta m;
        m.guid = Utf8 (APIGuidToString (g));

        GS::UniString tn;
        if (ACAPI_Element_GetElemTypeName (head.type, tn) == NoError)
            m.typeName = Utf8 (tn);

        GS::UniString info;
        if (ACAPI_Element_GetElementInfoString (&g, &info) == NoError)
            m.elemId = Utf8 (info);

        m.layer = LayerName (head.layer);

        auto it = storyName.find (head.floorInd);
        if (it != storyName.end ())
            m.story = it->second;

        // The expensive half — only when explicitly asked for.
        if (level == MetaLevel::Full) {
            FillClassifications (g, m);
            FillProperties (g, m);
        }

        set->byGuid[m.guid] = set->elems.size ();
        set->elems.push_back (std::move (m));
    }
    if (onProgress)
        onProgress (total, total);
    return set;
}

} // namespace geomsrv
