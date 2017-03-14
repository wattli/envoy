#include "common/buffer/buffer_impl.h"
#include "common/event/dispatcher_impl.h"
#include "common/json/json_loader.h"
#include "common/network/listen_socket_impl.h"
#include "common/network/utility.h"
#include "common/ssl/connection_impl.h"
#include "common/ssl/context_config_impl.h"
#include "common/ssl/context_impl.h"
#include "common/stats/stats_impl.h"

#include "test/mocks/network/mocks.h"
#include "test/mocks/runtime/mocks.h"
#include "test/mocks/stats/mocks.h"
#include "test/mocks/server/mocks.h"

using testing::_;
using testing::Invoke;

namespace Ssl {

static void testUtil(std::string client_ctx_json, std::string server_ctx_json,
                     std::string expected_digest, std::string expected_uri) {
  Stats::IsolatedStoreImpl stats_store;
  Runtime::MockLoader runtime;

  Json::ObjectPtr server_ctx_loader = Json::Factory::LoadFromString(server_ctx_json);
  ContextConfigImpl server_ctx_config(*server_ctx_loader);
  ContextManagerImpl manager(runtime);
  ServerContextPtr server_ctx(manager.createSslServerContext(stats_store, server_ctx_config));

  Event::DispatcherImpl dispatcher;
  Network::TcpListenSocket socket(uint32_t(10000), true);
  Network::MockListenerCallbacks callbacks;
  Network::MockConnectionHandler connection_handler;
  Network::ListenerPtr listener =
      dispatcher.createSslListener(connection_handler, *server_ctx, socket, callbacks, stats_store,
                                   Network::ListenerOptions::listenerOptionsWithBindToPort());

  Json::ObjectPtr client_ctx_loader = Json::Factory::LoadFromString(client_ctx_json);
  ContextConfigImpl client_ctx_config(*client_ctx_loader);
  ClientContextPtr client_ctx(manager.createSslClientContext(stats_store, client_ctx_config));
  Network::ClientConnectionPtr client_connection = dispatcher.createSslClientConnection(
      *client_ctx, Network::Utility::resolveUrl("tcp://127.0.0.1:10000"));
  client_connection->connect();

  Network::ConnectionPtr server_connection;
  Network::MockConnectionCallbacks server_connection_callbacks;
  EXPECT_CALL(callbacks, onNewConnection_(_))
      .WillOnce(Invoke([&](Network::ConnectionPtr& conn) -> void {
        server_connection = std::move(conn);
        server_connection->addConnectionCallbacks(server_connection_callbacks);
      }));

  EXPECT_CALL(server_connection_callbacks, onEvent(Network::ConnectionEvent::Connected))
      .WillOnce(Invoke([&](uint32_t) -> void {
        EXPECT_EQ(expected_digest, server_connection->ssl()->sha256PeerCertificateDigest());
        EXPECT_EQ(expected_uri, server_connection->ssl()->uriSanPeerCertificate());
        server_connection->close(Network::ConnectionCloseType::NoFlush);
        client_connection->close(Network::ConnectionCloseType::NoFlush);
        dispatcher.exit();
      }));
  EXPECT_CALL(server_connection_callbacks, onEvent(Network::ConnectionEvent::LocalClose));

  dispatcher.run(Event::Dispatcher::RunType::Block);
}

TEST(SslConnectionImplTest, ClientAuth) {
  std::string client_ctx_json = R"EOF(
  {
    "cert_chain_file": "test/common/ssl/test_data/approved_with_uri_san.crt",
    "private_key_file": "test/common/ssl/test_data/private_key_with_uri_san.pem"
  }
  )EOF";

  std::string server_ctx_json = R"EOF(
  {
    "cert_chain_file": "/tmp/envoy_test/unittestcert.pem",
    "private_key_file": "/tmp/envoy_test/unittestkey.pem",
    "ca_cert_file": "test/common/ssl/test_data/ca_with_uri_san.crt"
  }
  )EOF";

  testUtil(client_ctx_json, server_ctx_json,
           "713631e537617511f51a206752038dd42f6b09907f33427735bf7a7114e67756",
           "server1.example.com");

  client_ctx_json = R"EOF(
  {
    "cert_chain_file": "test/common/ssl/test_data/approved_with_dns_san.crt",
    "private_key_file": "test/common/ssl/test_data/private_key_with_dns_san.pem"
  }
  )EOF";

  server_ctx_json = R"EOF(
  {
    "cert_chain_file": "/tmp/envoy_test/unittestcert.pem",
    "private_key_file": "/tmp/envoy_test/unittestkey.pem",
    "ca_cert_file": "test/common/ssl/test_data/ca_with_dns_san.crt"
  }
  )EOF";

  // The SAN field only has DNS, expect "" for uriSanPeerCertificate().
  testUtil(client_ctx_json, server_ctx_json,
           "81c3db064120190839d8854dd70be13175f21ac05535a46fa89ab063ebdca7b3", "");

  client_ctx_json = R"EOF(
  {
    "cert_chain_file": "",
    "private_key_file": ""
  })EOF";

  server_ctx_json = R"EOF(
  {
    "cert_chain_file": "/tmp/envoy_test/unittestcert.pem",
    "private_key_file": "/tmp/envoy_test/unittestkey.pem",
    "ca_cert_file": ""
  }
  )EOF";

  // The SAN field only has DNS, expect "" for uriSanPeerCertificate().
  testUtil(client_ctx_json, server_ctx_json, "", "");

  client_ctx_json = R"EOF(
  {
    "cert_chain_file": "test/common/ssl/test_data/approved.crt",
    "private_key_file": "test/common/ssl/test_data/private_key.pem"
  }
  )EOF";

  server_ctx_json = R"EOF(
  {
    "cert_chain_file": "/tmp/envoy_test/unittestcert.pem",
    "private_key_file": "/tmp/envoy_test/unittestkey.pem",
    "ca_cert_file": "test/common/ssl/test_data/ca.crt"
  }
  )EOF";

  testUtil(client_ctx_json, server_ctx_json,
           "2ff7d57d2e5cb9cc0bfe56727a114de8039cabcc7658715db4e80e1a75e108ed", "");
}

TEST(SslConnectionImplTest, ClientAuthBadVerification) {
  Stats::IsolatedStoreImpl stats_store;
  Runtime::MockLoader runtime;

  std::string server_ctx_json = R"EOF(
  {
    "cert_chain_file": "/tmp/envoy_test/unittestcert.pem",
    "private_key_file": "/tmp/envoy_test/unittestkey.pem",
    "ca_cert_file": "test/common/ssl/test_data/ca.crt",
    "verify_certificate_hash": "7B:0C:3F:0D:97:0E:FC:16:70:11:7A:0C:35:75:54:6B:17:AB:CF:20:D8:AA:A0:ED:87:08:0F:FB:60:4C:40:77"
  }
  )EOF";

  Json::ObjectPtr server_ctx_loader = Json::Factory::LoadFromString(server_ctx_json);
  ContextConfigImpl server_ctx_config(*server_ctx_loader);
  ContextManagerImpl manager(runtime);
  ServerContextPtr server_ctx(manager.createSslServerContext(stats_store, server_ctx_config));

  Event::DispatcherImpl dispatcher;
  Network::TcpListenSocket socket(uint32_t(10000), true);
  Network::MockListenerCallbacks callbacks;
  Network::MockConnectionHandler connection_handler;
  Network::ListenerPtr listener =
      dispatcher.createSslListener(connection_handler, *server_ctx, socket, callbacks, stats_store,
                                   Network::ListenerOptions::listenerOptionsWithBindToPort());

  std::string client_ctx_json = R"EOF(
  {
    "cert_chain_file": "test/common/ssl/test_data/approved.crt",
    "private_key_file": "test/common/ssl/test_data/private_key.pem"
  }
  )EOF";

  Json::ObjectPtr client_ctx_loader = Json::Factory::LoadFromString(client_ctx_json);
  ContextConfigImpl client_ctx_config(*client_ctx_loader);
  ClientContextPtr client_ctx(manager.createSslClientContext(stats_store, client_ctx_config));
  Network::ClientConnectionPtr client_connection = dispatcher.createSslClientConnection(
      *client_ctx, Network::Utility::resolveUrl("tcp://127.0.0.1:10000"));
  client_connection->connect();

  Network::ConnectionPtr server_connection;
  Network::MockConnectionCallbacks server_connection_callbacks;
  EXPECT_CALL(callbacks, onNewConnection_(_))
      .WillOnce(Invoke([&](Network::ConnectionPtr& conn) -> void {
        server_connection = std::move(conn);
        server_connection->addConnectionCallbacks(server_connection_callbacks);
      }));

  EXPECT_CALL(server_connection_callbacks, onEvent(Network::ConnectionEvent::RemoteClose))
      .WillOnce(Invoke([&](uint32_t) -> void {
        client_connection->close(Network::ConnectionCloseType::NoFlush);
        dispatcher.exit();
      }));

  dispatcher.run(Event::Dispatcher::RunType::Block);
}

TEST(SslConnectionImplTest, SslError) {
  Stats::IsolatedStoreImpl stats_store;
  Runtime::MockLoader runtime;

  std::string server_ctx_json = R"EOF(
  {
    "cert_chain_file": "/tmp/envoy_test/unittestcert.pem",
    "private_key_file": "/tmp/envoy_test/unittestkey.pem",
    "ca_cert_file": "test/common/ssl/test_data/ca.crt",
    "verify_certificate_hash": "7B:0C:3F:0D:97:0E:FC:16:70:11:7A:0C:35:75:54:6B:17:AB:CF:20:D8:AA:A0:ED:87:08:0F:FB:60:4C:40:77"
  }
  )EOF";

  Json::ObjectPtr server_ctx_loader = Json::Factory::LoadFromString(server_ctx_json);
  ContextConfigImpl server_ctx_config(*server_ctx_loader);
  ContextManagerImpl manager(runtime);
  ServerContextPtr server_ctx(manager.createSslServerContext(stats_store, server_ctx_config));

  Event::DispatcherImpl dispatcher;
  Network::TcpListenSocket socket(uint32_t(10000), true);
  Network::MockListenerCallbacks callbacks;
  Network::MockConnectionHandler connection_handler;
  Network::ListenerPtr listener =
      dispatcher.createSslListener(connection_handler, *server_ctx, socket, callbacks, stats_store,
                                   Network::ListenerOptions::listenerOptionsWithBindToPort());

  Network::ClientConnectionPtr client_connection =
      dispatcher.createClientConnection(Network::Utility::resolveUrl("tcp://127.0.0.1:10000"));
  client_connection->connect();
  Buffer::OwnedImpl bad_data("bad_handshake_data");
  client_connection->write(bad_data);

  Network::ConnectionPtr server_connection;
  Network::MockConnectionCallbacks server_connection_callbacks;
  EXPECT_CALL(callbacks, onNewConnection_(_))
      .WillOnce(Invoke([&](Network::ConnectionPtr& conn) -> void {
        server_connection = std::move(conn);
        server_connection->addConnectionCallbacks(server_connection_callbacks);
      }));

  EXPECT_CALL(server_connection_callbacks, onEvent(Network::ConnectionEvent::RemoteClose))
      .WillOnce(Invoke([&](uint32_t) -> void {
        client_connection->close(Network::ConnectionCloseType::NoFlush);
        dispatcher.exit();
      }));

  dispatcher.run(Event::Dispatcher::RunType::Block);
}

class SslReadBufferLimitTest : public testing::Test {
public:
  void readBufferLimitTest(uint32_t read_buffer_limit, uint32_t expected_chunk_size) {
    const uint32_t buffer_size = 256 * 1024;

    Stats::IsolatedStoreImpl stats_store;
    Event::DispatcherImpl dispatcher;
    Network::TcpListenSocket socket(uint32_t(10000), true);
    Network::MockListenerCallbacks listener_callbacks;
    Network::MockConnectionHandler connection_handler;

    std::string server_ctx_json = R"EOF(
    {
      "cert_chain_file": "/tmp/envoy_test/unittestcert.pem",
      "private_key_file": "/tmp/envoy_test/unittestkey.pem",
      "ca_cert_file": "test/common/ssl/test_data/ca.crt"
    }
    )EOF";
    Json::ObjectPtr server_ctx_loader = Json::Factory::LoadFromString(server_ctx_json);
    ContextConfigImpl server_ctx_config(*server_ctx_loader);
    Runtime::MockLoader runtime;
    ContextManagerImpl manager(runtime);
    ServerContextPtr server_ctx(manager.createSslServerContext(stats_store, server_ctx_config));

    Network::ListenerPtr listener = dispatcher.createSslListener(
        connection_handler, *server_ctx, socket, listener_callbacks, stats_store,
        {.bind_to_port_ = true,
         .use_proxy_proto_ = false,
         .use_original_dst_ = false,
         .per_connection_buffer_limit_bytes_ = read_buffer_limit});

    std::string client_ctx_json = R"EOF(
    {
      "cert_chain_file": "test/common/ssl/test_data/approved.crt",
      "private_key_file": "test/common/ssl/test_data/private_key.pem"
    }
    )EOF";

    Json::ObjectPtr client_ctx_loader = Json::Factory::LoadFromString(client_ctx_json);
    ContextConfigImpl client_ctx_config(*client_ctx_loader);
    ClientContextPtr client_ctx(manager.createSslClientContext(stats_store, client_ctx_config));

    Network::ClientConnectionPtr client_connection = dispatcher.createSslClientConnection(
        *client_ctx, Network::Utility::resolveUrl("tcp://127.0.0.1:10000"));
    client_connection->connect();

    Network::ConnectionPtr server_connection;
    std::shared_ptr<Network::MockReadFilter> read_filter(new Network::MockReadFilter());
    EXPECT_CALL(listener_callbacks, onNewConnection_(_))
        .WillOnce(Invoke([&](Network::ConnectionPtr& conn) -> void {
          server_connection = std::move(conn);
          server_connection->addReadFilter(read_filter);
          EXPECT_EQ("", server_connection->nextProtocol());
        }));

    uint32_t filter_seen = 0;

    EXPECT_CALL(*read_filter, onNewConnection());
    EXPECT_CALL(*read_filter, onData(_))
        .WillRepeatedly(Invoke([&](Buffer::Instance& data) -> Network::FilterStatus {
          EXPECT_EQ(expected_chunk_size, data.length());
          filter_seen += data.length();
          data.drain(data.length());
          if (filter_seen == buffer_size) {
            server_connection->close(Network::ConnectionCloseType::FlushWrite);
          }
          return Network::FilterStatus::StopIteration;
        }));

    Network::MockConnectionCallbacks client_callbacks;
    client_connection->addConnectionCallbacks(client_callbacks);
    EXPECT_CALL(client_callbacks, onEvent(Network::ConnectionEvent::Connected));
    EXPECT_CALL(client_callbacks, onEvent(Network::ConnectionEvent::RemoteClose))
        .WillOnce(Invoke([&](uint32_t) -> void {
          EXPECT_EQ(buffer_size, filter_seen);
          dispatcher.exit();
        }));

    Buffer::OwnedImpl data(std::string(buffer_size, 'a'));
    client_connection->write(data);
    dispatcher.run(Event::Dispatcher::RunType::Block);
  }
};

TEST_F(SslReadBufferLimitTest, NoLimit) { readBufferLimitTest(0, 256 * 1024); }

TEST_F(SslReadBufferLimitTest, SomeLimit) { readBufferLimitTest(32 * 1024, 32 * 1024); }

} // Ssl
