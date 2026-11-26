// A small JSON reader and writer: enough for layouts and settings. Numbers are doubles.
#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace fi::json {

struct Value {
  enum class Kind { Null, Bool, Number, String, Array, Object } kind = Kind::Null;
  bool b = false;
  double n = 0;
  std::string s;
  std::vector<Value> a;
  std::map<std::string, Value> o;

  const Value* get(const std::string& key) const;  // nullptr unless an object with that key
  double num(const std::string& key, double fallback) const;
  std::string str(const std::string& key, const std::string& fallback = {}) const;
};

bool parse(const std::string& text, Value& out);  // false on anything malformed
std::string quote(const std::string& s);          // a JSON string literal, escaped
std::string number(double d);                     // short, round-trippable enough for layouts

}  // namespace fi::json
