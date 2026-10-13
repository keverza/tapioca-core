#ifndef EVP_ARCHVIZ_ROOFSLOPECANVAS_HPP
#define EVP_ARCHVIZ_ROOFSLOPECANVAS_HPP

#include <array>
#include <initializer_list>
#include <vector>
struct ImDrawList;
namespace geomsrv::archviz::overlayhud {
struct Vertex;
}
namespace geomsrv::archviz::roofcanvas {
// Preserve model anchors while ImGui tessellates grips and editable readouts.
// The existing glyph shader reprojects these at Present; no frame/camera changes.
struct Anchor {
    std::array<double, 3> model {};
    std::array<float, 2> screen {};
    std::array<double, 3> direction {};
    std::array<float, 2> screenDirection {};
    bool turn = false;
};
struct Batch {
    int first = 0, last = 0; // indices, not vertices (ImGui can reuse a vertex)
    std::vector<Anchor> anchors;
};
struct Frame {
    ImDrawList* list = nullptr;
    std::vector<Batch> batches;
};
class Scope {
  public:
    Scope (Frame& frame, std::initializer_list<Anchor> anchors);
    ~Scope ();
    Scope (const Scope&) = delete;
    Scope& operator= (const Scope&) = delete;

  private:
    Frame& frame;
    Batch batch;
};
bool Bind (const Frame& frame, const ImDrawList* list, int index, overlayhud::Vertex& vertex);
} // namespace geomsrv::archviz::roofcanvas
#endif
