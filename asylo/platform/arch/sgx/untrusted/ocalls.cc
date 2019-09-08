/*
 *
 * Copyright 2018 Asylo authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

// Stubs invoked by edger8r generated bridge code for ocalls.

// For |domainname| field in struct utsname and pipe2().
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <poll.h>
#include <pwd.h>
#include <sched.h>
#include <sys/epoll.h>
#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <syslog.h>
#include <unistd.h>
#include <utime.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iterator>
#include <vector>

#include "absl/memory/memory.h"
#include "asylo/enclave.pb.h"
#include "asylo/platform/arch/sgx/untrusted/generated_bridge_u.h"
#include "asylo/platform/arch/sgx/untrusted/sgx_client.h"
#include "asylo/platform/common/bridge_functions.h"
#include "asylo/platform/common/bridge_proto_serializer.h"
#include "asylo/platform/common/bridge_types.h"
#include "asylo/platform/common/debug_strings.h"
#include "asylo/platform/common/memory.h"
#include "asylo/platform/core/enclave_manager.h"
#include "asylo/platform/core/shared_name.h"
#include "asylo/platform/storage/utils/fd_closer.h"
#include "asylo/util/posix_error_space.h"
#include "asylo/util/status.h"
#include "asylo/util/status_macros.h"

#include "asylo/util/logging.h"

namespace {

// Stores a pointer to a function inside the enclave that translates
// |bridge_signum| to a value inside the enclave and calls the registered signal
// handler for that signal.
static void (*handle_signal_inside_enclave)(int, bridge_siginfo_t *,
                                            void *) = nullptr;

// Translates host |signum| to |bridge_signum|, and calls the function
// registered as the signal handler inside the enclave.
void TranslateToBridgeAndHandleSignal(int signum, siginfo_t *info,
                                      void *ucontext) {
  int bridge_signum = asylo::ToBridgeSignal(signum);
  if (bridge_signum < 0) {
    // Invalid incoming signal number.
    return;
  }
  struct bridge_siginfo_t bridge_siginfo;
  asylo::ToBridgeSigInfo(info, &bridge_siginfo);
  if (handle_signal_inside_enclave) {
    handle_signal_inside_enclave(bridge_signum, &bridge_siginfo, ucontext);
  }
}

// Triggers an ecall to enter an enclave to handle the incoming signal.
//
// In hardware mode, this is registered as the signal handler.
// In simulation mode, this is called if the signal arrives when the TCS is
// inactive.
void EnterEnclaveAndHandleSignal(int signum, siginfo_t *info, void *ucontext) {
  asylo::EnclaveSignalDispatcher::GetInstance()->EnterEnclaveAndHandleSignal(
      signum, info, ucontext);
}

// Checks the enclave TCS state to determine which function to call to handle
// the signal. If the TCS is active, calls the signal handler registered inside
// the enclave directly. If the TCS is inactive, triggers an ecall to enter
// enclave and handle the signal.
//
// In simulation mode, this is registered as the signal handler.
void HandleSignalInSim(int signum, siginfo_t *info, void *ucontext) {
  auto client_result =
      asylo::EnclaveSignalDispatcher::GetInstance()->GetClientForSignal(signum);
  if (!client_result.ok()) {
    return;
  }
  asylo::SgxClient *client =
      dynamic_cast<asylo::SgxClient *>(client_result.ValueOrDie());
  if (client->IsTcsActive()) {
    TranslateToBridgeAndHandleSignal(signum, info, ucontext);
  } else {
    EnterEnclaveAndHandleSignal(signum, info, ucontext);
  }
}

// Perform a snapshot key transfer from the parent to the child.
asylo::Status DoSnapshotKeyTransfer(asylo::EnclaveManager *manager,
                                    asylo::EnclaveClient *client,
                                    int self_socket, int peer_socket,
                                    bool is_parent) {
  asylo::platform::storage::FdCloser self_socket_closer(self_socket);
  // Close the socket for the other side, and enters the enclave to send the
  // snapshot key through the socket.
  if (close(peer_socket) < 0) {
    return asylo::Status(static_cast<asylo::error::PosixError>(errno),
                         absl::StrCat("close failed: ", strerror(errno)));
  }

  asylo::ForkHandshakeConfig fork_handshake_config;
  fork_handshake_config.set_is_parent(is_parent);
  fork_handshake_config.set_socket(self_socket);
  asylo::SgxClient *sgx_client = dynamic_cast<asylo::SgxClient *>(client);
  ASYLO_RETURN_IF_ERROR(sgx_client->EnterAndTransferSecureSnapshotKey(
      fork_handshake_config));

  return asylo::Status::OkStatus();
}

// A helper class to free the snapshot memory allocated during fork.
class SnapshotDataDeleter {
 public:
  explicit SnapshotDataDeleter(const asylo::SnapshotLayoutEntry &entry)
      : ciphertext_deleter_(reinterpret_cast<void *>(entry.ciphertext_base())),
        nonce_deleter_(reinterpret_cast<void *>(entry.nonce_base())) {}

 private:
  asylo::MallocUniquePtr<void> ciphertext_deleter_;
  asylo::MallocUniquePtr<void> nonce_deleter_;
};

}  // namespace

// Threading implementation-defined untrusted thread donate routine.
extern "C" int __asylo_donate_thread(const char *name);

//////////////////////////////////////
//              IO                  //
//////////////////////////////////////

int ocall_enc_untrusted_puts(const char *str) {
  int rc = puts(str);
  // This routine is intended for debugging, so flush immediately to ensure
  // output is written in the event the enclave aborts with buffered output.
  fflush(stdout);
  return rc;
}

void *ocall_enc_untrusted_malloc(bridge_size_t size) {
  void *ret = malloc(static_cast<size_t>(size));
  return ret;
}

void **ocall_enc_untrusted_allocate_buffers(bridge_size_t count,
                                            bridge_size_t size) {
  void **buffers = reinterpret_cast<void **>(
      malloc(static_cast<size_t>(count) * sizeof(void*)));
  for (int i = 0; i < count; i++) {
    buffers[i] = malloc(size);
  }
  return buffers;
}

void ocall_enc_untrusted_deallocate_free_list(void **free_list,
                                              bridge_size_t count) {
  // This function only releases memory on the untrusted heap pointed to by
  // buffer pointers stored in |free_list|, not freeing the |free_list| object
  // itself. The client making the host call is responsible for the deallocation
  // of the |free list| object.
  for (int i = 0; i < count; i++) {
    free(free_list[i]);
  }
}

int ocall_enc_untrusted_open(const char *path_name, int flags, uint32_t mode) {
  int host_flags = asylo::FromBridgeFileFlags(flags);
  int ret = open(path_name, host_flags, mode);
  return ret;
}

int ocall_enc_untrusted_fcntl(int fd, int bridge_cmd, int64_t arg) {
  int cmd = asylo::FromBridgeFcntlCmd(bridge_cmd);
  if (cmd == -1) {
    errno = EINVAL;
    return -1;
  }

  int ret;
  switch (cmd) {
    case F_SETFL:
      ret = fcntl(fd, cmd, asylo::FromBridgeFileFlags(arg));
      break;
    case F_SETFD:
      ret = fcntl(fd, cmd, asylo::FromBridgeFDFlags(arg));
      break;
    case F_GETFL:
      ret = fcntl(fd, cmd, arg);
      if (ret != -1) {
        ret = asylo::ToBridgeFileFlags(ret);
      }
      break;
    case F_GETFD:
      ret = fcntl(fd, cmd, arg);
      if (ret != -1) {
        ret = asylo::ToBridgeFDFlags(ret);
      }
      break;
    case F_GETPIPE_SZ:
      ret = fcntl(fd, cmd, arg);
      break;
    case F_SETPIPE_SZ:
      ret = fcntl(fd, cmd, arg);
      break;
    default:
      errno = EINVAL;
      return -1;
  }
  return ret;
}

int ocall_enc_untrusted_stat(const char *pathname,
                             struct bridge_stat *stat_buffer) {
  struct stat host_stat_buffer;
  int ret = stat(pathname, &host_stat_buffer);
  asylo::ToBridgeStat(&host_stat_buffer, stat_buffer);
  return ret;
}

int ocall_enc_untrusted_fstat(int fd, struct bridge_stat *stat_buffer) {
  struct stat host_stat_buffer;
  int ret = fstat(fd, &host_stat_buffer);
  asylo::ToBridgeStat(&host_stat_buffer, stat_buffer);
  return ret;
}

int ocall_enc_untrusted_lstat(const char *pathname,
                              struct bridge_stat *stat_buffer) {
  struct stat host_stat_buffer;
  int ret = lstat(pathname, &host_stat_buffer);
  asylo::ToBridgeStat(&host_stat_buffer, stat_buffer);
  return ret;
}

bridge_ssize_t ocall_enc_untrusted_write_with_untrusted_ptr(int fd,
                                                            const void *buf,
                                                            int size) {
  return static_cast<bridge_ssize_t>(write(fd, buf, size));
}

bridge_ssize_t ocall_enc_untrusted_read_with_untrusted_ptr(int fd, void *buf,
                                                           int size) {
  return static_cast<bridge_ssize_t>(read(fd, buf, size));
}

//////////////////////////////////////
//             Sockets              //
//////////////////////////////////////

int ocall_enc_untrusted_socket(int domain, int type, int protocol) {
  return socket(asylo::FromBridgeAfFamily(domain),
                asylo::FromBridgeSocketType(type), protocol);
}

int ocall_enc_untrusted_connect(int sockfd,
                                const struct bridge_sockaddr *bridge_addr) {
  struct sockaddr_storage tmp;
  socklen_t len = sizeof(tmp);
  struct sockaddr *addr = asylo::FromBridgeSockaddr(
      bridge_addr, reinterpret_cast<struct sockaddr *>(&tmp), &len);

  LOG_IF(FATAL, addr == nullptr) << "Unexpected bridge failure";
  LOG_IF(FATAL, len > sizeof(tmp)) << "Insufficient sockaddr buf space";

  int ret = connect(sockfd, addr, len);
  return ret;
}

int ocall_enc_untrusted_bind(int sockfd,
                             const struct bridge_sockaddr *bridge_addr) {
  struct sockaddr_storage tmp;
  socklen_t len = sizeof(tmp);
  struct sockaddr *addr = asylo::FromBridgeSockaddr(
      bridge_addr, reinterpret_cast<struct sockaddr *>(&tmp), &len);

  LOG_IF(FATAL, addr == nullptr) << "Unexpected bridge failure";
  LOG_IF(FATAL, len > sizeof(tmp)) << "Insufficient sockaddr buf space";

  int ret = bind(sockfd, addr, len);
  return ret;
}

int ocall_enc_untrusted_accept(int sockfd, struct bridge_sockaddr *addr) {
  struct sockaddr_storage tmp;
  socklen_t tmp_len = sizeof(tmp);
  int ret = accept(sockfd, reinterpret_cast<struct sockaddr *>(&tmp), &tmp_len);
  if (ret == -1) {
    return ret;
  }
  if (!asylo::ToBridgeSockaddr(reinterpret_cast<struct sockaddr *>(&tmp),
                               sizeof(tmp), addr)) {
    errno = EFAULT;
    return -1;
  }
  return ret;
}

bridge_ssize_t ocall_enc_untrusted_sendmsg(int sockfd,
                                           const struct bridge_msghdr *msg,
                                           int flags) {
  struct msghdr tmp;
  if (!asylo::FromBridgeMsgHdr(msg, &tmp)) {
    errno = EFAULT;
    return -1;
  }
  auto buf = absl::make_unique<struct iovec[]>(msg->msg_iovlen);
  for (int i = 0; i < msg->msg_iovlen; ++i) {
    if (!asylo::FromBridgeIovec(&msg->msg_iov[i], &buf[i])) {
      errno = EFAULT;
      return -1;
    }
  }
  tmp.msg_iov = buf.get();
  bridge_ssize_t ret =
      static_cast<bridge_ssize_t>(sendmsg(sockfd, &tmp, flags));
  return ret;
}

bridge_ssize_t ocall_enc_untrusted_recvmsg(int sockfd,
                                           struct bridge_msghdr *msg,
                                           int flags) {
  struct msghdr tmp;
  if (!asylo::FromBridgeMsgHdr(msg, &tmp)) {
    errno = EFAULT;
    return -1;
  }
  auto buf = absl::make_unique<struct iovec[]>(msg->msg_iovlen);
  for (int i = 0; i < msg->msg_iovlen; ++i) {
    if (!asylo::FromBridgeIovec(&msg->msg_iov[i], &buf[i])) {
      errno = EFAULT;
      return -1;
    }
  }
  tmp.msg_iov = buf.get();
  bridge_ssize_t ret =
      static_cast<bridge_ssize_t>(recvmsg(sockfd, &tmp, flags));
  if (!asylo::ToBridgeIovecArray(&tmp, msg)) {
    errno = EFAULT;
    return -1;
  }
  return ret;
}

char *ocall_enc_untrusted_inet_ntop(int af, const void *src,
                                    bridge_size_t src_size, char *dst,
                                    bridge_size_t buf_size) {
  // src_size is needed so edgr8r copes the correct number of bytes out of the
  // enclave. This suppresses unused variable errors.
  (void)src_size;
  const char *ret = inet_ntop(af, src, dst, static_cast<size_t>(buf_size));
  // edgr8r does not support returning const char*
  return const_cast<char *>(ret);
}

int ocall_enc_untrusted_inet_pton(AfFamily af, const char *src, void *dst,
                                  bridge_size_t dst_size) {
  // The line below is needed to surpress unused variable errors, as |dst_size|
  // is needed for the edgr8r generated code.
  (void) dst_size;
  return inet_pton(asylo::FromBridgeAfFamily(af), src, dst);
}

int ocall_enc_untrusted_getaddrinfo(const char *node, const char *service,
                                    const char *serialized_hints,
                                    bridge_size_t serialized_hints_len,
                                    char **serialized_res_start,
                                    bridge_size_t *serialized_res_len) {
  struct addrinfo *hints;
  std::string tmp_serialized_hints(serialized_hints,
                                   static_cast<size_t>(serialized_hints_len));
  if (!asylo::DeserializeAddrinfo(&tmp_serialized_hints, &hints)) {
    return -1;
  }

  struct addrinfo *res;
  int ret = getaddrinfo(node, service, hints, &res);
  if (ret != 0) {
    return asylo::ToBridgeAddressInfoErrors(ret);
  }
  asylo::FreeDeserializedAddrinfo(hints);

  std::string tmp_serialized_res;
  int bridge_error_code = -1;
  if (!asylo::SerializeAddrinfo(res, &tmp_serialized_res, &bridge_error_code)) {
    return bridge_error_code;
  }
  freeaddrinfo(res);

  // Allocate memory for the enclave to copy the result; enclave will free this.
  size_t tmp_serialized_res_len = tmp_serialized_res.length();
  char *serialized_res = static_cast<char *>(malloc(tmp_serialized_res_len));
  memcpy(serialized_res, tmp_serialized_res.c_str(), tmp_serialized_res_len);
  *serialized_res_start = serialized_res;
  *serialized_res_len = static_cast<bridge_size_t>(tmp_serialized_res_len);
  return 0;
}

int ocall_enc_untrusted_getsockopt(int sockfd, int level, int optname,
                                   void *optval, unsigned int optlen_in,
                                   unsigned int *optlen_out) {
  int ret =
      getsockopt(sockfd, level, asylo::FromBridgeOptionName(level, optname),
                 optval, reinterpret_cast<socklen_t *>(&optlen_in));
  *optlen_out = optlen_in;
  return ret;
}

int ocall_enc_untrusted_setsockopt(int sockfd, int level, int optname,
                                   const void *optval, bridge_size_t optlen) {
  return setsockopt(sockfd, level, asylo::FromBridgeOptionName(level, optname),
                    optval, static_cast<socklen_t>(optlen));
}

int ocall_enc_untrusted_getsockname(int sockfd, struct bridge_sockaddr *addr) {
  struct sockaddr_storage tmp;
  socklen_t tmp_len = sizeof(tmp);
  int ret =
      getsockname(sockfd, reinterpret_cast<struct sockaddr *>(&tmp), &tmp_len);

  LOG_IF(FATAL, tmp_len > sizeof(tmp)) << "Insufficient sockaddr buf space";

  // Only marshal the sockaddr if a valid one was returned.
  if (ret == 0) {
    if (!asylo::ToBridgeSockaddr(reinterpret_cast<struct sockaddr *>(&tmp),
                                 sizeof(tmp), addr)) {
      errno = EFAULT;
      return -1;
    }
  }
  return ret;
}

int ocall_enc_untrusted_getpeername(int sockfd, struct bridge_sockaddr *addr) {
  struct sockaddr_storage tmp;
  socklen_t tmp_len = sizeof(tmp);
  int ret =
      getpeername(sockfd, reinterpret_cast<struct sockaddr *>(&tmp), &tmp_len);
  if (ret == 0) {
    if (!asylo::ToBridgeSockaddr(reinterpret_cast<struct sockaddr *>(&tmp),
                                 tmp_len, addr)) {
      errno = EFAULT;
      return -1;
    }
  }
  return ret;
}

ssize_t ocall_enc_untrusted_recvfrom(const char *serialized_args,
                                     bridge_ssize_t serialized_args_len,
                                     char **buf_ptr, char **serialized_output,
                                     bridge_ssize_t *serialized_output_len) {
  std::string serialized_args_str(serialized_args, serialized_args_len);
  int sockfd = 0;
  size_t len = 0;
  int flags = 0;
  if (!asylo::DeserializeRecvFromArgs(serialized_args, &sockfd, &len, &flags) ||
      !buf_ptr) {
    errno = EINVAL;
    return -1;
  }
  *buf_ptr = static_cast<char *>(malloc(len));
  if (serialized_output) {
    struct sockaddr_storage src_addr;
    struct sockaddr *src_addr_ptr =
        reinterpret_cast<struct sockaddr *>(&src_addr);
    socklen_t addrlen;
    int ret = recvfrom(sockfd, *buf_ptr, len, flags, src_addr_ptr, &addrlen);
    size_t src_addr_len = 0;
    // If the address family is unsupported, then errno is set to indicate an
    // invalid argument error. The value of error_code is irrelevant in this
    // context.
    int error_code;
    // The caller is responsible for freeing the memory allocated by
    // SerializeRecvFromSrcAddr.
    if (!asylo::SerializeRecvFromSrcAddr(src_addr_ptr, serialized_output,
                                         &src_addr_len, &error_code)) {
      errno = EINVAL;
      return -1;
    }
    *serialized_output_len = static_cast<bridge_size_t>(src_addr_len);
    return ret;
  } else {
    return recvfrom(sockfd, *buf_ptr, len, flags, nullptr, nullptr);
  }
}

//////////////////////////////////////
//           poll.h                 //
//////////////////////////////////////

int ocall_enc_untrusted_poll(struct bridge_pollfd *fds, unsigned int nfds,
                             int timeout) {
  auto tmp = absl::make_unique<pollfd[]>(nfds);
  for (int i = 0; i < nfds; ++i) {
    if (!asylo::FromBridgePollfd(&fds[i], &tmp[i])) {
      errno = EFAULT;
      return -1;
    }
  }
  int ret = poll(tmp.get(), nfds, timeout);
  for (int i = 0; i < nfds; ++i) {
    if (!asylo::ToBridgePollfd(&tmp[i], &fds[i])) {
      errno = EFAULT;
      return -1;
    }
  }
  return ret;
}

//////////////////////////////////////
//           epoll.h                //
//////////////////////////////////////

int ocall_enc_untrusted_epoll_create(int size) { return epoll_create(size); }

int ocall_enc_untrusted_epoll_ctl(const char *serialized_args,
                                  bridge_size_t serialized_args_len) {
  std::string serialized_args_str(serialized_args,
                                  static_cast<size_t>(serialized_args_len));
  int epfd = 0;
  int op = 0;
  int hostfd = 0;
  struct epoll_event event;
  if (!asylo::DeserializeEpollCtlArgs(serialized_args_str, &epfd, &op, &hostfd,
                                      &event)) {
    errno = EINVAL;
    return -1;
  }
  return epoll_ctl(epfd, op, hostfd, &event);
}

int ocall_enc_untrusted_epoll_wait(const char *serialized_args,
                                   bridge_size_t serialized_args_len,
                                   char **serialized_events,
                                   bridge_size_t *serialized_events_len) {
  int epfd = 0;
  int maxevents = 0;
  int timeout = 0;
  std::string serialized_args_str(serialized_args,
                                  static_cast<size_t>(serialized_args_len));
  if (!asylo::DeserializeEpollWaitArgs(serialized_args_str, &epfd, &maxevents,
                                       &timeout)) {
    errno = EINVAL;
    return -1;
  }
  struct epoll_event *event_array = static_cast<struct epoll_event *>(
      malloc(sizeof(struct epoll_event) * maxevents));
  asylo::MallocUniquePtr<struct epoll_event> event_array_ptr(event_array);
  int ret = epoll_wait(epfd, event_array, maxevents, timeout);
  size_t len = 0;
  if (!asylo::SerializeEvents(event_array, ret, serialized_events, &len)) {
    errno = EINVAL;
    return -1;
  }
  *serialized_events_len = static_cast<bridge_size_t>(len);
  return ret;
}

//////////////////////////////////////
//           inotify.h              //
//////////////////////////////////////

int ocall_enc_untrusted_inotify_init1(int non_block) {
  int flags = non_block ? IN_NONBLOCK : 0;
  return inotify_init1(flags);
}

int ocall_enc_untrusted_inotify_add_watch(const char *serialized_args,
                                          bridge_size_t serialized_args_len) {
  std::string serialized_args_str(serialized_args, serialized_args_len);
  int fd = 0;
  char *pathname = nullptr;
  asylo::MallocUniquePtr<char> pathname_ptr(pathname);
  uint32_t mask = 0;
  if (!asylo::DeserializeInotifyAddWatchArgs(serialized_args_str, &fd,
                                             &pathname, &mask)) {
    errno = EINVAL;
    return -1;
  }
  return inotify_add_watch(fd, pathname, mask);
}

int ocall_enc_untrusted_inotify_rm_watch(const char *serialized_args,
                                         bridge_size_t serialized_args_len) {
  std::string serialized_args_str(serialized_args, serialized_args_len);
  int fd = 0;
  int wd = 0;
  if (!asylo::DeserializeInotifyRmWatchArgs(serialized_args_str, &fd, &wd)) {
    errno = EINVAL;
    return -1;
  }
  return inotify_rm_watch(fd, wd);
}

int ocall_enc_untrusted_inotify_read(int fd, bridge_size_t count,
                                     char **serialized_events,
                                     bridge_size_t *serialized_events_len) {
  size_t buf_size =
      std::max(sizeof(struct inotify_event) + NAME_MAX + 1, count);
  char *buf = static_cast<char *>(malloc(buf_size));
  asylo::MallocUniquePtr<char> buf_ptr(buf);
  int bytes_read = read(fd, buf, buf_size);
  if (bytes_read < 0) {
    // Errno will be set by read.
    return -1;
  }
  size_t len = 0;
  if (!asylo::SerializeInotifyEvents(buf, bytes_read, serialized_events,
                                     &len)) {
    return -1;
  }
  *serialized_events_len = static_cast<bridge_size_t>(len);
  return 0;
}

//////////////////////////////////////
//           ifaddrs.h              //
//////////////////////////////////////

int ocall_enc_untrusted_getifaddrs(char **serialized_ifaddrs,
                                   bridge_ssize_t *serialized_ifaddrs_len) {
  struct ifaddrs *ifaddr_list = nullptr;
  int ret = getifaddrs(&ifaddr_list);
  if (ret != 0) {
    return -1;
  }
  size_t len = 0;
  if (!asylo::SerializeIfAddrs(ifaddr_list, serialized_ifaddrs, &len)) {
    freeifaddrs(ifaddr_list);
    return -1;
  }
  *serialized_ifaddrs_len = static_cast<bridge_ssize_t>(len);
  freeifaddrs(ifaddr_list);
  return ret;
}

//////////////////////////////////////
//            pwd.h                 //
//////////////////////////////////////

int ocall_enc_untrusted_getpwuid(uid_t uid,
                                 struct BridgePassWd *bridge_password) {
  struct passwd *password = getpwuid(uid);
  if (!password) {
    return -1;
  }
  if (!asylo::ToBridgePassWd(password, bridge_password)) {
    errno = EFAULT;
    return -1;
  }
  return 0;
}

//////////////////////////////////////
//           sched.h                //
//////////////////////////////////////

int ocall_enc_untrusted_sched_getaffinity(int64_t pid, BridgeCpuSet *mask) {
  cpu_set_t host_mask;
  if (BRIDGE_CPU_SET_MAX_CPUS != CPU_SETSIZE) {
    LOG(ERROR) << "sched_getaffinity: CPU_SETSIZE (" << CPU_SETSIZE
               << ") is not equal to " << BRIDGE_CPU_SET_MAX_CPUS;
    errno = ENOSYS;
    return -1;
  }

  int ret =
      sched_getaffinity(static_cast<pid_t>(pid), sizeof(cpu_set_t), &host_mask);

  // Translate from host cpu_set_t to bridge_cpu_set_t.
  int total_cpus = CPU_COUNT(&host_mask);
  asylo::BridgeCpuSetZero(mask);
  for (int cpu = 0, cpus_so_far = 0; cpus_so_far < total_cpus; ++cpu) {
    if (CPU_ISSET(cpu, &host_mask)) {
      asylo::BridgeCpuSetAddBit(cpu, mask);
      ++cpus_so_far;
    }
  }

  return ret;
}

//////////////////////////////////////
//          signal.h                //
//////////////////////////////////////

int ocall_enc_untrusted_register_signal_handler(
    int bridge_signum, const struct BridgeSignalHandler *handler,
    const char *name) {
  std::string enclave_name(name);
  int signum = asylo::FromBridgeSignal(bridge_signum);
  if (signum < 0) {
    errno = EINVAL;
    return -1;
  }
  auto manager_result = asylo::EnclaveManager::Instance();
  if (!manager_result.ok()) {
    return -1;
  }
  // Registers the signal with an enclave so when the signal arrives,
  // EnclaveManager knows which enclave to enter to handle the signal.
  asylo::EnclaveManager *manager = manager_result.ValueOrDie();
  asylo::EnclaveClient *client = manager->GetClient(enclave_name);
  const asylo::EnclaveClient *old_client =
      asylo::EnclaveSignalDispatcher::GetInstance()->RegisterSignal(signum,
                                                                    client);
  if (old_client) {
    LOG(WARNING) << "Overwriting the signal handler for signal: " << signum
                 << " registered by enclave: " << manager->GetName(old_client);
  }
  struct sigaction newact;
  if (!handler || !handler->sigaction) {
    // Hardware mode: The registered signal handler triggers an ecall to enter
    // the enclave and handle the signal.
    newact.sa_sigaction = &EnterEnclaveAndHandleSignal;
  } else {
    // Simulation mode: The registered signal handler does the same as hardware
    // mode if the TCS is active, or calls the signal handler registered inside
    // the enclave directly if the TCS is inactive.
    handle_signal_inside_enclave = handler->sigaction;
    newact.sa_sigaction = &HandleSignalInSim;
  }
  if (handler) {
    asylo::FromBridgeSigSet(&handler->mask, &newact.sa_mask);
  }
  // Set the flag so that sa_sigaction is registered as the signal handler
  // instead of sa_handler.
  newact.sa_flags = asylo::FromBridgeSignalFlags(handler->flags);
  newact.sa_flags |= SA_SIGINFO;
  struct sigaction oldact;
  return sigaction(signum, &newact, &oldact);
}

int ocall_enc_untrusted_sigprocmask(int how, const bridge_sigset_t *set,
                                    bridge_sigset_t *oldset) {
  sigset_t tmp_set;
  asylo::FromBridgeSigSet(set, &tmp_set);
  sigset_t tmp_oldset;
  int ret =
      sigprocmask(asylo::FromBridgeSigMaskAction(how), &tmp_set, &tmp_oldset);
  asylo::ToBridgeSigSet(&tmp_oldset, oldset);
  return ret;
}

int ocall_enc_untrusted_raise(int bridge_sig) {
  int sig = asylo::FromBridgeSignal(bridge_sig);
  if (sig < 0) {
    errno = EINVAL;
    return -1;
  }
  return raise(sig);
}

//////////////////////////////////////
//         sys/resource.h           //
//////////////////////////////////////

int ocall_enc_untrusted_getrusage(enum RUsageTarget who,
                                  struct BridgeRUsage *bridge_usage) {
  struct rusage usage;
  int ret = getrusage(asylo::FromBridgeRUsageTarget(who), &usage);
  asylo::ToBridgeRUsage(&usage, bridge_usage);
  return ret;
}

//////////////////////////////////////
//           sys/file.h             //
//////////////////////////////////////

int ocall_enc_untrusted_flock(int fd, int operation) {
  return flock(fd, asylo::FromBridgeFLockOperation(operation));
}

//////////////////////////////////////
//          sys/select.h            //
//////////////////////////////////////

int ocall_enc_untrusted_select(int nfds, BridgeFDSet *bridge_readfds,
                               BridgeFDSet *bridge_writefds,
                               BridgeFDSet *bridge_exceptfds,
                               bridge_timeval *bridge_timeout) {
  fd_set readfds, writefds, exceptfds;
  if (bridge_readfds && !asylo::FromBridgeFDSet(bridge_readfds, &readfds)) {
    errno = EBADE;
    return -1;
  }
  if (bridge_writefds && !asylo::FromBridgeFDSet(bridge_writefds, &writefds)) {
    errno = EBADE;
    return -1;
  }
  if (bridge_exceptfds &&
      !asylo::FromBridgeFDSet(bridge_exceptfds, &exceptfds)) {
    errno = EBADE;
    return -1;
  }

  struct timeval timeout;
  if (!asylo::FromBridgeTimeVal(bridge_timeout, &timeout)) {
    errno = EBADE;
    return -1;
  }
  int ret = select(nfds, &readfds, &writefds, &exceptfds, &timeout);

  if (bridge_readfds && !asylo::ToBridgeFDSet(&readfds, bridge_readfds)) {
    errno = EBADE;
    return -1;
  }
  if (bridge_writefds && !asylo::ToBridgeFDSet(&writefds, bridge_writefds)) {
    errno = EBADE;
    return -1;
  }
  if (bridge_exceptfds && !asylo::ToBridgeFDSet(&exceptfds, bridge_exceptfds)) {
    errno = EBADE;
    return -1;
  }

  return ret;
}

//////////////////////////////////////
//          sys/syslog.h            //
//////////////////////////////////////

void ocall_enc_untrusted_openlog(const char *ident, int option, int facility) {
  openlog(ident, asylo::FromBridgeSysLogOption(option),
          asylo::FromBridgeSysLogFacility(facility));
}

void ocall_enc_untrusted_syslog(int priority, const char *message) {
  syslog(asylo::FromBridgeSysLogPriority(priority), "%s", message);
}

//////////////////////////////////////
//           time.h                 //
//////////////////////////////////////

int ocall_enc_untrusted_nanosleep(const struct bridge_timespec *req,
                                  struct bridge_timespec *rem) {
  int ret = nanosleep(reinterpret_cast<const struct timespec *>(req),
                      reinterpret_cast<struct timespec *>(rem));
  return ret;
}

int ocall_enc_untrusted_times(struct BridgeTms *bridge_buf) {
  struct tms buf;
  int ret = times(&buf);
  if (!asylo::ToBridgeTms(&buf, bridge_buf)) {
    errno = EFAULT;
    return -1;
  }
  return ret;
}

int ocall_enc_untrusted_clock_gettime(bridge_clockid_t clk_id,
                                      struct bridge_timespec *tp) {
  int ret = clock_gettime(static_cast<clockid_t>(clk_id),
                          reinterpret_cast<struct timespec *>(tp));
  return ret;
}

int ocall_enc_untrusted_getitimer(enum TimerType which,
                                  struct BridgeITimerVal *bridge_curr_value) {
  itimerval curr_value;
  int ret = getitimer(asylo::FromBridgeTimerType(which), &curr_value);
  if (bridge_curr_value == nullptr ||
      !asylo::ToBridgeITimerVal(&curr_value, bridge_curr_value)) {
    errno = EFAULT;
    return -1;
  }
  return ret;
}

int ocall_enc_untrusted_setitimer(enum TimerType which,
                                  struct BridgeITimerVal *bridge_new_value,
                                  struct BridgeITimerVal *bridge_old_value) {
  itimerval new_value, old_value;
  if (!asylo::FromBridgeITimerVal(bridge_new_value, &new_value)) {
    errno = EFAULT;
    return -1;
  }
  int ret =
      setitimer(asylo::FromBridgeTimerType(which), &new_value, &old_value);
  if (bridge_old_value != nullptr &&
      !asylo::ToBridgeITimerVal(&old_value, bridge_old_value)) {
    errno = EFAULT;
    return -1;
  }
  return ret;
}

//////////////////////////////////////
//           sys/time.h             //
//////////////////////////////////////

int ocall_enc_untrusted_gettimeofday(struct bridge_timeval *tv, void *tz) {
  return gettimeofday(reinterpret_cast<struct timeval *>(tv), nullptr);
}

//////////////////////////////////////
//         sys/utsname.h            //
//////////////////////////////////////

int ocall_enc_untrusted_uname(struct BridgeUtsName *bridge_utsname_val) {
  if (!bridge_utsname_val) {
    errno = EFAULT;
    return -1;
  }

  struct utsname utsname_val;
  int ret = uname(&utsname_val);
  if (ret != 0) {
    return ret;
  }

  if (!asylo::ConvertUtsName(utsname_val, bridge_utsname_val)) {
    errno = EINTR;
    return -1;
  }

  return ret;
}

//////////////////////////////////////
//            unistd.h              //
//////////////////////////////////////

int ocall_enc_untrusted_pipe2(int pipefd[2], int flags) {
  int ret = pipe2(pipefd, asylo::FromBridgeFileFlags(flags));
  return ret;
}

int64_t ocall_enc_untrusted_sysconf(enum SysconfConstants bridge_name) {
  int name = asylo::FromBridgeSysconfConstants(bridge_name);
  int64_t ret = -1;
  if (name != -1) {
    ret = sysconf(name);
  }
  return ret;
}

uint32_t ocall_enc_untrusted_sleep(uint32_t seconds) { return sleep(seconds); }

void ocall_enc_untrusted__exit(int rc) { _exit(rc); }

int ImportSnapshotFromFile(FILE *fp,
  const google::protobuf::RepeatedPtrField<asylo::SnapshotLayoutEntry> &snap_entry)
{
  LOG(INFO) << "ImportSnapshotFromFile";
  int ret = 0;
  for (int i = 0 ; i < snap_entry.size() ; ++i) {
    asylo::SnapshotLayoutEntry e = snap_entry[i];
	LOG(INFO) << "for entry( " << i << ")";
    void *nb = reinterpret_cast<void *>(e.nonce_base());
    size_t nsz = reinterpret_cast<size_t>(e.nonce_size());
    void *base = reinterpret_cast<void *>(e.ciphertext_base());
    size_t sz= reinterpret_cast<size_t>(e.ciphertext_size());
    LOG(INFO) << "data[" << i << "]: \nbase: " <<std::hex << base
			  << " sz: 0x" << std::hex << sz;
    ret += fread(nb, nsz, 1, fp);
	char *buf = (char *)malloc(sz);
    ret += fread(buf, sz, 1, fp);
    memcpy(base, buf, sz);
  }
  return ret;
}

int ExportSnapshotToFile(FILE *fp,
  google::protobuf::RepeatedPtrField<asylo::SnapshotLayoutEntry> snap_entry)
{
	int ret = 0;
  for (int i = 0 ; i < snap_entry.size() ; ++i) {
    asylo::SnapshotLayoutEntry e = snap_entry[i];
    void *nb = reinterpret_cast<void *>(e.nonce_base());
    size_t nsz = reinterpret_cast<size_t>(e.nonce_size());
    void *base = reinterpret_cast<void *>(e.ciphertext_base());
    size_t sz= reinterpret_cast<size_t>(e.ciphertext_size());
    LOG(INFO) << "data[" << i << "]: \nbase: " <<std::hex << base
			  << " sz: 0x" << std::hex << sz;
    ret += fwrite(nb, nsz, 1, fp);
    ret += fwrite(base, sz, 1, fp);
  }
  return ret;
}

pid_t ocall_enc_untrusted_fork(const char *enclave_name, const char *config,
                               bridge_size_t config_len,
                               bool restore_snapshot) {
  auto manager_result = asylo::EnclaveManager::Instance();
  if (!manager_result.ok()) {
    return -1;
  }
  asylo::EnclaveManager *manager = manager_result.ValueOrDie();
  asylo::SgxClient *client = dynamic_cast<asylo::SgxClient *>(
      manager->GetClient(enclave_name));

  if (!restore_snapshot) {
    // No need to take and restore a snapshot, just set indication that the new
    // enclave is created from fork.
    pid_t pid = fork();
    if (pid == 0) {
      // Set the process ID so that the new enclave created from fork does not
      // reject entry.
      client->SetProcessId();
    }
    return pid;
  }

  // A snapshot should be taken and restored for fork, take a snapshot of the
  // current enclave memory.
  // Here, we get the base address
  void *enclave_base_address = client->base_address();
  asylo::SnapshotLayout snapshot_layout;
  asylo::SnapshotLayout snapshot_layout2;
  asylo::Status status = client->EnterAndTakeSnapshot(&snapshot_layout);
  if (!status.ok()) {
    LOG(ERROR) << "EnterAndTakeSnapshot failed: " << status;
    errno = ENOMEM;
    return -1;
  }

  //save snapshot layout
  LOG(INFO) << "This is snapshot1 : " << &snapshot_layout;
  FILE * fp = fopen("/tmp/snapshot_layout", "wb");
  fwrite(&snapshot_layout, sizeof(asylo::SnapshotLayout), 1, fp);
  fclose(fp);

  //save snapshot image
  LOG(INFO) << "This is snapshot img : ";
  fp = fopen("/tmp/snapshot", "wb");
	ExportSnapshotToFile(fp, snapshot_layout.data());
	ExportSnapshotToFile(fp, snapshot_layout.bss());
	ExportSnapshotToFile(fp, snapshot_layout.heap());
	ExportSnapshotToFile(fp, snapshot_layout.thread());
	ExportSnapshotToFile(fp, snapshot_layout.stack());
  fclose(fp);

  // The snapshot memory should be freed in both the parent and the child
  // process.
  std::vector<SnapshotDataDeleter> data_deleter_;
  std::vector<SnapshotDataDeleter> bss_deleter_;
  std::vector<SnapshotDataDeleter> heap_deleter_;
  std::vector<SnapshotDataDeleter> thread_deleter_;
  std::vector<SnapshotDataDeleter> stack_deleter_;

  std::transform(snapshot_layout.data().cbegin(), snapshot_layout.data().cend(),
                 std::back_inserter(data_deleter_),
                 [](const asylo::SnapshotLayoutEntry &entry) {
                   return SnapshotDataDeleter(entry);
                 });

  std::transform(snapshot_layout.bss().cbegin(), snapshot_layout.bss().cend(),
                 std::back_inserter(bss_deleter_),
                 [](const asylo::SnapshotLayoutEntry &entry) {
                   return SnapshotDataDeleter(entry);
                 });

  std::transform(snapshot_layout.heap().cbegin(), snapshot_layout.heap().cend(),
                 std::back_inserter(heap_deleter_),
                 [](const asylo::SnapshotLayoutEntry &entry) {
                   return SnapshotDataDeleter(entry);
                 });

  std::transform(snapshot_layout.thread().cbegin(),
                 snapshot_layout.thread().cend(),
                 std::back_inserter(thread_deleter_),
                 [](const asylo::SnapshotLayoutEntry &entry) {
                   return SnapshotDataDeleter(entry);
                 });

  std::transform(snapshot_layout.stack().cbegin(),
                 snapshot_layout.stack().cend(),
                 std::back_inserter(stack_deleter_),
                 [](const asylo::SnapshotLayoutEntry &entry) {
                   return SnapshotDataDeleter(entry);
                 });

  asylo::EnclaveLoader *loader = manager->GetLoaderFromClient(client);

  // The child enclave should use the same loader as the parent. It loads by an
  // SGX loader or SGX embedded loader depending on the parent enclave.
  if (!dynamic_cast<asylo::SgxLoader *>(loader) &&
      !dynamic_cast<asylo::SgxEmbeddedLoader *>(loader)) {
    LOG(ERROR) << "Failed to get the loader for the enclave to fork";
    errno = EFAULT;
    return -1;
  }

  // Create a socket pair used for communication between the parent and child
  // enclave. |socket_pair[0]| is used by the parent enclave and
  // |socket_pair[1]| is used by the child enclave.
  int socket_pair[2];
  if (socketpair(AF_LOCAL, SOCK_STREAM, 0, socket_pair) < 0) {
    LOG(ERROR) << "Failed to create socket pair";
    errno = EFAULT;
    return -1;
  }

  // Create a pipe used to pass the child process fork state to the parent
  // process. If the child process failed to restore the enclave, the parent
  // fork should return error as well.
  int pipefd[2];
  if (pipe(pipefd) < 0) {
    LOG(ERROR) << "Failed to create pipe";
    errno = EFAULT;
    return -1;
  }

  pid_t pid = fork();
  if (pid == -1) {
    return pid;
  }

  // Here, we get the size of original enclave
  size_t enclave_size = client->size();

  // Parse the config from the enclave to load the child enclave with exactly
  // the same config as the parent enclave.
  asylo::EnclaveConfig enclave_config;
  if (!enclave_config.ParseFromArray(config, static_cast<size_t>(config_len))) {
    LOG(ERROR) << "Failed to parse EnclaveConfig";
    errno = EFAULT;
    return -1;
  }

  if (pid == 0) {
    if (close(pipefd[0]) < 0) {
      LOG(ERROR) << "failed to close pipefd: " << strerror(errno);
      errno = EFAULT;
      return -1;
    }
    // Load an enclave at the same virtual space as the parent.
    LOG(INFO) << "base: " << enclave_base_address << " sz: " << enclave_size;
    status = manager->LoadEnclave(enclave_name, *loader, enclave_config,
                                  enclave_base_address, enclave_size);
    if (!status.ok()) {
      LOG(ERROR) << "Load new enclave failed:" << status;
      errno = ENOMEM;
      return -1;
    }

    // Verifies that the new enclave is loaded at the same virtual address space
    // as the parent enclave.
    client = dynamic_cast<asylo::SgxClient *>(manager->GetClient(enclave_name));
    void *child_enclave_base_address = client->base_address();
    if (child_enclave_base_address != enclave_base_address) {
      LOG(ERROR) << "New enclave address: " << child_enclave_base_address
                 << " is different from the parent enclave address: "
                 << enclave_base_address;
      errno = EAGAIN;
      return -1;
    }

    std::string child_result = "Child fork succeeded";
	LOG(INFO) << "child result : " << child_result;
    asylo::Status status = DoSnapshotKeyTransfer(
        manager, client, socket_pair[0], socket_pair[1], /*is_parent=*/false);
    if (!status.ok()) {
      // Inform the parent process about the failure.
      child_result = "Child DoSnapshotKeyTransfer failed";
      if (write(pipefd[1], child_result.data(), child_result.size()) < 0) {
        LOG(ERROR) << "Failed to write child fork result to: " << pipefd[1]
                   << ", error: " << strerror(errno);
        return -1;
      }
      LOG(ERROR) << "DoSnapshotKeyTransfer failed: " << status;
      errno = EFAULT;
      return -1;
    }
	//Read the snapshot_layout file
	FILE * fp = fopen("/tmp/snapshot_layout2", "rb");
	fread(&snapshot_layout2, sizeof(asylo::SnapshotLayout), 1, fp);
	fclose(fp);
  LOG(INFO) << "This is snapshot2 : " << &snapshot_layout2;

    // restore snapshot from snapshot image according to the snapshot layout
	LOG(INFO) << "This is snapshot img : ";
	fp = fopen("/tmp/snapshot2", "rb");
  ImportSnapshotFromFile(fp, snapshot_layout.data());
  ImportSnapshotFromFile(fp, snapshot_layout.bss());
  ImportSnapshotFromFile(fp, snapshot_layout.heap());
  //ImportSnapshotFromFile(fp, snapshot_layout.thread());
  //ImportSnapshotFromFile(fp, snapshot_layout.stack());
	fclose(fp);

    //Break down the snapshot_layout to check
	//asylo::SnapshotLayout snapshot_layout2;
    //LOG(INFO) << "This is snapshot2 : " << &snapshot_layout2;

    // Enters the child enclave and restore the enclave memory.
    //status = client->EnterAndRestore(snapshot_layout2);
    status = client->EnterAndRestore(snapshot_layout);
    if (!status.ok()) {
      // Inform the parent process about the failure.
      child_result = "Child EnterAndRestore failed";
      if (write(pipefd[1], child_result.data(), child_result.size()) < 0) {
        LOG(ERROR) << "Failed to write child fork result to: " << pipefd[1]
                   << ", error: " << strerror(errno);
        return -1;
      }
      LOG(ERROR) << "EnterAndRestore failed: " << status;
      errno = EAGAIN;
      return -1;
    }
    // Inform the parent that child fork has succeeded.
    if (write(pipefd[1], child_result.data(), child_result.size()) < 0) {
      LOG(ERROR) << "Failed to write child fork result to: " << pipefd[1]
                 << ", error: " << strerror(errno);
      return -1;
    }
  } else {
    if (close(pipefd[1]) < 0) {
      LOG(ERROR) << "Failed to close pipefd: " << strerror(errno);
      errno = EFAULT;
      return -1;
    }
    asylo::Status status = DoSnapshotKeyTransfer(
        manager, client, /*self_socket=*/socket_pair[1],
        /*peer_socket=*/socket_pair[0], /*is_parent=*/true);
    if (!status.ok()) {
      LOG(ERROR) << "DoSnapshotKeyTransfer failed: " << status;
      errno = EFAULT;
      return -1;
    }
    // Wait for the information from the child process to confirm whether the
    // child enclave has been successfully restored. Timeout at 5 seconds.
    const int timeout_seconds = 5;
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(pipefd[0], &read_fds);
    struct timeval timeout;
    timeout.tv_sec = timeout_seconds;
    timeout.tv_usec = 0;
    int wait_result =
        select(/*nfds=*/pipefd[0] + 1, &read_fds, /*writefds=*/nullptr,
               /*exceptfds=*/nullptr, &timeout);
    if (wait_result < 0) {
      LOG(ERROR) << "Error while waiting for child fork result: "
                 << strerror(errno);
      return -1;
    } else if (wait_result == 0) {
      LOG(ERROR) << "Timeout waiting for fork result from the child";
      errno = EFAULT;
      return -1;
    }
    // Child fork result is ready to be read.
    char buf[64];
    int rc = read(pipefd[0], buf, sizeof(buf));
    if (rc <= 0) {
      LOG(ERROR) << "Failed to read child fork result";
      return -1;
    }
    buf[rc] = '\0';
    if (strncmp(buf, "Child fork succeeded", sizeof(buf)) != 0) {
      LOG(ERROR) << buf;
      return -1;
    }
  }
  return pid;
}

//////////////////////////////////////
//             wait.h               //
//////////////////////////////////////

pid_t ocall_enc_untrusted_wait3(struct BridgeWStatus *bridge_wstatus,
                                int options,
                                struct BridgeRUsage *bridge_usage) {
  struct rusage usage;
  int wstatus;
  pid_t ret = wait3(&wstatus, asylo::FromBridgeWaitOptions(options), &usage);
  asylo::ToBridgeRUsage(&usage, bridge_usage);
  if (bridge_wstatus) {
    *bridge_wstatus = asylo::ToBridgeWStatus(wstatus);
  }
  return ret;
}

pid_t ocall_enc_untrusted_waitpid(pid_t pid,
                                  struct BridgeWStatus *bridge_wstatus,
                                  int options) {
  int wstatus;
  pid_t ret = waitpid(pid, &wstatus, asylo::FromBridgeWaitOptions(options));
  if (bridge_wstatus) {
    *bridge_wstatus = asylo::ToBridgeWStatus(wstatus);
  }
  return ret;
}

//////////////////////////////////////
//           utime.h                //
//////////////////////////////////////

int ocall_enc_untrusted_utime(const char *filename,
                              const struct bridge_utimbuf *times) {
  struct utimbuf tmp;
  return utime(filename, asylo::FromBridgeUtimbuf(times, &tmp));
}

int ocall_enc_untrusted_utimes(
    const char *filename, const struct bridge_timeval *bridge_access_time,
    const struct bridge_timeval *bridge_modification_time) {
  struct timeval times[2];
  if (!asylo::FromBridgeTimeVal(bridge_access_time, &times[0]) ||
      !asylo::FromBridgeTimeVal(bridge_modification_time, &times[1])) {
    errno = EBADE;
    return -1;
  }
  return utimes(filename, times);
}

//////////////////////////////////////
//           Runtime support        //
//////////////////////////////////////

void *ocall_enc_untrusted_acquire_shared_resource(SharedNameKind kind,
                                                  const char *name) {
  asylo::SharedName shared_name(kind, std::string(name));
  auto manager_result = asylo::EnclaveManager::Instance();
  if (manager_result.ok()) {
    return manager_result.ValueOrDie()
        ->shared_resources()
        ->AcquireResource<void>(shared_name);
  } else {
    return nullptr;
  }
}

int ocall_enc_untrusted_release_shared_resource(SharedNameKind kind,
                                                const char *name) {
  asylo::SharedName shared_name(kind, std::string(name));
  auto manager_result = asylo::EnclaveManager::Instance();
  if (manager_result.ok()) {
    return manager_result.ValueOrDie()->shared_resources()->ReleaseResource(
        shared_name);
  }
  return false;
}

//////////////////////////////////////
//           Debugging              //
//////////////////////////////////////

void ocall_enc_untrusted_hex_dump(const void *buf, int nbytes) {
  fprintf(stderr, "%s\n", asylo::buffer_to_hex_string(buf, nbytes).c_str());
}

//////////////////////////////////////
//           Threading              //
//////////////////////////////////////

int ocall_enc_untrusted_thread_create(const char *name) {
  __asylo_donate_thread(name);
  return 0;
}

int ocall_dispatch_untrusted_call(uint64_t selector, void *buffer) {
  asylo::primitives::PrimitiveStatus status =
      asylo::primitives::Client::ExitCallback(
          selector,
          reinterpret_cast<asylo::primitives::NativeParameterStack *>(buffer));

  return status.error_code();
}
