#pragma once

#include <absl/container/node_hash_map.h>
#include <absl/synchronization/mutex.h>

#include <functional>
#include <memory>
#include <string>

#include "absl/time/time.h"
#include "vectro/frame/internal_message.h"
#include "vectro/pool/task_multi_processor.h"

namespace vectro {

// ------------------------------------------------------------
// ChannelConfig: configure per-channel behavior
// ------------------------------------------------------------
struct ChannelConfig {
  std::string id;                      // channel identifier
  pool::TaskProcessorConfig proc_cfg;  // underlying TaskMultiProcessor config
};

// ------------------------------------------------------------
// Channel: single logical FIFO queue for ordered message handling
// - Backpressure, circuit-breaker baked into TaskMultiProcessor
// - Supports lifecycle hooks, dynamic reconfig, metrics
// ------------------------------------------------------------
class Channel {
 public:
  using Msg = std::shared_ptr<frame::InternalMessage>;

  // Callback hooks
  std::function<void()> OnStart;         // channel startup
  std::function<void()> OnStop;          // channel graceful stop
  std::function<void(bool)> OnShutdown;  // force or graceful shutdown
  std::function<void(Msg)> OnEnqueue;    // message enqueued
  std::function<void(Msg)> OnProcessed;  // message processed
  std::function<void(Msg)> OnDropped;    // message dropped by policy
  std::function<void(Msg, std::exception_ptr)> OnError;  // handler exception

  explicit Channel(const ChannelConfig& cfg) : cfg_(cfg), proc_(cfg.proc_cfg) {
    if (OnStart)
      OnStart();
    // Register processing callback taking Msg by value
    proc_.RegisterCallback([this](Msg m) {
      try {
        if (OnProcessed)
          OnProcessed(m);
      } catch (...) {
        if (OnError)
          OnError(m, std::current_exception());
      }
    });
  }

  ~Channel() {
    ShutdownForce();
  }

  // Enqueue a message for processing
  void Enqueue(Msg msg) {
    if (OnEnqueue)
      OnEnqueue(msg);
    proc_.Submit(std::move(msg));
  }

  // Graceful shutdown: drain in-flight tasks then stop
  void ShutdownGraceful() {
    if (OnShutdown)
      OnShutdown(false);
    proc_.~TaskMultiProcessor<Msg>();  // drains then joins
    if (OnStop)
      OnStop();
  }

  // Force shutdown: cancel all tasks immediately
  void ShutdownForce() {
    if (OnShutdown)
      OnShutdown(true);
    proc_.~TaskMultiProcessor<Msg>();  // immediate exit
    if (OnStop)
      OnStop();
  }

  // Dynamic config update
  void UpdateConfig(const pool::TaskProcessorConfig& new_cfg) {
    proc_.UpdateConfig(new_cfg);
    // TODO: observability: emit config-change event
  }

 private:
  ChannelConfig cfg_;
  pool::TaskMultiProcessor<Msg> proc_;
};

// ------------------------------------------------------------
// ChannelManager: manages multiple named Channels
// - Thread-safe via absl::Mutex
// - Supports dynamic creation and cleanup
// ------------------------------------------------------------
class ChannelManager {
 public:
  using Msg = std::shared_ptr<frame::InternalMessage>;
  using Handler = std::function<void(Msg)>;
  using ChannelPtr = std::shared_ptr<Channel>;

  explicit ChannelManager(const ChannelConfig& default_cfg, Handler handler)
                  : default_cfg_(default_cfg), handler_(std::move(handler)) {}

  // Dispatch message to named channel
  void Dispatch(const std::string& id, Msg msg) {
    auto ch = GetChannel(id);
    if (!ch) {
      // No Channel
      // Drop the message and tell the state to be false
      // TODO: change signature to add `bool &submitted` state
      // TODO: Log error here
    }

    ch->Enqueue(std::move(msg));
    // TODO: observability: increment dispatch counter per channel
  }

  // Gracefully shutdown all channels
  void ShutdownAllGraceful() {
    absl::MutexLock lock(&mu_);
    for (auto& kv : channels_)
      kv.second->ShutdownGraceful();
    channels_.clear();
  }

  // Force shutdown all channels
  void ShutdownAllForce() {
    absl::MutexLock lock(&mu_);
    for (auto& kv : channels_)
      kv.second->ShutdownForce();
    channels_.clear();
  }

 private:
  ChannelPtr GetChannel(const std::string& id) {
    absl::MutexLock lock(&mu_);
    auto it = channels_.find(id);
    if (it != channels_.end()) {
      return it->second;
    } else {
      return nullptr;
    }

    auto ch = std::make_shared<Channel>(default_cfg_);
    // Wire processing to our handler; msg by value
    ch->OnProcessed = [h = handler_](Msg m) {
      try {
        h(m);
      } catch (...) { /* TODO: observability: log handler exceptions */
      }
    };
    channels_[id] = ch;
    // TODO: observability: emit channel-create event
    return ch;
  }

  ChannelConfig default_cfg_;
  Handler handler_;
  absl::Mutex mu_;
  absl::node_hash_map<std::string, ChannelPtr> channels_;
};
// ------------------------------------------------------------
// Usage Example:
//
// ChannelConfig cfg;
// cfg.id = "orders";
// cfg.proc_cfg.id = "Channel-orders";
// cfg.proc_cfg.num_threads = 1;  // FIFO ordering
// cfg.proc_cfg.max_queue_size = 5000;
// util::ChannelManager mgr(cfg);
//
// // Register hooks
// auto ch = mgr.GetChannel("orders");
// ch->OnEnqueue = [](auto &m) { LOG(INFO) << "Order queued"; };
// ch->OnProcessed = [](auto &m) { LOG(INFO) << "Order processed"; };
//
// // In controller route
// void Controller::RouteMessage(InternalMessage msg) {
//   std::string ch_id = msg.meta().tag.value();
//   channel_mgr_.Dispatch(ch_id, std::move(msg));
// }
//
// // On shutdown
// mgr.ShutdownAllGraceful();
// ------------------------------------------------------------

}  // namespace vectro
