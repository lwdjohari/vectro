#include <boost/asio.hpp>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <string>

#include "vectro/frame/frame_base.h"
#include "vectro/frame/internal_message.h"
// #include "vectro/plugin/heartbeat_emitter.h"
// #include "vectro/plugin/interceptor.h"
#include "vectro/plugin/plugin_bundle.h"

namespace vectro {
namespace asio = boost::asio;
// per-connection state with thread-safe, dual write paths and graceful & force
// shutdown modes
template <typename Stream>
class Session : public std::enable_shared_from_this<Session<Stream>> {
 public:
  using MessageHandler =
      std::function<void(std::shared_ptr<Session>, InternalMessage)>;
  using DisconnectHandler =
      std::function<void(std::shared_ptr<Session>, std::error_code)>;

  Session(Stream stream, boost::asio::any_io_executor executor,
          std::unique_ptr<FramerBase> framer, std::chrono::seconds read_timeout,
          std::chrono::seconds idle_timeout)
                  : socket_(std::move(stream)),
                    executor_(executor),
                    framer_(std::move(framer)),
                    read_timer_(executor),
                    idle_timer_(executor),
                    read_timeout_(read_timeout),
                    idle_timeout_(idle_timeout),
                    write_queue_sg_(socket_, executor) {}

  // Set plugin bundle for interceptors, heartbeat, etc.
  void SetPluginBundle(const PluginBundle& bundle) {
    plugin_bundle_ = bundle;
  }

  // Enable scatter/gather write queue; otherwise use direct writes
  void EnableWriteQueue(bool enable) {
    use_write_queue_ = enable;
  }

  // Start session I/O loops
  void Start(uint64_t id, std::string remote_ip, uint16_t remote_port,
             uint16_t local_port, MessageHandler on_message,
             DisconnectHandler on_disconnect) {
    id_ = id;
    remote_ip_ = std::move(remote_ip);
    remote_port_ = remote_port;
    local_port_ = local_port;
    on_message_ = std::move(on_message);
    on_disconnect_ = std::move(on_disconnect);

    StartRead();
    StartIdleTimer();
  }

  // Send a message asynchronously
  void AsyncSend(const InternalMessage& msg) {
    try {
      // Interceptor plugin may drop messages
      auto interceptor =
          GetTypedPlugin<Interceptor>(plugin_bundle_, PluginKeys::kInterceptor);
      if (interceptor &&
          !interceptor->Filter(const_cast<InternalMessage&>(msg))) {
        return;
      }

      auto buffers = framer_->Frame(msg);
      if (use_write_queue_) {
        auto shared = std::make_shared<std::vector<boost::asio::const_buffer>>(
            std::move(buffers));
        write_queue_sg_.AsyncSend(
            shared, [self = this->shared_from_this()](bool success) {
              // TODO: real-world: metric backpressure rejection
            });
      } else {
        write_deque_.emplace_back(std::move(buffers));
        if (!write_in_progress_)
          WriteNext();
      }
    } catch (const std::exception& ex) {
      // TODO: Error Propagation: log framing exceptions
    }
  }

  // Shutdown session: force or graceful
  void Close(bool force = false) {
    // TODO: ensure serialized flush via executor/strand
    asio::dispatch(executor_, [self = this->shared_from_this(), force]() {
      if (self->shutdown_in_progress_)
        return;
      if (force) {
        self->shutdown_in_progress_ = true;
        std::error_code ec;
        self->socket_.lowest_layer().cancel(ec);
        self->socket_.lowest_layer().close(ec);
        // clear pending buffers
        self->write_deque_.clear();
        if (self->use_write_queue_)
          self->write_queue_sg_.Close(true);
      } else {
        // Initiate graceful shutdown: drain writes
        self->graceful_shutdown_ = true;
        self->read_timer_.cancel();
        self->idle_timer_.cancel();
        if (self->use_write_queue_) {
          self->write_queue_sg_.Close(false);
        }
        if (!self->use_write_queue_ && !self->write_in_progress_ &&
            self->write_deque_.empty()) {
          self->Close(true);
        }
      }
    });
  }

  uint64_t id() const {
    return id_;
  }

 private:
  // Read path with timeout and race-condition guards
  void StartRead() {
    if (shutdown_in_progress_)
      return;
    auto buf = framer_->PrepareRead();
    socket_.async_read_some(
        boost::asio::buffer(buf),
        asio::bind_executor(executor_, [self = shared_from_this(), buf](
                                           std::error_code ec, std::size_t n) {
          if (self->shutdown_in_progress_)
            return;
          if (!ec) {
            // TODO: Performance Metrics: record bytes read, message count
            self->read_timer_.expires_after(self->read_timeout_);
            self->idle_timer_.expires_after(self->idle_timeout_);
            auto msgs = self->framer_->OnData(n);
            for (auto& m : msgs) {
              try {
                if (self->on_message_)
                  self->on_message_(self, std::move(m));
              } catch (...) { /* survive handler exceptions */
              }
            }
            self->StartRead();
          } else {
            self->Shutdown();
          }
        }));
    read_timer_.expires_after(read_timeout_);
    read_timer_.async_wait(asio::bind_executor(
        executor_, [self = shared_from_this()](std::error_code ec) {
          if (!ec && !self->shutdown_in_progress_)
            self->Shutdown();
        }));
  }

  // Idle timeout path
  void StartIdleTimer() {
    idle_timer_.expires_after(idle_timeout_);
    idle_timer_.async_wait(asio::bind_executor(
        executor_, [self = shared_from_this()](std::error_code ec) {
          if (!ec && !self->shutdown_in_progress_)
            self->Shutdown();
        }));
  }

  // Direct-write path with graceful drain
  void WriteNext() {
    if (write_deque_.empty()) {
      write_in_progress_ = false;
      if (graceful_shutdown_)
        Close(true);
      return;
    }
    write_in_progress_ = true;
    auto buffers = std::move(write_deque_.front());
    write_deque_.pop_front();
    boost::asio::async_write(
        socket_, buffers,
        asio::bind_executor(executor_, [self = shared_from_this()](
                                           std::error_code ec, std::size_t) {
          if (!ec) {
            self->write_in_progress_ = false;
            self->WriteNext();
          } else {
            self->Shutdown();
          }
        }));
  }

  // Final shutdown cleanup
  void Shutdown() {
    if (shutdown_in_progress_)
      return;
    shutdown_in_progress_ = true;
    if (on_disconnect_)
      on_disconnect_(shared_from_this(), {});
    Close(true);
    // Resource Cleanup: clear buffers & break cycles
    write_deque_.clear();
    if (use_write_queue_)
      write_queue_sg_.Close(true);
    framer_.reset();
    plugin_bundle_ = {};
    on_message_ = nullptr;
    on_disconnect_ = nullptr;
  }

  Stream socket_;
  asio::any_io_executor executor_;
  std::unique_ptr<FramerBase> framer_;

  // Direct-write queue
  std::deque<std::vector<asio::const_buffer>> write_deque_;
  bool write_in_progress_ = false;

  // Scatter/Gather queue
  stream::WriteQueueSG<Stream> write_queue_sg_;
  bool use_write_queue_ = false;

  // Timers
  boost::asio::steady_timer read_timer_;
  boost::asio::steady_timer idle_timer_;
  std::chrono::seconds read_timeout_, idle_timeout_;

  // Shutdown flags
  bool shutdown_in_progress_ = false;
  bool graceful_shutdown_ = false;

  uint64_t id_{0};
  std::string remote_ip_;
  uint16_t remote_port_{0}, local_port_{0};
  MessageHandler on_message_;
  DisconnectHandler on_disconnect_;
  PluginBundle plugin_bundle_;
};
}  // namespace vectro
