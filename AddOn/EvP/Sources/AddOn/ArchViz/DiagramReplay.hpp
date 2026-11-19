#ifndef EVP_ARCHVIZ_DIAGRAMREPLAY_HPP
#define EVP_ARCHVIZ_DIAGRAMREPLAY_HPP
// A live diagram input written as JSON, and read back offline (#201).
//
// The file holds exactly what Capture and BuildScene read: the published massing rows
// (guid, function, story, elevation, height, colour, slice chains), the building records,
// the story cut when there is one, the project storeys with their floorIds, and the
// revision stamps, and -- optional -- the Massing workflow's planned floors. Nothing else of massingslices::Result is
// kept -- overlay layers, metrics and coverage are derived presentation, not diagram input -- so a replayed file
// rebuilds the same Scene, LayoutPlan and Frame as the live session that wrote it.
//
// Pure: no ACAPI, file IO or renderer. The caller reads and writes the text.
#include "ArchViz/DiagramSnapshot.hpp"
#include <string>

namespace geomsrv::archviz::diagram {
constexpr char kCaptureFormat[] = "tapioca.diagram.capture";
constexpr int64_t kCaptureVersion = 1;
constexpr size_t kMaxCaptureText = 64u * 1024u * 1024u;

struct CaptureInputs {
    std::shared_ptr<const massingslices::Result> slices; // null: the session had none
    ProjectStoreys storeys;
    RevisionSet revisions;
    std::shared_ptr<const FloorPlans> plans; // null: the session had none
    std::string note;                        // free text the writer attached (project, build, reason)
};

// The snapshot's inputs, never its derived fields: Capture re-derives them on replay.
std::string WriteCapture (const DiagramInputSnapshot& snapshot, const std::string& note = {});
// The capture text with its revision stamps zeroed and no note: what the diagram is built
// from, and nothing about when. Two snapshots with the same signature build the same
// diagram, whatever bumped the stamps between them -- the live follow keys on it.
std::string ContentSignature (const DiagramInputSnapshot& snapshot);
// Strict: an unknown format or version, or a malformed row, refuses the whole file.
bool ReadCapture (const std::string& text, CaptureInputs& out, std::string& error);
// ReadCapture then Capture: the offline twin of the live session's capture step.
bool ReplayCapture (const std::string& text, DiagramInputSnapshot& out, std::string& error);
} // namespace geomsrv::archviz::diagram
#endif
