# Vectro
Vector Protocol that have meaning to be 
**Unified async networking for C++17 — TCP, TLS, WebSocket, UDP. One library.**

> High-performance, plugin-friendly, zero-copy networking devolop to survive production at scale.

> [!WARNING]
> Currently under heavy development, do not use for prod yet.<br/>
> Status : WIP, Experimental & Unstable.
>
---

## What is Vectro?

Vectro is a production-grade networking framework for **C++17**, offering:

- Unified session handling for **TCP**, **TLS**, **WebSocket**, and **UDP**
- **Zero-copy message buffers** using allocator-aware design
- Plugin-driven lifecycle: intercept, tag, rate limit, heartbeat
- Backpressure control, graceful/force shutdown, timeouts
- TLS-ready
- TCP Server & Client
- WebSocket (WS/WSS) Server & Client
- UDP with pseudo-session support

---

## Feature Matrix

### 1. Transport Support

| Feature       | TCP | TLS | WebSocket | UDP |
|---------------|-----|-----|-----------|-----|
| Async I/O     | ✅  | ✅  | ✅        | ✅  |
| Multi-port    | ✅  | ✅  | ✅        | ✅  |
| Session object| ✅  | ✅  | ✅        | ✅ (pseudo) |
| TLS Support   | —   | ✅  | ✅ (WSS)  | (DTLS planned) |

### 2. Buffer & Message Layer

| Capability                   | Status | Notes |
|------------------------------|--------|-------|
| Zero-copy RawBuffer          | ✅     | Shared + allocator-aware |
| Framer pluggability          | ✅     | TCP, WS, UDP supported |
| InternalMessage abstraction  | ✅     | `trace_id`, `route_hint`, payload |
| Metadata tagging             | ✅     | Flexible per message |

### 3. Session Lifecycle

| Lifecycle Feature            | Status |
|------------------------------|--------|
| Graceful shutdown            | ✅     |
| Force close                  | ✅     |
| Draining mode                | ✅     |
| Accept timeout               | ✅     |
| Read/write timeouts          | ✅     |
| Idle session expiry          | ✅     |
| Disconnection detection      | ✅     |

### 4. Plugin System

| Plugin Type       | Purpose                        | Scope |
|-------------------|---------------------------------|-------|
| Interceptor       | Drop/modify outbound message   | Session/Controller |
| HeartbeatEmitter  | Periodic pings                 | Session |
| RateLimiter       | Custom throttle logic          | Session/Controller |
| OverloadShedder   | Backpressure-aware rejection   | Controller |
| SessionTagger     | ACL/priority labeling          | On connect |
| ControllerHook    | Metrics/alert hooks            | Global |

### 5. Observability

| Feature                    | Status |
|----------------------------|--------|
| Per-message trace ID       | ✅     |
| Tagged sessions            | ✅     |
| Session registry           | ✅     |
| Structured logs via plugin | ✅     |
| Metrics export readiness   | ✅     |
| CLI, Prometheus (planned)  | 🔄     |

### 6. Real-World Readiness

| Capability                 | Status |
|----------------------------|--------|
| Long-running client/server | ✅     |
| Allocator integration      | ✅     |
| TLS cert reload            | ✅     |
| Plugin (Pluggable Module)  | ✅     |
| Scoped backpressure toggle | ✅     |

---

## Architecture

```
[ Accept Loop (multi-port) ]
        |
     [ Session<T> ]
        |
   [ FramerBase<T> ]
        |
  [ InternalMessage ]
        |
  [ PluginBundle ]
        |
   [ Controller ]
        |
  [ Router -> Broadcast / Targeted ]
```

---

## Who Should Use This?

- **Enterprise**: building real-time APIs, gateways, platform or session routers
- **Fintech**: needing secure TLS transport with timeout and backpressure
- **IoT platforms**: hi-number of devices and multiple protocols
- **Game servers**: high-frequency, low-latency transport across multiple clients
- **Structured session lifecycle, plugin control, and raw message access**

---

## Build & Run

### Requirements

- C++17 compiler
- CMake 3.14+
- Boost Asio (Header Only)
- Boost Beast (Header Only)
- OpenSSL
- Abseil

### Git Submodule

```bash
git submodule https://github.com/lwdjohari/vectro.git third_party/vectro
cd third_party/vectro
git fetch --tags
git checkout <TAGS>
git submodule update --init --depth 1
```

### CMAKE Integration

```cmake
cmake_minimum_required(VERSION 3.14)

# change this project name to your project.
project(PROJECT CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(third_party/vectro build-vectro)

add_executable(PROJECT src/main.cc)
target_link_libraries(PROJECT
    PRIVATE
        vectro::vectro
    )
target_compile_features(PROJECT PUBLIC cxx_std_17)
```

---

## Usage Examples

### TCP Server

```cpp
auto controller = std::make_shared<Controller<TcpSession>>(io.get_executor());
auto server = std::make_shared<VtTcpServer<TcpStream>>(io, controller, LengthPrefixedFramerFactory);

PluginBundle bundle;
bundle.Set(PluginKeys::kInterceptor, std::make_shared<MyInterceptor>());
bundle.Set(PluginKeys::kHeartbeatEmitter, std::make_shared<MyHeartbeat>());

server->AddPort(1883, bundle);
server->Start();
```

### WebSocket Server

```cpp
asio::ssl::context ssl_ctx(asio::ssl::context::tlsv12);
ssl_ctx.use_certificate_chain_file("cert.pem");
ssl_ctx.use_private_key_file("key.pem", asio::ssl::context::pem);

auto controller = std::make_shared<Controller<WsTlsSession>>(io.get_executor());
auto server = std::make_shared<VtTcpServer<WsTlsStream>>(io, controller, WebSocketFramerFactory);

PluginBundle bundle;
bundle.Set(PluginKeys::kInterceptor, std::make_shared<MyLogger>());
bundle.Set(PluginKeys::kHeartbeatEmitter, std::make_shared<MyPinger>());

server->AddPort(8443, bundle);
server->Start();
```


---

## TLS Context Providers in Vectro

### What Are TLS Context Providers?

TLS Context Providers are **pluggable modules** in Vectro that give you full control over how TLS (`asio::ssl::context`) is configured, loaded, and refreshed — without scattering TLS logic across your codebase.

They let you:

- Reload server certificates without restarting
- Plug in CA-based trust on clients
- Support self-signed certs (safely!)
- Replace `verify_none` with real security when needed

---

### Why They Exist

#### The problem in most C++ networking code:

| Issue | Result |
|-------|--------|
| Hardcoded `asio::ssl::context` | TLS config tightly coupled to sockets |
| No reload logic | Must restart server to renew certs |
| No trust customization | Can’t switch between CA and pinned certs |
| TLS setup copy-pasted | Prone to mistakes and inconsistency |
| Testing TLS behavior is painful | Context can't be swapped or injected |

**Vectro introduces TLS Context Providers to solve all of this with one unified architecture.**

---

### Two Types of Providers

| Role | Interface | Usage |
|------|-----------|-------|
| Server | `TlsContextProvider` | Used when accepting secure (TLS/WSS) connections |
| Client | `TlsClientContextProvider` | Used when connecting outbound with secure clients |

---

### Server: `TlsContextProvider`

#### Interface

```cpp
class TlsContextProvider {
 public:
  virtual std::shared_ptr<asio::ssl::context> GetContext() const = 0;
  virtual ~TlsContextProvider() = default;
};
```

#### Built-in Implementation: Reloadable TLS with Let's Encrypt

```cpp
auto tls = std::make_shared<DefaultReloadableTlsContextProvider>(
    "fullchain.pem", "privkey.pem"  // Let's Encrypt live cert files
);

tls->StartReloadTimer(io, std::chrono::seconds(60));  // Reload every 60s

VtTcpServer<WsTlsStream> server(io, controller, WebSocketFramerFactory);
server.AddPort(443, plugin_bundle, tls);  // WSS server
```

**Why this works:**
- Let's Encrypt auto-renew tools like `certbot` or `lego` update cert files
- Your server re-reads them every 60 seconds with no downtime
- No restart needed, no broken sockets

---

### Client: `TlsClientContextProvider`

#### Interface

```cpp
class TlsClientContextProvider {
 public:
  virtual std::shared_ptr<asio::ssl::context> GetContext() const = 0;
  virtual ~TlsClientContextProvider() = default;
};
```

---

### Use Case #1: CA-Based Trust (Real TLS, public servers)

You’re connecting to `mqtt.example.com` or `api.acme.org` — secured by a certificate from Let's Encrypt or DigiCert.

**Use this:**

```cpp
auto tls = std::make_shared<CaVerifiedTlsClientContextProvider>("mozilla-ca.pem");

auto client = std::make_shared<VtTcpClient<TlsStream>>(
    io, "secure-client", "mqtt.example.com", 8883, LengthPrefixedFramerFactory, tls);
```

- `mozilla-ca.pem` contains root CA certs from Firefox or your OS
- Verifies that the server is trusted and domain name matches

**If Possible, Do not use `verify_none` in production!**

---

### Use Case #2: Self-Signed Certificate Trust

You're connecting to an **internal server** using a self-signed certificate:

- You control the server
- It's not public or CA-issued
- You don’t want `verify_none`

**Use this:**

```cpp
auto tls = std::make_shared<SelfSignedTlsClientContextProvider>("internal-server.pem");

auto client = std::make_shared<VtTcpClient<WsTlsStream>>(
    io, "internal-wss", "10.0.0.5", 443, WebSocketFramerFactory, tls);
```

**Why this is safe:**
- You trust only that pinned certificate
- Man-in-the-middle is rejected
- Still supports `verify_peer` mode

---

### Use Case #3: Dev Environment (Insecure)

You just want TLS transport without validation (e.g. test server with `openssl req` cert).

**Use this:**

```cpp
auto tls = std::make_shared<DefaultTlsClientContextProvider>();  // verify_none

auto client = std::make_shared<VtTcpClient<WsTlsStream>>(
    io, "dev-client", "localhost", 8443, WebSocketFramerFactory, tls);
```

**When to use:**
- Local testing
- You control both ends
- Never use in production or on public networks

---

### Custom Providers (Bring Your Own Trust Model)

You can implement `TlsContextProvider` or `TlsClientContextProvider` to:
- Load certs from Vault or TPM
- Enforce mTLS policies
- Integrate with enterprise PKI

Example:

```cpp
class VaultClientTlsProvider : public TlsClientContextProvider {
 public:
  std::shared_ptr<asio::ssl::context> GetContext() const override {
    auto ctx = std::make_shared<asio::ssl::context>(asio::ssl::context::tlsv13_client);
    ctx->load_verify_file(FetchCertFromVault("trusted.pem"));
    ctx->set_verify_mode(asio::ssl::verify_peer);
    return ctx;
  }
};
```

---

### Summary: When to Use What `TlsProvider`

| Scenario | Use This Provider |
|----------|-------------------|
| Local dev / testing | `DefaultTlsClientContextProvider` |
| Secure production server | `CaVerifiedTlsClientContextProvider("ca.pem")` |
| Internal infra with pinned cert | `SelfSignedTlsClientContextProvider("cert.pem")` |
| Server with auto-renewing certs | `DefaultReloadableTlsContextProvider("fullchain.pem", "privkey.pem")` |
| Enterprise TLS control | Implement custom provider |

---

## `TlsProvider` Final Word

TLS is the baseline of secure systems.

Vectro gives you a zero-pain way to handle:
- Certificate loading
- Live reloading
- Peer trust
- Flexibility to grow

> TLS is no longer the part of your stack you fear. It's modular, testable, and production-safe.

---

## License

MIT License © 2025 Linggawasistha Djohari

> Focus to what matters. Connect with control. Deploy with power.