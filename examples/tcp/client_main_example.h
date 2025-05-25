#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <iostream>
#include <stdexcept>
#include "absl/time/time.h"
#include "vectro/frame/simple_line_framer.h"
#include "vectro/tcp/tcp_client.h"
#include "vectro/tls/default_tls_client_context_provider.h"

using namespace vectro;
using namespace vectro::tcp;
using namespace vectro::frame;
namespace asio = boost::asio;

int main() {
  // 1) Create an IO context and run it on a background thread
  asio::io_context io{2};
  std::thread io_thread([&] { io.run(); });

  // 2) Configure the client timeouts and queue size
  ClientConfig cfg;
  cfg.connect_timeout = absl::Seconds(5);  // fail if no connect in 5s
  cfg.read_timeout = absl::Seconds(20);    // disconnect if no data in 20s
  cfg.idle_timeout = absl::Seconds(60);    // close if idle 60s
  cfg.max_queue_size = 2048;               // backpressure threshold

  // 3) Prepare a TLS context provider for secure streams
  auto tls_provider = std::make_shared<tls::DefaultTlsClientContextProvider>();

  // 4) Instantiate a TLS client using our generic template
  //    Stream = asio::ssl::stream<asio::ip::tcp::socket>
  auto client =
      std::make_shared<TcpClient<asio::ssl::stream<asio::ip::tcp::socket>>>(
          io.get_executor(),
          // supply a factory that creates a new SimpleLineFramer per connection
          []() { return std::make_unique<SimpleLineFramer>(); }, cfg,
          plugin::PluginBundle{},  // no extra plugins for now
          tls_provider     // required for is_tls_stream_v<Stream>
      );

  // 5) Handle unsolicited messages (e.g. server pushes)
  client->SetOnMessage([](auto msg) {
    // msg->buffer().data() / size() give you the raw bytes
    std::string line(reinterpret_cast<char*>(msg->buffer().data()),
                     msg->buffer().size());
    std::cout << "[Server] " << line << "\n";
  });

  // 6) Handle disconnects and attempt reconnect
  std::function<void()> do_connect;
  client->SetOnDisconnect([&](auto ec) {
    std::cerr << "Disconnected: " << ec.message() << "; retrying in 3s\n";
    // schedule reconnect after 3s
    asio::steady_timer t(io.get_executor());
    t.expires_after(std::chrono::seconds(3));
    t.async_wait([&](auto) { do_connect(); });
  });

  // 7) Connect (initial attempt)
  do_connect = [&]() {
    client->Connect("example.com", 443, [&](auto ec) {
      if (ec) {
        std::cerr << "Connect failed: " << ec.message() << "\n";
      } else {
        std::cout << "Connected to example.com:443\n";
      }
    });
  };
  do_connect();

  // 8) Send periodic heartbeats
  asio::steady_timer hb_timer(io.get_executor());
  std::function<void()> send_heartbeat = [&]() {
    auto msg = std::make_shared<InternalMessage>();
    // Fill metadata if needed: msg->Meta().connection_id = ...
    // Write payload directly into RawBuffer via a small helper:
    auto& rb = msg->buffer();
    const char* text = "PING\n";
    std::memcpy(rb.data(), text, std::strlen(text));
    rb = RawBuffer(rb.data(), std::strlen(text), rb.deleter());  // adjust size
    client->Send(msg);

    hb_timer.expires_after(std::chrono::seconds(30));
    hb_timer.async_wait([&](auto) { send_heartbeat(); });
  };
  send_heartbeat();

  // 9) Keep main thread alive until user hits ENTER
  std::cout << "Press ENTER to quit...\n";
  std::string dummy;
  std::getline(std::cin, dummy);

  // 10) Gracefully shutdown
  client->Close(false);
  io.stop();
  io_thread.join();
  return 0;
}
