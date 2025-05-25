#pragma once

#include <absl/container/node_hash_map.h>
#include <absl/synchronization/mutex.h>
#include <absl/time/time.h>

#include <boost/asio.hpp>
#include <memory>
#include <vector>

#include "vectro/controller.h"
#include "vectro/frame/frame_base.h"
#include "vectro/plugin/plugin_bundle.h"
// #include "vectro/server_tag.h"
#include "vectro/tcp/tcp_acceptor_loop.h"
#include "vectro/tls/tls_context_provider.h"

namespace vectro {
namespace tcp {
namespace asio = boost::asio;

using Port = uint16_t;
struct PortConfig {
  Port port;
  PluginBundle bundle;
  std::shared_ptr<TlsContextProvider> tls_provider;
  absl::Duration accept_timeout;
  bool delayed_accept;
};

template <typename Stream>
class TcpServer {
 public:
  using SessionType = Session<Stream>;

  explicit TcpServer(asio::io_context& io,
                     std::shared_ptr<Controller<Stream>> controller,
                     FrameFactory framer_factory)
                  : io_(io),
                    controller_(std::move(controller)),
                    framer_factory_(std::move(framer_factory)),
                    shutting_down_(false) {}

  /// Add a listening port (can be called before Start or at runtime)
  void AddPort(Port port, PluginBundle bundle,
               std::shared_ptr<TlsContextProvider> tls_provider = nullptr,
               absl::Duration accept_timeout = absl::Seconds(10),
               bool delayed_accept = false) {
    absl::MutexLock lock(&mu_);
    ports_.push_back(PortConfig{port, std::move(bundle),
                                std::move(tls_provider), accept_timeout,
                                delayed_accept});
  }

  /// Remove a listening port at runtime (stop and erase)
  void RemovePort(Port port) {
    absl::MutexLock lock(&mu_);
    if (auto it = acceptors_.find(port); it != acceptors_.end()) {
      it->second->Stop();
      acceptors_.erase(it);
    }
    ports_.erase(std::remove_if(ports_.begin(), ports_.end(),
                                [&](auto& c) { return c.port == port; }),
                 ports_.end());
  }

  /// Start all configured acceptors
  void Start() {
    absl::MutexLock lock(&mu_);
    for (auto& cfg : ports_) {
      std::shared_ptr<asio::ssl::context> ctx;
      if constexpr (is_tls_stream_v<Stream>) {
        if (!cfg.tls_provider) {
          throw std::runtime_error("TLS stream requires TlsContextProvider");
        }
        ctx = cfg.tls_provider->GetContext();
      }

      auto loop = std::make_shared<AcceptorLoop<Stream>>(
          io_, cfg.port, controller_, framer_factory_, cfg.bundle, ctx,
          cfg.accept_timeout, cfg.delayed_accept);

      loop->OnAcceptStart = [this](Port p) {
        // TODO: metrics: increment accept_start for port p
      };

      loop->OnAcceptSuccess = [this](Port p,
                                     const asio::ip::tcp::endpoint& ep) {
        // TODO: metrics: increment accept_success for port p
      };

      loop->OnAcceptDrop = [this](Port p, const std::string& reason) {
        // TODO: metrics: increment accept_drop for port p, log reason
      };

      loop->OnHandshakeError = [this](Port p, std::error_code ec) {
        // TODO: metrics: increment tls_handshake_error for port p
      };

      loop->OnFatalError = [this](Port p, std::exception_ptr eptr) {
        // TODO: observability: alert on fatal acceptor error for port p
      };

      if (shutting_down_) {
        loop->EnableDraining(true);
      }
      loop->Start();
      acceptors_[cfg.port] = std::move(loop);
    }
  }

  /// Stop accepting and tear down all sessions
  /// @param force  if true, force-close sessions immediately; if false, drain
  /// gracefully
  void Stop(bool force) {
    {
      absl::MutexLock lock(&mu_);
      for (auto& [port, loop] : acceptors_) {
        loop->Stop();
      }
      shutting_down_ = true;
    }
    controller_->ShutdownAllSessions(force);
  }

  /// Toggle drain mode at runtime (new accepts will be closed)
  void EnableDrainingMode(bool enable) {
    absl::MutexLock lock(&mu_);
    shutting_down_ = enable;
    for (auto& [_, loop] : acceptors_) {
      loop->EnableDraining(enable);
    }
  }

  // void ForceCloseSessionsByTag(const std::string& tag) {
  //   controller_->ForceCloseByTag(tag);
  // }

  // Hooks for server events
  std::function<void(Port)> OnAcceptStart;
  std::function<void(Port, const asio::ip::tcp::endpoint&)> OnAcceptSuccess;
  std::function<void(Port, const std::string&)> OnAcceptDrop;
  std::function<void(Port, std::error_code)> OnHandshakeError;
  std::function<void(Port, std::exception_ptr)> OnFatalError;

 private:
  asio::io_context& io_;
  std::shared_ptr<Controller<Stream>> controller_;
  FrameFactory framer_factory_;

  absl::Mutex mu_;
  std::vector<PortConfig> ports_;
  absl::node_hash_map<Port, std::shared_ptr<AcceptorLoop<Stream>>> acceptors_;
  bool shutting_down_;
};

}  // namespace tcp
}  // namespace vectro