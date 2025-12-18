#include <iostream>
#include <vector>
#include <string>
#include <cstdint>
#include <cstring>
#include <cerrno>

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "utils/utils.h"

// Helper to append a 32-bit unsigned integer in host byte order
static inline void append_u32(std::vector<uint8_t>& buf, uint32_t v) {
  uint8_t raw[4];
  std::memcpy(raw, &v, 4);
  buf.insert(buf.end(), raw, raw + 4);
}

// Encode a vector<string> command into the protocol payload
// Payload format:
//   [nstr: u32] { [len: u32][bytes: u8 * len] } * nstr
static bool build_payload(const std::vector<std::string>& cmd, std::vector<uint8_t>& payload) {
  payload.clear();
  // Safety: rough estimate to avoid oversize payloads before building
  size_t estimated = 4; // nstr
  for (const auto& s : cmd) {
    if (s.size() > k_max_msg) return false;
    estimated += 4 + s.size();
    if (estimated > k_max_msg) return false;
  }

  append_u32(payload, static_cast<uint32_t>(cmd.size()));
  for (const auto& s : cmd) {
    append_u32(payload, static_cast<uint32_t>(s.size()));
    payload.insert(payload.end(),
                   reinterpret_cast<const uint8_t*>(s.data()),
                   reinterpret_cast<const uint8_t*>(s.data()) + s.size());
  }
  return true;
}

// Send a request [len: u32][payload: bytes...] and read response:
// Response format:
//   [len: u32 = 4 + data.size()][status: u32][data: bytes...]
static bool send_command(int fd,
                         const std::vector<std::string>& cmd,
                         uint32_t& status_out,
                         std::string& data_out) {
  status_out = 0;
  data_out.clear();

  // Build request payload
  std::vector<uint8_t> payload;
  if (!build_payload(cmd, payload)) {
    std::cerr << "Failed to build payload (too large?)" << std::endl;
    return false;
  }

  // Frame with 4-byte length
  uint32_t len = static_cast<uint32_t>(payload.size());
  if (len > k_max_msg) {
    std::cerr << "Request payload too large" << std::endl;
    return false;
  }

  // Write: [len][payload]
  std::vector<uint8_t> wbuf;
  wbuf.reserve(4 + payload.size());
  append_u32(wbuf, len);
  wbuf.insert(wbuf.end(), payload.begin(), payload.end());

  try {
    write_all(fd, reinterpret_cast<const char*>(wbuf.data()), wbuf.size());
  } catch (const std::exception& e) {
    std::cerr << "Write error: " << e.what() << std::endl;
    return false;
  }

  // Read response length
  uint8_t hdr[4];
  try {
    read_full(fd, reinterpret_cast<char*>(hdr), 4);
  } catch (const std::exception& e) {
    std::cerr << "Read header error: " << e.what() << std::endl;
    return false;
  }

  uint32_t rlen = 0;
  std::memcpy(&rlen, hdr, 4);
  if (rlen > k_max_msg || rlen < 4) {
    std::cerr << "Invalid response length: " << rlen << std::endl;
    return false;
  }

  std::vector<uint8_t> rbuf(rlen);
  try {
    read_full(fd, reinterpret_cast<char*>(rbuf.data()), rlen);
  } catch (const std::exception& e) {
    std::cerr << "Read body error: " << e.what() << std::endl;
    return false;
  }

  // Parse: [status: u32][data...]
  std::memcpy(&status_out, rbuf.data(), 4);
  size_t dlen = rlen - 4;
  data_out.assign(reinterpret_cast<const char*>(rbuf.data() + 4), dlen);
  return true;
}

static void print_result(const std::vector<std::string>& cmd, uint32_t status, const std::string& data) {
  std::cout << "> ";
  for (const auto& s : cmd) {
    std::cout << s << " ";
  }
  std::cout << "\n";

  if (status == 0) {
    if (!data.empty()) {
      std::cout << "OK: " << data << "\n";
    } else {
      std::cout << "OK\n";
    }
  } else if (status == ResponseStatus::RES_NX) {
    std::cout << "(nil)\n";
  } else if (status == ResponseStatus::RES_ERR) {
    std::cout << "ERR\n";
  } else {
    std::cout << "STATUS(" << status << "): " << data << "\n";
  }
}

int main() {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    std::cerr << "Cannot create socket: " << std::strerror(errno) << std::endl;
    return 1;
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(1234);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1

  if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
    std::cerr << "Cannot connect to 127.0.0.1:1234 - " << std::strerror(errno) << std::endl;
    ::close(fd);
    return 1;
  }

  // Demo sequence using the new protocol
  uint32_t status = 0;
  std::string data;

  // set foo bar
  {
    std::vector<std::string> cmd = {"set", "foo", "bar"};
    if (send_command(fd, cmd, status, data)) {
      print_result(cmd, status, data);
    } else {
      std::cerr << "Failed to send 'set' command\n";
    }
  }

  // get foo
  {
    std::vector<std::string> cmd = {"get", "foo"};
    if (send_command(fd, cmd, status, data)) {
      print_result(cmd, status, data);
    } else {
      std::cerr << "Failed to send 'get' command\n";
    }
  }

  // del foo
  {
    std::vector<std::string> cmd = {"del", "foo"};
    if (send_command(fd, cmd, status, data)) {
      print_result(cmd, status, data);
    } else {
      std::cerr << "Failed to send 'del' command\n";
    }
  }

  // get foo (should be NX)
  {
    std::vector<std::string> cmd = {"get", "foo"};
    if (send_command(fd, cmd, status, data)) {
      print_result(cmd, status, data);
    } else {
      std::cerr << "Failed to send 'get' command\n";
    }
  }

  ::close(fd);
  return 0;
}