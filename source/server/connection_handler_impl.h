#pragma once

#include <atomic>
#include <cstdint>
#include <list>
#include <memory>

#include "envoy/common/time.h"
#include "envoy/event/deferred_deletable.h"
#include "envoy/network/connection.h"
#include "envoy/network/connection_handler.h"
#include "envoy/network/filter.h"
#include "envoy/network/listen_socket.h"
#include "envoy/network/listener.h"
#include "envoy/server/active_udp_listener_config.h"
#include "envoy/server/listener_manager.h"
#include "envoy/stats/scope.h"
#include "envoy/stats/timespan.h"

#include "common/common/linked_object.h"
#include "common/common/non_copyable.h"
#include "common/network/generic_listener_filter.h"
#include "common/network/internal_listener_impl.h"
#include "common/network/listen_socket_impl.h"
#include "common/stream_info/stream_info_impl.h"

#include "spdlog/spdlog.h"

namespace Envoy {
namespace Server {

#define ALL_LISTENER_STATS(COUNTER, GAUGE, HISTOGRAM)                                              \
  COUNTER(downstream_cx_destroy)                                                                   \
  COUNTER(downstream_cx_overflow)                                                                  \
  COUNTER(downstream_cx_total)                                                                     \
  COUNTER(downstream_global_cx_overflow)                                                           \
  COUNTER(downstream_pre_cx_timeout)                                                               \
  COUNTER(no_filter_chain_match)                                                                   \
  GAUGE(downstream_cx_active, Accumulate)                                                          \
  GAUGE(downstream_pre_cx_active, Accumulate)                                                      \
  HISTOGRAM(downstream_cx_length_ms, Milliseconds)

/**
 * Wrapper struct for listener stats. @see stats_macros.h
 */
struct ListenerStats {
  ALL_LISTENER_STATS(GENERATE_COUNTER_STRUCT, GENERATE_GAUGE_STRUCT, GENERATE_HISTOGRAM_STRUCT)
};

#define ALL_PER_HANDLER_LISTENER_STATS(COUNTER, GAUGE)                                             \
  COUNTER(downstream_cx_total)                                                                     \
  GAUGE(downstream_cx_active, Accumulate)

/**
 * Wrapper struct for per-handler listener stats. @see stats_macros.h
 */
struct PerHandlerListenerStats {
  ALL_PER_HANDLER_LISTENER_STATS(GENERATE_COUNTER_STRUCT, GENERATE_GAUGE_STRUCT)
};

class ActiveUdpListenerBase;

/**
 * Server side connection handler. This is used both by workers as well as the
 * main thread for non-threaded listeners.
 */
class ConnectionHandlerImpl : public Network::ConnectionHandler,
                              NonCopyable,
                              Logger::Loggable<Logger::Id::conn_handler> {
public:
  ConnectionHandlerImpl(Event::Dispatcher& dispatcher, absl::optional<uint32_t> worker_index);

  // Network::ConnectionHandler
  uint64_t numConnections() const override { return num_handler_connections_; }
  void incNumConnections() override;
  void decNumConnections() override;
  void addListener(absl::optional<uint64_t> overridden_listener,
                   Network::ListenerConfig& config) override;
  void removeListeners(uint64_t listener_tag) override;
  Network::UdpListenerCallbacksOptRef getUdpListenerCallbacks(uint64_t listener_tag) override;
  void removeFilterChains(uint64_t listener_tag,
                          const std::list<const Network::FilterChain*>& filter_chains,
                          std::function<void()> completion) override;
  void stopListeners(uint64_t listener_tag) override;
  void stopListeners() override;
  void disableListeners() override;
  void enableListeners() override;
  const std::string& statPrefix() const override { return per_handler_stat_prefix_; }

  /**
   * Wrapper for an active listener owned by this handler.
   */
  class ActiveListenerImplBase : public virtual Network::ConnectionHandler::ActiveListener {
  public:
    ActiveListenerImplBase(Network::ConnectionHandler& parent, Network::ListenerConfig* config);

    // Network::ConnectionHandler::ActiveListener.
    uint64_t listenerTag() override { return config_->listenerTag(); }

    ListenerStats stats_;
    PerHandlerListenerStats per_worker_stats_;
    Network::ListenerConfig* config_{};
  };

private:
  struct ActiveTcpConnection;
  using ActiveTcpConnectionPtr = std::unique_ptr<ActiveTcpConnection>;
  class StreamListener {
  public:
    virtual ~StreamListener() = default;
    // virtual ListenerStats& listenerStats() PURE;
    // virtual PerHandlerListenerStats& per_worker_stats_() PURE;
    virtual void onNewConnection() PURE;
    virtual void onDestroyConnection() PURE;
    virtual Stats::TimespanPtr newTimespan(TimeSource& time_source) PURE;
    virtual Network::ListenerConfig& listenerConfig() PURE;
    virtual void removeConnection(ActiveTcpConnection& conn) PURE;
  };

  struct ActiveTcpSocket;
  using ActiveTcpSocketPtr = std::unique_ptr<ActiveTcpSocket>;
  class ActiveConnections;
  using ActiveConnectionsPtr = std::unique_ptr<ActiveConnections>;
  struct ActiveInternalSocket;
  using ActiveInternalSocketPtr = std::unique_ptr<ActiveInternalSocket>;

  /**
   * Wrapper for an active tcp listener owned by this handler.
   */
  class ActiveTcpListener : public Network::TcpListenerCallbacks,
                            public ActiveListenerImplBase,
                            public StreamListener,
                            public Network::BalancedConnectionHandler {
  public:
    ActiveTcpListener(ConnectionHandlerImpl& parent, Network::ListenerConfig& config);
    ActiveTcpListener(ConnectionHandlerImpl& parent, Network::ListenerPtr&& listener,
                      Network::ListenerConfig& config);
    ~ActiveTcpListener() override;
    bool listenerConnectionLimitReached() const {
      // TODO(tonya11en): Delegate enforcement of per-listener connection limits to overload
      // manager.
      return !config_->openConnections().canCreate();
    }
    void onAcceptWorker(Network::ConnectionSocketPtr&& socket,
                        bool hand_off_restored_destination_connections, bool rebalanced);

    void decNumConnections() {
      ASSERT(num_listener_connections_ > 0);
      --num_listener_connections_;
      config_->openConnections().dec();
    }

    // Network::TcpListenerCallbacks
    void onAccept(Network::ConnectionSocketPtr&& socket) override;
    void onReject() override { stats_.downstream_global_cx_overflow_.inc(); }

    // ActiveListenerImplBase
    Network::Listener* listener() override { return listener_.get(); }
    void pauseListening() override { listener_->disable(); }
    void resumeListening() override { listener_->enable(); }
    void shutdownListener() override { listener_.reset(); }

    // StreamListener
    void onNewConnection() override {
      stats_.downstream_cx_total_.inc();
      stats_.downstream_cx_active_.inc();
      per_worker_stats_.downstream_cx_total_.inc();
      per_worker_stats_.downstream_cx_active_.inc();
      // Active connections on the handler (not listener). The per listener connections have already
      // been incremented at this point either via the connection balancer or in the socket accept
      // path if there is no configured balancer.
      ++parent_.num_handler_connections_;
    }
    void onDestroyConnection() override {
      stats_.downstream_cx_active_.dec();
      stats_.downstream_cx_destroy_.inc();
      per_worker_stats_.downstream_cx_active_.dec();
      // Active listener connections (not handler).
      decNumConnections();
      // Active handler connections (not listener).
      parent_.decNumConnections();
    }
    Network::ListenerConfig& listenerConfig() override { return *config_; }

    Stats::TimespanPtr newTimespan(TimeSource& time_source) override;

    // Network::BalancedConnectionHandler
    uint64_t numConnections() const override { return num_listener_connections_; }
    void incNumConnections() override {
      ++num_listener_connections_;
      config_->openConnections().inc();
    }
    void post(Network::ConnectionSocketPtr&& socket) override;

    /**
     * Remove and destroy an active connection.
     * @param connection supplies the connection to remove.
     */
    void removeConnection(ActiveTcpConnection& connection) override;

    /**
     * Create a new connection from a socket accepted by the listener.
     */
    void newConnection(Network::ConnectionSocketPtr&& socket,
                       std::unique_ptr<StreamInfo::StreamInfo> stream_info);

    /**
     * Return the active connections container attached with the given filter chain.
     */
    ActiveConnections& getOrCreateActiveConnections(const Network::FilterChain& filter_chain);

    /**
     * Schedule to remove and destroy the active connections which are not tracked by listener
     * config. Caution: The connection are not destroyed yet when function returns.
     */
    void deferredRemoveFilterChains(
        const std::list<const Network::FilterChain*>& draining_filter_chains);

    /**
     * Update the listener config. The follow up connections will see the new config. The existing
     * connections are not impacted.
     */
    void updateListenerConfig(Network::ListenerConfig& config);

    ConnectionHandlerImpl& parent_;
    Network::ListenerPtr listener_;
    const std::chrono::milliseconds listener_filters_timeout_;
    const bool continue_on_listener_filters_timeout_;
    std::list<ActiveTcpSocketPtr> sockets_;
    absl::node_hash_map<const Network::FilterChain*, ActiveConnectionsPtr> connections_by_context_;

    // The number of connections currently active on this listener. This is typically used for
    // connection balancing across per-handler listeners.
    std::atomic<uint64_t> num_listener_connections_{};
    bool is_deleting_{false};
  };

  /**
   * Wrapper for an active internal listener owned by this handler.
   */
  class ActiveInternalListener : public Network::InternalListenerCallbacks,
                                 public ActiveListenerImplBase,
                                 public StreamListener {
  public:
    ActiveInternalListener(ConnectionHandlerImpl& parent, Network::ListenerConfig& config);

    ~ActiveInternalListener() override;

    void incNumConnections() {
      ++num_listener_connections_;
      config_->openConnections().inc();
    }

    void decNumConnections() {
      ASSERT(num_listener_connections_ > 0);
      --num_listener_connections_;
      config_->openConnections().dec();
    }

    // Network::InternalListenerCallbacks
    void onNewSocket(Network::ConnectionSocketPtr socket) override;
    
    // ActiveListenerImplBase
    Network::Listener* listener() override { return internal_listener_.get(); }
    void pauseListening() override { internal_listener_->disable(); }
    void resumeListening() override { internal_listener_->enable(); }
    void shutdownListener() override;

    // StreamListener
    void onNewConnection() override {
      stats_.downstream_cx_total_.inc();
      stats_.downstream_cx_active_.inc();
      per_worker_stats_.downstream_cx_total_.inc();
      per_worker_stats_.downstream_cx_active_.inc();
      ++parent_.num_handler_connections_;
    }
    void onDestroyConnection() override {
      stats_.downstream_cx_active_.dec();
      stats_.downstream_cx_destroy_.inc();
      per_worker_stats_.downstream_cx_active_.dec();
      decNumConnections();
      parent_.decNumConnections();
    }
    Network::ListenerConfig& listenerConfig() override { return *config_; }
    Stats::TimespanPtr newTimespan(TimeSource& time_source) override;

    /**
     * Remove and destroy an active connection.
     * @param connection supplies the connection to remove.
     */
    void removeConnection(ActiveTcpConnection& connection) override;

    /**
     * Create a new connection from a socket accepted by the listener.
     */
    void newConnection(Network::ConnectionSocketPtr&& socket,
                       const envoy::config::core::v3::Metadata& dynamic_metadata);

    /**
     * Return the active connections container attached with the given filter chain.
     */
    ActiveConnections& getOrCreateActiveConnections(const Network::FilterChain& filter_chain);

    /**
     * Schedule to remove and destroy the active connections which are not tracked by listener
     * config. Caution: The connection are not destroyed yet when function returns.
     */
    void deferredRemoveFilterChains(
        const std::list<const Network::FilterChain*>& draining_filter_chains);

    /**
     * Update the listener config. The follow up connections will see the new config. The existing
     * connections are not impacted.
     */
    void updateListenerConfig(Network::ListenerConfig& config);

    ConnectionHandlerImpl& parent_;
    std::unique_ptr<Network::InternalListenerImpl> internal_listener_;
    const std::chrono::milliseconds listener_filters_timeout_;
    const bool continue_on_listener_filters_timeout_;
    std::list<ActiveInternalSocketPtr> sockets_;
    absl::node_hash_map<const Network::FilterChain*, ActiveConnectionsPtr> connections_by_context_;

    // The number of connections currently active on this listener. This is typically used for
    // connection balancing across per-handler listeners.
    std::atomic<uint64_t> num_listener_connections_{};
    bool is_deleting_{false};
  };

  /**
   * Wrapper for a group of active connections which are attached to the same filter chain context.
   */
  class ActiveConnections : public Event::DeferredDeletable {
  public:
    ActiveConnections(StreamListener& listener, const Network::FilterChain& filter_chain);
    ~ActiveConnections() override;

    // Listener filter chain pair is the owner of the connections.
    StreamListener& listener_;
    const Network::FilterChain& filter_chain_;
    // Owned connections
    std::list<ActiveTcpConnectionPtr> connections_;
  };

  /**
   * Wrapper for an active TCP connection owned by this handler.
   */
  struct ActiveTcpConnection : LinkedObject<ActiveTcpConnection>,
                               public Event::DeferredDeletable,
                               public Network::ConnectionCallbacks {
    ActiveTcpConnection(ActiveConnections& active_connections,
                        Network::ConnectionPtr&& new_connection, TimeSource& time_system,
                        std::unique_ptr<StreamInfo::StreamInfo>&& stream_info);
    ~ActiveTcpConnection() override;

    // Network::ConnectionCallbacks
    void onEvent(Network::ConnectionEvent event) override {
      // Any event leads to destruction of the connection.
      if (event == Network::ConnectionEvent::LocalClose ||
          event == Network::ConnectionEvent::RemoteClose) {
        active_connections_.listener_.removeConnection(*this);
      }
    }
    void onAboveWriteBufferHighWatermark() override {}
    void onBelowWriteBufferLowWatermark() override {}

    std::unique_ptr<StreamInfo::StreamInfo> stream_info_;
    ActiveConnections& active_connections_;
    Network::ConnectionPtr connection_;
    Stats::TimespanPtr conn_length_;
  };

  /**
   * Wrapper for an active accepted TCP socket owned by this handler.
   */
  struct ActiveTcpSocket : public Network::ListenerFilterManager,
                           public Network::ListenerFilterCallbacks,
                           LinkedObject<ActiveTcpSocket>,
                           public Event::DeferredDeletable {
    ActiveTcpSocket(ActiveTcpListener& listener, Network::ConnectionSocketPtr&& socket,
                    bool hand_off_restored_destination_connections)
        : stream_listener_(listener), socket_(std::move(socket)),
          hand_off_restored_destination_connections_(hand_off_restored_destination_connections),
          stream_info_(std::make_unique<StreamInfo::StreamInfoImpl>(
              stream_listener_.parent_.dispatcher_.timeSource(),
              StreamInfo::FilterState::LifeSpan::Connection)),
          iter_(accept_filters_.end()) {
      stream_listener_.stats_.downstream_pre_cx_active_.inc();
      stream_info_->setDownstreamLocalAddress(socket_->localAddress());
      stream_info_->setDownstreamRemoteAddress(socket_->remoteAddress());
      stream_info_->setDownstreamDirectRemoteAddress(socket_->directRemoteAddress());
    }
    ~ActiveTcpSocket() override {
      accept_filters_.clear();
      stream_listener_.stats_.downstream_pre_cx_active_.dec();

      // If the underlying socket is no longer attached, it means that it has been transferred to
      // an active connection. In this case, the active connection will decrement the number
      // of listener connections.
      // TODO(mattklein123): In general the way we account for the number of listener connections
      // is incredibly fragile. Revisit this by potentially merging ActiveTcpSocket and
      // ActiveTcpConnection, having a shared object which does accounting (but would require
      // another allocation, etc.).
      if (socket_ != nullptr) {
        stream_listener_.decNumConnections();
      }
    }

    void onTimeout();
    void startTimer();
    void unlink();
    void newConnection();
    bool isListenerFiltersCompleted() { return iter_ == accept_filters_.end(); }
    bool isConnected() { return connected_; }

    // Network::ListenerFilterManager
    void addAcceptFilter(const Network::ListenerFilterMatcherSharedPtr& listener_filter_matcher,
                         Network::ListenerFilterPtr&& filter) override {
      accept_filters_.emplace_back(std::make_unique<Network::GenericListenerFilter>(
          listener_filter_matcher, std::move(filter)));
    }

    // Network::ListenerFilterCallbacks
    Network::ConnectionSocket& socket() override { return *socket_.get(); }
    Event::Dispatcher& dispatcher() override { return stream_listener_.parent_.dispatcher_; }
    void continueFilterChain(bool success) override;
    void setDynamicMetadata(const std::string& name, const ProtobufWkt::Struct& value) override;
    envoy::config::core::v3::Metadata& dynamicMetadata() override {
      return stream_info_->dynamicMetadata();
    };
    const envoy::config::core::v3::Metadata& dynamicMetadata() const override {
      return stream_info_->dynamicMetadata();
    };

    ActiveTcpListener& stream_listener_;
    Network::ConnectionSocketPtr socket_;
    const bool hand_off_restored_destination_connections_;
    Event::TimerPtr timer_;
    std::unique_ptr<StreamInfo::StreamInfo> stream_info_;

  private:
    std::list<Network::ListenerFilterWrapperPtr> accept_filters_;
    std::list<Network::ListenerFilterWrapperPtr>::iterator iter_;
    bool connected_{false};
  };

  /**
   * Wrapper for an active accepted internal socket owned by this handler.
   */
  struct ActiveInternalSocket : public Network::ListenerFilterManager,
                                public Network::ListenerFilterCallbacks,
                                LinkedObject<ActiveInternalSocket>,
                                public Event::DeferredDeletable {
    ActiveInternalSocket(ActiveInternalListener& listener, Network::ConnectionSocketPtr socket)
        : stream_listener_(listener), socket_(std::move(socket)),
          stream_info_(std::make_unique<StreamInfo::StreamInfoImpl>(
              stream_listener_.parent_.dispatcher_.timeSource(),
              StreamInfo::FilterState::LifeSpan::Connection)),
          iter_(accept_filters_.end()) {
      stream_listener_.stats_.downstream_pre_cx_active_.inc();
      stream_info_->setDownstreamLocalAddress(socket_->localAddress());
      stream_info_->setDownstreamRemoteAddress(socket_->remoteAddress());
      stream_info_->setDownstreamDirectRemoteAddress(socket_->directRemoteAddress());
    }
    ~ActiveInternalSocket() override {
      accept_filters_.clear();
      stream_listener_.stats_.downstream_pre_cx_active_.dec();

      // If the underlying socket is no longer attached, it means that it has been transferred to
      // an active connection. In this case, the active connection will decrement the number
      // of listener connections.
      // TODO(mattklein123): In general the way we account for the number of listener connections
      // is incredibly fragile. Revisit this by potentially merging ActiveInternalSocket and
      // ActiveTcpConnection, having a shared object which does accounting (but would require
      // another allocation, etc.).
      if (socket_ != nullptr) {
        stream_listener_.decNumConnections();
      }
    }

    void onTimeout();
    void startTimer();
    void unlink();
    void newConnection();
    bool isListenerFiltersCompleted() { return iter_ == accept_filters_.end(); }
    bool isConnected() { return connected_; }

    // Network::ListenerFilterManager
    void addAcceptFilter(const Network::ListenerFilterMatcherSharedPtr& listener_filter_matcher,
                         Network::ListenerFilterPtr&& filter) override {
      accept_filters_.emplace_back(std::make_unique<Network::GenericListenerFilter>(
          listener_filter_matcher, std::move(filter)));
    }

    // Network::ListenerFilterCallbacks
    Network::ConnectionSocket& socket() override { return *socket_.get(); }
    Event::Dispatcher& dispatcher() override { return stream_listener_.parent_.dispatcher_; }
    void continueFilterChain(bool success) override;
    void setDynamicMetadata(const std::string& name, const ProtobufWkt::Struct& value) override;
    envoy::config::core::v3::Metadata& dynamicMetadata() override {
      return stream_info_->dynamicMetadata();
    };
    const envoy::config::core::v3::Metadata& dynamicMetadata() const override {
      return stream_info_->dynamicMetadata();
    };

    ActiveInternalListener& stream_listener_;
    Network::ConnectionSocketPtr socket_;
    Event::TimerPtr timer_;
    std::unique_ptr<StreamInfo::StreamInfo> stream_info_;

  private:
    std::list<Network::ListenerFilterWrapperPtr> accept_filters_;
    std::list<Network::ListenerFilterWrapperPtr>::iterator iter_;
    bool connected_{false};
  };

  using ActiveTcpListenerOptRef = absl::optional<std::reference_wrapper<ActiveTcpListener>>;
  using UdpListenerCallbacksOptRef =
      absl::optional<std::reference_wrapper<Network::UdpListenerCallbacks>>;
  using ActiveInternalListenerOptRef =
      absl::optional<std::reference_wrapper<ActiveInternalListener>>;

  struct ActiveListenerDetails {
    // Strong pointer to the listener, whether TCP, UDP, QUIC, etc.
    Network::ConnectionHandler::ActiveListenerPtr listener_;

    absl::variant<absl::monostate, std::reference_wrapper<ActiveTcpListener>,
                  std::reference_wrapper<Network::UdpListenerCallbacks>,
                  std::reference_wrapper<ActiveInternalListener>>
        typed_listener_;

    // Helpers for accessing the data in the variant for cleaner code.
    ActiveTcpListenerOptRef tcpListener();
    UdpListenerCallbacksOptRef udpListener();
    ActiveInternalListenerOptRef internalListener();
  };
  using ActiveListenerDetailsOptRef = absl::optional<std::reference_wrapper<ActiveListenerDetails>>;

  ActiveTcpListenerOptRef findActiveTcpListenerByAddress(const Network::Address::Instance& address);
  ActiveListenerDetailsOptRef findActiveListenerByTag(uint64_t listener_tag);

  // This has a value on worker threads, and no value on the main thread.
  const absl::optional<uint32_t> worker_index_;
  Event::Dispatcher& dispatcher_;
  const std::string per_handler_stat_prefix_;
  std::list<std::pair<Network::Address::InstanceConstSharedPtr, ActiveListenerDetails>> listeners_;
  std::atomic<uint64_t> num_handler_connections_{};
  bool disable_listeners_;
};

class ActiveUdpListenerBase : public ConnectionHandlerImpl::ActiveListenerImplBase,
                              public Network::ConnectionHandler::ActiveUdpListener {
public:
  ActiveUdpListenerBase(uint32_t worker_index, uint32_t concurrency,
                        Network::ConnectionHandler& parent, Network::Socket& listen_socket,
                        Network::UdpListenerPtr&& listener, Network::ListenerConfig* config);
  ~ActiveUdpListenerBase() override;

  // Network::UdpListenerCallbacks
  void onData(Network::UdpRecvData&& data) final;
  uint32_t workerIndex() const final { return worker_index_; }
  void post(Network::UdpRecvData&& data) final;

  // ActiveListenerImplBase
  Network::Listener* listener() override { return udp_listener_.get(); }

protected:
  uint32_t destination(const Network::UdpRecvData& /*data*/) const override {
    // By default, route to the current worker.
    return worker_index_;
  }

  const uint32_t worker_index_;
  const uint32_t concurrency_;
  Network::ConnectionHandler& parent_;
  Network::Socket& listen_socket_;
  Network::UdpListenerPtr udp_listener_;
};

/**
 * Wrapper for an active udp listener owned by this handler.
 */
class ActiveRawUdpListener : public ActiveUdpListenerBase,
                             public Network::UdpListenerFilterManager,
                             public Network::UdpReadFilterCallbacks {
public:
  ActiveRawUdpListener(uint32_t worker_index, uint32_t concurrency,
                       Network::ConnectionHandler& parent, Event::Dispatcher& dispatcher,
                       Network::ListenerConfig& config);
  ActiveRawUdpListener(uint32_t worker_index, uint32_t concurrency,
                       Network::ConnectionHandler& parent,
                       Network::SocketSharedPtr listen_socket_ptr, Event::Dispatcher& dispatcher,
                       Network::ListenerConfig& config);
  ActiveRawUdpListener(uint32_t worker_index, uint32_t concurrency,
                       Network::ConnectionHandler& parent, Network::Socket& listen_socket,
                       Network::SocketSharedPtr listen_socket_ptr, Event::Dispatcher& dispatcher,
                       Network::ListenerConfig& config);
  ActiveRawUdpListener(uint32_t worker_index, uint32_t concurrency,
                       Network::ConnectionHandler& parent, Network::Socket& listen_socket,
                       Network::UdpListenerPtr&& listener, Network::ListenerConfig& config);

  // Network::UdpListenerCallbacks
  void onReadReady() override;
  void onWriteReady(const Network::Socket& socket) override;
  void onReceiveError(Api::IoError::IoErrorCode error_code) override;
  Network::UdpPacketWriter& udpPacketWriter() override { return *udp_packet_writer_; }

  // Network::UdpWorker
  void onDataWorker(Network::UdpRecvData&& data) override;

  // ActiveListenerImplBase
  void pauseListening() override { udp_listener_->disable(); }
  void resumeListening() override { udp_listener_->enable(); }
  void shutdownListener() override {
    // The read filter should be deleted before the UDP listener is deleted.
    // The read filter refers to the UDP listener to send packets to downstream.
    // If the UDP listener is deleted before the read filter, the read filter may try to use it
    // after deletion.
    read_filter_.reset();
    udp_listener_.reset();
  }

  // Network::UdpListenerFilterManager
  void addReadFilter(Network::UdpListenerReadFilterPtr&& filter) override;

  // Network::UdpReadFilterCallbacks
  Network::UdpListener& udpListener() override;

private:
  Network::UdpListenerReadFilterPtr read_filter_;
  Network::UdpPacketWriterPtr udp_packet_writer_;
};

} // namespace Server
} // namespace Envoy
