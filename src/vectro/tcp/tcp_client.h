// tcp_client.h
#pragma once

#include <absl/container/node_hash_map.h>
#include <absl/synchronization/mutex.h>
#include <absl/time/time.h>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <functional>
#include <memory>

#include "vectro/frame/framer_base.h"
#include "vectro/frame/internal_message.h"
#include "vectro/plugin/plugin_bundle.h"
#include "vectro/tls/tls_client_context_provider.h"
#include "vectro/stream/write_queue_sg.h"

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
};

/// Placeholder for client‐side metrics
struct MetricsSnapshot {
  size_t pending_requests = 0;
  size_t queued_writes = 0;
};

using frame::InternalMessage;
using frame::InternalMessageMeta;

/// Callback types
using MessageHandler = std::function<void(std::shared_ptr<InternalMessage>)>;
using ConnectHandler = std::function<void(const boost::system::error_code&)>;
using DisconnectHandler = std::function<void(const boost::system::error_code&)>;
using ResponseHandler = std::function<void(const boost::system::error_code&,
                                           std::shared_ptr<InternalMessage>)>;

template <typename Stream>
class TcpClient : public std::enable_shared_from_this<TcpClient<Stream>> {
 public:
  using FrameFactory = std::function<std::unique_ptr<frame::FramerBase>()>;
  using Msg = std::shared_ptr<InternalMessage>;

  TcpClient(
      asio::any_io_executor executor, FrameFactory framer_factory,
      ClientConfig config = {}, PluginBundle plugins = {},
      std::shared_ptr<tls::TlsClientContextProvider> tls_provider = nullptr)
                  : executor_(executor),
                    framer_factory_(std::move(framer_factory)),
                    config_(std::move(config)),
                    plugins_(std::move(plugins)),
                    connect_timer_(executor_),
                    read_timer_(executor_),
                    idle_timer_(executor_),
                    next_correlation_id_(1),
                    pending_requests_(0) {
    // Build stream
    if constexpr (is_tls_stream_v<Stream>) {
      if (!tls_provider)
        throw std::runtime_error("TlsClientContextProvider required");
      auto ctx = tls_provider->GetContext();
      socket_ = std::make_unique<Stream>(executor_, *ctx);
    } else {
      socket_ = std::make_unique<Stream>(executor_);
    }
    // Build framer & write queue
    framer_ = framer_factory_();
    write_queue_ = std::make_unique<WriteQueueSG<Stream>>(
        *socket_, executor_, config_.max_queue_size);
  }

  ~TcpClient() {
    Close(true);
  }

  void Connect(const std::string& host, uint16_t port,
               ConnectHandler on_connect) {
    on_connect_ = std::move(on_connect);
    resolver_ = std::make_unique<tcp::resolver>(executor_);

    // connect timeout
    connect_timer_.expires_after(config_.connect_timeout);
    connect_timer_.async_wait([self = this->shared_from_this()](auto ec) {
      if (!ec)
        self->socket_->lowest_layer().cancel();
    });

    // resolve + connect
    resolver_->async_resolve(
        host, std::to_string(port),
        [self = this->shared_from_this()](auto ec, auto results) {
          self->connect_timer_.cancel();
          if (ec) {
            if (self->on_connect_)
              self->on_connect_(ec);
            return;
          }
          asio::async_connect(
              self->socket_->lowest_layer(), results,
              asio::bind_executor(self->executor_, [self](auto ec, auto) {
                if (ec) {
                  if (self->on_connect_)
                    self->on_connect_(ec);
                  return;
                }
                if constexpr (is_tls_stream_v<Stream>)
                  self->DoHandshake();
                else {
                  if (self->on_connect_)
                    self->on_connect_({});
                  self->StartRead();
                }
              }));
        });
  }

  void Send(Msg msg) {
    // TODO: backpressure plugin
    auto bufvec =
        std::make_shared<std::vector<asio::const_buffer>>(framer_->Frame(*msg));
    write_queue_->AsyncSend(bufvec, [self = this->shared_from_this(),
                                     msg](bool ok) {
      if (!ok)
        if (auto bp = self->plugins_.Get<IBackpressurePolicy>("backpressure"))
          bp->OnDrop(*msg);
    });
  }

  void SendRequest(Msg request, absl::Duration timeout,
                   ResponseHandler on_response) {
    uint64_t cid = next_correlation_id_++;
    request->meta().correlation_id = cid;
    {
      absl::MutexLock lock(&pending_mu_);
      pending_responses_.emplace(cid, std::move(on_response));
    }
    pending_requests_.fetch_add(1, std::memory_order_relaxed);
    Send(request);

    auto timer = std::make_shared<asio::steady_timer>(executor_);
    timer->expires_after(timeout);
    timer->async_wait([self = this->shared_from_this(), cid, timer](auto ec) {
      if (ec)
        return;
      ResponseHandler cb;
      {
        absl::MutexLock lock(&self->pending_mu_);
        auto it = self->pending_responses_.find(cid);
        if (it == self->pending_responses_.end())
          return;
        cb = std::move(it->second);
        self->pending_responses_.erase(it);
      }
      self->pending_requests_.fetch_sub(1, std::memory_order_relaxed);
      if (cb)
        cb(asio::error::timed_out, nullptr);
    });
  }

  void Close(bool force = false) {
    shutdown_ = true;
    if (force) {
      socket_->lowest_layer().cancel();
      socket_->lowest_layer().close();
      write_queue_->Close(true);
    } else {
      write_queue_->Close(false);
      // TODO: post-drain socket close
    }
    connect_timer_.cancel();
    read_timer_.cancel();
    idle_timer_.cancel();
    if (on_disconnect_)
      on_disconnect_({});
  }

  void SetOnMessage(MessageHandler h) {
    on_message_ = std::move(h);
  }
  void SetOnDisconnect(DisconnectHandler h) {
    on_disconnect_ = std::move(h);
  }

  template <typename T>
  void RegisterPlugin(const std::string& key, std::shared_ptr<T> p) {
    plugins_.Register(key, std::move(p));
  }

  MetricsSnapshot SnapshotMetrics() const {
    MetricsSnapshot m;
    m.pending_requests = pending_requests_.load();
    return m;
  }

 private:
  void DoHandshake() {
    socket_->async_handshake(
        boost::asio::ssl::stream_base::client,
        asio::bind_executor(executor_,
                            [self = this->shared_from_this()](auto ec) {
                              if (self->on_connect_)
                                self->on_connect_(ec);
                              if (!ec)
                                self->StartRead();
                            }));
  }

  void StartRead() {
    if (shutdown_)
      return;
    auto& rb = framer_->PrepareRead();
    socket_->async_read_some(
        boost::asio::buffer(rb.data(), rb.size()),
        asio::bind_executor(executor_, [self = this->shared_from_this()](
                                           auto ec, std::size_t n) {
          if (ec) {
            self->Close(true);
            return;
          }
          self->read_timer_.cancel();
          self->read_timer_.expires_after(self->config_.read_timeout);
          self->idle_timer_.expires_after(self->config_.idle_timeout);

          auto msgs = self->framer_->OnData(n);
          for (auto& m : msgs) {
            auto ptr = std::make_shared<InternalMessage>(std::move(m));
            uint64_t cid = ptr->meta().correlation_id;
            ResponseHandler cb;
            {
              absl::MutexLock lock(&self->pending_mu_);
              auto it = self->pending_responses_.find(cid);
              if (it != self->pending_responses_.end()) {
                cb = std::move(it->second);
                self->pending_responses_.erase(it);
              }
            }
            if (cb) {
              self->pending_requests_.fetch_sub(1, std::memory_order_relaxed);
              cb({}, ptr);
            } else if (self->on_message_) {
              self->on_message_(ptr);
            }
          }
          self->StartRead();
        }));

    read_timer_.expires_after(config_.read_timeout);
    read_timer_.async_wait(asio::bind_executor(
        executor_, [self = this->shared_from_this()](auto ec) {
          if (!ec)
            self->Close(true);
        }));

    idle_timer_.expires_after(config_.idle_timeout);
    idle_timer_.async_wait([self = this->shared_from_this()](auto ec) {
      if (!ec)
        self->Close(true);
    });
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
  absl::Mutex pending_mu_;
  absl::node_hash_map<uint64_t, ResponseHandler> pending_responses_;

  MessageHandler on_message_;
  ConnectHandler on_connect_;
  DisconnectHandler on_disconnect_;
  bool shutdown_ = false;
};

}  // namespace tcp
}  // namespace vectro
