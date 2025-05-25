#pragma once

#include <boost/asio/ssl/stream.hpp>
#include <type_traits>

namespace vectro {
namespace tls {
namespace detail {

// Primary template: not a TLS stream
template <typename T>
struct is_tls_stream : std::false_type {};

// Specialization for Boost.Asio SSL streams
template <typename NextLayer>
struct is_tls_stream<boost::asio::ssl::stream<NextLayer>> : std::true_type {};

}  // namespace detail

// Public alias and variable template
template <typename T>
constexpr bool is_tls_stream_v = detail::is_tls_stream<T>::value;

}  // namespace tls
}  // namespace vectro