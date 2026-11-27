// Agents in the room: what they can ask of it, as plain requests and replies.
//
// The app runs a door on localhost (see src/app/agent_door.h); every request that comes through it lands here,
// on the main thread, and is applied to the room. The API speaks degrees and metres:
//
//   GET  /scene                          the panels, the agents, the notes, where you're looking
//   GET  /windows                        open windows that could be pinned
//   POST /pin      {window | match, yaw?, height?, distance?, width?}
//   POST /move     {panel, yaw?, height?, distance?, width?}        panel: an index, or text to match
//   POST /unpin    {panel}
//   POST /arrange  {panels: [...], around?: yaw, distance?, height?, gap?}   in a row round you, left to right
//   POST /presence {state?, says?, looking_at?, yaw?}   the agent's own presence (name from X-Agent)
//   POST /leave                                         the agent goes
//   POST /note     {text, yaw?, height?, distance?, near?}   a note in the room; returns its id
//   POST /unnote   {id}
//   GET  /look                                          a picture of the room (handled by the app)
//
// Agents can arrange the room but cannot click or type into Milk's windows: there is no route for it, and a
// request for one is refused by name. That takes a grant she gives, which doesn't exist yet.
#pragma once
#include "core/json.h"
#include "core/room.h"
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace fi {

struct AgentRequest {
  std::string method;  // GET or POST
  std::string path;    // without the query
  std::map<std::string, std::string> query;
  std::string body;
  std::string agent;  // who is asking (the X-Agent header); "agent" if they didn't say
};

struct AgentReply {
  int status = 200;
  std::string type = "application/json";
  std::string body;
};

struct Agent {
  std::string name;
  std::string state = "here";  // working, waiting, talking, ... (their word)
  std::string says;            // the last thing they said, shown for a while
  double says_at = 0;          // ms
  int looking_at = -1;         // a panel, or -1
  float yaw = 0;               // where they stand round you, radians
  double seen_at = 0;          // ms; an agent not heard from for a few minutes fades away
};

struct Note {
  int id = 0;
  std::string text, by;
  float yaw = 0, height = 0.45f, distance = 1.2f;
  float width = 0.28f;
};

class AgentDesk {
 public:
  explicit AgentDesk(Room& room) : room_(room) {}

  // The app's part: things only it can do with real windows. pin returns the new panel's index, or -1.
  std::function<std::vector<OpenWindow>()> open_windows;
  std::function<int(const OpenWindow&)> pin;
  std::function<void(int)> unpin;
  std::function<bool(int)> is_live;  // the panel's window is there and showing

  AgentReply handle(const AgentRequest& req, double now_ms);
  void tick(double now_ms);  // agents not heard from in a while leave

  std::vector<Agent> agents;
  std::vector<Note> notes;
  bool changed = false;  // the layout changed: the app saves it
  static constexpr double kAgentTimeoutMs = 5 * 60 * 1000, kSaysMs = 12 * 1000;

  std::string scene_json() const;
  int find_panel(const json::Value* v) const;  // an index, or text matched against title then exe; -1 if none

 private:
  Agent& agent_named(const std::string& name, double now_ms);
  Room& room_;
  int next_note_ = 1;
};

std::string to_lower(std::string s);

}  // namespace fi
