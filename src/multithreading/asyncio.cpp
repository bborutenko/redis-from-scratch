#include "asyncio.h"


#include <cassert>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../utils/utils.h"

namespace async {

// ===================== KVStore =====================

struct KVStore::Impl {
  std::unordered_map<std::string, std::string> data;
};

bool KVStore::get(const std::string& key, std::string& out) const {
  // Lazy allocation to avoid relying on a constructor we didn't declare in the header.
  if (!impl_) {
    // Casting away const for lazy initialization; safe because it's internal and transparent.
    KVStore* self = const_cast<KVStore*>(this);
    self->impl_ = new Impl();
  }
  auto it = impl_->data.find(key);
  if (it == impl_->data.end()) return false;
  out = it->second;
  return true;
}

void KVStore::set(const std::string& key, std::string value) {
  if (!impl_) impl_ = new Impl();
  impl_->data[key] = std::move(value);
}

bool KVStore::del(const std::string& key) {
  if (!impl_) impl_ = new Impl();
  return impl_->data.erase(key) > 0;
}

// ===================== Connection =====================

namespace {
inline void bufConsume(std::vector<uint8_t>& buf, size_t n) {
  if (n == 0) return;
  if (n >= buf.size()) {
    buf.clear();
    return;
  }
  buf.erase(buf.begin(), buf.begin() + static_cast<long>(n));
}

inline void bufAppend(std::vector<uint8_t>& buf, const uint8_t* data, size_t n) {
  if (n == 0) return;
  buf.insert(buf.end(), data, data + n);
}

inline bool readU32(const uint8_t*& cur, const uint8_t* end, uint32_t& out) {
  if (cur + 4 > end) return false;
  std::memcpy(&out, cur, 4);
  cur += 4;
  return true;
}

inline bool readStr(const uint8_t*& cur, const uint8_t* end, size_t n, std::string& out) {
  if (cur + n > end) return false;
  out.assign(reinterpret_cast<const char*>(cur), reinterpret_cast<const char*>(cur + n));
  cur += n;
  return true;
}
}  // namespace

Connection::Connection(int fd)
  : fd_(fd), want_read_(true), want_write_(false), want_close_(false) {}

Connection::~Connection() = default;

int Connection::getFd() const { return fd_; }
bool Connection::wantsRead() const { return want_read_; }
bool Connection::wantsWrite() const { return want_write_; }
bool Connection::wantsClose() const { return want_close_; }

void Connection::handleReadable(KVStore& store) {
  uint8_t buf[64 * 1024];
  ssize_t rv = ::read(fd_, buf, sizeof(buf));
  if (rv < 0 && errno == EAGAIN) return;

  if (rv < 0) {
    want_close_ = true;
    return;
  }

  if (rv == 0) {
    // Peer closed. If we have partial data, it's a protocol error.
    if (!incoming_.empty()) {
      want_close_ = true;
      return;
    }
    want_close_ = true;
    return;
  }

  bufAppend(incoming_, buf, static_cast<size_t>(rv));

  // Process as many complete requests as possible.
  while (tryOneRequest(store)) {}

  if (!outgoing_.empty()) {
    want_read_ = false;
    want_write_ = true;
    // Try to write immediately to reduce latency.
    handleWritable();
  }
}

void Connection::handleWritable() {
  if (outgoing_.empty()) {
    want_write_ = false;
    want_read_ = true;
    return;
  }

  ssize_t rv = ::write(fd_, outgoing_.data(), outgoing_.size());
  if (rv < 0 && errno == EAGAIN) return;

  if (rv < 0) {
    want_close_ = true;
    return;
  }

  bufConsume(outgoing_, static_cast<size_t>(rv));

  if (outgoing_.empty()) {
    want_write_ = false;
    want_read_ = true;
  }
}

bool Connection::tryOneRequest(KVStore& store) {
  // Basic framing: [len: u32][payload: len bytes]
  if (incoming_.size() < 4) return false;

  uint32_t len = 0;
  std::memcpy(&len, incoming_.data(), 4);
  if (len > k_max_msg) {
    want_close_ = true;
    return false;
  }
  if (incoming_.size() < 4u + len) return false;

  const uint8_t* req = incoming_.data() + 4u;

  // Parse request as: [nstr: u32] { [len: u32][bytes...] } * nstr
  std::vector<std::string> cmd;
  if (!parseRequest(req, len, cmd)) {
    want_close_ = true;
    bufConsume(incoming_, 4u + len);
    return false;
  }

  // Execute command against store.
  uint32_t status = 0;
  std::string data;

  if (cmd.size() == 2 && cmd[0] == "get") {
    std::string value;
    if (store.get(cmd[1], value)) {
      data = std::move(value);
    } else {
      status = ResponseStatus::RES_NX;
    }
  } else if (cmd.size() == 3 && cmd[0] == "set") {
    store.set(cmd[1], std::move(cmd[2]));
  } else if (cmd.size() == 2 && cmd[0] == "del") {
    bool existed = store.del(cmd[1]);
    // Could encode existence in the data or status if desired; keep status 0 for success.
    (void)existed;
  } else {
    status = ResponseStatus::RES_ERR;
  }

  appendResponse(status, data);

  // Consume this request from incoming buffer.
  bufConsume(incoming_, 4u + len);
  return true;
}

void Connection::appendResponse(uint32_t status, const std::string& data) {
  // Response: [len: u32 = 4 + data.size()][status: u32][data: bytes...]
  uint32_t resp_len = 4u + static_cast<uint32_t>(data.size());
  appendOutgoing(reinterpret_cast<const uint8_t*>(&resp_len), 4);
  appendOutgoing(reinterpret_cast<const uint8_t*>(&status), 4);
  if (!data.empty()) {
    appendOutgoing(reinterpret_cast<const uint8_t*>(data.data()), data.size());
  }
}

void Connection::consumeIncoming(size_t n) {
  bufConsume(incoming_, n);
}

void Connection::appendOutgoing(const uint8_t* data, size_t n) {
  bufAppend(outgoing_, data, n);
}

bool Connection::parseRequest(const uint8_t* data, size_t size, std::vector<std::string>& out) {
  const uint8_t* cur = data;
  const uint8_t* end = data + size;

  uint32_t nstr = 0;
  if (!readU32(cur, end, nstr)) return false;
  if (nstr > k_max_msg) return false;  // safety limit

  out.clear();
  out.reserve(nstr);
  for (uint32_t i = 0; i < nstr; ++i) {
    uint32_t slen = 0;
    if (!readU32(cur, end, slen)) return false;
    std::string s;
    if (!readStr(cur, end, slen, s)) return false;
    out.emplace_back(std::move(s));
  }
  if (cur != end) return false;  // trailing garbage
  return true;
}

// ===================== EventLoop =====================

EventLoop::EventLoop(int listen_fd, KVStore& store)
  : listen_fd_(listen_fd), store_(store) {}

EventLoop::~EventLoop() {
  // Close and delete any remaining connections.
  for (Connection* c : fd2conn_) {
    if (!c) continue;
    ::close(c->getFd());
    delete c;
  }
}

void EventLoop::run() {
  for (;;) {
    runOnce();
  }
}

void EventLoop::runOnce() {
  preparePollArgs();
  waitForEvents();
  handleListeningSocket();
  handleConnectionSockets();
}

void EventLoop::preparePollArgs() {
  poll_args_.clear();
  // Always poll the listening socket for readable events.
  pollfd pfd_listen{};
  pfd_listen.fd = listen_fd_;
  pfd_listen.events = POLLIN;
  pfd_listen.revents = 0;
  poll_args_.push_back(pfd_listen);

  // Add all active connections.
  for (Connection* conn : fd2conn_) {
    if (!conn) continue;
    pollfd pfd{};
    pfd.fd = conn->getFd();
    pfd.events = POLLERR;
    if (conn->wantsRead()) pfd.events |= POLLIN;
    if (conn->wantsWrite()) pfd.events |= POLLOUT;
    pfd.revents = 0;
    poll_args_.push_back(pfd);
  }
}

void EventLoop::waitForEvents() {
  int rv = ::poll(poll_args_.data(), static_cast<nfds_t>(poll_args_.size()), -1);
  if (rv < 0 && errno == EINTR) return;
  if (rv < 0) {
    throw std::runtime_error("poll() failed");
  }
}

void EventLoop::handleListeningSocket() {
  if (poll_args_.empty()) return;
  const pollfd& pfd = poll_args_[0];
  if (!(pfd.revents & POLLIN)) return;

  if (Connection* conn = acceptOne()) {
    const int cfd = conn->getFd();
    if (static_cast<size_t>(cfd) >= fd2conn_.size()) {
      fd2conn_.resize(static_cast<size_t>(cfd) + 1, nullptr);
    }
    fd2conn_[static_cast<size_t>(cfd)] = conn;
  }
}

void EventLoop::handleConnectionSockets() {
  // Iterate poll results for connection fds.
  for (size_t i = 1; i < poll_args_.size(); ++i) {
    const uint32_t ready = static_cast<uint32_t>(poll_args_[i].revents);
    const int fd = poll_args_[i].fd;
    if (fd < 0 || static_cast<size_t>(fd) >= fd2conn_.size()) continue;
    Connection* conn = fd2conn_[static_cast<size_t>(fd)];
    if (!conn) continue;

    if (ready & POLLIN) {
      conn->handleReadable(store_);
    }
    if (ready & POLLOUT) {
      conn->handleWritable();
    }

    if (ready & (POLLERR | POLLHUP | POLLNVAL) || conn->wantsClose()) {
      ::close(conn->getFd());
      fd2conn_[static_cast<size_t>(fd)] = nullptr;
      delete conn;
    }
  }
}

Connection* EventLoop::acceptOne() {
  sockaddr_storage ss{};
  socklen_t slen = sizeof(ss);
  int cfd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&ss), &slen);
  if (cfd < 0) {
    // Non-fatal if would block.
    if (errno == EAGAIN || errno == EWOULDBLOCK) return nullptr;
    throw std::runtime_error("accept() failed");
  }

  setNonBlocking(cfd);
  Connection* conn = new Connection(cfd);
  return conn;
}

void EventLoop::setNonBlocking(int fd) {
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

}  // namespace async