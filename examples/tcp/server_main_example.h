// main.cpp

#include <asio.hpp>
#include <asio/ssl.hpp>

#include "absl/time/time.h"
#include "vectro/controller.h"
#include "vectro/frame/simple_line_framer.h"
#include "vectro/tcp/tcp_server.h"
#include "vectro/tls/default_tls_server_context_provider.h"

using namespace vectro;
using namespace vectro::tcp;
using namespace vectro::frame;
using asio_t   = asio::ip::tcp;
namespace asio = asio;

int main() {
  // 1) io_context drives both acceptors and all Sessions
  asio::io_context io{std::thread::hardware_concurrency()};

  // 2) Create Controller with 4 channels for business logic
  auto controller =
      std::make_shared<Controller<Session<asio::ip::tcp::socket>>>(
          io.get_executor(), /*num_channels=*/4);

  // 3) Install simple echo router
  struct EchoRouter : public IMessageRouter<Session<asio_t::socket>> {
    void Route(InternalMessage msg) override {
      // Echo the received line back to the same session
      auto session_id         = msg.meta().connection_id;
      msg.meta().dest_session = session_id;
      // copy payload back
      session->AsyncSend(std::make_shared<InternalMessage>(std::move(msg)));
    }
  };
  controller->SetRouter(std::make_shared<EchoRouter>());

  // 4) Hook in simple metrics/logging
  controller->OnInbound = [](const auto& msg) {
    // TODO: metrics: ++inbound_counter
    return true;
  };
  controller->OnProcess = [](const auto& msg) {
    // TODO: metrics: record processing start time
  };
  controller->OnOutbound = [](const auto& msg) {
    // TODO: metrics: ++outbound_counter
  };

  // 5) Build TCP server with both plaintext and TLS ports
  tcp::Server<asio_t::socket> server_plain{
      io, controller,
      /*framer_factory=*/
      []() {
        return std::make_unique<SimpleLineFramer>();
      }};
  tcp::Server<asio::ssl::stream<asio_t::socket>> server_tls{
      io, controller,
      /*framer_factory=*/
      []() {
        return std::make_unique<SimpleLineFramer>();
      }};

  // 6) Add a plaintext listener on port 9000
  server_plain.AddPort(9000, PluginBundle{});

  // 7) Add a TLS listener on port 9443
  auto tls_provider = std::make_shared<DefaultTlsServerContextProvider>();
  server_tls.AddPort(9443, PluginBundle{}, tls_provider);

  // 8) Start both servers
  server_plain.Start();
  server_tls.Start();

  // 9) Handle SIGINT for graceful shutdown
  asio::signal_set signals(io, SIGINT, SIGTERM);
  signals.async_wait([&](auto, auto) {
    // Stop acceptors & drain sessions gracefully
    server_plain.Stop(/*force=*/false);
    server_tls.Stop(/*force=*/false);
    io.stop();
  });

  // 10) Run the I/O loop
  io.run();
  return 0;
}
