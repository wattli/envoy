#pragma once

#include "libevent.h"

#include "envoy/event/deferred_deletable.h"
#include "envoy/event/dispatcher.h"
#include "envoy/network/connection_handler.h"

#include "common/common/logger.h"

namespace Event {

/**
 * libevent implementation of Event::Dispatcher.
 */
class DispatcherImpl : Logger::Loggable<Logger::Id::main>, public Dispatcher {
public:
  DispatcherImpl();
  ~DispatcherImpl();

  /**
   * @return event_base& the libevent base.
   */
  event_base& base() { return *base_; }

  // Event::Dispatcher
  void clearDeferredDeleteList() override;
  Network::ClientConnectionPtr
  createClientConnection(Network::Address::InstancePtr address) override;
  Network::ClientConnectionPtr
  createSslClientConnection(Ssl::ClientContext& ssl_ctx,
                            Network::Address::InstancePtr address) override;
  Network::DnsResolverPtr createDnsResolver() override;
  FileEventPtr createFileEvent(int fd, FileReadyCb cb, FileTriggerType trigger,
                               uint32_t events) override;
  Filesystem::WatcherPtr createFilesystemWatcher() override;
  Network::ListenerPtr createListener(Network::ConnectionHandler& conn_handler,
                                      Network::ListenSocket& socket, Network::ListenerCallbacks& cb,
                                      Stats::Store& stats_store,
                                      const Network::ListenerOptions& listener_options) override;
  Network::ListenerPtr createSslListener(Network::ConnectionHandler& conn_handler,
                                         Ssl::ServerContext& ssl_ctx, Network::ListenSocket& socket,
                                         Network::ListenerCallbacks& cb, Stats::Store& stats_store,
                                         const Network::ListenerOptions& listener_options) override;
  TimerPtr createTimer(TimerCb cb) override;
  void deferredDelete(DeferredDeletablePtr&& to_delete) override;
  void exit() override;
  SignalEventPtr listenForSignal(int signal_num, SignalCb cb) override;
  void post(std::function<void()> callback) override;
  void run(RunType type) override;

private:
  void runPostCallbacks();

  Libevent::BasePtr base_;
  TimerPtr deferred_delete_timer_;
  TimerPtr post_timer_;
  std::vector<DeferredDeletablePtr> to_delete_1_;
  std::vector<DeferredDeletablePtr> to_delete_2_;
  std::vector<DeferredDeletablePtr>* current_to_delete_;
  std::mutex post_lock_;
  std::list<std::function<void()>> post_callbacks_;
  bool deferred_deleting_{};
};

} // Event
