#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <poll.h>

namespace async {

// Forward-declare to avoid including system headers in clients.
class Connection;

// Simple in-memory key-value store.
class KVStore {
 public:
  KVStore() = default;
  // Non-copyable, but movable
  KVStore(const KVStore&) = delete;
  KVStore& operator=(const KVStore&) = delete;

  // Returns true and fills 'out' if key exists; false otherwise.
  bool get(const std::string& key, std::string& out) const;

  // Sets key to value (move).
  void set(const std::string& key, std::string value);

  // Deletes key; returns true if existed.
  bool del(const std::string& key);

 private:
  // Pimpl-friendly: we keep implementation details in the .cpp
  struct Impl;
  Impl* impl_ = nullptr;
};

// A single client TCP connection with its I/O buffers and request processing.
class Connection {
 public:
  explicit Connection(int fd);
  ~Connection();

  // File descriptor associated with this connection.
  int getFd() const;

  // Interest flags for the event loop.
  bool wantsRead() const;
  bool wantsWrite() const;
  bool wantsClose() const;

  // Called by the event loop when socket is readable/writable.
  void handleReadable(KVStore& store);
  void handleWritable();

  // Non-copyable
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

 private:
  // Internal helpers (defined in .cpp)
  bool tryOneRequest(KVStore& store);
  void appendResponse(uint32_t status, const std::string& data);
  void consumeIncoming(size_t n);
  void appendOutgoing(const uint8_t* data, size_t n);
  static bool parseRequest(const uint8_t* data, size_t size, std::vector<std::string>& out);

  int fd_ = -1;
  bool want_read_ = false;
  bool want_write_ = false;
  bool want_close_ = false;
  std::vector<uint8_t> incoming_;
  std::vector<uint8_t> outgoing_;
};

// A poll-based event loop that accepts and drives connections.
class EventLoop {
 public:
  // The event loop does not own listen_fd; caller owns its lifecycle.
  EventLoop(int listen_fd, KVStore& store);
  ~EventLoop();

  // Run the loop until fatal error or external termination.
  void run();

  // Execute a single iteration (useful for tests).
  void runOnce();

  // Non-copyable
  EventLoop(const EventLoop&) = delete;
  EventLoop& operator=(const EventLoop&) = delete;

 private:
  void preparePollArgs();
  void waitForEvents();
  void handleListeningSocket();
  void handleConnectionSockets();
  Connection* acceptOne();
  static void setNonBlocking(int fd);

  int listen_fd_ = -1;
  KVStore& store_;
  std::vector<Connection*> fd2conn_;
  std::vector<pollfd> poll_args_;
};

}  // namespace async