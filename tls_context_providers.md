# TLS Context Providers in Vectro

## What Are TLS Context Providers?

TLS Context Providers are **pluggable modules** in Vectro that give you full control over how TLS (`asio::ssl::context`) is configured, loaded, and refreshed — without scattering TLS logic across your codebase.

They let you:

- Reload server certificates without restarting
- Plug in CA-based trust on clients
- Support self-signed certs (safely!)
- Replace `verify_none` with real security when needed

---

## Why They Exist

### The problem in most C++ networking code:

| Issue | Result |
|-------|--------|
| Hardcoded `asio::ssl::context` | TLS config tightly coupled to sockets |
| No reload logic | Must restart server to renew certs |
| No trust customization | Can’t switch between CA and pinned certs |
| TLS setup copy-pasted | Prone to mistakes and inconsistency |
| Testing TLS behavior is painful | Context can't be swapped or injected |

**Vectro introduces TLS Context Providers to solve all of this with one unified architecture.**

---

## Two Types of Providers

| Role | Interface | Usage |
|------|-----------|-------|
| Server | `TlsContextProvider` | Used when accepting secure (TLS/WSS) connections |
| Client | `TlsClientContextProvider` | Used when connecting outbound with secure clients |

---

## Server: `TlsContextProvider`

### Interface

```cpp
class TlsContextProvider {
 public:
  virtual std::shared_ptr<asio::ssl::context> GetContext() const = 0;
  virtual ~TlsContextProvider() = default;
};
```

### Built-in Implementation: Reloadable TLS with Let's Encrypt

```cpp
auto tls = std::make_shared<DefaultReloadableTlsContextProvider>(
    "fullchain.pem", "privkey.pem"  // Let's Encrypt live cert files
);

tls->StartReloadTimer(io, std::chrono::seconds(60));  // Reload every 60s

TcpServer<WsTlsStream> server(io, controller, WebSocketFramerFactory);
server.AddPort(443, plugin_bundle, tls);  // WSS server
```

**Why this works:**
- Let's Encrypt auto-renew tools like `certbot` or `lego` update cert files
- Your server re-reads them every 60 seconds with no downtime
- No restart needed, no broken sockets

---

## Client: `TlsClientContextProvider`

### Interface

```cpp
class TlsClientContextProvider {
 public:
  virtual std::shared_ptr<asio::ssl::context> GetContext() const = 0;
  virtual ~TlsClientContextProvider() = default;
};
```

---

## Use Case #1: CA-Based Trust (Real TLS, public servers)

You’re connecting to `mqtt.example.com` or `api.acme.org` — secured by a certificate from Let's Encrypt or DigiCert.

**Use this:**

```cpp
auto tls = std::make_shared<CaVerifiedTlsClientContextProvider>("mozilla-ca.pem");

auto client = std::make_shared<TcpClient<TlsStream>>(
    io, "secure-client", "mqtt.example.com", 8883, LengthPrefixedFramerFactory, tls);
```

- `mozilla-ca.pem` contains root CA certs from Firefox or your OS
- Verifies that the server is trusted and domain name matches

**Do not use `verify_none` in production!**

---

## Use Case #2: Self-Signed Certificate Trust

You're connecting to an **internal server** using a self-signed certificate:

- You control the server
- It's not public or CA-issued
- You don’t want `verify_none`

**Use this:**

```cpp
auto tls = std::make_shared<SelfSignedTlsClientContextProvider>("internal-server.pem");

auto client = std::make_shared<TcpClient<WsTlsStream>>(
    io, "internal-wss", "10.0.0.5", 443, WebSocketFramerFactory, tls);
```

**Why this is safe:**
- You trust only that pinned certificate
- Man-in-the-middle is rejected
- Still supports `verify_peer` mode

---

## Use Case #3: Dev Environment (Insecure)

You just want TLS transport without validation (e.g. test server with `openssl req` cert).

**Use this:**

```cpp
auto tls = std::make_shared<DefaultTlsClientContextProvider>();  // verify_none

auto client = std::make_shared<TcpClient<WsTlsStream>>(
    io, "dev-client", "localhost", 8443, WebSocketFramerFactory, tls);
```

**When to use:**
- Local testing
- You control both ends
- Never use in production or on public networks

---

## Custom Providers (Bring Your Own Trust Model)

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

## Summary: When to Use What

| Scenario | Use This Provider |
|----------|-------------------|
| Local dev / testing | `DefaultTlsClientContextProvider` |
| Secure production server | `CaVerifiedTlsClientContextProvider("ca.pem")` |
| Internal infra with pinned cert | `SelfSignedTlsClientContextProvider("cert.pem")` |
| TcpServer with auto-renewing certs | `DefaultReloadableTlsContextProvider("fullchain.pem", "privkey.pem")` |
| Enterprise TLS control | Implement custom provider |

---

## Final Word

TLS is the baseline of secure systems.

Vectro gives you a zero-pain way to handle:
- Certificate loading
- Live reloading
- Peer trust
- Flexibility to grow

> TLS is no longer the part of your stack you fear. It's modular, testable, and production-safe.