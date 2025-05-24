#include "vectro/tls/default_tls_client_context_provider.h"

#include <iostream>

namespace vectro {
namespace tls {
DefaultTlsClientContextProvider::DefaultTlsClientContextProvider() {
  try {
    ctx_ =
        std::make_shared<asio::ssl::context>(asio::ssl::context::tlsv12_client);
    ctx_->set_verify_mode(asio::ssl::verify_none);
    ctx_->set_options(asio::ssl::context::default_workarounds);
    std::cout << "[TLS CLIENT] Default context initialized with verify_none.\n";
  } catch (const std::exception& e) {
    std::cerr << "[TLS CLIENT] Failed to initialize default context: "
              << e.what() << "\n";
    throw;
  }
}

std::shared_ptr<asio::ssl::context>
DefaultTlsClientContextProvider::GetContext() const {
  return ctx_;
}
}  // namespace tls
}  // namespace vectro
