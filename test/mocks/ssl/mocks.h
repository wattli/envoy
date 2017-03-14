#pragma once

#include "envoy/ssl/connection.h"
#include "envoy/ssl/context.h"
#include "envoy/ssl/context_config.h"
#include "envoy/ssl/context_manager.h"
#include "envoy/stats/stats.h"

namespace Ssl {

class MockContextManager : public ContextManager {
public:
  MockContextManager();
  ~MockContextManager();

  ClientContextPtr createSslClientContext(Stats::Scope& scope, ContextConfig& config) override {
    return ClientContextPtr{createSslClientContext_(scope, config)};
  }

  ServerContextPtr createSslServerContext(Stats::Scope& scope, ContextConfig& config) override {
    return ServerContextPtr{createSslServerContext_(scope, config)};
  }

  MOCK_METHOD2(createSslClientContext_, ClientContext*(Stats::Scope& scope, ContextConfig& config));
  MOCK_METHOD2(createSslServerContext_, ServerContext*(Stats::Scope& stats, ContextConfig& config));
  MOCK_METHOD0(daysUntilFirstCertExpires, size_t());
  MOCK_METHOD1(iterateContexts, void(std::function<void(Context&)> callback));
};

class MockConnection : public Connection {
public:
  MockConnection();
  ~MockConnection();

  MOCK_METHOD0(sha256PeerCertificateDigest, std::string());
  MOCK_METHOD0(uriSanPeerCertificate, std::string());
};

class MockClientContext : public ClientContext {
public:
  MockClientContext();
  ~MockClientContext();

  MOCK_METHOD0(daysUntilFirstCertExpires, size_t());
  MOCK_METHOD0(getCaCertInformation, std::string());
  MOCK_METHOD0(getCertChainInformation, std::string());
};

} // Ssl
