#pragma once
#include <boost/asio.hpp>
#include <deque>
#include <functional>
#include <memory>
#include <vector>

namespace vectro {
namespace stream {
namespace asio = boost::asio;

// Scatter/Gather write queue with optional backpressure and shutdown modes
// - max_queue enforcement can be toggled
// - force shutdown cancels in-flight writes; graceful shutdown drains queue
// TODO: real-world: increment drop metric when backpressure rejection occurs
// TODO: real-world: log and metric write failures in DoWrite
// TODO: real-world: emit metrics for force and graceful shutdown events
template <typename Stream>
class WriteQueueSG {
 public:
  using BufferList = std::vector<asio::const_buffer>;
  using CompletionHandler = std::function<void(bool)>;

  WriteQueueSG(Stream& stream, asio::any_io_executor executor,
               std::size_t max_queue = 1024)
                  : stream_(stream),
                    strand_(executor),
                    max_queue_size_(max_queue) {}

  // Toggle max-queue backpressure enforcement
  void EnableMaxQueue(bool enable) {
    asio::dispatch(strand_, [this, enable]() { max_queue_enabled_ = enable; });
  }

  // Enqueue scatter/gather buffers for async write
  void AsyncSend(std::shared_ptr<BufferList> buffers,
                 CompletionHandler handler = nullptr) {
    asio::dispatch(
        strand_, [this, buffers = std::move(buffers), handler]() mutable {
          if (max_queue_enabled_ && write_queue_.size() >= max_queue_size_) {
            if (handler)
              handler(false);
            return;
          }
          write_queue_.emplace_back(std::move(buffers), std::move(handler));
          if (!writing_)
            DoWrite();
        });
  }

  // Shutdown: force=true cancels in-flight writes immediately;
  // force=false initiates graceful shutdown (drain remaining writes)
  void Close(bool force = false) {
    asio::dispatch(strand_, [this, force]() {
      if (force) {
        // Force shutdown: cancel all async operations and clear queue
        std::error_code ec;
        stream_.lowest_layer().cancel(ec);
        write_queue_.clear();
        writing_ = false;
      } else {
        // Graceful shutdown: mark and drain
        graceful_shutdown_ = true;
      }
    });
  }

 private:
  void DoWrite() {
    if (write_queue_.empty()) {
      writing_ = false;
      if (graceful_shutdown_) {
        write_queue_.clear();
      }
      return;
    }
    writing_ = true;
    auto [buffers, handler] = std::move(write_queue_.front());
    write_queue_.pop_front();

    asio::async_write(
        stream_, *buffers,
        asio::bind_executor(
            strand_, [this, buffers, handler](std::error_code ec, std::size_t) {
              if (handler)
                handler(!ec);
              if (!ec) {
                DoWrite();
              } else {
                write_queue_.clear();
              }
            }));
  }

  Stream& stream_;
  asio::strand<asio::any_io_executor> strand_;
  std::deque<std::pair<std::shared_ptr<BufferList>, CompletionHandler>>
      write_queue_;
  std::size_t max_queue_size_;
  bool max_queue_enabled_ = true;
  bool writing_ = false;
  bool graceful_shutdown_ = false;
};

}  // namespace stream
}  // namespace vectro
