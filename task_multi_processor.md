# 🚀 Vectro TaskMultiProcessor

**Vectro** includes a battle-tested,single header-only, multithreaded task queue processor for C++17+ powered by Abseil. It provides flexible scheduling modes, backpressure controls, timeouts, futures/promises, and graceful shutdown—all wrapped in a single, easy-to-use template.

---

## 📋 Table of Contents

1. [Features](#features)
2. [Comparison](#comparison)
3. [Installation](#installation)
4. [Quick Start](#quick-start)
5. [API Reference](#api-reference)
6. [Real-World Examples](#real-world-examples)
7. [License](#license)

---

## 🔑 Features

* **Scheduling Modes**

  * **FIFO**: strict arrival order (single or multi-consumer)
  * **Round-Robin**: even load distribution across workers
  * **Work-Stealing**: idle threads steal from busiest queues
* **Backpressure & Circuit-Breaker**

  * Configurable `max_queue_size` drops excess tasks
  * `backoff_duration` + jitter smooth bursty workloads
  * Rate-limited drop notifications
* **Timeouts & Cancellation**

  * `SubmitWithTimeout` drops tasks past a deadline
  * Cancellation-token overloads for immediate abort
* **Futures & Promises**

  * `SubmitWithResult<R>` returns `std::future<R>` for async results
  * Cancellation aware overload
* **Graceful Shutdown**

  * Optional drain-on-shutdown with timeout
  * Barrier to ensure all workers exit before destructor returns
  * Final callback hook for cleanup
* **Exception Safety**

  * All callbacks (global and per-task) wrapped in `try/catch`
* **Observability & Metrics**

  * `GetQueueSize()`, `GetDroppedCount()`, `GetHighWaterMark()`,
    `GetProcessedCount()`, `GetStolenCount()`
* **Thread Affinity**

  * Built-in Linux `pthread_setaffinity_np` binding
  * Pluggable `SetThreadAffinity` hook

---

## ⚖️ Comparison

| Framework                | Scheduling Modes        | Futures/Promise | Backpressure & Drops | Graceful Shutdown | Dependency     |
| ------------------------ | ----------------------- | --------------- | -------------------- | ----------------- | -------------- |
| **vectro::pool::TaskMultiProcessor** | FIFO, RR, Work-Stealing | ✔️              | ✔️                   | ✔️                | C++17 + Abseil |
| Intel TBB (oneTBB)       | Work-Stealing           | n/a             | ✖️                   | ✖️                | oneTBB library |
| Boost.Asio `io_context`  | Single queue + strands  | Boost.Fiber?    | ✖️                   | ✔️                | Boost.Asio     |
| Folly Executor           | FIFO, custom executors  | ✔️              | ✖️                   | ✔️                | Folly library  |
| Microsoft PPL            | Task\_Group             | ✔️              | ✖️                   | ✖️                | MSVC runtime   |
| Boost.Thread pool        | Fixed-size, FIFO        | ✖️              | ✖️                   | ✖️                | Boost.Thread   |
| cpp-taskflow             | Task graph, async       | ✔️              | ✖️                   | ✔️                | Header-only    |

---

## 🛠️ Installation

1. **Copy** `task_multi_processor.h` into your include path or use from **vectro** library.
2. **Add** Abseil to your project and link against its synchronization and time libraries.
3. **Include** the header:

   ```cpp
   #include "vectro/pool/task_multi_processor.h"
   //or
   #include "task_multi_processor.h"
   ```

---

## ⚡ Quick Start

```cpp
#include "vectro/pool/task_multi_processor.h"
#include <iostream>

int main() {
  using namespace vectro::pool;

  // 1) Configure
  TaskProcessorConfig cfg;
  cfg.id = "SimpleExample";
  cfg.num_threads = 2;
  cfg.mode = ScheduleMode::RoundRobin;

  // 2) Create processor
  TaskMultiProcessor<int> qp(cfg);

  // 3) Register a simple callback
  qp.RegisterCallback([](int x) {
    std::cout << "Processed: " << x << '\n';
  });

  // 4) Submit tasks
  for (int i = 0; i < 10; ++i) qp.Submit(i);

  // 5) Destructor drains and joins threads
  return 0;
}
```

---

## 📖 API Reference

### `TaskProcessorConfig`

A simple struct with public members:

```cpp
std::string id;
size_t num_threads;
ScheduleMode mode;
StealPolicy steal_policy;
size_t max_queue_size;
size_t backpressure_threshold;
Duration backoff_duration;
double jitter_fraction;
Duration drop_notification_interval;
Duration shutdown_timeout;
bool enable_circuit_breaker;
bool enable_backoff;
bool bind_threads;
bool enable_drain_on_shutdown;
```

### `TaskMultiProcessor<T>`

```cpp
// Constructor & destructor
explicit TaskMultiProcessor(const TaskProcessorConfig& cfg = {});
~TaskMultiProcessor();

// Configuration
void UpdateConfig(const TaskProcessorConfig& cfg);
void SetThreadAffinity(AffinityFn fn);
void SetFinalCallback(FinalCallback cb);
void RegisterCallback(const Callback &cb);

// Submit Task APIs
enum { FIFO=0, RoundRobin=1, WorkStealing=2 };
void Submit(const T& item);
bool SubmitWithTimeout(const T& item, Time deadline);
bool SubmitWithTimeout(const T& item, Time deadline, shared_ptr<atomic<bool>> token);
template<typename R> std::future<R> SubmitWithResult(const T& item, function<R(const T&)> fn);
template<typename R> std::future<R> SubmitWithResult(const T& item, function<R(const T&)> fn, shared_ptr<atomic<bool>> token);

// Metrics
size_t GetQueueSize() const;
size_t GetDroppedCount();
size_t GetHighWaterMark() const;
size_t GetProcessedCount() const;
size_t GetStolenCount() const;
```

---

## 🌐 Real-World Examples

**1. Serialized Logging (FIFO + single thread)**

```cpp
struct LogMessage { int level; std::string text; Time ts; };
TaskProcessorConfig cfg;
cfg.id = "Logger";
cfg.mode = ScheduleMode::FIFO;
cfg.num_threads = 1;
TaskMultiProcessor<LogMessage> logger(cfg);
logger.RegisterCallback([](const LogMessage &m){ write_to_disk(m); });
logger.Submit({1, "App started", Now()});
```

**2. Video Transcoding (Round-Robin)**

```cpp
struct Frame { int id; /* data */ };
TaskProcessorConfig cfg;
cfg.id = "Transcoder";
cfg.mode = ScheduleMode::RoundRobin;
cfg.num_threads = 4;
TaskMultiProcessor<Frame> trans(cfg);
trans.RegisterCallback([](const Frame& f){ encode_frame(f); });
for(auto& f: frames) trans.Submit(f);
```

**3. Web Crawling (Work-Stealing)**

```cpp
struct URLTask { std::string url; int depth; };
TaskProcessorConfig cfg;
cfg.id = "Crawler";
cfg.mode = ScheduleMode::WorkStealing;
cfg.num_threads = 8;
cfg.steal_policy = [](auto &sizes){
  return distance(sizes.begin(), max_element(sizes.begin(), sizes.end()));
};
TaskMultiProcessor<URLTask> crawler(cfg);
crawler.RegisterCallback([](const URLTask &u){ fetch_and_parse(u.url); });
crawler.Submit({"https://example.com",0});
```

**4. Timeouts & Cancellation**

```cpp
auto token = make_shared<atomic<bool>>(false);
bool ok = crawler.SubmitWithTimeout(task, Now()+Milliseconds(2000), token);
if(!ok) log_warn("Dropped or canceled");

auto fut = crawler.SubmitWithResult<int>(task, computeFn, token);
if(fut.wait_for(2s)==future_status::timeout) token->store(true);
```

---

## 📄 License

MIT License. See [LICENSE](LICENSE).
