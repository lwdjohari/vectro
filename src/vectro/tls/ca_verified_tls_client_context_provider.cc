#include "vectro/tls/ca_verified_tls_client_context_provider.h"

#include <iostream>

namespace vectro {
namespace tls {
CaVerifiedTlsClientContextProvider::CaVerifiedTlsClientContextProvider(
    const std::string& ca_bundle_path) {
  try {
    ctx_ =
        std::make_shared<asio::ssl::context>(asio::ssl::context::tlsv12_client);
    ctx_->set_verify_mode(asio::ssl::verify_peer);
    ctx_->load_verify_file(ca_bundle_path);
    ctx_->set_options(asio::ssl::context::default_workarounds);
    std::cout << "[TLS CLIENT] CA-verified context loaded from: "
              << ca_bundle_path << "\n";
  } catch (const std::exception& e) {
    std::cerr << "[TLS CLIENT] Failed to load CA bundle: " << e.what() << "\n";
    throw;
  }
}

std::shared_ptr<asio::ssl::context>
CaVerifiedTlsClientContextProvider::GetContext() const {
  return ctx_;
}
}  // namespace tls
}  // namespace vectro
