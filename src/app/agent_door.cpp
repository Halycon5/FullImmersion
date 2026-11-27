#include "app/agent_door.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <bcrypt.h>
#include <fstream>

namespace fi {

namespace {

std::string random_token() {
  unsigned char b[24];
  BCryptGenRandom(nullptr, b, sizeof b, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  static const char* hex = "0123456789abcdef";
  std::string s;
  for (unsigned char c : b) s += hex[c >> 4], s += hex[c & 15];
  return s;
}

std::string url_decode(const std::string& s) {
  std::string o;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '+') o += ' ';
    else if (s[i] == '%' && i + 2 < s.size()) {
      o += char(std::stoi(s.substr(i + 1, 2), nullptr, 16));
      i += 2;
    } else o += s[i];
  }
  return o;
}

const char* reason(int status) {
  switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 503: return "Service Unavailable";
    default: return "Error";
  }
}

void send_all(SOCKET s, const std::string& data) {
  size_t at = 0;
  while (at < data.size()) {
    int n = ::send(s, data.data() + at, int(std::min<size_t>(data.size() - at, 1 << 20)), 0);
    if (n <= 0) return;
    at += size_t(n);
  }
}

void reply(SOCKET s, const AgentReply& r) {
  std::string head = "HTTP/1.1 " + std::to_string(r.status) + " " + reason(r.status) + "\r\nContent-Type: " + r.type +
                     "\r\nContent-Length: " + std::to_string(r.body.size()) + "\r\nConnection: close\r\n\r\n";
  send_all(s, head + r.body);
}

}  // namespace

AgentDoor::~AgentDoor() { close(); }

bool AgentDoor::open(const std::wstring& dir, std::string& err) {
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa)) {
    err = "WSAStartup failed";
    return false;
  }
  SOCKET l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (l == INVALID_SOCKET) {
    err = "no socket";
    return false;
  }
  BOOL excl = TRUE;  // nobody else may share the port
  setsockopt(l, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof excl);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // this PC only
  for (int p = 7717; p < 7727 && !port_; ++p) {
    a.sin_port = htons(u_short(p));
    if (bind(l, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0) port_ = p;
  }
  if (!port_ || listen(l, 8) != 0) {
    closesocket(l);
    err = "no free port from 7717";
    return false;
  }
  listen_ = l;
  token_ = random_token();
  {
    std::ofstream f(dir + L"\\agent_door.json", std::ios::binary | std::ios::trunc);
    f << "{\"port\": " << port_ << ", \"token\": \"" << token_ << "\"}\n";
  }
  thread_ = std::thread([this] { run(); });
  return true;
}

void AgentDoor::close() {
  {
    std::lock_guard<std::mutex> g(mu_);
    if (stopping_ && !thread_.joinable()) return;
    stopping_ = true;
  }
  cv_.notify_all();
  if (listen_ != ~uintptr_t(0)) closesocket(SOCKET(listen_));
  listen_ = ~uintptr_t(0);
  if (thread_.joinable()) thread_.join();
}

void AgentDoor::run() {
  for (;;) {
    SOCKET s = accept(SOCKET(listen_), nullptr, nullptr);
    if (s == INVALID_SOCKET) return;  // closed
    DWORD ms = 5000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof ms);
    answer(uintptr_t(s));
    shutdown(s, SD_SEND);
    closesocket(s);
    std::lock_guard<std::mutex> g(mu_);
    if (stopping_) return;
  }
}

void AgentDoor::answer(uintptr_t sock) {
  SOCKET s = SOCKET(sock);
  std::string in;
  char buf[8192];
  size_t head_end = std::string::npos;
  while (head_end == std::string::npos) {
    int n = recv(s, buf, sizeof buf, 0);
    if (n <= 0) return;
    in.append(buf, size_t(n));
    head_end = in.find("\r\n\r\n");
    if (in.size() > 65536) return reply(s, {413, "application/json", "{\"error\": \"headers too long\"}"});
  }
  std::string head = in.substr(0, head_end);
  std::string body = in.substr(head_end + 4);

  AgentRequest req;
  size_t sp1 = head.find(' '), sp2 = head.find(' ', sp1 + 1), eol = head.find("\r\n");
  if (sp1 == std::string::npos || sp2 == std::string::npos) return reply(s, {400, "application/json", "{}"});
  req.method = head.substr(0, sp1);
  std::string target = head.substr(sp1 + 1, sp2 - sp1 - 1);
  size_t q = target.find('?');
  req.path = target.substr(0, q);
  if (q != std::string::npos) {
    std::string qs = target.substr(q + 1);
    size_t at = 0;
    while (at <= qs.size()) {
      size_t amp = qs.find('&', at);
      std::string kv = qs.substr(at, amp == std::string::npos ? std::string::npos : amp - at);
      size_t eq = kv.find('=');
      if (!kv.empty()) req.query[url_decode(kv.substr(0, eq))] = eq == std::string::npos ? "" : url_decode(kv.substr(eq + 1));
      if (amp == std::string::npos) break;
      at = amp + 1;
    }
  }
  std::map<std::string, std::string> headers;
  size_t at = eol == std::string::npos ? head.size() : eol + 2;
  while (at < head.size()) {
    size_t e = head.find("\r\n", at);
    std::string line = head.substr(at, e == std::string::npos ? std::string::npos : e - at);
    size_t c = line.find(':');
    if (c != std::string::npos) {
      std::string k = to_lower(line.substr(0, c)), v = line.substr(c + 1);
      while (!v.empty() && v.front() == ' ') v.erase(v.begin());
      headers[k] = v;
    }
    if (e == std::string::npos) break;
    at = e + 2;
  }
  if (headers.count("origin"))
    return reply(s, {403, "application/json", "{\"error\": \"web pages can't reach the room\"}"});
  if (headers["authorization"] != "Bearer " + token_)
    return reply(s, {401, "application/json",
                     "{\"error\": \"needs Authorization: Bearer <token> from %APPDATA%\\\\FullImmersion\\\\agent_door.json\"}"});
  size_t length = headers.count("content-length") ? size_t(std::strtoull(headers["content-length"].c_str(), nullptr, 10)) : 0;
  if (length > (1 << 20)) return reply(s, {413, "application/json", "{\"error\": \"body too long\"}"});
  while (body.size() < length) {
    int n = recv(s, buf, sizeof buf, 0);
    if (n <= 0) return;
    body.append(buf, size_t(n));
  }
  body.resize(length);
  req.body = std::move(body);
  req.agent = headers.count("x-agent") ? headers["x-agent"] : "agent";

  auto p = std::make_shared<Pending>();
  p->req = std::move(req);
  std::unique_lock<std::mutex> g(mu_);
  waiting_.push_back(p);
  if (!cv_.wait_for(g, std::chrono::seconds(10), [&] { return p->done || stopping_; }))
    p->reply = {503, "application/json", "{\"error\": \"the room is busy\"}"};
  else if (!p->done)
    p->reply = {503, "application/json", "{\"error\": \"the room is closing\"}"};
  AgentReply r = p->reply;
  g.unlock();
  reply(s, r);
}

void AgentDoor::serve(const std::function<AgentReply(const AgentRequest&)>& handle) {
  std::deque<std::shared_ptr<Pending>> todo;
  {
    std::lock_guard<std::mutex> g(mu_);
    todo.swap(waiting_);
  }
  if (todo.empty()) return;
  for (auto& p : todo) {
    AgentReply r = handle(p->req);
    std::lock_guard<std::mutex> g(mu_);
    p->reply = std::move(r);
    p->done = true;
  }
  cv_.notify_all();
}

}  // namespace fi
