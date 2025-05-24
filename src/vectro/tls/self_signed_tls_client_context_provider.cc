#include "vectro/tls/self_signed_tls_client_context_provider.h"

#include <iostream>

namespace vectro {
namespace tls {
SelfSignedTlsClientContextProvider::SelfSignedTlsClientContextProvider(
    const std::string& trusted_cert_path) {
  try {
    ctx_ =
        std::make_shared<asio::ssl::context>(asio::ssl::context::tlsv12_client);
    ctx_->set_verify_mode(asio::ssl::verify_peer);
    ctx_->load_verify_file(trusted_cert_path);
    ctx_->set_options(asio::ssl::context::default_workarounds);
    std::cout << "[TLS CLIENT] Self-signed certificate trusted from: "
              << trusted_cert_path << "\n";
  } catch (const std::exception& e) {
    std::cerr << "[TLS CLIENT] Failed to load self-signed cert: " << e.what()
              << "\n";
    throw;
  }
}

std::shared_ptr<asio::ssl::context>
SelfSignedTlsClientContextProvider::GetContext() const {
  return ctx_;
}
}  // namespace tls

}  // namespace vectro
