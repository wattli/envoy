#include "integration.h"
#include "ssl_integration_test.h"
#include "utility.h"

#include "common/event/dispatcher_impl.h"
#include "common/network/utility.h"
#include "common/ssl/context_config_impl.h"
#include "common/ssl/context_manager_impl.h"

using testing::Return;

namespace Ssl {

std::unique_ptr<Runtime::Loader> SslIntegrationTest::runtime_;
std::unique_ptr<ContextManager> SslIntegrationTest::context_manager_;
ServerContextPtr SslIntegrationTest::upstream_ssl_ctx_;
ClientContextPtr SslIntegrationTest::client_ssl_ctx_alpn_;
ClientContextPtr SslIntegrationTest::client_ssl_ctx_no_alpn_;

void SslIntegrationTest::SetUpTestCase() {
  test_server_ =
      MockRuntimeIntegrationTestServer::create("test/config/integration/server_ssl.json");
  context_manager_.reset(new ContextManagerImpl(*runtime_));
  upstream_ssl_ctx_ = createUpstreamSslContext();
  client_ssl_ctx_alpn_ = createClientSslContext(true);
  client_ssl_ctx_no_alpn_ = createClientSslContext(false);
  fake_upstreams_.emplace_back(
      new FakeUpstream(upstream_ssl_ctx_.get(), 11000, FakeHttpConnection::Type::HTTP1));
  fake_upstreams_.emplace_back(
      new FakeUpstream(upstream_ssl_ctx_.get(), 11001, FakeHttpConnection::Type::HTTP1));
}

void SslIntegrationTest::TearDownTestCase() {
  test_server_.reset();
  fake_upstreams_.clear();
  upstream_ssl_ctx_.reset();
  client_ssl_ctx_alpn_.reset();
  client_ssl_ctx_no_alpn_.reset();
  context_manager_.reset();
}

ServerContextPtr SslIntegrationTest::createUpstreamSslContext() {
  std::string json = R"EOF(
{
  "cert_chain_file": "test/config/integration/certs/upstreamcert.pem",
  "private_key_file": "test/config/integration/certs/upstreamkey.pem"
}
)EOF";

  Json::ObjectPtr loader = Json::Factory::LoadFromString(json);
  ContextConfigImpl cfg(*loader);
  return context_manager_->createSslServerContext(test_server_->store(), cfg);
}

ClientContextPtr SslIntegrationTest::createClientSslContext(bool alpn) {
  std::string json_no_alpn = R"EOF(
{
  "ca_cert_file": "test/config/integration/certs/cacert.pem",
  "cert_chain_file": "test/config/integration/certs/clientcert.pem",
  "private_key_file": "test/config/integration/certs/clientkey.pem"
}
)EOF";

  std::string json_alpn = R"EOF(
{
  "ca_cert_file": "test/config/integration/certs/cacert.pem",
  "cert_chain_file": "test/config/integration/certs/clientcert.pem",
  "private_key_file": "test/config/integration/certs/clientkey.pem",
  "alpn_protocols": "h2,http/1.1"
}
)EOF";

  Json::ObjectPtr loader = Json::Factory::LoadFromString(alpn ? json_alpn : json_no_alpn);
  ContextConfigImpl cfg(*loader);
  return context_manager_->createSslClientContext(test_server_->store(), cfg);
}

Network::ClientConnectionPtr SslIntegrationTest::makeSslClientConnection(bool alpn) {
  return dispatcher_->createSslClientConnection(
      alpn ? *client_ssl_ctx_alpn_ : *client_ssl_ctx_no_alpn_,
      Network::Utility::resolveUrl("tcp://127.0.0.1:10001"));
}

void SslIntegrationTest::checkStats() {
  Stats::Counter& counter =
      test_server_->store().counter("listener.tcp://127.0.0.1:10001.ssl.handshake");
  EXPECT_EQ(1U, counter.value());
  counter.reset();
}

TEST_F(SslIntegrationTest, RouterRequestAndResponseWithGiantBodyBuffer) {
  testRouterRequestAndResponseWithBody(makeSslClientConnection(false),
                                       Http::CodecClient::Type::HTTP1, 16 * 1024 * 1024,
                                       16 * 1024 * 1024, false);
  checkStats();
}

TEST_F(SslIntegrationTest, RouterRequestAndResponseWithBodyNoBuffer) {
  testRouterRequestAndResponseWithBody(makeSslClientConnection(false),
                                       Http::CodecClient::Type::HTTP1, 1024, 512, false);
  checkStats();
}

TEST_F(SslIntegrationTest, RouterRequestAndResponseWithBodyNoBufferHttp2) {
  testRouterRequestAndResponseWithBody(makeSslClientConnection(true),
                                       Http::CodecClient::Type::HTTP2, 1024, 512, false);
  checkStats();
}

TEST_F(SslIntegrationTest, RouterHeaderOnlyRequestAndResponse) {
  testRouterHeaderOnlyRequestAndResponse(makeSslClientConnection(false),
                                         Http::CodecClient::Type::HTTP1);
  checkStats();
}

TEST_F(SslIntegrationTest, RouterUpstreamDisconnectBeforeResponseComplete) {
  testRouterUpstreamDisconnectBeforeResponseComplete(makeSslClientConnection(false),
                                                     Http::CodecClient::Type::HTTP1);
  checkStats();
}

TEST_F(SslIntegrationTest, RouterDownstreamDisconnectBeforeRequestComplete) {
  testRouterDownstreamDisconnectBeforeRequestComplete(makeSslClientConnection(false),
                                                      Http::CodecClient::Type::HTTP1);
  checkStats();
}

TEST_F(SslIntegrationTest, RouterDownstreamDisconnectBeforeResponseComplete) {
  testRouterDownstreamDisconnectBeforeResponseComplete(makeSslClientConnection(false),
                                                       Http::CodecClient::Type::HTTP1);
  checkStats();
}

// This test must be here vs integration_admin_test so that it tests a server with loaded certs.
TEST_F(SslIntegrationTest, AdminCertEndpoint) {
  BufferingStreamDecoderPtr response = IntegrationUtil::makeSingleRequest(
      ADMIN_PORT, "GET", "/certs", "", Http::CodecClient::Type::HTTP1);
  EXPECT_TRUE(response->complete());
  EXPECT_STREQ("200", response->headers().Status()->value().c_str());
}

TEST_F(SslIntegrationTest, AltAlpn) {
  // Connect with ALPN, but we should end up using HTTP/1.
  MockRuntimeIntegrationTestServer* server =
      dynamic_cast<MockRuntimeIntegrationTestServer*>(test_server_.get());
  ON_CALL(server->runtime_->snapshot_, featureEnabled("ssl.alt_alpn", 0))
      .WillByDefault(Return(true));
  testRouterRequestAndResponseWithBody(makeSslClientConnection(true),
                                       Http::CodecClient::Type::HTTP1, 1024, 512, false);
  checkStats();
}

} // Ssl
