#include "vectro/plugin/plugin_bundle.h"

namespace vectro {
namespace plugin {
/* Public */
void PluginBundle::Set(const std::string& key,
                       std::shared_ptr<PluginBase> plugin) {
  plugins_[key] = std::move(plugin);
}

std::shared_ptr<PluginBase> PluginBundle::Get(const std::string& key) const {
  auto it = plugins_.find(key);
  return it != plugins_.end() ? it->second : nullptr;
}

const std::shared_ptr<PluginBase>& PluginBundle::GetRef(
    const std::string& key) const {
  static std::shared_ptr<PluginBase> null{};
  auto it = plugins_.find(key);
  return it != plugins_.end() ? it->second : null;
}

void PluginBundle::Remove(const std::string& key) {
  plugins_.erase(key);
}

bool PluginBundle::Has(const std::string& key) const {
  return plugins_.find(key) != plugins_.end();
}

void PluginBundle::MergeFrom(const PluginBundle& fallback) {
  for (const auto& [k, v] : fallback.plugins_) {
    plugins_.insert({k, v});
  }
}

PluginBundle PluginBundle::OverlayWith(const PluginBundle& upper) const {
  PluginBundle result = *this;
  for (const auto& [k, v] : upper.plugins_) {
    result.plugins_[k] = v;
  }
  return result;
}

bool PluginBundle::IsEmpty() const {
  return plugins_.empty();
}

}  // namespace plugin
}  // namespace vectro