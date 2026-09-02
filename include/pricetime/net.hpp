#pragma once

// Minimal blocking sockets, Windows + POSIX. Just enough for the engine: a
// TCP listener, a TCP stream with send-all/recv-all, and UDP send/receive.
// Blocking I/O is a deliberate choice at this scale: the engine dedicates a
// thread to each direction, so readiness APIs (epoll/IOCP) would add latency
// and complexity for exactly zero benefit until there are many clients.

#include <cstddef>
#include <cstdint>
#include <string>

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#include <emmintrin.h>
#define PRICETIME_NET_X86 1
#else
#define PRICETIME_NET_X86 0
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// winsock2.h must precede windows.h in any TU; including it here first makes
// that ordering automatic for every user of this header.
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace pricetime::net {

#if defined(_WIN32)
using socket_t = SOCKET;
inline constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
using socket_t = int;
inline constexpr socket_t kInvalidSocket = -1;
#endif

// Winsock demands per-process initialization; POSIX needs nothing. Construct
// one of these before using any socket and keep it alive.
class NetInit {
 public:
  NetInit() {
#if defined(_WIN32)
    WSADATA data;
    ok_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
#endif
  }
  ~NetInit() {
#if defined(_WIN32)
    if (ok_) WSACleanup();
#endif
  }
  NetInit(const NetInit&) = delete;
  NetInit& operator=(const NetInit&) = delete;
  [[nodiscard]] bool ok() const noexcept { return ok_; }

 private:
  bool ok_ = true;
};

inline void close_socket(socket_t s) noexcept {
  if (s == kInvalidSocket) return;
#if defined(_WIN32)
  closesocket(s);
#else
  ::close(s);
#endif
}

// Unblocks any thread stuck in recv/accept on this socket; the pattern that
// makes clean shutdown possible with blocking I/O.
inline void shutdown_socket(socket_t s) noexcept {
  if (s == kInvalidSocket) return;
#if defined(_WIN32)
  shutdown(s, SD_BOTH);
#else
  ::shutdown(s, SHUT_RDWR);
#endif
}

// Nagle's algorithm exists to coalesce small writes into fewer packets, the
// exact opposite of what an order-entry link wants. Every latency number this
// project reports would be a lie about the network stack without this.
inline void set_no_delay(socket_t s) noexcept {
  int one = 1;
  setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
}

inline void set_non_blocking(socket_t s) noexcept {
#if defined(_WIN32)
  u_long one = 1;
  ioctlsocket(s, FIONBIO, &one);
#else
  fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
}

[[nodiscard]] inline bool last_error_would_block() noexcept {
#if defined(_WIN32)
  return WSAGetLastError() == WSAEWOULDBLOCK;
#else
  return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

// The polite spin instruction: tells the core (and its SMT sibling) that this
// is a wait loop, not work.
inline void cpu_relax() noexcept {
#if PRICETIME_NET_X86
  _mm_pause();
#endif
}

// --- TCP -------------------------------------------------------------------

class TcpStream {
 public:
  TcpStream() = default;
  explicit TcpStream(socket_t s) : sock_(s) {}
  ~TcpStream() { close(); }
  TcpStream(const TcpStream&) = delete;
  TcpStream& operator=(const TcpStream&) = delete;
  TcpStream(TcpStream&& o) noexcept : sock_(o.sock_), spin_(o.spin_) {
    o.sock_ = kInvalidSocket;
    o.spin_ = false;
  }
  TcpStream& operator=(TcpStream&& o) noexcept {
    if (this != &o) {
      close();
      sock_ = o.sock_;
      spin_ = o.spin_;
      o.sock_ = kInvalidSocket;
      o.spin_ = false;
    }
    return *this;
  }

  [[nodiscard]] bool connect(const std::string& host, std::uint16_t port) {
    close();
    sock_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock_ == kInvalidSocket) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
      close();
      return false;
    }
    if (::connect(sock_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
      close();
      return false;
    }
    set_no_delay(sock_);
    return true;
  }

  // Busy-poll mode: the socket goes non-blocking and recv_all/send_all spin
  // instead of sleeping in the kernel. A blocking recv costs a scheduler
  // wakeup (~10 us+ on Windows) every time data arrives; spinning trades a
  // whole core for making that cost vanish. This is the standard trade in
  // latency-sensitive trading systems, and both sides of it are measured in
  // this repo rather than asserted.
  void set_busy_poll() noexcept {
    spin_ = true;
    set_non_blocking(sock_);
  }

  // Loops until every byte is on the wire; TCP send is allowed to be partial.
  [[nodiscard]] bool send_all(const std::uint8_t* data, std::size_t len) {
    std::size_t sent = 0;
    while (sent < len) {
      const auto n = ::send(sock_, reinterpret_cast<const char*>(data + sent),
                            static_cast<int>(len - sent), 0);
      if (n < 0 && spin_ && last_error_would_block()) {
        cpu_relax();
        continue;
      }
      if (n <= 0) return false;
      sent += static_cast<std::size_t>(n);
    }
    return true;
  }

  // One recv call: returns bytes read (>0), 0 on clean EOF, -1 on error.
  // In busy-poll mode, spins until at least one byte arrives. The building
  // block for buffered readers that amortize the syscall across messages.
  [[nodiscard]] int recv_some(std::uint8_t* data, std::size_t cap) {
    for (;;) {
      const auto n = ::recv(sock_, reinterpret_cast<char*>(data), static_cast<int>(cap), 0);
      if (n < 0 && spin_ && last_error_would_block()) {
        cpu_relax();
        continue;
      }
      if (n < 0) return -1;
      return static_cast<int>(n);
    }
  }

  // Loops until exactly `len` bytes have arrived; false on EOF or error.
  [[nodiscard]] bool recv_all(std::uint8_t* data, std::size_t len) {
    std::size_t got = 0;
    while (got < len) {
      const auto n =
          ::recv(sock_, reinterpret_cast<char*>(data + got), static_cast<int>(len - got), 0);
      if (n < 0 && spin_ && last_error_would_block()) {
        cpu_relax();
        continue;
      }
      if (n <= 0) return false;
      got += static_cast<std::size_t>(n);
    }
    return true;
  }

  void interrupt() noexcept { shutdown_socket(sock_); }

  void close() noexcept {
    close_socket(sock_);
    sock_ = kInvalidSocket;
  }

  [[nodiscard]] bool valid() const noexcept { return sock_ != kInvalidSocket; }

 private:
  socket_t sock_ = kInvalidSocket;
  bool spin_ = false;
};

class TcpListener {
 public:
  TcpListener() = default;
  ~TcpListener() { close(); }
  TcpListener(const TcpListener&) = delete;
  TcpListener& operator=(const TcpListener&) = delete;

  // Port 0 asks the OS for an ephemeral port; port() reports what was bound.
  [[nodiscard]] bool listen(std::uint16_t port) {
    close();
    sock_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock_ == kInvalidSocket) return false;
    int one = 1;
    setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one),
               sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (::bind(sock_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
      close();
      return false;
    }
    if (::listen(sock_, 1) != 0) {
      close();
      return false;
    }
    sockaddr_in bound{};
#if defined(_WIN32)
    int len = sizeof(bound);
#else
    socklen_t len = sizeof(bound);
#endif
    if (getsockname(sock_, reinterpret_cast<sockaddr*>(&bound), &len) == 0) {
      port_ = ntohs(bound.sin_port);
    }
    return true;
  }

  [[nodiscard]] TcpStream accept() {
    const socket_t s = ::accept(sock_, nullptr, nullptr);
    if (s != kInvalidSocket) set_no_delay(s);
    return TcpStream(s);
  }

  // Unblocks a pending accept. Close alone is not enough: on Linux, closing
  // a listening socket does not reliably wake a thread blocked in accept();
  // shutdown() does (accept returns with an error).
  void interrupt() noexcept {
    shutdown_socket(sock_);
    close();
  }

  void close() noexcept {
    close_socket(sock_);
    sock_ = kInvalidSocket;
  }

  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  [[nodiscard]] bool valid() const noexcept { return sock_ != kInvalidSocket; }

 private:
  socket_t sock_ = kInvalidSocket;
  std::uint16_t port_ = 0;
};

// --- UDP -------------------------------------------------------------------

// Sends datagrams to one destination. Point it at a multicast group address
// for real fan-out, or at 127.0.0.1 for tests. The code is identical; only
// the address class differs.
class UdpSender {
 public:
  UdpSender() = default;
  ~UdpSender() { close(); }
  UdpSender(const UdpSender&) = delete;
  UdpSender& operator=(const UdpSender&) = delete;

  [[nodiscard]] bool open(const std::string& host, std::uint16_t port) {
    close();
    sock_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_ == kInvalidSocket) return false;
    dest_ = sockaddr_in{};
    dest_.sin_family = AF_INET;
    dest_.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &dest_.sin_addr) != 1) {
      close();
      return false;
    }
    return true;
  }

  [[nodiscard]] bool send(const std::uint8_t* data, std::size_t len) {
    const auto n = ::sendto(sock_, reinterpret_cast<const char*>(data), static_cast<int>(len),
                            0, reinterpret_cast<const sockaddr*>(&dest_), sizeof(dest_));
    return n >= 0 && static_cast<std::size_t>(n) == len;
  }

  void close() noexcept {
    close_socket(sock_);
    sock_ = kInvalidSocket;
  }

 private:
  socket_t sock_ = kInvalidSocket;
  sockaddr_in dest_{};
};

class UdpReceiver {
 public:
  UdpReceiver() = default;
  ~UdpReceiver() { close(); }
  UdpReceiver(const UdpReceiver&) = delete;
  UdpReceiver& operator=(const UdpReceiver&) = delete;

  [[nodiscard]] bool bind(std::uint16_t port) {
    close();
    sock_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_ == kInvalidSocket) return false;
    // A generous receive buffer: loopback UDP drops silently when this
    // overflows, and a dropped market-data packet in a test looks exactly
    // like a bug in the feed.
    int buf = 4 * 1024 * 1024;
    setsockopt(sock_, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&buf), sizeof(buf));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (::bind(sock_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
      close();
      return false;
    }
    sockaddr_in bound{};
#if defined(_WIN32)
    int len = sizeof(bound);
#else
    socklen_t len = sizeof(bound);
#endif
    if (getsockname(sock_, reinterpret_cast<sockaddr*>(&bound), &len) == 0) {
      port_ = ntohs(bound.sin_port);
    }
    return true;
  }

  // Blocks up to timeout_ms; returns bytes received, 0 on timeout, -1 on error.
  [[nodiscard]] int recv(std::uint8_t* data, std::size_t cap, int timeout_ms) {
#if defined(_WIN32)
    DWORD tv = static_cast<DWORD>(timeout_ms);
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#endif
    const auto n = ::recvfrom(sock_, reinterpret_cast<char*>(data), static_cast<int>(cap), 0,
                              nullptr, nullptr);
    if (n < 0) return 0;  // treat timeout and error alike: nothing arrived
    return static_cast<int>(n);
  }

  void close() noexcept {
    close_socket(sock_);
    sock_ = kInvalidSocket;
  }

  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

 private:
  socket_t sock_ = kInvalidSocket;
  std::uint16_t port_ = 0;
};

}  // namespace pricetime::net
