#pragma once

#include <absl/time/clock.h>
#include <absl/time/time.h>

#include <atomic>
#include <boost/asio.hpp>
#include <chrono>
#include <functional>
#include <memory>

#include "vectro/controller.h"
#include "vectro/plugin/plugin_bundle.h"
#include "vectro/session.h"
#include "vectro/tcp/acceptor_loop_interface.h"

namespace vectro {
namespace tcp {
namespace asio = boost::asio;
// Default implementation of AcceptorLoop using a dedicated strand
// to serialize all handlers (accept, timer, Stop) and eliminate races.
// Changes:
// 1. Introduced `strand_` to bind all async handlers.
// 2. Wrapped async_accept, timer.async_wait, and Stop() through strand.
// 3. TODO: Placeholder comments added for backlog monitoring & storm protection.
// 4. Timer expiration now cancels the accept operation instead of closing the
//    socket to avoid moved-from races and undefined behavior.
// 5. TODO: Backpressure strategies outlined as comments for max-concurrency gating.
// 6. Drain state change callback added for external subscription.
// 7. Drain callback now receives an Abseil timestamp (using absl::Now()).
// 8. Recommendation: consider an async pre-session plugin hook to inspect TLS
//    handshake (e.g., SNI, ALPN) or IP geolocation before final accept/reject.
template <typename Stream>
class AcceptorLoop : public AcceptorLoopInterface<Stream>,
                     public std::enable_shared_from_this<AcceptorLoop<Stream>> {
 public:
  using SessionT = Session<Stream>;
  using SessionPtr = std::shared_ptr<SessionT>;
  using AcceptorPtr = std::shared_ptr<asio::ip::tcp::acceptor>;
  using FrameFactory = std::function<std::unique_ptr<FrameBase>()>;
  using OnAcceptFilter = std::function<bool(const asio::ip::tcp::endpoint&)>;
  using ControllerPtr = std::shared_ptr<Controller<SessionT>>;
  // DrainCallback now passes (is_draining, timestamp)
  using DrainCallback =
      std::function<void(bool /*is_draining*/, absl::Time /*when*/)>;

  AcceptorLoop(asio::io_context& io, AcceptorPtr acceptor,
               ControllerPtr controller, FrameFactory framer_factory,
               PluginBundle plugin_bundle, std::chrono::seconds accept_timeout,
               OnAcceptFilter filter = nullptr)
                  : io_(io),
                    strand_(asio::make_strand(io)),
                    acceptor_(std::move(acceptor)),
                    controller_(std::move(controller)),
                    framer_factory_(std::move(framer_factory)),
                    plugin_bundle_(std::move(plugin_bundle)),
                    accept_timeout_(accept_timeout),
                    filter_(std::move(filter)) {}

  // Begin accepting connections
  void Start() override {
    if (stopped_)
      return;
    asio::dispatch(strand_,
                   [self = this->shared_from_this()]() { self->DoAccept(); });
  }

  // Stop accepting new connections and close acceptor
  void Stop() override {
    asio::dispatch(strand_, [self = this->shared_from_this()]() {
      self->stopped_ = true;
      std::error_code ec;
      self->acceptor_->cancel(ec);
      self->acceptor_->close(ec);
    });
  }

  // Enable or disable draining mode
  void EnableDraining(bool enable) override {
    asio::dispatch(strand_, [self = this->shared_from_this(), enable]() {
      self->draining_ = enable;
      // Fire drain state change callback with timestamp
      if (self->drain_callback_) {
        self->drain_callback_(enable, absl::Now());
      }
    });
  }

  // Subscribe to draining state changes (on/off)
  void SetDrainCallback(DrainCallback cb) {
    asio::dispatch(strand_, [self = this->shared_from_this(),
                             cb = std::move(cb)]() mutable {
      self->drain_callback_ = std::move(cb);
    });
  }

 protected:
  virtual void OnSessionAccepted(SessionPtr session,
                                 const asio::ip::tcp::endpoint& remote) {
    static std::atomic<uint64_t> id_seq{1};
    uint64_t id = id_seq++;

    // Real-world: increment active session count
    // active_sessions_.fetch_add(1, std::memory_order_relaxed);

    session->Start(
        id, remote.address().to_string(), remote.port(),
        acceptor_->local_endpoint().port(),
        [ctl = controller_](auto /*self*/, InternalMessage msg) {
          ctl->RouteMessage(std::move(msg));
        },
        [ctl = controller_](auto /*self*/) {
          ctl->UnregisterSession(/*id*/);
          // Real-world: decrement active session count in cleanup
          // active_sessions_.fetch_sub(1, std::memory_order_relaxed);
        });

    controller_->RegisterSession(id, session);
  }

 private:
  void DoAccept() {
    if (stopped_)
      return;

    // Backpressure: max-concurrency gating placeholder
    // if (active_sessions_.load() >= max_sessions_) {
    //   // Fail fast: drop the connection or delay next accept
    //   return;
    // }

    auto self = this->shared_from_this();
    auto socket = std::make_shared<Stream>(io_);
    auto timer = std::make_shared<asio::steady_timer>(io_);
    timer->expires_after(accept_timeout_);

    acceptor_->async_accept(
        socket->lowest_layer(),
        asio::bind_executor(
            strand_, [this, self, socket, timer](std::error_code ec) {
              // Cancel the timeout timer so it won't fire after accept
              timer->cancel();
              if (stopped_)
                return;

              if (!ec) {
                std::error_code rem_ec;
                auto remote = socket->lowest_layer().remote_endpoint(rem_ec);
                // Real-world: log remote_endpoint failure if any
                // if (rem_ec) {
                //   LOG(WARN) << "remote_endpoint error: " << rem_ec.message();
                //   metrics_counter_.Increment("remote_endpoint_errors");
                // }

                // Real-world: instead of just filter_(remote), you might want
                // to invoke an async pre-session plugin hook that can inspect
                // the handshake (SNI, certificate, ALPN) before accepting or
                // rejecting the connection:
                //   if (plugin_bundle_.OnPreSession(socket)) { ... }

                if (!rem_ec && (!filter_ || filter_(remote))) {
                  if (!draining_) {
                    CreateSession(socket, remote);
                  } else {
                    socket->lowest_layer().close(rem_ec);
                  }
                } else {
                  socket->lowest_layer().close(rem_ec);
                }
              } else {
                // Real-world: record accept error and consider exponential
                // backoff Example: LOG(WARN) << "Accept error: " <<
                // ec.message(); metrics_counter_.Increment("accept_errors");
              }

              // Issue next accept (optionally after delay)
              DoAccept();
            }));

    timer->async_wait(asio::bind_executor(
        strand_, [self = this->shared_from_this(), timer](std::error_code ec) {
          if (!ec) {
            std::error_code cancel_ec;
            self->acceptor_->cancel(cancel_ec);
          }
        }));
  }

  void CreateSession(std::shared_ptr<Stream> socket,
                     const asio::ip::tcp::endpoint& remote) {
    auto session = std::make_shared<SessionT>(
        std::move(*socket), io_.get_executor(), framer_factory_(),
        std::chrono::seconds(10), std::chrono::seconds(30));

    session->SetPluginBundle(plugin_bundle_);
    OnSessionAccepted(session, remote);
  }

  asio::io_context& io_;
  asio::strand<asio::io_context::executor_type> strand_;
  AcceptorPtr acceptor_;
  ControllerPtr controller_;
  FrameFactory framer_factory_;
  PluginBundle plugin_bundle_;
  std::chrono::seconds accept_timeout_;
  OnAcceptFilter filter_;

  // Drain state change callback for external subscribers
  DrainCallback drain_callback_;

  // For backpressure (max concurrent sessions)
  // std::atomic<size_t> active_sessions_{0};
  // size_t max_sessions_{10000};

  std::atomic<bool> stopped_{false};
  std::atomic<bool> draining_{false};
};

}  // namespace tcp
}  // namespace vectro
