#pragma once

#include <absl/container/node_hash_map.h>
#include <absl/time/time.h>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <functional>
#include <memory>
#include <mutex>

#include "vectro/frame/frame_base.h"
#include "vectro/frame/internal_message.h"
#include "vectro/plugin/plugin_bundle.h"
#include "vectro/stream/write_queue_sg.h"
#include "vectro/tls/tls_client_context_provider.h"

namespace vectro {
namespace tcp {

using boost::asio::ip::tcp;
namespace asio = boost::asio;

/// Configurable timeouts & limits for TcpClient
struct ClientConfig {
  absl::Duration connect_timeout = absl::Seconds(10);
  absl::Duration read_timeout = absl::Seconds(30);
  absl::Duration idle_timeout = absl::Seconds(60);
  std::size_t max_queue_size = 1024;
  // TODO: add backpressure thresholds, retry policies, etc.
};

/// Snapshot of client‐side metrics; to be filled in by metrics plugin
struct MetricsSnapshot {
  size_t pending_requests = 0;
  size_t queued_writes = 0;
  // TODO: histograms for latencies, error counters, etc.
};

/// Callback types
using ClientMessageHandler =
    std::function<void(std::shared_ptr<frame::InternalMessage>)>;
using ClientConnectHandler = std::function<void(const boost::system::error_code&)>;
using ClientDisconnectHandler = std::function<void(const boost::system::error_code&)>;
using ClientResponseHandler = std::function<void(
    const boost::system::error_code&, std::shared_ptr<frame::InternalMessage>)>;

template <typename Stream>
class TcpClient : public std::enable_shared_from_this<TcpClient<Stream>> {
 public:
  using FrameFactory = std::function<std::unique_ptr<frame::FramerBase>()>;
  using Msg = std::shared_ptr<frame::InternalMessage>;

  TcpClient(
      asio::any_io_executor executor, FrameFactory framer_factory,
      ClientConfig config = {}, PluginBundle plugins = {},
      std::shared_ptr<tls::TlsClientContextProvider> tls_provider = nullptr)
                  : executor_(executor),
                    framer_factory_(std::move(framer_factory)),
                    config_(std::move(config)),
                    plugins_(std::move(plugins)),
                    connect_timer_(executor),
                    read_timer_(executor),
                    idle_timer_(executor),
                    next_correlation_id_(1),
                    pending_requests_(0) {
    // Construct the underlying stream (plain or TLS)
    if constexpr (is_tls_stream_v<Stream>) {
      if (!tls_provider) {
        throw std::runtime_error(
            "TlsClientContextProvider required for TLS Stream");
      }
      auto ctx = tls_provider->GetContext();
      socket_ = std::make_unique<Stream>(executor_, *ctx);
    } else {
      socket_ = std::make_unique<Stream>(executor_);
    }

    // Build framer
    framer_ = framer_factory_();

    // Build write queue with backpressure limit
    write_queue_ = std::make_unique<WriteQueueSG<Stream>>(
        *socket_, executor_, config_.max_queue_size);
  }

  ~TcpClient() {
    Close(true);
  }

  /// Asynchronously connect to host:port, with timeout.
  void Connect(const std::string& host, uint16_t port,
               ClientConnectHandler on_connect) {
    on_connect_ = std::move(on_connect);

    // Resolve DNS
    resolver_ = std::make_unique<tcp::resolver>(executor_);
    tcp::resolver::query q(host, std::to_string(port));

    auto self = this->shared_from_this();
    // Start connect timeout
    connect_timer_.expires_after(config_.connect_timeout);
    connect_timer_.async_wait([self](auto ec) {
      if (!ec) {
        // Timeout expired → cancel socket operations
        std::error_code cancel_ec;
        self->socket_->lowest_layer().cancel(cancel_ec);
      }
    });

    resolver_->async_resolve(q, [self](boost::system::error_code ec,
                                       tcp::resolver::results_type results) {
      if (ec) {
        self->connect_timer_.cancel();
        if (self->on_connect_)
          self->on_connect_(ec);
        return;
      }
      // Attempt connect
      asio::async_connect(
          self->socket_->lowest_layer(), results,
          asio::bind_executor(self->executor_,
                              [self](boost::system::error_code ec, auto) {
                                self->connect_timer_.cancel();
                                if (ec) {
                                  if (self->on_connect_)
                                    self->on_connect_(ec);
                                  return;
                                }
                                // For TLS, perform handshake
                                if constexpr (is_tls_stream_v<Stream>) {
                                  self->DoHandshake();
                                } else {
                                  if (self->on_connect_)
                                    self->on_connect_({});
                                  // Kick off read loop
                                  self->StartRead();
                                }
                              }));
    });
  }

  /// Send one‐way message; may be rejected on backpressure
  void Send(Msg msg) {
    // TODO: consult backpressure plugin here before enqueue
    write_queue_->AsyncSend(
        std::make_shared<std::vector<asio::const_buffer>>(framer_->Frame(*msg)),
        [self = this->shared_from_this(), msg](bool ok) {
          if (!ok) {
            // TODO: metrics: dropped_writes++
            if (auto bp =
                    self->plugins_.Get<IBackpressurePolicy>("backpressure")) {
              bp->OnDrop(*msg);
            }
          }
        });
  }

  /// Send request with response callback and per‐request timeout
  void SendRequest(Msg request, absl::Duration timeout,
                   ClientResponseHandler on_response) {
    uint64_t cid = next_correlation_id_++;
    request->meta().correlation_id = cid;
    {
      std::lock_guard<std::mutex> lock(pending_mu_);
      pending_responses_[cid] = on_response;
    }
    pending_requests_.fetch_add(1, std::memory_order_relaxed);

    Send(request);

    // Request timeout
    auto timer = std::make_shared<asio::steady_timer>(executor_);
    timer->expires_after(timeout);
    timer->async_wait([self = this->shared_from_this(), cid, timer](auto ec) {
      if (ec)
        return;  // cancelled
      ClientResponseHandler cb;
      {
        std::lock_guard<std::mutex> lock(self->pending_mu_);
        auto it = self->pending_responses_.find(cid);
        if (it == self->pending_responses_.end())
          return;
        cb = it->second;
        self->pending_responses_.erase(it);
      }
      self->pending_requests_.fetch_sub(1, std::memory_order_relaxed);
      if (cb)
        cb(asio::error::timed_out, nullptr);
    });
  }

  /// Graceful (force=false) or immediate (force=true) shutdown
  void Close(bool force = false) {
    if (force) {
      std::error_code ec;
      socket_->lowest_layer().cancel(ec);
      socket_->lowest_layer().close(ec);
      write_queue_->Close(true);
    } else {
      write_queue_->Close(false);
      // TODO: after drain complete, close socket
    }
    connect_timer_.cancel();
    read_timer_.cancel();
    idle_timer_.cancel();
    if (on_disconnect_)
      on_disconnect_({});
  }

  /// Handlers for unsolicited messages
  void SetOnMessage(ClientMessageHandler h) {
    on_message_ = std::move(h);
  }
  void SetOnDisconnect(ClientDisconnectHandler h) {
    on_disconnect_ = std::move(h);
  }

  /// Plugin registration
  template <typename T>
  void RegisterPlugin(const std::string& key, std::shared_ptr<T> plugin) {
    plugins_.Register(key, std::move(plugin));
  }

  /// Snapshot metrics
  MetricsSnapshot SnapshotMetrics() const {
    MetricsSnapshot m;
    m.pending_requests = pending_requests_.load();
    // m.queued_writes = write_queue_->Size(); // TODO: add Size()
    return m;
  }

 private:
  // Perform TLS handshake if needed
  void DoHandshake() {
    auto self = this->shared_from_this();
    socket_->async_handshake(boost::asio::ssl::stream_base::client,
                             asio::bind_executor(executor_, [self](auto ec) {
                               if (self->on_connect_)
                                 self->on_connect_(ec);
                               if (!ec)
                                 self->StartRead();
                             }));
  }

  // Read loop: applies framer, timers, dispatch to handlers
  void StartRead() {
    if (shutdown_)
      return;
    auto buf = framer_->PrepareRead();
    socket_->async_read_some(
        boost::asio::buffer(buf),
        asio::bind_executor(executor_, [self = this->shared_from_this(), buf](
                                           auto ec, std::size_t n) {
          if (ec) {
            self->HandleDisconnect(ec);
            return;
          }
          // Cancel old read timeout
          self->read_timer_.cancel();
          // Reset timers
          self->read_timer_.expires_after(self->config_.read_timeout);
          self->idle_timer_.expires_after(self->config_.idle_timeout);

          // Process frames
          auto msgs = self->framer_->OnData(n);
          for (auto& m : msgs) {
            auto cid = m.meta().correlation_id;
            // Response to a pending request?
            ClientResponseHandler cb;
            {
              std::lock_guard<std::mutex> lock(self->pending_mu_);
              auto it = self->pending_responses_.find(cid);
              if (it != self->pending_responses_.end()) {
                cb = it->second;
                self->pending_responses_.erase(it);
              }
            }
            if (cb) {
              self->pending_requests_.fetch_sub(1, std::memory_order_relaxed);
              cb({}, std::make_shared<frame::InternalMessage>(std::move(m)));
            } else if (self->on_message_) {
              // unsolicited
              self->on_message_(
                  std::make_shared<frame::InternalMessage>(std::move(m)));
            }
          }
          // Continue reading
          self->StartRead();
        }));

    // Arm read timeout
    read_timer_.expires_after(config_.read_timeout);
    read_timer_.async_wait(asio::bind_executor(
        executor_, [self = this->shared_from_this()](auto ec) {
          if (!ec)
            self->HandleDisconnect(asio::error::timed_out);
        }));

    // Arm idle timeout
    idle_timer_.expires_after(config_.idle_timeout);
    idle_timer_.async_wait([self = this->shared_from_this()](auto ec) {
      if (!ec)
        self->HandleDisconnect(asio::error::timed_out);
    });
  }

  void HandleDisconnect(const boost::system::error_code& ec) {
    if (shutdown_)
      return;
    shutdown_ = true;
    Close(true);
  }

  asio::any_io_executor executor_;
  FrameFactory framer_factory_;
  ClientConfig config_;
  PluginBundle plugins_;

  std::unique_ptr<Stream> socket_;
  std::unique_ptr<frame::FramerBase> framer_;
  std::unique_ptr<WriteQueueSG<Stream>> write_queue_;

  std::unique_ptr<tcp::resolver> resolver_;
  asio::steady_timer connect_timer_;
  asio::steady_timer read_timer_;
  asio::steady_timer idle_timer_;

  std::atomic<uint64_t> next_correlation_id_;
  std::atomic<size_t> pending_requests_;
  std::unordered_map<uint64_t, ClientResponseHandler> pending_responses_;
  mutable std::mutex pending_mu_;

  ClientMessageHandler on_message_;
  ClientConnectHandler on_connect_;
  ClientDisconnectHandler on_disconnect_;

  bool shutdown_ = false;
};

}  // namespace tcp
}  // namespace vectro
