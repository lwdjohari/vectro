#pragma once

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "absl/strings/str_format.h"
#include "absl/synchronization/mutex.h"
#include "absl/synchronization/notification.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"

#if defined(__linux__)
#include <pthread.h>
#endif

namespace vectro {
namespace pool {

using absl::Duration;
using absl::Milliseconds;
using absl::Now;
using absl::Time;

// Scheduling modes
enum class ScheduleMode { FIFO = 0, RoundRobin, WorkStealing };

// Steal policy callback
typedef std::function<size_t(const std::vector<size_t>&)> StealPolicy;
// Final callback on shutdown
typedef std::function<void()> FinalCallback;
// Thread affinity callback
typedef std::function<void(std::thread&, size_t)> AffinityFn;

// Configuration for TaskMultiProcessor
// Supports scheduling, backpressure, circuit-breaker, shutdown, affinity
// Adds Task Timeouts & Cancellation and Futures/Promises support
// template <typename Clock = absl::DefaultClock>
struct TaskProcessorConfig {
  std::string id = "TaskProcessor";
  size_t num_threads = std::thread::hardware_concurrency();
  ScheduleMode mode = ScheduleMode::FIFO;
  StealPolicy steal_policy;

  size_t max_queue_size = 10000;
  size_t backpressure_threshold = 5000;
  Duration backoff_duration = Milliseconds(10);
  double jitter_fraction = 0.1;

  Duration drop_notification_interval = Milliseconds(1000);
  Duration shutdown_timeout = Milliseconds(5000);

  bool enable_circuit_breaker = true;
  bool enable_backoff = true;
  bool bind_threads = false;
  bool enable_drain_on_shutdown = true;
};

// Internal task wrapper holds per-task callbacks to avoid global accumulation
template <typename T>
struct TaskItem {
  T payload;
  std::vector<std::function<void(const T&)>> wrappers;
};

// TaskMultiProcessor: generic, thread-safe, multithreaded queue with advanced
// policies
template <typename T>
class TaskMultiProcessor {
 public:
  using Callback = std::function<void(const T&)>;

  explicit TaskMultiProcessor(const TaskProcessorConfig& cfg = {})
                  : cfg_(cfg),
                    running_(true),
                    stopped_(false),
                    tasks_in_queue_(0),
                    dropped_count_(0),
                    high_water_mark_(0),
                    processed_count_(0),
                    stolen_count_(0),
                    rr_index_(0),
                    last_drop_notification_(Now()),
                    active_workers_(cfg.num_threads),
                    steal_policy_(cfg.steal_policy ? cfg.steal_policy
                                                   : DefaultStealPolicy) {
    queues_.resize(cfg.num_threads);
    qmutexes_.reserve(cfg.num_threads);
    for (size_t i = 0; i < cfg.num_threads; ++i) {
      qmutexes_.push_back(std::make_unique<absl::Mutex>());
    }
    for (size_t i = 0; i < cfg_.num_threads; ++i) {
      workers_.emplace_back(&TaskMultiProcessor::ProcessLoop, this, i);
      if (cfg_.bind_threads) {
        if (thread_affinity_)
          thread_affinity_(workers_.back(), i);
        else
          DefaultAffinity(workers_.back(), i);
      }
    }
  }

  ~TaskMultiProcessor() {
    running_.store(false, std::memory_order_release);
    // wake up workers without holding other locks
    cv_.SignalAll();
    Time start = Now();
    if (cfg_.enable_drain_on_shutdown) {
      while (tasks_in_queue_.load() > 0 &&
             Now() - start < cfg_.shutdown_timeout) {
        absl::SleepFor(Milliseconds(1));
      }
    }
    // Join all threads
    for (auto& t : workers_) {
      if (t.joinable())
        t.join();
    }
    // Wait for all worker loops to finish
    while (active_workers_.load() > 0) {
      absl::SleepFor(Milliseconds(1));
    }
    stopped_.store(true, std::memory_order_release);
    if (final_callback_) {
      try {
        final_callback_();
      } catch (...) {
      }
    }
  }

  // Configuration
  void UpdateConfig(const TaskProcessorConfig& cfg) {
    cfg_ = cfg;
  }
  void SetThreadAffinity(AffinityFn fn) {
    thread_affinity_ = fn;
  }
  void SetFinalCallback(FinalCallback cb) {
    final_callback_ = cb;
  }
  void RegisterCallback(const Callback& cb) {
    absl::MutexLock lock(&cb_mu_);
    global_callbacks_.push_back(cb);
  }

  // Plain enqueue
  void Submit(const T& item) {
    SubmitImpl(item, {});
  }

  // Submit with timeout
  bool SubmitWithTimeout(const T& item, Time deadline) {
    if (Now() >= deadline)
      return false;
    SubmitImpl(item, {});
    return true;
  }
  bool SubmitWithTimeout(const T& item, Time deadline,
                          std::shared_ptr<std::atomic<bool>> token) {
    if ((token && token->load()) || Now() >= deadline)
      return false;
    SubmitImpl(item, {});
    return true;
  }

  // Submit with future
  template <typename R>
  std::future<R> SubmitWithResult(const T& item,
                                   std::function<R(const T&)> fn) {
    auto p = std::make_shared<std::promise<R>>();
    auto fut = p->get_future();
    SubmitImpl(item, {[fn, p](const T& t) {
                  try {
                    p->set_value(fn(t));
                  } catch (...) {
                    p->set_exception(std::current_exception());
                  }
                }});
    return fut;
  }

  // Submit with future + cancellation
  template <typename R>
  std::future<R> SubmitWithResult(const T& item, std::function<R(const T&)> fn,
                                   std::shared_ptr<std::atomic<bool>> token) {
    auto p = std::make_shared<std::promise<R>>();
    auto fut = p->get_future();
    if (token && token->load(std::memory_order_relaxed)) {
      p->set_exception(std::make_exception_ptr(
          std::runtime_error("Task canceled before enqueue")));
      return fut;
    }
    SubmitImpl(
        item, {[fn, p, token](const T& t) {
          try {
            if (token && token->load(std::memory_order_relaxed)) {
              p->set_exception(std::make_exception_ptr(
                  std::runtime_error("Task canceled before execution")));
            } else {
              p->set_value(fn(t));
            }
          } catch (...) {
            p->set_exception(std::current_exception());
          }
        }});
    return fut;
  }

  // Metrics
  size_t GetQueueSize() const {
    return tasks_in_queue_.load();
  }
  size_t GetDroppedCount() {
    return dropped_count_.exchange(0);
  }
  size_t GetHighWaterMark() const {
    return high_water_mark_.load();
  }
  size_t GetProcessedCount() const {
    return processed_count_.load();
  }
  size_t GetStolenCount() const {
    return stolen_count_.load();
  }

 private:
  void SubmitImpl(const T& item, std::vector<Callback> wrappers) {
    size_t curr = tasks_in_queue_.load();
    
    // Drop message by Circuit-Breaker
    if (cfg_.enable_circuit_breaker && cfg_.max_queue_size &&
        curr >= cfg_.max_queue_size) {
      dropped_count_.fetch_add(1);
      return;
    }

    // Backpressure handling
    if (cfg_.enable_backoff && cfg_.backpressure_threshold &&
        curr >= cfg_.backpressure_threshold) {
      int64_t base = absl::ToInt64Milliseconds(cfg_.backoff_duration);
      int64_t jit = static_cast<int64_t>(base * cfg_.jitter_fraction);
      thread_local static std::mt19937_64 rng((std::random_device())());
      int64_t ms =
          base + std::uniform_int_distribution<int64_t>(-jit, jit)(rng);
      if (ms > 0){
        absl::SleepFor(Milliseconds(ms));
      }
    }

    // add to queue
    TaskItem<T> ti{item, std::move(wrappers)};
    size_t idx = (cfg_.mode == ScheduleMode::FIFO
                      ? 0
                      : rr_index_.fetch_add(1) % cfg_.num_threads);
    {
      absl::MutexLock lock(qmutexes_[idx].get());
      queues_[idx].push_back(std::move(ti));
      size_t hwm = queues_[idx].size(), prev = high_water_mark_.load();
      while (hwm > prev && !high_water_mark_.compare_exchange_weak(prev, hwm)) {
        // yield to avoids sudden spike that lead to wildfire &
        // avoids aggressive spinning
        std::this_thread::yield();
      }
    }
    tasks_in_queue_.fetch_add(1);
    {
      absl::MutexLock lock(&mutex_);
      cv_.SignalAll();
    }
  }

  void ProcessLoop(size_t id) {
    while (running_.load()) {
      TaskItem<T> ti;
      bool got = false;
      {
        absl::MutexLock lock(&mutex_);
        // wait until shutdown or a task arrives
        while (running_.load() && tasks_in_queue_.load() == 0) {
          cv_.Wait(&mutex_);
        }
      }
      // own queue
      {
        absl::MutexLock lock(qmutexes_[id].get());
        auto& dq = queues_[id];
        if (!dq.empty()) {
          ti = std::move(dq.front());
          dq.pop_front();
          got = true;
        }
      }
      // work stealing
      if (!got && cfg_.mode == ScheduleMode::WorkStealing) {
        std::vector<size_t> sizes(cfg_.num_threads);
        for (size_t i = 0; i < cfg_.num_threads; ++i)
          sizes[i] = queues_[i].size();
        size_t vic = steal_policy_(sizes);
        absl::MutexLock lock(qmutexes_[vic].get());
        auto& dq = queues_[vic];
        if (!dq.empty()) {
          ti = std::move(dq.back());
          dq.pop_back();
          got = true;
          stolen_count_.fetch_add(1);
        }
      }
      if (got) {
        tasks_in_queue_.fetch_sub(1);
        processed_count_.fetch_add(1);
        {
          absl::MutexLock lock(&cb_mu_);
          for (auto& g : global_callbacks_) {
            try {
              g(ti.payload);
            } catch (...) { /* swallow */
            }
          }
        }
        for (auto& w : ti.wrappers) {
          try {
            w(ti.payload);
          } catch (...) { /* swallow */
          }
        }
      }
      HandleDrops();
    }
    // signal worker exit
    active_workers_.fetch_sub(1);
    stopped_.store(true);
  }

  void HandleDrops() {
    size_t d = dropped_count_.exchange(0);
    if (d) {
      Time now = Now();
      if (now - last_drop_notification_ >= cfg_.drop_notification_interval) {
        std::fprintf(stderr, "[%s] dropped %zu tasks", cfg_.id.c_str(), d);
        last_drop_notification_ = now;
      }
    }
  }

  static void DefaultAffinity(std::thread& t, size_t i) {
#if defined(__linux__)
    cpu_set_t cs;
    CPU_ZERO(&cs);
    CPU_SET(i % std::thread::hardware_concurrency(), &cs);
    pthread_setaffinity_np(t.native_handle(), sizeof(cs), &cs);
#endif
  }

  static size_t DefaultStealPolicy(const std::vector<size_t>& sizes) {
    return std::distance(sizes.begin(),
                         std::max_element(sizes.begin(), sizes.end()));
  }

  TaskProcessorConfig cfg_;
  std::atomic<bool> running_, stopped_;
  std::atomic<size_t> active_workers_;  // tracks remaining workers
  std::vector<std::deque<TaskItem<T>>> queues_;
  std::vector<std::unique_ptr<absl::Mutex>> qmutexes_;
  // std::vector<absl::Mutex> qmutexes_;
  std::vector<std::thread> workers_;
  absl::Mutex mutex_;
  absl::CondVar cv_;
  absl::Mutex cb_mu_;
  std::vector<Callback> global_callbacks_;

  std::atomic<size_t> tasks_in_queue_, dropped_count_, high_water_mark_;
  std::atomic<size_t> processed_count_, stolen_count_, rr_index_;
  Time last_drop_notification_;
  StealPolicy steal_policy_;
  AffinityFn thread_affinity_;
  FinalCallback final_callback_;
};

// ================= Real-World Usage Examples =================
// 1. Serialized Logging (FIFO + Single Thread)
//    Ensures log messages are written strictly in arrival order.
//
// struct LogMessage { int level; std::string text; Time ts; };
// TaskProcessorConfig cfg1;
// cfg1.id = "Logger";
// cfg1.mode = ScheduleMode::FIFO;
// cfg1.num_threads = 1;
// util::TaskMultiProcessor<LogMessage> logger(cfg1);
// logger.RegisterCallback([](const LogMessage &m) {
//   // Safe, ordered write to disk
//   write_to_disk(m);
// });
// logger.Submit({INFO, "Start up", Now()});

// 2. Video Transcoding Pool (Round-Robin)
//    Distributes frames evenly across 4 worker threads.
//
// struct Frame { int id; /* pixel data */ };
// TaskProcessorConfig cfg2;
// cfg2.id = "Transcoder";
// cfg2.mode = ScheduleMode::RoundRobin;
// cfg2.num_threads = 4;
// util::TaskMultiProcessor<Frame> transcoder(cfg2);
// transcoder.RegisterCallback([](const Frame &f) {
//   encode_frame(f);
// });
// for (auto &frame : frames) transcoder.Submit(frame);

// 3. Web Crawler (Work-Stealing)
//    Balances slow and fast fetch tasks dynamically.
//
// struct URLTask { std::string url; int depth; };
// TaskProcessorConfig cfg3;
// cfg3.id = "Crawler";
// cfg3.mode = ScheduleMode::WorkStealing;
// cfg3.num_threads = 8;
// cfg3.steal_policy = [](const auto &sizes) {
//   // steal from largest queue
//   return std::distance(sizes.begin(), std::max_element(sizes.begin(),
//   sizes.end()));
// };
// util::TaskMultiProcessor<URLTask> crawler(cfg3);
// crawler.RegisterCallback([](const URLTask &u) {
//   fetch_and_parse(u.url, u.depth);
// });
// crawler.Submit({"https://example.com", 0});

// 4. Task with Timeout
//    Drop if not queued before deadline.
//
// auto success = crawler.SubmitWithTimeout(task, Now() + Milliseconds(2000));
// if (!success) {
//   log_warning("Task dropped due to timeout");
// }

// 5. Task with Future Result
//    Retrieve compute result asynchronously.
//
// struct ComputeTask { int data; };
// auto fut = transcoder.SubmitWithResult<int>(
//     {42},
//     [](const ComputeTask &t) { return do_compute(t.data); }
// );
// int result = fut.get();

// 6. Cancelable Task
//    Use cancellation token to abort before enqueue or before execution.
//
// auto token = std::make_shared<std::atomic<bool>>(false);
// auto fut2 = transcoder.SubmitWithResult<int>(
//     task,
//     [](const ComputeTask &t) { return heavy_compute(t.data); },
//     token
// );
// // Cancel before enqueue
// token->store(true);
// try {
//   int res = fut2.get();
// } catch (const std::exception &e) {
//   log_error("Task canceled: " + std::string(e.what()));
// }

}  // namespace pool
}  // namespace vectro
