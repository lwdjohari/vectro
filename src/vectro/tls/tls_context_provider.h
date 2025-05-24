#pragma once

#include <boost/asio/ssl/context.hpp>
#include <memory>

namespace vectro {
namespace tls {
namespace asio = boost::asio;
class TlsContextProvider {
 public:
  virtual std::shared_ptr<asio::ssl::context> GetContext() const = 0;
  virtual ~TlsContextProvider() = default;
};
}  // namespace tls
}  // namespace vectro
