#include "workflow/node_registry.hpp"

#include <utility>

namespace workflow
{

void NodeRegistry::add(NodeType type, NodeHandler handler)
{
    handlers_[type] = std::move(handler);
}

NodeHandler const* NodeRegistry::find(NodeType type) const noexcept
{
    const auto it = handlers_.find(type);
    return it == handlers_.end() ? nullptr : &it->second;
}

bool NodeRegistry::has(NodeType type) const noexcept
{
    return handlers_.find(type) != handlers_.end();
}

}  // namespace workflow
