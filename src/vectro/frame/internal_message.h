#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "vectro/frame/raw_buffer.h"

namespace vectro {
namespace frame {

enum class MessageOrigin { kUnknown = 0, kServer = 1, kClient = 2 };

struct InternalMessageMeta {
  uint64_t trace_id      = 0;  // Unique trace ID
  uint64_t connection_id = 0;  // Session or client ID
  std::chrono::steady_clock::time_point timestamp;
  std::optional<std::string> tag;         // Optional: used for classification
  std::string remote_ip;                  // Peer IP address
  uint16_t remote_port = 0;               // Peer port
  uint16_t local_port  = 0;               // Local server port
  std::optional<std::string> route_hint;  // Optional: routing label
  bool is_behind_reverse_proxy = false;
  bool is_can_get_real_ip      = true;
  MessageOrigin origin         = MessageOrigin::kUnknown;
};

class InternalMessage {
 public:
  InternalMessage() = default;

  InternalMessage(InternalMessageMeta meta, RawBuffer buffer);

  InternalMessageMeta& Meta();
  const InternalMessageMeta& Meta() const;

  RawBuffer& buffer();
  const RawBuffer& buffer() const;

  bool valid() const;

 private:
  InternalMessageMeta meta_;
  RawBuffer buffer_;
};
}  // namespace frame
}  // namespace vectro
