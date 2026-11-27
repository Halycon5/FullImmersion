// The agents' door: a small HTTP server on 127.0.0.1 that hands each request to the main thread.
//
// Where it is and its key are written to %APPDATA%\FullImmersion\agent_door.json ({"port": ..., "token": ...}),
// readable only by Milk's account. Every request needs "Authorization: Bearer <token>"; a request from a web
// page (it carries an Origin header) is refused, so a site in her browser can never reach the room. The token
// changes each time the room starts. What agents may do is decided by fi::AgentDesk, not here.
#pragma once
#include "core/agents.h"
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace fi {

class AgentDoor {
 public:
  ~AgentDoor();
  bool open(const std::wstring& dir, std::string& err);  // binds the first free port from 7717
  void close();
  // On the main thread, once a frame: answers everything waiting.
  void serve(const std::function<AgentReply(const AgentRequest&)>& handle);
  int port() const { return port_; }

 private:
  struct Pending {
    AgentRequest req;
    AgentReply reply;
    bool done = false;
  };
  void run();
  void answer(uintptr_t sock);
  uintptr_t listen_ = ~uintptr_t(0);
  int port_ = 0;
  std::string token_;
  std::thread thread_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<std::shared_ptr<Pending>> waiting_;
  bool stopping_ = false;
};

}  // namespace fi
