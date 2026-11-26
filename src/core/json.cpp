#include "core/json.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace fi::json {

const Value* Value::get(const std::string& key) const {
  if (kind != Kind::Object) return nullptr;
  auto it = o.find(key);
  return it == o.end() ? nullptr : &it->second;
}
double Value::num(const std::string& key, double fallback) const {
  auto v = get(key);
  return v && v->kind == Kind::Number ? v->n : fallback;
}
std::string Value::str(const std::string& key, const std::string& fallback) const {
  auto v = get(key);
  return v && v->kind == Kind::String ? v->s : fallback;
}

namespace {
struct Reader {
  const char* p;
  const char* end;
  int depth = 0;

  void ws() {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
  }
  bool lit(const char* word) {
    size_t n = strlen(word);
    if (size_t(end - p) < n || memcmp(p, word, n) != 0) return false;
    p += n;
    return true;
  }
  static void utf8(std::string& out, unsigned cp) {
    if (cp < 0x80) out += char(cp);
    else if (cp < 0x800) out += char(0xC0 | (cp >> 6)), out += char(0x80 | (cp & 0x3F));
    else if (cp < 0x10000)
      out += char(0xE0 | (cp >> 12)), out += char(0x80 | ((cp >> 6) & 0x3F)), out += char(0x80 | (cp & 0x3F));
    else
      out += char(0xF0 | (cp >> 18)), out += char(0x80 | ((cp >> 12) & 0x3F)),
          out += char(0x80 | ((cp >> 6) & 0x3F)), out += char(0x80 | (cp & 0x3F));
  }
  bool hex4(unsigned& v) {
    if (end - p < 4) return false;
    v = 0;
    for (int i = 0; i < 4; ++i) {
      char c = *p++;
      v <<= 4;
      if (c >= '0' && c <= '9') v |= unsigned(c - '0');
      else if (c >= 'a' && c <= 'f') v |= unsigned(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') v |= unsigned(c - 'A' + 10);
      else return false;
    }
    return true;
  }
  bool string(std::string& out) {
    if (p >= end || *p != '"') return false;
    ++p;
    while (p < end && *p != '"') {
      char c = *p++;
      if (c != '\\') {
        out += c;
        continue;
      }
      if (p >= end) return false;
      char e = *p++;
      switch (e) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          unsigned cp;
          if (!hex4(cp)) return false;
          if (cp >= 0xD800 && cp < 0xDC00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
            p += 2;
            unsigned lo;
            if (!hex4(lo)) return false;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
          utf8(out, cp);
          break;
        }
        default: return false;
      }
    }
    if (p >= end) return false;
    ++p;
    return true;
  }
  bool value(Value& v) {
    if (++depth > 64) return false;
    ws();
    if (p >= end) return false;
    bool ok = true;
    if (*p == '{') {
      v.kind = Value::Kind::Object;
      ++p;
      ws();
      if (p < end && *p == '}') {
        ++p;
      } else {
        for (;;) {
          ws();
          std::string key;
          if (!string(key)) return false;
          ws();
          if (p >= end || *p++ != ':') return false;
          Value item;
          if (!value(item)) return false;
          v.o[key] = std::move(item);
          ws();
          if (p < end && *p == ',') { ++p; continue; }
          if (p < end && *p == '}') { ++p; break; }
          return false;
        }
      }
    } else if (*p == '[') {
      v.kind = Value::Kind::Array;
      ++p;
      ws();
      if (p < end && *p == ']') {
        ++p;
      } else {
        for (;;) {
          Value item;
          if (!value(item)) return false;
          v.a.push_back(std::move(item));
          ws();
          if (p < end && *p == ',') { ++p; continue; }
          if (p < end && *p == ']') { ++p; break; }
          return false;
        }
      }
    } else if (*p == '"') {
      v.kind = Value::Kind::String;
      ok = string(v.s);
    } else if (lit("true")) {
      v.kind = Value::Kind::Bool;
      v.b = true;
    } else if (lit("false")) {
      v.kind = Value::Kind::Bool;
    } else if (lit("null")) {
      v.kind = Value::Kind::Null;
    } else {
      std::string num;
      while (p < end && (strchr("+-.eE", *p) || (*p >= '0' && *p <= '9'))) num += *p++;
      if (num.empty()) return false;
      char* stop = nullptr;
      v.kind = Value::Kind::Number;
      v.n = strtod(num.c_str(), &stop);
      ok = stop && *stop == '\0';
    }
    --depth;
    return ok;
  }
};
}  // namespace

bool parse(const std::string& text, Value& out) {
  Reader r{text.data(), text.data() + text.size()};
  Value v;
  if (!r.value(v)) return false;
  r.ws();
  if (r.p != r.end) return false;
  out = std::move(v);
  return true;
}

std::string quote(const std::string& s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20) {
          char b[8];
          snprintf(b, sizeof b, "\\u%04x", c);
          o += b;
        } else {
          o += char(c);
        }
    }
  }
  return o + "\"";
}

std::string number(double d) {
  char b[32];
  snprintf(b, sizeof b, "%.6g", d);
  return b;
}

}  // namespace fi::json
