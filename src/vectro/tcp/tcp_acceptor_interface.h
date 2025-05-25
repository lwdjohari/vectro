// acceptor_loop_interface.h
#pragma once

#include <memory>
#include <boost/asio.hpp>

namespace vectro {
namespace tls {

// AcceptorLoop pure‐virtual interface
template<typename Stream>
class AcceptorLoopInterface {
 public:
  using Ptr = std::shared_ptr<AcceptorLoopInterface<Stream>>;

  virtual ~AcceptorLoopInterface() = default;

  /// Begin accepting connections.
  virtual void Start() = 0;

  /// Stop accepting (and close the acceptor).
  virtual void Stop() = 0;

  /// Toggle “draining” mode: new sockets are closed instead of passed on.
  virtual void EnableDraining(bool enable) = 0;
};

}  // namespace tls
}  // namespace vectro
