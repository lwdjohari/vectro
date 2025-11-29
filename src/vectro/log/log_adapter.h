#pragma once

#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "absl/strings/str_format.h"
#include "absl/synchronization/notification.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"

namespace vectro {
namespace log {

// Severity levels (syslog-inspired)
enum class LogLevel {
  Trace = 0,
  Debug,
  Info,
  Notice,
  Warning,
  Error,
  Critical,
  Alert,
  Emergency
};

// Encapsulates a log message with metadata
struct LogMessage {
  std::string logger_id;
  LogLevel level;
  absl::Time timestamp;
  std::thread::id thread_id;
  std::string message;
};

// LogAdapter: instance-based, thread-safe, non-blocking with
// backoff, jitter, circuit breaker, and robust error handling.
template <typename Clock = std::chrono::steady_clock>
class LogAdapter {
 public:
  using Callback = std::function<void(const LogMessage&)>;

  explicit LogAdapter(const std::string& id)
      : logger_id_(id),
        running_(true),
        stopped_(false),
        max_queue_size_(10000),
        backpressure_threshold_(5000),
        backoff_duration_ms_(10),
        jitter_fraction_(0.1),
        drop_notification_interval_ms_(1000),
        shutdown_timeout_ms_(5000),
        dropped_messages_(0),
        high_water_mark_(0),
        last_drop_notification_(absl::Now()) {
    worker_ = std::make_unique<std::thread>(&LogAdapter::ProcessLoop, this);
  }

  ~LogAdapter() {
    // Signal shutdown
    {
      absl::MutexLock lock(&queue_mu_);
      running_.store(false, std::memory_order_release);
      queue_cv_.Signal();
    }
    // Wait up to shutdown_timeout_ms_ for worker to stop
    auto start = Clock::now();
    while (!stopped_.load(std::memory_order_acquire) &&
           Clock::now() - start <
               std::chrono::milliseconds(shutdown_timeout_ms_.load())) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // Join or detach to avoid blocking destructor
    if (worker_->joinable()) {
      if (stopped_)
        worker_->join();
      else
        worker_->detach();
    }
  }

  LogAdapter(const LogAdapter&)            = delete;
  LogAdapter& operator=(const LogAdapter&) = delete;
  LogAdapter(LogAdapter&&)                 = default;
  LogAdapter& operator=(LogAdapter&&)      = default;

  // Configuration setters
  void SetMaxQueueSize(size_t max) {
    max_queue_size_.store(max, std::memory_order_relaxed);
  }
  void SetBackpressureThreshold(size_t threshold) {
    backpressure_threshold_.store(threshold, std::memory_order_relaxed);
  }
  void SetBackoffDuration(std::chrono::milliseconds dur) {
    backoff_duration_ms_.store(dur.count(), std::memory_order_relaxed);
  }
  void SetJitterFraction(double frac) {
    jitter_fraction_.store(frac, std::memory_order_relaxed);
  }
  void SetDropNotificationInterval(std::chrono::milliseconds interval) {
    drop_notification_interval_ms_.store(interval.count(),
                                         std::memory_order_relaxed);
  }
  void SetShutdownTimeout(std::chrono::milliseconds timeout) {
    shutdown_timeout_ms_.store(timeout.count(), std::memory_order_relaxed);
  }

  // Metrics accessors
  size_t GetDroppedCount() {
    return dropped_messages_.exchange(0, std::memory_order_relaxed);
  }
  size_t GetQueueSize() {
    absl::MutexLock lock(&queue_mu_);
    return queue_.size();
  }
  size_t GetHighWaterMark() {
    return high_water_mark_.load(std::memory_order_relaxed);
  }

  // Callback registration
  void RegisterCallback(Callback cb) {
    auto old_ptr = callbacks_.load(std::memory_order_acquire);
    auto new_ptr = std::make_shared<std::vector<Callback>>(*old_ptr);
    new_ptr->push_back(std::move(cb));
    while (!callbacks_.compare_exchange_weak(old_ptr, new_ptr,
                                             std::memory_order_release,
                                             std::memory_order_acquire)) {
      new_ptr = std::make_shared<std::vector<Callback>>(*old_ptr);
      new_ptr->push_back(cb);
    }
  }
  void ClearCallbacks() {
    callbacks_.store(std::make_shared<std::vector<Callback>>(),
                     std::memory_order_release);
  }

  // Non-blocking log entry
  void Log(LogLevel level, const std::string& msg) {
    LogMessage lm{logger_id_, level, absl::Now(), std::this_thread::get_id(),
                  msg};
    Enqueue(std::move(lm));
  }

 private:
  // Enqueue log message: may backoff, drop, and updates high-water mark
  void Enqueue(LogMessage&& lm) {
    try {
      size_t sz;
      {
        absl::MutexLock lock(&queue_mu_);
        sz = queue_.size();
      }
      // Circuit breaker: drop if full
      if (sz >= max_queue_size_.load(std::memory_order_relaxed)) {
        dropped_messages_.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      // Backpressure: sleep with jitter if above threshold
      size_t thr = backpressure_threshold_.load(std::memory_order_relaxed);
      if (sz >= thr) {
        int64_t base   = backoff_duration_ms_.load(std::memory_order_relaxed);
        double frac    = jitter_fraction_.load(std::memory_order_relaxed);
        int64_t jitter = static_cast<int64_t>(base * frac);
        thread_local static std::mt19937_64 rng((std::random_device())());
        std::uniform_int_distribution<int64_t> dist(-jitter, jitter);
        int64_t ms = base + dist(rng);
        if (ms > 0)
          std::this_thread::sleep_for(std::chrono::milliseconds(ms));
      }
      {
        absl::MutexLock lock(&queue_mu_);
        // Double-check after backoff
        if (queue_.size() >= max_queue_size_.load(std::memory_order_relaxed)) {
          dropped_messages_.fetch_add(1, std::memory_order_relaxed);
          return;
        }
        queue_.emplace_back(std::move(lm));
        // Update high-water mark
        size_t hwm  = queue_.size();
        size_t prev = high_water_mark_.load(std::memory_order_relaxed);
        while (hwm > prev && !high_water_mark_.compare_exchange_weak(
                                 prev, hwm, std::memory_order_relaxed)) {
        }
      }
      queue_cv_.Signal();
    } catch (const std::exception& e) {
      std::string err =
          absl::StrFormat("[LogAdapter] Enqueue exception: %s\n", e.what());
      fwrite(err.data(), 1, err.size(), stderr);
    } catch (...) {
      const char* err = "[LogAdapter] Enqueue unknown exception\n";
      fwrite(err, 1, strlen(err), stderr);
    }
  }

  // Worker loop: processes batches and rate-limits drop notifications
  void ProcessLoop() {
    try {
      while (true) {
        std::deque<LogMessage> batch;
        {
          absl::MutexLock lock(&queue_mu_);
          if (!running_.load(std::memory_order_acquire) && queue_.empty())
            break;
          if (queue_.empty()) {
            queue_cv_.Wait(&queue_mu_);
            continue;
          }
          batch.swap(queue_);
        }
        for (auto& msg : batch)
          Dispatch(msg);
        HandleDrops();
      }
      // Drain remaining
      std::deque<LogMessage> final_batch;
      {
        absl::MutexLock lock(&queue_mu_);
        final_batch.swap(queue_);
      }
      for (auto& msg : final_batch)
        Dispatch(msg);
      HandleDrops();
    } catch (const std::exception& ex) {
      std::string err = absl::StrFormat(
          "[LogAdapter] Worker thread exception: %s\n", ex.what());
      fwrite(err.data(), 1, err.size(), stderr);
    } catch (...) {
      const char* err = "[LogAdapter] Worker thread unknown exception\n";
      fwrite(err, 1, strlen(err), stderr);
    }
    stopped_.store(true, std::memory_order_release);
  }

  // Dispatch to callbacks or fallback
  void Dispatch(const LogMessage& lm) {
    auto cbs = callbacks_.load(std::memory_order_acquire);
    if (cbs->empty()) {
      DefaultLog(lm);
    } else {
      for (const auto& cb : *cbs) {
        try {
          cb(lm);
        } catch (const std::exception& e) {
          error_buffer_ +=
              absl::StrFormat("[Callback exception] %s\n", e.what());
        } catch (...) {
          error_buffer_ += "[Callback unknown exception]\n";
        }
      }
      if (!error_buffer_.empty()) {
        fwrite(error_buffer_.data(), 1, error_buffer_.size(), stderr);
        error_buffer_.clear();
      }
    }
  }

  // Handle drop notifications with rate-limiting to avoid spam
  void HandleDrops() {
    size_t dropped = dropped_messages_.exchange(0, std::memory_order_relaxed);
    if (dropped) {
      absl::Time now = absl::Now();
      int64_t interval =
          drop_notification_interval_ms_.load(std::memory_order_relaxed);
      if (absl::ToInt64Milliseconds(now - last_drop_notification_) >=
          interval) {
        LogMessage lm{
            logger_id_, LogLevel::Warning, now, std::this_thread::get_id(),
            absl::StrFormat("[%s] dropped %zu msgs", logger_id_, dropped)};
        DefaultLog(lm);
        last_drop_notification_ = now;
      }
    }
  }

  // Default synchronous write; includes logger_id
  void DefaultLog(const LogMessage& lm) {
    try {
      std::string ts  = absl::FormatTime(absl::RFC3339_full, lm.timestamp,
                                         absl::LocalTimeZone());
      const char* lvl = ToString(lm.level);
      auto tid        = std::hash<std::thread::id>()(lm.thread_id);
      std::string out =
          absl::StrFormat("[%s] [%s] [%s] [t%llu] %s\n", ts, lm.logger_id, lvl,
                          static_cast<unsigned long long>(tid), lm.message);
      fwrite(out.data(), 1, out.size(), stderr);
    } catch (const std::exception& e) {
      std::string err =
          absl::StrFormat("[LogAdapter] DefaultLog exception: %s\n", e.what());
      fwrite(err.data(), 1, err.size(), stderr);
    } catch (...) {
      const char* err = "[LogAdapter] DefaultLog unknown exception\n";
      fwrite(err, 1, strlen(err), stderr);
    }
  }

  static const char* ToString(LogLevel lvl) {
    static constexpr const char* kNames[] = {
        "TRACE", "DEBUG",    "INFO",  "NOTICE",   "WARNING",
        "ERROR", "CRITICAL", "ALERT", "EMERGENCY"};
    size_t i = static_cast<size_t>(lvl);
    return i < (sizeof(kNames) / sizeof(kNames[0])) ? kNames[i] : "UNKNOWN";
  }

  // Instance state
  const std::string logger_id_;
  std::atomic<bool> running_, stopped_;
  absl::Mutex queue_mu_;
  absl::CondVar queue_cv_{&queue_mu_};
  std::deque<LogMessage> queue_;
  std::unique_ptr<std::thread> worker_;
  std::atomic<size_t> max_queue_size_, backpressure_threshold_;
  std::atomic<int64_t> backoff_duration_ms_;
  std::atomic<double> jitter_fraction_;
  std::atomic<int64_t> drop_notification_interval_ms_, shutdown_timeout_ms_;
  std::atomic<size_t> dropped_messages_, high_water_mark_;
  std::atomic<std::shared_ptr<std::vector<Callback>>> callbacks_;
  absl::Time last_drop_notification_;
  std::string error_buffer_;
};

// ========== Singleton and Convenience Macros ==========

// Default process-wide singleton logger
inline LogAdapter<>& DefaultLogAdapter() {
  static LogAdapter<> instance("DefaultLogAdapter");
  return instance;
}

// Convenience macros for AppLogger
#define LOG_ADPT_TRACE(msg) \
  DefaultLogAdapter().Log(logging::LogLevel::Trace, msg)
#define LOG_ADPT_DEBUG(msg) \
  DefaultLogAdapter().Log(logging::LogLevel::Debug, msg)
#define LOG_ADPT_INFO(msg) DefaultLogAdapter().Log(logging::LogLevel::Info, msg)
#define LOG_ADPT_NOTICE(msg) \
  DefaultLogAdapter().Log(logging::LogLevel::Notice, msg)
#define LOG_ADPT_WARNING(msg) \
  DefaultLogAdapter().Log(logging::LogLevel::Warning, msg)
#define LOG_ADPT_ERROR(msg) \
  DefaultLogAdapter().Log(logging::LogLevel::Error, msg)
#define LOG_ADPT_CRITICAL(msg) \
  DefaultLogAdapter().Log(logging::LogLevel::Critical, msg)
#define LOG_ADPT_ALERT(msg) \
  DefaultLogAdapter().Log(logging::LogLevel::Alert, msg)
#define LOG_ADPT_EMERGENCY(msg) \
  DefaultLogAdapter().Log(logging::LogLevel::Emergency, msg)

}  // namespace log
}  // namespace vectro