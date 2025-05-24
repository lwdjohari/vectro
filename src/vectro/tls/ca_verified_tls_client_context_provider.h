#pragma once

#include <boost/asio/ssl/context.hpp>
#include <memory>
#include <string>

#include "vectro/tls/tls_client_context_provider.h"

namespace vectro {
namespace tls {
namespace asio = boost::asio;
class CaVerifiedTlsClientContextProvider : public TlsClientContextProvider {
 public:
  explicit CaVerifiedTlsClientContextProvider(
      const std::string& ca_bundle_path);

  std::shared_ptr<asio::ssl::context> GetContext() const override;

 private:
  std::shared_ptr<asio::ssl::context> ctx_;
};
}  // namespace tls
}  // namespace vectro
