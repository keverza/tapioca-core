#ifndef EVP_ARCHVIZ_ROOFSLOPEDOCUMENT_HPP
#define EVP_ARCHVIZ_ROOFSLOPEDOCUMENT_HPP

// The roof generator's working form (roofslope::Data: local frame, index links) and the
// shared editor document (edit::RoofDesign: world coordinates, stable ids). The document is
// the authority; Data is derived from it for meshing, overlay layers and Bake, so the
// generator, Mesh and the Bake planner keep their contracts unchanged.
#include "ArchViz/RoofSlope.hpp"
#include "Edit/EditDocument.hpp"
#include <string>
#include <vector>

namespace geomsrv::archviz::roofslope {

// Writes a generated plan into `doc`: each outer ring with its openings becomes one
// footprint entity, every control a roof node with a new id, every rectangle a cell. Any
// previous roof and its footprint entities are replaced. A node that is a drain of one cell
// and a corner of another becomes two nodes, one of each kind, at the same place.
bool Seed (edit::Document& doc, const Data& data, std::string& error);
// Derives the generator's Data from the document's roof and meshes it (Refresh). `nodeIds`,
// when given, receives the roof node id behind every Data node index. Refuses a document
// without a roof, a cell that is not a four-corner rectangle in the layout frame, and any
// geometric failure Refresh reports, with its reason.
bool Read (const edit::Document& doc, Data& data, std::string& error, std::vector<edit::Id>* nodeIds = nullptr);
// A Session validator (EditSession.hpp): a document with a roof must read and mesh.
bool Check (const edit::Document& doc, std::string& error);

} // namespace geomsrv::archviz::roofslope
#endif
