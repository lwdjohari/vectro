#pragma once
#include <memory>
#include <string>

#include "absl/container/node_hash_map.h"
#include "vectro/plugin/plugin_base.h"
#include "vectro/plugin/plugin_key.h"

namespace vectro {
namespace plugin {
// Container that owns plugin instances per component (session, client, server).
class PluginBundle {
 public:
  void Set(const std::string& key, std::shared_ptr<PluginBase> plugin);
  std::shared_ptr<PluginBase> Get(const std::string& key) const;
  const std::shared_ptr<PluginBase>& GetRef(const std::string& key) const;
  void Remove(const std::string& key);
  bool Has(const std::string& key) const;
  void MergeFrom(const PluginBundle& fallback);
  PluginBundle OverlayWith(const PluginBundle& upper) const;
  bool IsEmpty() const;

 private:
  absl::node_hash_map<std::string, std::shared_ptr<PluginBase>> plugins_;
};

// Plugin typed access helper
template <typename T>
std::shared_ptr<T> GetTypedPlugin(const PluginBundle& bundle,
                                  const std::string& key) {
  auto base = bundle.Get(key);
  return base ? std::dynamic_pointer_cast<T>(base) : nullptr;
}
}  // namespace plugin
}  // namespace vectro
