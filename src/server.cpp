#include <iostream>
#include <stdexcept>


#include <cassert>

#include <fcntl.h>
#include <netinet/in.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "multithreading/asyncio.h"

namespace {

int create_socket(int domain, int type, int protocol = 0) {
  int fd = ::socket(domain, type, protocol);
  if (fd < 0) {
    throw std::runtime_error("socket() failed");
  }
  return fd;
}

void set_sockopt_int(int fd, int level, int optname, int value) {
  if (::setsockopt(fd, level, optname, &value, sizeof(value)) != 0) {
    ::close(fd);
    throw std::runtime_error("setsockopt() failed");
  }
}

void set_nonblock(int fd) {
  int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    ::close(fd);
    throw std::runtime_error("fcntl(F_GETFL) failed");
  }
  if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    ::close(fd);
    throw std::runtime_error("fcntl(F_SETFL, O_NONBLOCK) failed");
  }
}

void bind_and_listen(int fd, uint16_t port, uint32_t ip_host_order, int backlog) {
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(ip_host_order);

  if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    throw std::runtime_error("bind() failed");
  }

  if (::listen(fd, backlog) != 0) {
    ::close(fd);
    throw std::runtime_error("listen() failed");
  }
}

}  // namespace

int main() {
  constexpr uint16_t kPort = 1234;

  try {
    int listen_fd = create_socket(AF_INET, SOCK_STREAM, 0);
    set_sockopt_int(listen_fd, SOL_SOCKET, SO_REUSEADDR, 1);
    // Non-blocking listen socket is a safer default when used with poll().
    set_nonblock(listen_fd);
    bind_and_listen(listen_fd, kPort, /*0.0.0.0*/ 0, SOMAXCONN);

    std::cout << "Server is ready to accept connections on 0.0.0.0:" << kPort << std::endl;

    async::KVStore store;
    async::EventLoop loop(listen_fd, store);
    loop.run();

    // Normally unreachable; loop.run() is an infinite loop.
    ::close(listen_fd);
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Fatal: " << e.what() << std::endl;
    return 1;
  }
}