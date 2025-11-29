#pragma once

#include <absl/container/node_hash_map.h>
#include <absl/synchronization/mutex.h>
#include <absl/time/time.h>

#include <asio.hpp>
#include <functional>
#include <memory>
#include <string>

#include "vectro/channel.h"
#include "vectro/frame/internal_message.h"
#include "vectro/pool/task_multi_processor.h"

namespace vectro {
namespace asio = asio;
// ------------------------------------------------------------
// Controller: central orchestration of sessions, clients, and routing
// ------------------------------------------------------------
template <typename Session>
class Controller : public std::enable_shared_from_this<Controller<Session>> {
 public:
  using SessionPtr    = std::shared_ptr<Session>;
  using ClientPtr     = std::shared_ptr<TcpClient>;
  using Msg           = std::shared_ptr<InternalMessage>;
  using EventCallback = std::function<void(const Msg&)>;
  // ----------------------------------------------------------------
  // Hooks for extension — now work in terms of shared_ptr<InternalMessage>
  // ----------------------------------------------------------------
  std::function<bool(const Msg&)> OnInbound;   // return false to drop
  std::function<void(const Msg&)> OnProcess;   // inside the channel, pre-route
  std::function<void(const Msg&)> OnOutbound;  // before enqueue to outbound

  // explicit Controller(asio::any_io_executor exec, size_t num_channels = 4,
  //                     const ChannelConfig& channel_cfg = {})
  //                 : strand_(exec),
  //                   num_channels_(num_channels),
  //                   channel_cfg_(channel_cfg),
  //                   inbound_manager_(channel_cfg_),
  //                   outbound_manager_(channel_cfg_) {}

  explicit Controller(asio::any_io_executor exec, size_t num_channels = 4,
                      const ChannelConfig& cfg = {})
      : strand_(exec),
        num_channels_(num_channels),
        channel_cfg_(cfg),
        // Inbound: pass Msg into HandleProcessing
        inbound_manager_(cfg,
                         [this](Msg m) {
                           this->HandleProcessing(std::move(m));
                         }),
        // Outbound: pass Msg into HandleOutbound
        outbound_manager_(cfg, [this](Msg m) {
          this->HandleOutbound(std::move(m));
        }) {}

  // Register a new session and assign it a channel
  void RegisterSession(uint64_t id, SessionPtr session) {
    size_t ch               = id % num_channels_;  // default modulo assigner
    session->meta().channel = ch;                  // Session→Channel Assignment
    // TODO: observability: emit OnChannelAssign(id, ch)
    session_manager_.Add(id, session);
    if (callbacks_)
      callbacks_->OnSessionConnected(id);
  }

  // Ingress: now takes a shared_ptr directly
  // auto ptr = std::make_shared<InternalMessage>(std::move(raw_msg));
  // controller->RouteMessage(ptr);
  void RouteMessage(Msg msg) {
    // Pre‐dispatch hook
    if (OnInbound && !OnInbound(msg)) {
      return;  // dropped early
    }
    // Dispatch into per‐channel inbound queue
    auto ch = std::to_string(msg->meta().channel.value_or(0));
    inbound_manager_.Dispatch(ch, msg);
    // TODO: metrics: inbound dispatch count per channel
  }

  // void RouteMessage(InternalMessage msg) {
  //   // Inbound hook before queuing
  //   if (OnInbound && !OnInbound(msg)) {
  //     // dropped early
  //     return;
  //   }
  //   // Dispatch into per-channel inbound queue
  //   auto ch = std::to_string(msg.meta().channel.value_or(0));
  //   inbound_manager_.Dispatch(ch, std::move(msg));
  //   // TODO: metrics: inbound dispatch count by channel
  // }

  // Egress: takes a shared_ptr, applies hook, then dispatch
  void SendResponse(Msg msg) {
    if (OnOutbound)
      OnOutbound(msg);
    auto ch = std::to_string(msg->meta().channel.value_or(0));
    outbound_manager_.Dispatch(ch, msg);
    // TODO: metrics: outbound dispatch count per channel
  }

  // // Egress is handled similarly through outbound_manager_
  // void SendResponse(InternalMessage msg) {
  //   if (OnOutbound) OnOutbound(msg);
  //   auto ch = std::to_string(msg.meta().channel.value_or(0));
  //   outbound_manager_.Dispatch(ch, std::move(msg));
  //   // TODO: metrics: outbound dispatch count by channel
  // }

  // Coordinated shutdown
  void ShutdownAllSessions(bool force) {
    session_manager_.Drain(force);
    inbound_manager_.ShutdownAllGraceful();
    outbound_manager_.ShutdownAllGraceful();
  }

 private:
  // Called by inbound-channel worker
  void HandleProcessing(Msg msg) {
    asio::dispatch(
        strand_, [self = shared_from_this(), msg = std::move(msg)]() {
          try {
            if (self->OnProcess)
              self->OnProcess(msg);
            if (self->router_)
              self->router_->Route(std::move(*msg));
          } catch (...) {
            if (self->callbacks_)
              self->callbacks_->OnError(*msg, std::current_exception());
          }
        });
  }

  // // Invoked by inbound channel workers on processing thread
  // void HandleProcessing(Msg msg) {
  //   asio::dispatch(strand_, [self = shared_from_this(), msg =
  //   std::move(msg)]() mutable {
  //     try {
  //       if (self->OnProcess) self->OnProcess(msg);
  //       if (self->router_) self->router_->Route(std::move(msg));
  //     } catch (...) {
  //       if (self->callbacks_) self->callbacks_->OnError(msg,
  //       std::current_exception());
  //     }
  //   });
  // }

  // Called by outbound-channel worker
  void HandleOutbound(Msg msg) {
    asio::dispatch(
        strand_, [self = shared_from_this(), msg = std::move(msg)]() {
          try {
            uint64_t id = msg->meta().dest_session.value_or(0);
            if (auto s = self->session_manager_.Get(id)) {
              s->AsyncSend(std::move(*msg));
            }
          } catch (...) {
            if (self->callbacks_)
              self->callbacks_->OnError(*msg, std::current_exception());
          }
        });
  }

  // // Invoked by outbound channel workers on processing thread
  // void HandleOutbound(Msg msg) {
  //   asio::dispatch(strand_, [self = shared_from_this(), msg =
  //   std::move(msg)]() mutable {
  //     try {
  //       // send to session
  //       auto id = msg.meta().dest_session.value_or(0);
  //       if (auto s = self->session_manager_.Get(id)) {
  //         s->AsyncSend(std::move(msg));
  //       }
  //     } catch (...) {
  //       if (self->callbacks_) self->callbacks_->OnError(msg,
  //       std::current_exception());
  //     }
  //   });
  // }

  asio::strand<asio::any_io_executor> strand_;
  size_t num_channels_;
  ChannelConfig channel_cfg_;
  ChannelManager inbound_manager_;   // Msg-based
  ChannelManager outbound_manager_;  // Msg-based
  SessionManager<Session> session_manager_;
  absl::node_hash_map<std::string, ClientPtr> clients_;
  std::shared_ptr<IMessageRouter<Session>> router_;
  std::shared_ptr<ControllerCallbacks> callbacks_;
};

// ------------------------------------------------------------
// End of Controller implementation
// ------------------------------------------------------------

}  // namespace vectro