#include "vectro/frame/internal_message.h"

namespace vectro {
namespace frame {
/* public */  

  InternalMessage::InternalMessage(InternalMessageMeta meta, RawBuffer buffer)
                  : meta_(std::move(meta)), buffer_(std::move(buffer)) {}

  InternalMessageMeta& InternalMessage::Meta() {
    return meta_;
  }

  const InternalMessageMeta& InternalMessage::Meta() const {
    return meta_;
  }

  RawBuffer& InternalMessage::buffer() {
    return buffer_;
  }

  const RawBuffer& InternalMessage::buffer() const {
    return buffer_;
  }

  bool InternalMessage::valid() const {
    return buffer_.valid();
  }

 
}  // namespace frame
}  // namespace vectro