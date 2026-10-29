#ifndef TAPIOCA_VIRTUAL_EDITOR_MATERIAL_PICKER_HPP
#define TAPIOCA_VIRTUAL_EDITOR_MATERIAL_PICKER_HPP
#include <memory>
namespace geomsrv::archviz::overlayhud {
struct State;
}
namespace geomsrv::archviz {
// Main-thread owner queues catalog/picker work after layout, never from Present.
void FollowMaterialPicker (const std::shared_ptr<overlayhud::State>& state);
} // namespace geomsrv::archviz
#endif
