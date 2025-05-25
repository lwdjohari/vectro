#pragma once

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

// Default implementation of AcceptorLoop using a dedicated strand
// to serialize all handlers (accept, timer, Stop) and eliminate races.
// Changes:
// 1. Introduced `strand_` to bind all async handlers.
// 2. Wrapped async_accept, timer.async_wait, and Stop() through strand.
// 3. Placeholder comments added for backlog monitoring & storm protection.

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

  AcceptorLoop(asio::io_context& io, AcceptorPtr acceptor,
               ControllerPtr controller, FrameFactory framer_factory,
               PluginBundle plugin_bundle, std::chrono::seconds accept_timeout,
               OnAcceptFilter filter = nullptr)
                  : io_(io),
                    strand_(
                        asio::make_strand(io))  // initialize dedicated strand
                    ,
                    acceptor_(std::move(acceptor)),
                    controller_(std::move(controller)),
                    framer_factory_(std::move(framer_factory)),
                    plugin_bundle_(std::move(plugin_bundle)),
                    accept_timeout_(accept_timeout),
                    filter_(std::move(filter)) {}

  void Start() override {
    if (stopped_)
      return;
    // Bind Start through strand to serialize with Stop
    asio::dispatch(strand_,
                   [self = this->shared_from_this()]() { self->DoAccept(); });
  }

  void Stop() override {
    // Post Stop operations to strand to avoid races
    asio::dispatch(strand_, [self = this->shared_from_this()]() {
      self->stopped_ = true;
      std::error_code ec;
      self->acceptor_->cancel(ec);
      self->acceptor_->close(ec);
    });
  }

  void EnableDraining(bool enable) override {
    // Safe to update draining_ under strand
    asio::dispatch(strand_, [self = this->shared_from_this(), enable]() {
      self->draining_ = enable;
    });
  }

 protected:
  virtual void OnSessionAccepted(SessionPtr session,
                                 const asio::ip::tcp::endpoint& remote) {
    static std::atomic<uint64_t> id_seq{1};
    uint64_t id = id_seq++;

    session->Start(
        id, remote.address().to_string(), remote.port(),
        acceptor_->local_endpoint().port(),
        [ctl = controller_](auto /*self*/, InternalMessage msg) {
          ctl->RouteMessage(std::move(msg));
        },
        [ctl = controller_](auto /*self*/) { ctl->UnregisterSession(/*id*/); });

    controller_->RegisterSession(id, session);
  }

 private:
  void DoAccept() {
    if (stopped_)
      return;

    // Placeholder: integrate backlog monitoring and accept storm protection
    // here

    auto self = this->shared_from_this();
    auto socket = std::make_shared<Stream>(io_);
    auto timer = std::make_shared<asio::steady_timer>(io_);
    timer->expires_after(accept_timeout_);

    // Bound accept handler to strand
    acceptor_->async_accept(
        socket->lowest_layer(),
        asio::bind_executor(
            strand_, [this, self, socket, timer](std::error_code ec) {
              timer->cancel();
              if (stopped_)
                return;

              if (!ec) {
                std::error_code rem_ec;
                auto remote = socket->lowest_layer().remote_endpoint(rem_ec);
                if (!rem_ec && (!filter_ || filter_(remote))) {
                  // Placeholder: plugin filter invoked here could implement
                  // rate limiting
                  if (!draining_) {
                    CreateSession(socket, remote);
                  } else {
                    socket->lowest_layer().close(rem_ec);
                  }
                } else {
                  socket->lowest_layer().close(rem_ec);
                }
              } else {
                // Placeholder: record accept error and consider backoff
                // strategy
              }

              // Placeholder: after handling accept, potentially delay before
              // next accept
              DoAccept();
            }));

    // Bound timer handler to strand
    timer->async_wait(
        asio::bind_executor(strand_, [socket](std::error_code ec) {
          if (!ec) {
            std::error_code ignore;
            socket->lowest_layer().close(ignore);
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
  asio::strand<asio::io_context::executor_type>
      strand_;  // new strand for acceptor loop
  AcceptorPtr acceptor_;
  ControllerPtr controller_;
  FrameFactory framer_factory_;
  PluginBundle plugin_bundle_;
  std::chrono::seconds accept_timeout_;
  OnAcceptFilter filter_;

  std::atomic<bool> stopped_{false};
  std::atomic<bool> draining_{false};
};

}  // namespace tcp
}  // namespace vectro
