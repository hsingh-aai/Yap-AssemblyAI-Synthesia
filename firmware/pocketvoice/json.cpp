// Just enough JSON to read the fields these APIs send.
//
// Two traps live here, both found the hard way on the Voice Agent socket:
//
//  - Frames are not null-terminated and can be large. session.ready echoes the
//    agent's whole config back, system prompt included - 4.6 KB for Jack - so
//    nothing can be copied into a fixed buffer first. Everything below works
//    on the raw frame with an explicit length.
//
//  - That config carries its own "type" fields describing the audio format,
//    so a plain substring search finds "audio" long before the
//    "type":"session.ready" that actually starts the conversation. Keys are
//    therefore matched at the top level only, stepping over nested objects.

#include "pv.h"

// Pointer to the first character of a top-level key's value, or null.
static const char* topValue(const char* buf, size_t len, const char* key) {
  const size_t klen = strlen(key);
  const char* end = buf + len;
  int  depth  = 0;
  bool in_str = false, esc = false;

  for (const char* p = buf; p < end; ++p) {
    const char c = *p;
    if (esc) { esc = false; continue; }
    if (c == '\\') { esc = true; continue; }

    if (c == '"') {
      if (in_str) { in_str = false; continue; }
      if (depth == 1 && (size_t)(end - p) > klen + 2 &&
          memcmp(p + 1, key, klen) == 0 && p[1 + klen] == '"') {
        const char* q = p + 2 + klen;
        while (q < end && (*q == ' ' || *q == ':')) ++q;
        return q < end ? q : nullptr;
      }
      in_str = true;
      continue;
    }
    if (in_str) continue;
    if (c == '{' || c == '[') ++depth;
    else if (c == '}' || c == ']') --depth;
  }
  return nullptr;
}

const char* jsonFind(const char* buf, size_t len, const char* key,
                     size_t* vlen) {
  const char* v = topValue(buf, len, key);
  if (!v || *v != '"') return nullptr;
  ++v;
  const char* end = buf + len;
  const char* r = v;
  while (r < end && *r != '"') {
    if (*r == '\\' && r + 1 < end) ++r;
    ++r;
  }
  *vlen = (size_t)(r - v);
  return v;
}

String jsonStr(const char* buf, size_t len, const char* key) {
  size_t n = 0;
  const char* p = jsonFind(buf, len, key, &n);
  if (!p) return String();
  String out;
  out.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    if (p[i] == '\\' && i + 1 < n) {
      switch (p[++i]) {                 // enough escapes for speech text
        case 'n': out += '\n'; break;
        case 't': out += ' ';  break;
        case 'u': i += 4;      break;
        default:  out += p[i]; break;
      }
      continue;
    }
    out += p[i];
  }
  return out;
}

bool jsonTrue(const char* buf, size_t len, const char* key) {
  const char* v = topValue(buf, len, key);
  return v && (size_t)(buf + len - v) >= 4 && memcmp(v, "true", 4) == 0;
}
