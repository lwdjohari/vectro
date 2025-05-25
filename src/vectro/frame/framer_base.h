// framer_base.h
#pragma once

#include <cstddef>
#include <vector>

#include "vectro/frame/internal_message.h"
#include "vectro/frame/raw_buffer.h"

namespace vectro {
namespace frame {

/// Abstract byte‐stream → message boundary parser using RawBuffer.
/// Implementations manage one or more RawBuffer scratch areas and
/// produce zero‐copy InternalMessage instances.
class FramerBase {
 public:
  virtual ~FramerBase() = default;

  /// Return a scratch RawBuffer to read raw bytes into.
  /// The caller will do:
  ///   auto& buf = framer->PrepareRead();
  ///   socket.async_read_some(buffer(buf.data(), buf.size()), ...);
  /// and then call OnData(bytes_read).
  virtual RawBuffer& PrepareRead() = 0;

  /// Called after N bytes have been read into the RawBuffer from PrepareRead().
  /// Must return 0 or more InternalMessage instances, each owning its own
  /// RawBuffer.
  virtual std::vector<InternalMessage> OnData(std::size_t n) = 0;
};

}  // namespace frame
}  // namespace vectro
