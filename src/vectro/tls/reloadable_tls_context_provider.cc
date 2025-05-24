#include "vectro/tls/reloadable_tls_context_provider.h"

#include <fstream>
#include <iostream>

namespace vectro {
namespace tls {

ReloadableTlsContextProvider::ReloadableTlsContextProvider(
    std::string cert_path, std::string key_path)
                : cert_path_(std::move(cert_path)),
                  key_path_(std::move(key_path)),
                  initialized_(false) {
  ReloadContext();
}

std::shared_ptr<asio::ssl::context> ReloadableTlsContextProvider::GetContext()
    const {
  std::lock_guard<std::mutex> lock(mutex_);
  return current_;
}

bool ReloadableTlsContextProvider::ReloadContext() {
  try {
    auto ctx =
        std::make_shared<asio::ssl::context>(asio::ssl::context::tlsv12_server);
    ctx->use_certificate_chain_file(cert_path_);
    ctx->use_private_key_file(key_path_, asio::ssl::context::pem);
    ctx->set_options(asio::ssl::context::default_workarounds);

    std::lock_guard<std::mutex> lock(mutex_);
    current_ = ctx;
    initialized_ = true;

    std::cout << "[TLS] Reloaded cert/key successfully.\n";
    return true;
  } catch (const std::exception& e) {
    std::cerr << "[TLS] Reload failed: " << e.what() << "\n";
    if (!initialized_) {
      throw;
    }
    return false;
  }
}

void ReloadableTlsContextProvider::StartReloadTimer(
    asio::io_context& io, std::chrono::seconds interval) {
  auto timer = std::make_shared<asio::steady_timer>(io, interval);
  auto self = this;

  std::function<void()> schedule = [timer, self, interval, &io,
                                    schedule]() mutable {
    timer->async_wait([timer, self, interval, &io,
                       schedule](const std::error_code& ec) mutable {
      if (!ec)
        self->ReloadContext();
      timer->expires_after(interval);
      schedule();
    });
  };

  schedule();
}

void ReloadableTlsContextProvider::StopReloadTimer() {}
}  // namespace tls

}  // namespace vectro
