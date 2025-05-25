// acceptor_loop.h
#pragma once

#include "vectro/tcp/acceptor_loop_interface.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>

#include "vectro/controller.h"
#include "vectro/plugin/plugin_bundle.h"
#include "vectro/session.h"

namespace vectro {
namespace tls {

// ——— Default implementation = maintainer-owned ———
template<typename Stream>
class AcceptorLoop
  : public AcceptorLoopInterface<Stream>,
    public std::enable_shared_from_this<AcceptorLoop<Stream>> {
 public:
  using SessionT       = Session<Stream>;
  using SessionPtr     = std::shared_ptr<SessionT>;
  using AcceptorPtr    = std::shared_ptr<asio::ip::tcp::acceptor>;
  using FrameFactory   = std::function<std::unique_ptr<FrameBase>()>;
  using OnAcceptFilter = std::function<bool(const asio::ip::tcp::endpoint&)>;
  using ControllerPtr  = std::shared_ptr<Controller<SessionT>>;

  AcceptorLoop(asio::io_context& io,
               AcceptorPtr acceptor,
               ControllerPtr controller,
               FrameFactory framer_factory,
               PluginBundle plugin_bundle,
               std::chrono::seconds accept_timeout,
               OnAcceptFilter filter = nullptr)
    : io_(io)
    , acceptor_(std::move(acceptor))
    , controller_(std::move(controller))
    , framer_factory_(std::move(framer_factory))
    , plugin_bundle_(std::move(plugin_bundle))
    , accept_timeout_(accept_timeout)
    , filter_(std::move(filter))
  {}

  // interface overrides
  void Start() override {
    if (stopped_) return;
    DoAccept();
  }

  void Stop() override {
    stopped_ = true;
    std::error_code ec;
    acceptor_->cancel(ec);
    acceptor_->close(ec);
  }

  void EnableDraining(bool enable) override {
    draining_ = enable;
  }

 protected:
  //--------------------------------------------------------------------
  // Extension point: override this to customize what happens *after*
  // a connection is accepted and passes the filter.
  //--------------------------------------------------------------------
  virtual void OnSessionAccepted(SessionPtr session,
                                 const asio::ip::tcp::endpoint& remote) {
    // — default behavior —
    static std::atomic<uint64_t> id_seq{1};
    uint64_t id = id_seq++;

    session->Start(
      id,
      remote.address().to_string(),
      remote.port(),
      acceptor_->local_endpoint().port(),
      [ctl = controller_](auto /*self*/, InternalMessage msg) {
        ctl->RouteMessage(std::move(msg));
      },
      [ctl = controller_](auto /*self*/) {
        ctl->UnregisterSession(/*id*/);
      });

    controller_->RegisterSession(id, session);
  }

 private:
  void DoAccept() {
    if (stopped_) return;

    auto self   = this->shared_from_this();
    auto socket = std::make_shared<Stream>(io_);
    auto timer  = std::make_shared<asio::steady_timer>(io_);
    timer->expires_after(accept_timeout_);

    acceptor_->async_accept(
      socket->lowest_layer(),
      [this, self, socket, timer](std::error_code ec) {
        timer->cancel();
        if (stopped_) return;

        if (!ec) {
          std::error_code rem_ec;
          auto remote = socket->lowest_layer().remote_endpoint(rem_ec);
          if (!rem_ec && (!filter_ || filter_(remote))) {
            if (!draining_) {
              CreateSession(socket, remote);
            } else {
              socket->lowest_layer().close(rem_ec);
            }
          } else {
            socket->lowest_layer().close(rem_ec);
          }
        }
        DoAccept();
      });

    timer->async_wait(
      [socket](std::error_code ec) {
        if (!ec) {
          std::error_code ignore;
          socket->lowest_layer().close(ignore);
        }
      });
  }

  void CreateSession(std::shared_ptr<Stream> socket,
                     const asio::ip::tcp::endpoint& remote) {
    // move socket into the Session ctor
    auto session = std::make_shared<SessionT>(
      std::move(*socket),
      io_.get_executor(),
      framer_factory_(),
      std::chrono::seconds(10),   // read timeout
      std::chrono::seconds(30));  // idle timeout

    session->SetPluginBundle(plugin_bundle_);

    // *** single customization hook ***
    OnSessionAccepted(session, remote);
  }

  // all internal state — unchanged
  asio::io_context& io_;
  AcceptorPtr       acceptor_;
  ControllerPtr     controller_;
  FrameFactory      framer_factory_;
  PluginBundle      plugin_bundle_;
  std::chrono::seconds accept_timeout_;
  OnAcceptFilter    filter_;

  std::atomic<bool> stopped_{false};
  std::atomic<bool> draining_{false};
};

}  // namespace tls
}  // namespace vectro
