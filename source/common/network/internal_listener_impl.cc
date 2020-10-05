#include "common/network/internal_listener_impl.h"

#include "envoy/common/exception.h"
#include "envoy/common/platform.h"
#include "envoy/config/core/v3/base.pb.h"
#include "envoy/network/exception.h"

#include "common/common/assert.h"
#include "common/common/empty_string.h"
#include "common/common/fmt.h"
#include "common/common/utility.h"
#include "common/event/dispatcher_impl.h"
#include "common/network/address_impl.h"
#include "common/network/buffered_io_socket_handle_impl.h"
#include "common/network/io_socket_handle_impl.h"

#include "absl/strings/str_cat.h"

namespace Envoy {
namespace Network {
namespace {
// uint64_t next_internal_connection_id = 0;
}
void InternalListenerImpl::setUpInternalListener() {
  dispatcher_.registerInternalListener(
      internal_listener_id_,
      [this](const Address::InstanceConstSharedPtr&,
             std::unique_ptr<Network::ConnectionSocket> internal_conn_socket) {
        cb_.onNewSocket(std::move(internal_conn_socket));
      });
}

InternalListenerImpl::InternalListenerImpl(Event::DispatcherImpl& dispatcher,
                                           const std::string& listener_id,
                                           InternalListenerCallbacks& cb)
    : BaseListenerImpl(dispatcher, nullptr), internal_listener_id_(listener_id),
      dispatcher_(dispatcher), cb_(cb) {}

void InternalListenerImpl::enable() { NOT_IMPLEMENTED_GCOVR_EXCL_LINE; }

void InternalListenerImpl::disable() { NOT_IMPLEMENTED_GCOVR_EXCL_LINE; }

} // namespace Network
} // namespace Envoy