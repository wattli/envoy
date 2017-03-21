#include "common/buffer/buffer_impl.h"
#include "common/http/codec_client.h"
#include "common/http/http1/conn_pool.h"
#include "common/network/utility.h"
#include "common/upstream/upstream_impl.h"

#include "test/common/http/common.h"
#include "test/mocks/event/mocks.h"
#include "test/mocks/http/mocks.h"
#include "test/mocks/network/mocks.h"
#include "test/mocks/runtime/mocks.h"
#include "test/mocks/upstream/mocks.h"
#include "test/test_common/utility.h"

using testing::_;
using testing::DoAll;
using testing::InSequence;
using testing::Invoke;
using testing::NiceMock;
using testing::Return;
using testing::ReturnRef;
using testing::SaveArg;

namespace Http {
namespace Http1 {

/**
 * A test version of ConnPoolImpl that allows for mocking beneath the codec clients.
 */
class ConnPoolImplForTest : public ConnPoolImpl {
public:
  ConnPoolImplForTest(Event::MockDispatcher& dispatcher, Upstream::ClusterInfoPtr cluster)
      : ConnPoolImpl(
            dispatcher,
            Upstream::HostPtr{new Upstream::HostImpl(
                cluster, "", Network::Utility::resolveUrl("tcp://127.0.0.1:9000"), false, 1, "")},
            Upstream::ResourcePriority::Default),
        mock_dispatcher_(dispatcher) {}

  ~ConnPoolImplForTest() {
    EXPECT_EQ(0U, ready_clients_.size());
    EXPECT_EQ(0U, busy_clients_.size());
    EXPECT_EQ(0U, pending_requests_.size());
  }

  struct TestCodecClient {
    Http::MockClientConnection* codec_;
    Network::MockClientConnection* connection_;
    CodecClient* codec_client_;
    Event::MockTimer* connect_timer_;
  };

  CodecClientPtr createCodecClient(Upstream::Host::CreateConnectionData& data) override {
    // We expect to own the connection, but already have it, so just release it to prevent it from
    // getting deleted.
    data.connection_.release();
    return CodecClientPtr{createCodecClient_()};
  }

  MOCK_METHOD0(createCodecClient_, CodecClient*());
  MOCK_METHOD0(onClientDestroy, void());

  void expectClientCreate() {
    test_clients_.emplace_back();
    TestCodecClient& test_client = test_clients_.back();
    test_client.connection_ = new NiceMock<Network::MockClientConnection>();
    test_client.codec_ = new NiceMock<Http::MockClientConnection>();
    test_client.connect_timer_ = new NiceMock<Event::MockTimer>(&mock_dispatcher_);

    Network::ClientConnectionPtr connection{test_client.connection_};
    test_client.codec_client_ = new CodecClientForTest(
        std::move(connection), test_client.codec_, [this](CodecClient* codec_client) -> void {
          for (auto i = test_clients_.begin(); i != test_clients_.end(); i++) {
            if (i->codec_client_ == codec_client) {
              onClientDestroy();
              test_clients_.erase(i);
              return;
            }
          }
        }, nullptr);

    EXPECT_CALL(mock_dispatcher_, createClientConnection_(_))
        .WillOnce(Return(test_client.connection_));
    EXPECT_CALL(*this, createCodecClient_()).WillOnce(Return(test_client.codec_client_));
    EXPECT_CALL(*test_client.connect_timer_, enableTimer(_));
  }

  Event::MockDispatcher& mock_dispatcher_;
  std::vector<TestCodecClient> test_clients_;
};

/**
 * Test fixture for all connection pool tests.
 */
class Http1ConnPoolImplTest : public testing::Test {
public:
  Http1ConnPoolImplTest() : conn_pool_(dispatcher_, cluster_) {}

  ~Http1ConnPoolImplTest() {
    // Make sure all gauges are 0.
    for (Stats::GaugePtr gauge : cluster_->stats_store_.gauges()) {
      EXPECT_EQ(0U, gauge->value());
    }
  }

  NiceMock<Event::MockDispatcher> dispatcher_;
  std::shared_ptr<Upstream::MockClusterInfo> cluster_{new NiceMock<Upstream::MockClusterInfo>()};
  ConnPoolImplForTest conn_pool_;
  NiceMock<Runtime::MockLoader> runtime_;
};

/**
 * Helper for dealing with an active test request.
 */
struct ActiveTestRequest {
  enum class Type { Pending, CreateConnection, Immediate };

  ActiveTestRequest(Http1ConnPoolImplTest& parent, size_t client_index, Type type)
      : parent_(parent), client_index_(client_index) {

    if (type == Type::CreateConnection) {
      parent.conn_pool_.expectClientCreate();
    }

    if (type == Type::Immediate) {
      expectNewStream();
    }

    handle_ = parent.conn_pool_.newStream(outer_decoder_, callbacks_);

    if (type == Type::Immediate) {
      EXPECT_EQ(nullptr, handle_);
    } else {
      EXPECT_NE(nullptr, handle_);
    }

    if (type == Type::CreateConnection) {
      expectNewStream();

      EXPECT_CALL(*parent_.conn_pool_.test_clients_[client_index_].connect_timer_, disableTimer());
      parent.conn_pool_.test_clients_[client_index_].connection_->raiseEvents(
          Network::ConnectionEvent::Connected);
    }
  }

  void completeResponse(bool with_body) {
    // Test additional metric writes also.
    Http::HeaderMapPtr response_headers(
        new TestHeaderMapImpl{{":status", "200"}, {"x-envoy-upstream-canary", "true"}});

    inner_decoder_->decodeHeaders(std::move(response_headers), !with_body);
    if (with_body) {
      Buffer::OwnedImpl data;
      inner_decoder_->decodeData(data, true);
    }
  }

  void expectNewStream() {
    EXPECT_CALL(*parent_.conn_pool_.test_clients_[client_index_].codec_, newStream(_))
        .WillOnce(DoAll(SaveArgAddress(&inner_decoder_), ReturnRef(request_encoder_)));
    EXPECT_CALL(callbacks_.pool_ready_, ready());
  }

  void startRequest() { callbacks_.outer_encoder_->encodeHeaders(TestHeaderMapImpl{}, true); }

  Http1ConnPoolImplTest& parent_;
  size_t client_index_;
  NiceMock<Http::MockStreamDecoder> outer_decoder_;
  Http::ConnectionPool::Cancellable* handle_{};
  NiceMock<Http::MockStreamEncoder> request_encoder_;
  Http::StreamDecoder* inner_decoder_{};
  ConnPoolCallbacks callbacks_;
};

/**
 * Test all timing stats are set.
 */
TEST_F(Http1ConnPoolImplTest, VerifyTimingStats) {
  EXPECT_CALL(cluster_->stats_store_, deliverTimingToSinks("upstream_cx_connect_ms", _));
  EXPECT_CALL(cluster_->stats_store_, deliverTimingToSinks("upstream_cx_length_ms", _));

  ActiveTestRequest r1(*this, 0, ActiveTestRequest::Type::CreateConnection);
  r1.startRequest();
  r1.completeResponse(false);

  EXPECT_CALL(conn_pool_, onClientDestroy());
  conn_pool_.test_clients_[0].connection_->raiseEvents(Network::ConnectionEvent::RemoteClose);
  dispatcher_.clearDeferredDeleteList();
}

/**
 * Test that buffer limits are set.
 */
TEST_F(Http1ConnPoolImplTest, VerifyBufferLimits) {
  NiceMock<Http::MockStreamDecoder> outer_decoder;
  ConnPoolCallbacks callbacks;
  conn_pool_.expectClientCreate();
  EXPECT_CALL(*cluster_, perConnectionBufferLimitBytes()).WillOnce(Return(8192));
  EXPECT_CALL(*conn_pool_.test_clients_.back().connection_, setReadBufferLimit(8192));
  Http::ConnectionPool::Cancellable* handle = conn_pool_.newStream(outer_decoder, callbacks);
  EXPECT_NE(nullptr, handle);

  EXPECT_CALL(conn_pool_, onClientDestroy());
  conn_pool_.test_clients_[0].connection_->raiseEvents(Network::ConnectionEvent::RemoteClose);
  dispatcher_.clearDeferredDeleteList();
}

/**
 * Tests a request that generates a new connection, completes, and then a second request that uses
 * the same connection.
 */
TEST_F(Http1ConnPoolImplTest, MultipleRequestAndResponse) {
  InSequence s;

  // Request 1 should kick off a new connection.
  ActiveTestRequest r1(*this, 0, ActiveTestRequest::Type::CreateConnection);
  r1.startRequest();
  r1.completeResponse(false);

  // Request 2 should not.
  ActiveTestRequest r2(*this, 0, ActiveTestRequest::Type::Immediate);
  r2.startRequest();
  r2.completeResponse(true);

  // Cause the connection to go away.
  EXPECT_CALL(conn_pool_, onClientDestroy());
  conn_pool_.test_clients_[0].connection_->raiseEvents(Network::ConnectionEvent::RemoteClose);
  dispatcher_.clearDeferredDeleteList();
}

/**
 * Test when we overflow max pending requests.
 */
TEST_F(Http1ConnPoolImplTest, MaxPendingRequests) {
  cluster_->resource_manager_.reset(
      new Upstream::ResourceManagerImpl(runtime_, "fake_key", 1, 1, 1024, 1));

  NiceMock<Http::MockStreamDecoder> outer_decoder;
  ConnPoolCallbacks callbacks;
  conn_pool_.expectClientCreate();
  Http::ConnectionPool::Cancellable* handle = conn_pool_.newStream(outer_decoder, callbacks);
  EXPECT_NE(nullptr, handle);

  NiceMock<Http::MockStreamDecoder> outer_decoder2;
  ConnPoolCallbacks callbacks2;
  EXPECT_CALL(callbacks2.pool_failure_, ready());
  Http::ConnectionPool::Cancellable* handle2 = conn_pool_.newStream(outer_decoder2, callbacks2);
  EXPECT_EQ(nullptr, handle2);

  handle->cancel();

  EXPECT_CALL(conn_pool_, onClientDestroy());
  conn_pool_.test_clients_[0].connection_->raiseEvents(Network::ConnectionEvent::RemoteClose);
  dispatcher_.clearDeferredDeleteList();

  EXPECT_EQ(1U, cluster_->stats_.upstream_rq_pending_overflow_.value());
}

/**
 * Tests a connection failure before a request is bound which should result in the pending request
 * getting purged.
 */
TEST_F(Http1ConnPoolImplTest, ConnectFailure) {
  InSequence s;

  // Request 1 should kick off a new connection.
  NiceMock<Http::MockStreamDecoder> outer_decoder;
  ConnPoolCallbacks callbacks;
  conn_pool_.expectClientCreate();
  Http::ConnectionPool::Cancellable* handle = conn_pool_.newStream(outer_decoder, callbacks);
  EXPECT_NE(nullptr, handle);

  EXPECT_CALL(callbacks.pool_failure_, ready());
  EXPECT_CALL(*conn_pool_.test_clients_[0].connect_timer_, disableTimer());
  conn_pool_.test_clients_[0].connection_->raiseEvents(Network::ConnectionEvent::RemoteClose);
  EXPECT_CALL(conn_pool_, onClientDestroy());
  dispatcher_.clearDeferredDeleteList();

  EXPECT_EQ(1U, cluster_->stats_.upstream_cx_connect_fail_.value());
  EXPECT_EQ(1U, cluster_->stats_.upstream_rq_pending_failure_eject_.value());
}

/**
 * Tests a connect timeout. Also test that we can add a new request during ejection processing.
 */
TEST_F(Http1ConnPoolImplTest, ConnectTimeout) {
  InSequence s;

  // Request 1 should kick off a new connection.
  NiceMock<Http::MockStreamDecoder> outer_decoder1;
  ConnPoolCallbacks callbacks1;
  conn_pool_.expectClientCreate();
  EXPECT_NE(nullptr, conn_pool_.newStream(outer_decoder1, callbacks1));

  NiceMock<Http::MockStreamDecoder> outer_decoder2;
  ConnPoolCallbacks callbacks2;
  EXPECT_CALL(callbacks1.pool_failure_, ready())
      .WillOnce(Invoke([&]() -> void {
        conn_pool_.expectClientCreate();
        EXPECT_NE(nullptr, conn_pool_.newStream(outer_decoder2, callbacks2));
      }));

  conn_pool_.test_clients_[0].connect_timer_->callback_();

  EXPECT_CALL(callbacks2.pool_failure_, ready());
  conn_pool_.test_clients_[1].connect_timer_->callback_();

  EXPECT_CALL(conn_pool_, onClientDestroy()).Times(2);
  dispatcher_.clearDeferredDeleteList();

  EXPECT_EQ(2U, cluster_->stats_.upstream_cx_connect_fail_.value());
  EXPECT_EQ(2U, cluster_->stats_.upstream_cx_connect_timeout_.value());
}

/**
 * Test cancelling before the request is bound to a connection.
 */
TEST_F(Http1ConnPoolImplTest, CancelBeforeBound) {
  InSequence s;

  // Request 1 should kick off a new connection.
  NiceMock<Http::MockStreamDecoder> outer_decoder;
  ConnPoolCallbacks callbacks;
  conn_pool_.expectClientCreate();
  Http::ConnectionPool::Cancellable* handle = conn_pool_.newStream(outer_decoder, callbacks);
  EXPECT_NE(nullptr, handle);

  handle->cancel();
  conn_pool_.test_clients_[0].connection_->raiseEvents(Network::ConnectionEvent::Connected);

  // Cause the connection to go away.
  EXPECT_CALL(conn_pool_, onClientDestroy());
  conn_pool_.test_clients_[0].connection_->raiseEvents(Network::ConnectionEvent::RemoteClose);
  dispatcher_.clearDeferredDeleteList();
}

/**
 * Test an upstream disconnection while there is a bound request.
 */
TEST_F(Http1ConnPoolImplTest, DisconnectWhileBound) {
  InSequence s;

  // Request 1 should kick off a new connection.
  NiceMock<Http::MockStreamDecoder> outer_decoder;
  ConnPoolCallbacks callbacks;
  conn_pool_.expectClientCreate();
  Http::ConnectionPool::Cancellable* handle = conn_pool_.newStream(outer_decoder, callbacks);
  EXPECT_NE(nullptr, handle);

  NiceMock<Http::MockStreamEncoder> request_encoder;
  Http::StreamDecoder* inner_decoder;
  EXPECT_CALL(*conn_pool_.test_clients_[0].codec_, newStream(_))
      .WillOnce(DoAll(SaveArgAddress(&inner_decoder), ReturnRef(request_encoder)));
  EXPECT_CALL(callbacks.pool_ready_, ready());

  conn_pool_.test_clients_[0].connection_->raiseEvents(Network::ConnectionEvent::Connected);

  // We should get a reset callback when the connection disconnects.
  Http::MockStreamCallbacks stream_callbacks;
  EXPECT_CALL(stream_callbacks, onResetStream(StreamResetReason::ConnectionTermination));
  request_encoder.getStream().addCallbacks(stream_callbacks);

  // Kill the connection while it has an active request.
  EXPECT_CALL(conn_pool_, onClientDestroy());
  conn_pool_.test_clients_[0].connection_->raiseEvents(Network::ConnectionEvent::RemoteClose);
  dispatcher_.clearDeferredDeleteList();
}

/**
 * Test that we correctly handle reaching max connections.
 */
TEST_F(Http1ConnPoolImplTest, MaxConnections) {
  InSequence s;

  // Request 1 should kick off a new connection.
  NiceMock<Http::MockStreamDecoder> outer_decoder1;
  ConnPoolCallbacks callbacks;
  conn_pool_.expectClientCreate();
  Http::ConnectionPool::Cancellable* handle = conn_pool_.newStream(outer_decoder1, callbacks);

  EXPECT_NE(nullptr, handle);

  // Request 2 should not kick off a new connection.
  NiceMock<Http::MockStreamDecoder> outer_decoder2;
  ConnPoolCallbacks callbacks2;
  handle = conn_pool_.newStream(outer_decoder2, callbacks2);
  EXPECT_EQ(1U, cluster_->stats_.upstream_cx_overflow_.value());

  EXPECT_NE(nullptr, handle);

  // Connect event will bind to request 1.
  NiceMock<Http::MockStreamEncoder> request_encoder;
  Http::StreamDecoder* inner_decoder;
  EXPECT_CALL(*conn_pool_.test_clients_[0].codec_, newStream(_))
      .WillOnce(DoAll(SaveArgAddress(&inner_decoder), ReturnRef(request_encoder)));
  EXPECT_CALL(callbacks.pool_ready_, ready());

  conn_pool_.test_clients_[0].connection_->raiseEvents(Network::ConnectionEvent::Connected);

  // Finishing request 1 will immediately bind to request 2.
  EXPECT_CALL(*conn_pool_.test_clients_[0].codec_, newStream(_))
      .WillOnce(DoAll(SaveArgAddress(&inner_decoder), ReturnRef(request_encoder)));
  EXPECT_CALL(callbacks2.pool_ready_, ready());

  callbacks.outer_encoder_->encodeHeaders(TestHeaderMapImpl{}, true);
  Http::HeaderMapPtr response_headers(new TestHeaderMapImpl{{":status", "200"}});
  inner_decoder->decodeHeaders(std::move(response_headers), true);

  callbacks2.outer_encoder_->encodeHeaders(TestHeaderMapImpl{}, true);
  response_headers.reset(new TestHeaderMapImpl{{":status", "200"}});
  inner_decoder->decodeHeaders(std::move(response_headers), true);

  // Cause the connection to go away.
  EXPECT_CALL(conn_pool_, onClientDestroy());
  conn_pool_.test_clients_[0].connection_->raiseEvents(Network::ConnectionEvent::RemoteClose);
  dispatcher_.clearDeferredDeleteList();
}

/**
 * Test when upstream sends us 'connection: close'
 */
TEST_F(Http1ConnPoolImplTest, ConnectionCloseHeader) {
  InSequence s;

  // Request 1 should kick off a new connection.
  NiceMock<Http::MockStreamDecoder> outer_decoder;
  ConnPoolCallbacks callbacks;
  conn_pool_.expectClientCreate();
  Http::ConnectionPool::Cancellable* handle = conn_pool_.newStream(outer_decoder, callbacks);

  EXPECT_NE(nullptr, handle);

  NiceMock<Http::MockStreamEncoder> request_encoder;
  Http::StreamDecoder* inner_decoder;
  EXPECT_CALL(*conn_pool_.test_clients_[0].codec_, newStream(_))
      .WillOnce(DoAll(SaveArgAddress(&inner_decoder), ReturnRef(request_encoder)));
  EXPECT_CALL(callbacks.pool_ready_, ready());

  conn_pool_.test_clients_[0].connection_->raiseEvents(Network::ConnectionEvent::Connected);
  callbacks.outer_encoder_->encodeHeaders(TestHeaderMapImpl{}, true);

  // Response with 'connection: close' which should cause the connection to go away.
  EXPECT_CALL(conn_pool_, onClientDestroy());
  Http::HeaderMapPtr response_headers(
      new TestHeaderMapImpl{{":status", "200"}, {"Connection", "Close"}});
  inner_decoder->decodeHeaders(std::move(response_headers), true);
  dispatcher_.clearDeferredDeleteList();

  EXPECT_EQ(0U, cluster_->stats_.upstream_cx_destroy_with_active_rq_.value());
}

/**
 * Test when we reach max requests per connection.
 */
TEST_F(Http1ConnPoolImplTest, MaxRequestsPerConnection) {
  InSequence s;

  cluster_->max_requests_per_connection_ = 1;

  // Request 1 should kick off a new connection.
  NiceMock<Http::MockStreamDecoder> outer_decoder;
  ConnPoolCallbacks callbacks;
  conn_pool_.expectClientCreate();
  Http::ConnectionPool::Cancellable* handle = conn_pool_.newStream(outer_decoder, callbacks);

  EXPECT_NE(nullptr, handle);

  NiceMock<Http::MockStreamEncoder> request_encoder;
  Http::StreamDecoder* inner_decoder;
  EXPECT_CALL(*conn_pool_.test_clients_[0].codec_, newStream(_))
      .WillOnce(DoAll(SaveArgAddress(&inner_decoder), ReturnRef(request_encoder)));
  EXPECT_CALL(callbacks.pool_ready_, ready());

  conn_pool_.test_clients_[0].connection_->raiseEvents(Network::ConnectionEvent::Connected);
  callbacks.outer_encoder_->encodeHeaders(TestHeaderMapImpl{}, true);

  // Response with 'connection: close' which should cause the connection to go away.
  EXPECT_CALL(conn_pool_, onClientDestroy());
  Http::HeaderMapPtr response_headers(new TestHeaderMapImpl{{":status", "200"}});
  inner_decoder->decodeHeaders(std::move(response_headers), true);
  dispatcher_.clearDeferredDeleteList();

  EXPECT_EQ(0U, cluster_->stats_.upstream_cx_destroy_with_active_rq_.value());
  EXPECT_EQ(1U, cluster_->stats_.upstream_cx_max_requests_.value());
}

TEST_F(Http1ConnPoolImplTest, ConcurrentConnections) {
  InSequence s;

  cluster_->resource_manager_.reset(
      new Upstream::ResourceManagerImpl(runtime_, "fake_key", 2, 1024, 1024, 1));
  ActiveTestRequest r1(*this, 0, ActiveTestRequest::Type::CreateConnection);
  r1.startRequest();

  ActiveTestRequest r2(*this, 1, ActiveTestRequest::Type::CreateConnection);
  r2.startRequest();

  ActiveTestRequest r3(*this, 0, ActiveTestRequest::Type::Pending);

  // Finish r1, which gets r3 going.
  r3.expectNewStream();
  r1.completeResponse(false);
  r3.startRequest();

  r2.completeResponse(false);
  r3.completeResponse(false);

  // Disconnect both clients.
  EXPECT_CALL(conn_pool_, onClientDestroy()).Times(2);
  conn_pool_.test_clients_[1].connection_->raiseEvents(Network::ConnectionEvent::RemoteClose);
  conn_pool_.test_clients_[0].connection_->raiseEvents(Network::ConnectionEvent::RemoteClose);
  dispatcher_.clearDeferredDeleteList();
}

TEST_F(Http1ConnPoolImplTest, DrainCallback) {
  InSequence s;
  ReadyWatcher drained;

  EXPECT_CALL(drained, ready());
  conn_pool_.addDrainedCallback([&]() -> void { drained.ready(); });

  ActiveTestRequest r1(*this, 0, ActiveTestRequest::Type::CreateConnection);
  ActiveTestRequest r2(*this, 0, ActiveTestRequest::Type::Pending);
  r2.handle_->cancel();

  EXPECT_CALL(drained, ready());
  r1.startRequest();
  r1.completeResponse(false);

  EXPECT_CALL(conn_pool_, onClientDestroy());
  dispatcher_.clearDeferredDeleteList();
}

} // Http1
} // Http
