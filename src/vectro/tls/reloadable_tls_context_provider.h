#pragma once

#include <boost/asio/ssl/context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>

#include "vectro/tls/tls_context_provider.h"

namespace vectro {
namespace tls {

namespace asio = boost::asio;

class ReloadableTlsContextProvider : public TlsContextProvider {
 public:
  ReloadableTlsContextProvider(std::string cert_path, std::string key_path);

  void StartReloadTimer(asio::io_context& io, std::chrono::seconds interval);
  void StopReloadTimer();
  std::shared_ptr<asio::ssl::context> GetContext() const override;

 private:
  bool ReloadContext();
  // std::shared_ptr<asio::steady_timer> timer_;
  const std::string cert_path_;
  const std::string key_path_;

  mutable std::mutex mutex_;
  std::shared_ptr<asio::ssl::context> current_;
  std::atomic<bool> initialized_;
};
}  // namespace tls
}  // namespace vectro
