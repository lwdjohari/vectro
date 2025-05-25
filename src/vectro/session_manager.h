#pragma once
#include <absl/container/node_hash_map.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace vectro {

// SessionManager: thread-safe registry of active sessions
// Features to add for production readiness:
// 1. Graceful & Force Drain: ability to signal all sessions to close gracefully
// or forcefully
// 2. Automatic Removal: unregister sessions upon their own shutdown callback
// 3. Counters & Gauges: metrics for adds, removes, current count, and lifecycle
// events
// 4. Error Handling & Fault Tolerance: robust handling of lookup and removal
// errors

template <typename Session>
class SessionManager {
 public:
  using SessionPtr = std::shared_ptr<Session>;

  // Add a new session
  // TODO: emit counter "sessions_added"
  void Add(uint64_t id, SessionPtr session) {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.emplace(id, std::move(session));
  }

  // Remove a session by ID
  // TODO: emit counter "sessions_removed"
  void Remove(uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.erase(id);
  }

  // Lookup a session; returns nullptr if not found
  // TODO: log warning on lookup miss for debugging
  SessionPtr Get(uint64_t id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(id);
    return it != sessions_.end() ? it->second : nullptr;
  }

  // Retrieve a snapshot of all sessions
  std::vector<SessionPtr> All() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SessionPtr> result;
    result.reserve(sessions_.size());
    for (const auto& kv : sessions_) {
      result.push_back(kv.second);
    }
    return result;
  }

  // SessionManager<MySession> mgr = /*…*/;

  // // Graceful drain: signal each session to close without dropping in-flight
  // work mgr.ForEach([](const std::shared_ptr<MySession>& session) {
  // session->Close(false);
  // });

  // mgr.ForEach([](const std::shared_ptr<MySession>& session) {
  // session->Close(true);
  // });
  // mgr.RemoveAll();  // if you add a RemoveAll helper, or call DrainForce()

  // mgr.ForEach([](const auto& session) {
  // LOG(INFO) << "Session " << session->id() << " is active";
  // });

  // Iterator-based access: apply a function to each session under lock without
  // creating a vector
  template <typename Fn>
  void ForEach(Fn&& fn) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& kv : sessions_) {
      fn(kv.second);
    }
  }

  // Current number of active sessions
  // TODO: expose as gauge in metrics system
  std::size_t Count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sessions_.size();
  }

  // Graceful drain: signal all sessions to close without dropping in-flight
  // work
  // TODO: implement by invoking session->Close(false) on each session
  void DrainGraceful() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& kv : sessions_) {
      auto& session = kv.second;
      if (session)
        session->Close(false);
    }
  }

  // Forceful drain: immediately cancel all sessions
  // TODO: implement by invoking session->Close(true) on each session
  void DrainForce() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& kv : sessions_) {
      auto& session = kv.second;
      if (session)
        session->Close(true);
    }
    sessions_.clear();
  }

 private:
  mutable std::mutex mutex_;  // protects sessions_
  absl::node_hash_map<uint64_t, SessionPtr> sessions_;
};

}  // namespace vectro
