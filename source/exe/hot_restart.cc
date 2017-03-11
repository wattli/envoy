#include "hot_restart.h"

#include "envoy/event/dispatcher.h"
#include "envoy/event/file_event.h"
#include "envoy/server/instance.h"
#include "envoy/server/options.h"

#include "common/common/utility.h"

#include <sys/mman.h>
#include <sys/prctl.h>

namespace Server {

// Increment this whenever there is a shared memory / RPC change that will prevent a hot restart
// from working. Operations code can then cope with this and do a full restart.
const uint64_t SharedMemory::VERSION = 5;

SharedMemory& SharedMemory::initialize(Options& options) {
  int flags = O_RDWR;
  std::string shmem_name = fmt::format("/envoy_shared_memory_{}", options.baseId());
  if (options.restartEpoch() == 0) {
    flags |= O_CREAT | O_EXCL;

    // If we are meant to be first, attempt to unlink a previous shared memory instance. If this
    // is a clean restart this should then allow the shm_open() call below to succeed.
    shm_unlink(shmem_name.c_str());
  }

  int shmem_fd = shm_open(shmem_name.c_str(), flags, S_IRUSR | S_IWUSR);
  if (shmem_fd == -1) {
    PANIC(fmt::format("cannot open shared memory region {} check user permissions", shmem_name));
  }

  if (options.restartEpoch() == 0) {
    int rc = ftruncate(shmem_fd, sizeof(SharedMemory));
    RELEASE_ASSERT(rc != -1);
    UNREFERENCED_PARAMETER(rc);
  }

  SharedMemory* shmem = reinterpret_cast<SharedMemory*>(
      mmap(nullptr, sizeof(SharedMemory), PROT_READ | PROT_WRITE, MAP_SHARED, shmem_fd, 0));
  RELEASE_ASSERT(shmem != MAP_FAILED);

  if (options.restartEpoch() == 0) {
    shmem->size_ = sizeof(SharedMemory);
    shmem->version_ = VERSION;
    shmem->initializeMutex(shmem->log_lock_);
    shmem->initializeMutex(shmem->access_log_lock_);
    shmem->initializeMutex(shmem->stat_lock_);
  } else {
    RELEASE_ASSERT(shmem->size_ == sizeof(SharedMemory));
    RELEASE_ASSERT(shmem->version_ == VERSION);
  }

  return *shmem;
}

void SharedMemory::initializeMutex(pthread_mutex_t& mutex) {
  pthread_mutexattr_t attribute;
  pthread_mutexattr_init(&attribute);
  pthread_mutexattr_setpshared(&attribute, PTHREAD_PROCESS_SHARED);
  pthread_mutexattr_setrobust(&attribute, PTHREAD_MUTEX_ROBUST);
  pthread_mutex_init(&mutex, &attribute);
}

std::string SharedMemory::version() { return fmt::format("{}.{}", VERSION, sizeof(SharedMemory)); }

HotRestartImpl::HotRestartImpl(Options& options)
    : options_(options), shmem_(SharedMemory::initialize(options)), log_lock_(shmem_.log_lock_),
      access_log_lock_(shmem_.access_log_lock_), stat_lock_(shmem_.stat_lock_) {

  my_domain_socket_ = bindDomainSocket(options.restartEpoch());
  child_address_ = createDomainSocketAddress((options.restartEpoch() + 1));
  if (options.restartEpoch() != 0) {
    parent_address_ = createDomainSocketAddress((options.restartEpoch() + -1));
  }

  // If our parent ever goes away just terminate us so that we don't have to rely on ops/launching
  // logic killing the entire process tree. We should never exist without our parent.
  int rc = prctl(PR_SET_PDEATHSIG, SIGTERM);
  RELEASE_ASSERT(rc != -1);
  UNREFERENCED_PARAMETER(rc);
}

Stats::RawStatData* HotRestartImpl::alloc(const std::string& name) {
  // Try to find the existing slot in shared memory, otherwise allocate a new one.
  std::unique_lock<Thread::BasicLockable> lock(stat_lock_);
  for (Stats::RawStatData& data : shmem_.stats_slots_) {
    if (!data.initialized()) {
      data.initialize(name);
      return &data;
    } else if (data.matches(name)) {
      data.ref_count_++;
      return &data;
    }
  }

  return nullptr;
}

void HotRestartImpl::free(Stats::RawStatData& data) {
  // We must hold the lock since the reference decrement can race with an initialize above.
  std::unique_lock<Thread::BasicLockable> lock(stat_lock_);
  ASSERT(data.ref_count_ > 0);
  if (--data.ref_count_ > 0) {
    return;
  }

  memset(&data, 0, sizeof(Stats::RawStatData));
}

int HotRestartImpl::bindDomainSocket(uint64_t id) {
  // This actually creates the socket and binds it. We use the socket in datagram mode so we can
  // easily read single messages.
  int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0);
  sockaddr_un address = createDomainSocketAddress(id);
  int rc = bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
  if (rc != 0) {
    throw EnvoyException(
        fmt::format("unable to bind domain socket with id={} (see --base-id option)", id));
  }

  return fd;
}

sockaddr_un HotRestartImpl::createDomainSocketAddress(uint64_t id) {
  // Right now we only allow a maximum of 3 concurrent envoy processes to be running. When the third
  // stats up it will kill the oldest parent.
  const uint64_t MAX_CONCURRENT_PROCESSES = 3;
  id = id % MAX_CONCURRENT_PROCESSES;

  // This creates an anonymous domain socket name (where the first byte of the name of \0).
  sockaddr_un address;
  memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;
  StringUtil::strlcpy(&address.sun_path[1],
                      fmt::format("envoy_domain_socket_{}", options_.baseId() + id).c_str(),
                      sizeof(address.sun_path) - 1);
  address.sun_path[0] = 0;
  return address;
}

void HotRestartImpl::drainParentListeners() {
  if (options_.restartEpoch() == 0) {
    return;
  }

  // No reply expected.
  RpcBase rpc(RpcMessageType::DrainListenersRequest);
  sendMessage(parent_address_, rpc);
}

int HotRestartImpl::duplicateParentListenSocket(std::string address) {
  if (options_.restartEpoch() == 0) {
    return -1;
  }

  RpcGetListenSocketRequest rpc;
  memcpy(rpc.address_, address.c_str(), address.length() + 1);
  sendMessage(parent_address_, rpc);
  RpcGetListenSocketReply* reply =
      receiveTypedRpc<RpcGetListenSocketReply, RpcMessageType::GetListenSocketReply>();
  return reply->fd_;
}

void HotRestartImpl::getParentStats(GetParentStatsInfo& info) {
  memset(&info, 0, sizeof(info));
  if (options_.restartEpoch() == 0 || parent_terminated_) {
    return;
  }

  RpcBase rpc(RpcMessageType::GetStatsRequest);
  sendMessage(parent_address_, rpc);
  RpcGetStatsReply* reply = receiveTypedRpc<RpcGetStatsReply, RpcMessageType::GetStatsReply>();
  info.memory_allocated_ = reply->memory_allocated_;
  info.num_connections_ = reply->num_connections_;
}

void HotRestartImpl::initialize(Event::Dispatcher& dispatcher, Server::Instance& server) {
  socket_event_ = dispatcher.createFileEvent(my_domain_socket_, [this](uint32_t events) -> void {
    ASSERT(events == Event::FileReadyType::Read);
    UNREFERENCED_PARAMETER(events);
    onSocketEvent();
  }, Event::FileTriggerType::Edge, Event::FileReadyType::Read);
  server_ = &server;
}

HotRestartImpl::RpcBase* HotRestartImpl::receiveRpc(bool block) {
  // By default the domain socket is non blocking. If we need to block, make it blocking first.
  if (block) {
    int rc = fcntl(my_domain_socket_, F_SETFL, 0);
    RELEASE_ASSERT(rc != -1);
    UNREFERENCED_PARAMETER(rc);
  }

  iovec iov[1];
  iov[0].iov_base = &rpc_buffer_[0];
  iov[0].iov_len = rpc_buffer_.size();

  // We always setup to receive an FD even though most messages do not pass one.
  uint8_t control_buffer[CMSG_SPACE(sizeof(int))];
  memset(control_buffer, 0, CMSG_SPACE(sizeof(int)));

  msghdr message;
  memset(&message, 0, sizeof(message));
  message.msg_iov = iov;
  message.msg_iovlen = 1;
  message.msg_control = control_buffer;
  message.msg_controllen = CMSG_SPACE(sizeof(int));

  int rc = recvmsg(my_domain_socket_, &message, 0);
  if (!block && rc == -1 && errno == EAGAIN) {
    return nullptr;
  }

  RELEASE_ASSERT(rc != -1);
  RELEASE_ASSERT(message.msg_flags == 0);

  // Turn non-blocking back on if we made it blocking.
  if (block) {
    int rc = fcntl(my_domain_socket_, F_SETFL, O_NONBLOCK);
    RELEASE_ASSERT(rc != -1);
    UNREFERENCED_PARAMETER(rc);
  }

  RpcBase* rpc = reinterpret_cast<RpcBase*>(&rpc_buffer_[0]);
  RELEASE_ASSERT(static_cast<uint64_t>(rc) == rpc->length_);

  // We should only get control data in a GetListenSocketReply. If that's the case, pull the
  // cloned fd out of the control data and stick it into the RPC so that higher level code does
  // need to deal with any of this.
  for (cmsghdr* cmsg = CMSG_FIRSTHDR(&message); cmsg != nullptr;
       cmsg = CMSG_NXTHDR(&message, cmsg)) {

    if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS &&
        rpc->type_ == RpcMessageType::GetListenSocketReply) {

      reinterpret_cast<RpcGetListenSocketReply*>(rpc)->fd_ =
          *reinterpret_cast<int*>(CMSG_DATA(cmsg));
    } else {
      RELEASE_ASSERT(false);
    }
  }

  return rpc;
}

void HotRestartImpl::sendMessage(sockaddr_un& address, RpcBase& rpc) {
  iovec iov[1];
  iov[0].iov_base = &rpc;
  iov[0].iov_len = rpc.length_;

  msghdr message;
  memset(&message, 0, sizeof(message));
  message.msg_name = &address;
  message.msg_namelen = sizeof(address);
  message.msg_iov = iov;
  message.msg_iovlen = 1;
  int rc = sendmsg(my_domain_socket_, &message, 0);
  RELEASE_ASSERT(rc != -1);
  UNREFERENCED_PARAMETER(rc);
}

void HotRestartImpl::onGetListenSocket(RpcGetListenSocketRequest& rpc) {
  RpcGetListenSocketReply reply;
  reply.fd_ = server_->getListenSocketFd(std::string(rpc.address_));
  if (reply.fd_ == -1) {
    // In this case there is no fd to duplicate so we just send a normal message.
    sendMessage(child_address_, reply);
  } else {
    iovec iov[1];
    iov[0].iov_base = &reply;
    iov[0].iov_len = reply.length_;

    uint8_t control_buffer[CMSG_SPACE(sizeof(int))];
    memset(control_buffer, 0, CMSG_SPACE(sizeof(int)));

    msghdr message;
    memset(&message, 0, sizeof(message));
    message.msg_name = &child_address_;
    message.msg_namelen = sizeof(child_address_);
    message.msg_iov = iov;
    message.msg_iovlen = 1;
    message.msg_control = control_buffer;
    message.msg_controllen = CMSG_SPACE(sizeof(int));

    cmsghdr* control_message = CMSG_FIRSTHDR(&message);
    control_message->cmsg_level = SOL_SOCKET;
    control_message->cmsg_type = SCM_RIGHTS;
    control_message->cmsg_len = CMSG_LEN(sizeof(int));
    *reinterpret_cast<int*>(CMSG_DATA(control_message)) = reply.fd_;

    int rc = sendmsg(my_domain_socket_, &message, 0);
    RELEASE_ASSERT(rc != -1);
    UNREFERENCED_PARAMETER(rc);
  }
}

void HotRestartImpl::onSocketEvent() {
  while (true) {
    RpcBase* base_message = receiveRpc(false);
    if (!base_message) {
      return;
    }

    switch (base_message->type_) {
    case RpcMessageType::ShutdownAdminRequest: {
      server_->shutdownAdmin();
      RpcShutdownAdminReply rpc;
      rpc.original_start_time_ = server_->startTimeFirstEpoch();
      sendMessage(child_address_, rpc);
      break;
    }

    case RpcMessageType::GetListenSocketRequest: {
      RpcGetListenSocketRequest* message =
          reinterpret_cast<RpcGetListenSocketRequest*>(base_message);
      onGetListenSocket(*message);
      break;
    }

    case RpcMessageType::GetStatsRequest: {
      GetParentStatsInfo info;
      server_->getParentStats(info);
      RpcGetStatsReply rpc;
      rpc.memory_allocated_ = info.memory_allocated_;
      rpc.num_connections_ = info.num_connections_;
      sendMessage(child_address_, rpc);
      break;
    }

    case RpcMessageType::DrainListenersRequest: {
      server_->drainListeners();
      break;
    }

    case RpcMessageType::TerminateRequest: {
      log().warn("shutting down due to child request");
      kill(getpid(), SIGTERM);
      break;
    }

    default: {
      RpcBase rpc(RpcMessageType::UnknownRequestReply);
      sendMessage(child_address_, rpc);
      break;
    }
    }
  }
}

void HotRestartImpl::shutdownParentAdmin(ShutdownParentAdminInfo& info) {
  if (options_.restartEpoch() == 0) {
    return;
  }

  RpcBase rpc(RpcMessageType::ShutdownAdminRequest);
  sendMessage(parent_address_, rpc);
  RpcShutdownAdminReply* reply =
      receiveTypedRpc<RpcShutdownAdminReply, RpcMessageType::ShutdownAdminReply>();
  info.original_start_time_ = reply->original_start_time_;
}

void HotRestartImpl::terminateParent() {
  if (options_.restartEpoch() == 0 || parent_terminated_) {
    return;
  }

  RpcBase rpc(RpcMessageType::TerminateRequest);
  sendMessage(parent_address_, rpc);
  parent_terminated_ = true;
}

std::string HotRestartImpl::version() { return SharedMemory::version(); }

} // Server
