#include "core/agents.h"
#include <algorithm>
#include <cmath>

namespace fi {

namespace {

constexpr float kDeg = kPi / 180.0f;
using json::number;
using json::quote;
using Kind = json::Value::Kind;

AgentReply ok(std::string body) { return {200, "application/json", std::move(body)}; }
AgentReply fail(int status, const std::string& why) { return {status, "application/json", "{\"error\": " + quote(why) + "}"}; }

bool has(const json::Value& v, const char* key) {
  auto p = v.get(key);
  return p && p->kind == Kind::Number;
}
float deg(const json::Value& v, const char* key, float fallback_rad) {
  return has(v, key) ? float(v.num(key, 0)) * kDeg : fallback_rad;
}
float metres(const json::Value& v, const char* key, float fallback, float lo, float hi) {
  return std::clamp(has(v, key) ? float(v.num(key, fallback)) : fallback, lo, hi);
}
float wrap(float a) {  // to -pi..pi
  while (a > kPi) a -= 2 * kPi;
  while (a < -kPi) a += 2 * kPi;
  return a;
}

}  // namespace

std::string to_lower(std::string s) {
  for (auto& c : s) c = char(std::tolower((unsigned char)c));
  return s;
}

int AgentDesk::find_panel(const json::Value* v) const {
  if (!v) return -1;
  if (v->kind == Kind::Number) {
    int i = int(v->n);
    return i >= 0 && i < (int)room_.panels.size() && double(i) == v->n ? i : -1;
  }
  if (v->kind != Kind::String || v->s.empty()) return -1;
  std::string want = to_lower(v->s);
  for (int pass = 0; pass < 2; ++pass)
    for (int i = 0; i < (int)room_.panels.size(); ++i) {
      const Panel& p = room_.panels[i];
      if (to_lower(pass == 0 ? p.title : p.exe).find(want) != std::string::npos) return i;
    }
  return -1;
}

Agent& AgentDesk::agent_named(const std::string& name, double now) {
  for (auto& a : agents)
    if (a.name == name) {
      a.seen_at = now;
      return a;
    }
  Agent a;
  a.name = name;
  a.seen_at = now;
  // Stand where nobody else is: alternate left and right of straight ahead, a little behind the panels' line.
  int n = (int)agents.size();
  float off = 0.6f + 0.4f * float(n / 2);
  a.yaw = room_.camera.yaw + ((n % 2) ? off : -off);
  agents.push_back(a);
  return agents.back();
}

void AgentDesk::tick(double now) {
  agents.erase(std::remove_if(agents.begin(), agents.end(),
                              [&](const Agent& a) { return now - a.seen_at > kAgentTimeoutMs; }),
               agents.end());
  for (auto& a : agents)
    if (!a.says.empty() && now - a.says_at > kSaysMs) a.says.clear();
}

std::string AgentDesk::scene_json() const {
  std::string o = "{\n  \"units\": \"degrees and metres; yaw 0 is straight ahead, positive to the right; height "
                  "against your eyes\",\n  \"looking\": {\"yaw\": " +
                  number(room_.camera.yaw / kDeg) + ", \"pitch\": " + number(room_.camera.pitch / kDeg) +
                  "},\n  \"panels\": [";
  for (size_t i = 0; i < room_.panels.size(); ++i) {
    const Panel& p = room_.panels[i];
    bool live = is_live ? is_live(int(i)) : p.window != 0;
    o += i ? ",\n    " : "\n    ";
    o += "{\"panel\": " + std::to_string(i) + ", \"exe\": " + quote(p.exe) + ", \"title\": " + quote(p.title) +
         ", \"live\": " + (live ? "true" : "false") + ", \"yaw\": " + number(p.yaw / kDeg) +
         ", \"height\": " + number(p.height) + ", \"distance\": " + number(p.distance) +
         ", \"width\": " + number(p.width) + ", \"panel_height\": " + number(p.panel_height()) + "}";
  }
  o += room_.panels.empty() ? "],\n  \"agents\": [" : "\n  ],\n  \"agents\": [";
  for (size_t i = 0; i < agents.size(); ++i) {
    const Agent& a = agents[i];
    o += i ? ", " : "";
    o += "{\"name\": " + quote(a.name) + ", \"state\": " + quote(a.state) + ", \"says\": " + quote(a.says) +
         ", \"looking_at\": " + std::to_string(a.looking_at) + ", \"yaw\": " + number(a.yaw / kDeg) + "}";
  }
  o += "],\n  \"notes\": [";
  for (size_t i = 0; i < notes.size(); ++i) {
    const Note& n = notes[i];
    o += i ? ", " : "";
    o += "{\"id\": " + std::to_string(n.id) + ", \"text\": " + quote(n.text) + ", \"by\": " + quote(n.by) +
         ", \"yaw\": " + number(n.yaw / kDeg) + ", \"height\": " + number(n.height) +
         ", \"distance\": " + number(n.distance) + "}";
  }
  o += "]\n}\n";
  return o;
}

AgentReply AgentDesk::handle(const AgentRequest& req, double now) {
  const std::string& p = req.path;
  std::string who = req.agent.empty() ? "agent" : req.agent.substr(0, 40);

  for (const char* refused : {"/click", "/type", "/key", "/input", "/mouse", "/scroll"})
    if (p == refused)
      return fail(403, "Agents can arrange the room but can't click or type into Milk's windows. That needs a grant "
                       "from her, and there isn't one.");

  if (req.method == "GET") {
    if (p == "/scene") return ok(scene_json());
    if (p == "/windows") {
      std::string o = "[";
      bool first = true;
      for (auto& w : open_windows ? open_windows() : std::vector<OpenWindow>{}) {
        bool pinned = std::any_of(room_.panels.begin(), room_.panels.end(), [&](auto& q) { return q.window == w.window; });
        o += first ? "\n  " : ",\n  ";
        first = false;
        o += "{\"window\": " + std::to_string(w.window) + ", \"exe\": " + quote(w.exe) +
             ", \"title\": " + quote(w.title) + ", \"pinned\": " + (pinned ? "true" : "false") + "}";
      }
      return ok(o + (first ? "]\n" : "\n]\n"));
    }
    return fail(404, "No such thing to read. Try /scene, /windows or /look.");
  }
  if (req.method != "POST") return fail(405, "GET or POST");

  json::Value b;
  if (req.body.empty()) b.kind = Kind::Object;
  else if (!json::parse(req.body, b) || b.kind != Kind::Object) return fail(400, "The body must be a JSON object.");

  agent_named(who, now);  // anything an agent does shows they're here

  if (p == "/pin") {
    if (!open_windows || !pin) return fail(503, "Pinning isn't available here.");
    auto open = open_windows();
    const OpenWindow* chosen = nullptr;
    auto taken = [&](const OpenWindow& w) {
      return std::any_of(room_.panels.begin(), room_.panels.end(), [&](auto& q) { return q.window == w.window; });
    };
    if (auto w = b.get("window"); w && w->kind == Kind::Number) {
      for (auto& o : open)
        if (double(o.window) == w->n) chosen = &o;
    } else if (auto m = b.get("match"); m && m->kind == Kind::String && !m->s.empty()) {
      std::string want = to_lower(m->s);
      for (int pass = 0; pass < 2 && !chosen; ++pass)
        for (auto& o : open)
          if (!taken(o) && to_lower(pass == 0 ? o.title : o.exe).find(want) != std::string::npos) {
            chosen = &o;
            break;
          }
    } else {
      return fail(400, "Say which window: \"window\" (from /windows) or \"match\" (text in its title or program).");
    }
    if (!chosen) return fail(404, "No open window like that.");
    if (taken(*chosen)) return fail(409, "That window is already pinned.");
    int i = pin(*chosen);
    if (i < 0) return fail(500, "The window couldn't be pinned.");
    Panel& q = room_.panels[i];
    q.yaw = deg(b, "yaw", q.yaw);
    q.height = metres(b, "height", q.height, -3, 3);
    q.distance = metres(b, "distance", q.distance, 0.4f, 8);
    q.width = metres(b, "width", q.width, 0.15f, 8);
    changed = true;
    return ok("{\"panel\": " + std::to_string(i) + ", \"title\": " + quote(q.title) + "}");
  }
  if (p == "/move") {
    int i = find_panel(b.get("panel"));
    if (i < 0) return fail(404, "No such panel (give its index from /scene, or text from its title).");
    Panel& q = room_.panels[i];
    q.yaw = deg(b, "yaw", q.yaw);
    q.height = metres(b, "height", q.height, -3, 3);
    q.distance = metres(b, "distance", q.distance, 0.4f, 8);
    q.width = metres(b, "width", q.width, 0.15f, 8);
    changed = true;
    agent_named(who, now).looking_at = i;
    return ok("{\"panel\": " + std::to_string(i) + "}");
  }
  if (p == "/unpin") {
    int i = find_panel(b.get("panel"));
    if (i < 0) return fail(404, "No such panel.");
    if (unpin) unpin(i);
    else room_.panels.erase(room_.panels.begin() + i);
    for (auto& a : agents)
      if (a.looking_at == i) a.looking_at = -1;
      else if (a.looking_at > i) --a.looking_at;
    changed = true;
    return ok("{\"unpinned\": " + std::to_string(i) + "}");
  }
  if (p == "/arrange") {
    auto list = b.get("panels");
    if (!list || list->kind != Kind::Array || list->a.empty()) return fail(400, "\"panels\": a list of panels, left to right.");
    std::vector<int> order;
    for (auto& e : list->a) {
      int i = find_panel(&e);
      if (i < 0) return fail(404, "No such panel: " + (e.kind == Kind::String ? e.s : json::number(e.n)));
      if (std::find(order.begin(), order.end(), i) == order.end()) order.push_back(i);
    }
    float around = deg(b, "around", room_.camera.yaw);
    float gap = metres(b, "gap", 0.06f, 0, 2);
    bool set_distance = has(b, "distance"), set_height = has(b, "height");
    float distance = metres(b, "distance", 1.4f, 0.4f, 8), height = metres(b, "height", 0, -3, 3);
    // Each panel takes its width plus the gap along the circle; the row is centred on `around`.
    float total = 0;
    for (int i : order) {
      Panel& q = room_.panels[i];
      if (set_distance) q.distance = distance;
      total += (q.width + gap) / q.distance;
    }
    float at = around - total / 2;
    for (int i : order) {
      Panel& q = room_.panels[i];
      float span = (q.width + gap) / q.distance;
      q.yaw = wrap(at + span / 2);
      if (set_height) q.height = height;
      at += span;
    }
    changed = true;
    return ok("{\"arranged\": " + std::to_string(order.size()) + "}");
  }
  if (p == "/presence") {
    Agent& a = agent_named(who, now);
    if (auto s = b.get("state"); s && s->kind == Kind::String) a.state = s->s.substr(0, 40);
    if (auto s = b.get("says"); s && s->kind == Kind::String) {
      a.says = s->s.substr(0, 400);
      a.says_at = now;
    }
    if (auto l = b.get("looking_at")) a.looking_at = find_panel(l);
    a.yaw = deg(b, "yaw", a.yaw);
    return ok("{\"name\": " + quote(a.name) + "}");
  }
  if (p == "/leave") {
    agents.erase(std::remove_if(agents.begin(), agents.end(), [&](auto& a) { return a.name == who; }), agents.end());
    return ok("{}");
  }
  if (p == "/note") {
    auto t = b.get("text");
    if (!t || t->kind != Kind::String || t->s.empty()) return fail(400, "\"text\": what the note says.");
    Note n;
    n.id = next_note_++;
    n.text = t->s.substr(0, 1000);
    n.by = who;
    n.yaw = room_.camera.yaw;
    if (int near = find_panel(b.get("near")); near >= 0) {  // just right of the panel's top corner
      const Panel& q = room_.panels[near];
      n.distance = q.distance - 0.02f;
      n.yaw = q.yaw + (q.width / 2 + n.width / 2 + 0.03f) / q.distance;
      n.height = q.height + q.panel_height() / 2 - 0.12f;
    }
    n.yaw = deg(b, "yaw", n.yaw);
    n.height = metres(b, "height", n.height, -3, 3);
    n.distance = metres(b, "distance", n.distance, 0.4f, 8);
    notes.push_back(n);
    if (notes.size() > 64) notes.erase(notes.begin());
    return ok("{\"id\": " + std::to_string(n.id) + "}");
  }
  if (p == "/unnote") {
    int id = int(b.num("id", -1));
    auto it = std::find_if(notes.begin(), notes.end(), [&](auto& n) { return n.id == id; });
    if (it == notes.end()) return fail(404, "No note with that id.");
    notes.erase(it);
    return ok("{}");
  }
  return fail(404, "No such action. See /scene, /windows, /pin, /move, /unpin, /arrange, /presence, /note, /look.");
}

}  // namespace fi
