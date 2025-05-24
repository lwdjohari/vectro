#pragma once

#include <boost/asio/ssl/context.hpp>
#include <memory>

#include "vectro/tls/tls_client_context_provider.h"

namespace vectro {
namespace tls {
namespace asio = boost::asio;
class DefaultTlsClientContextProvider : public TlsClientContextProvider {
 public:
  DefaultTlsClientContextProvider();

  std::shared_ptr<asio::ssl::context> GetContext() const override;

 private:
  std::shared_ptr<asio::ssl::context> ctx_;
};
}  // namespace tls

}  // namespace vectro
