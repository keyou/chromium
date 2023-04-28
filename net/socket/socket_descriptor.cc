// Copyright 2013 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/socket/socket_descriptor.h"

#include "build/build_config.h"
#include "perfetto/tracing/string_helpers.h"
#include "perfetto/tracing/track_event_category_registry.h"

#if BUILDFLAG(IS_WIN)
#include <ws2tcpip.h>
#include "net/base/winsock_init.h"
#elif BUILDFLAG(IS_POSIX) || BUILDFLAG(IS_FUCHSIA)
#include <sys/socket.h>
#include <sys/types.h>
#endif

#if BUILDFLAG(IS_APPLE)
#include <unistd.h>
#endif

#include "base/logging.h"
#include "base/trace_event/typed_macros.h"

namespace net {

SocketDescriptor CreatePlatformSocket(int family, int type, int protocol) {
#if BUILDFLAG(IS_WIN)
  EnsureWinsockInit();
  SocketDescriptor result = ::WSASocket(family, type, protocol, nullptr, 0,
                                        WSA_FLAG_OVERLAPPED);
  if (result != kInvalidSocket && family == AF_INET6) {
    DWORD value = 0;
    if (setsockopt(result, IPPROTO_IPV6, IPV6_V6ONLY,
                   reinterpret_cast<const char*>(&value), sizeof(value))) {
      closesocket(result);
      return kInvalidSocket;
    }
  }
  return result;
#elif BUILDFLAG(IS_POSIX) || BUILDFLAG(IS_FUCHSIA)
  SocketDescriptor result = ::socket(family, type, protocol);
#if BUILDFLAG(IS_APPLE)
  // Disable SIGPIPE on this socket. Although Chromium globally disables
  // SIGPIPE, the net stack may be used in other consumers which do not do
  // this. SO_NOSIGPIPE is a Mac-only API. On Linux, it is a flag on send.
  if (result != kInvalidSocket) {
    TRACE_EVENT("net", "net::CreatePlatformSocketKY", "socket_fd_",
                (type == SOCK_DGRAM ? "create_udp_fd_" : "create_tcp_fd_") +
                    std::to_string(result),
                "type(TCP=1,UDP=2)", type);

    LOG(ERROR) << "keyou: net::CreatePlatformSocket: fd=" << result
               << ", type=" << type;
    int value = 1;
    if (setsockopt(result, SOL_SOCKET, SO_NOSIGPIPE, &value, sizeof(value))) {
      close(result);
      return kInvalidSocket;
    }
  }
#endif
  return result;
#endif  // BUILDFLAG(IS_WIN)
}

}  // namespace net
