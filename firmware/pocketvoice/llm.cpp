// LLM Gateway, used by Notetaker to turn a transcript into notes.

#include "pv.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

static constexpr char kLlmUrl[] =
    "https://llm-gateway.assemblyai.com/v1/chat/completions";

static WiFiClientSecure& tls() {
  static WiFiClientSecure client;
  static bool ready = false;
  if (!ready) {
    // Demo shortcut: no certificate pinning. Before the 100-unit build this
    // should carry the root CA so the device can't be MITM'd on event wifi.
    client.setInsecure();
    client.setTimeout(30);
    ready = true;
  }
  return client;
}

// Escapes just enough for a JSON string body: transcripts are plain speech,
// but a stray quote or newline would still break the request.
static String jsonEscape(const String& s) {
  String out;
  out.reserve(s.length() + 16);
  for (size_t i = 0; i < s.length(); ++i) {
    const char c = s[i];
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      case '\r': break;
      case '\t': out += ' ';    break;
      default:
        if ((uint8_t)c < 0x20) break;     // drop other control bytes
        out += c;
    }
  }
  return out;
}

// Pulls the assistant text out of the OpenAI-shaped response. "content"
// appears once per choice and we only ever ask for one.
static String extractContent(Stream& in, uint32_t timeout_ms = 40000) {
  const char* needle = "\"content\":\"";
  size_t match = 0;
  String out;
  bool capturing = false, escaped = false;
  const uint32_t deadline = millis() + timeout_ms;

  while (millis() < deadline) {
    if (!in.available()) { delay(2); continue; }
    const char c = in.read();
    if (!capturing) {
      match = (c == needle[match]) ? match + 1
            : (c == needle[0] ? 1 : 0);
      if (needle[match] == '\0') capturing = true;
      continue;
    }
    if (escaped) {
      switch (c) {
        case 'n': out += '\n'; break;
        case 't': out += ' ';  break;
        case 'u': for (int i = 0; i < 4 && in.available(); ++i) in.read(); break;
        default:  out += c;    break;
      }
      escaped = false;
      continue;
    }
    if (c == '\\') { escaped = true; continue; }
    if (c == '"') return out;
    out += c;
  }
  return out;
}

String llmAsk(const char* model, const String& prompt, int max_tokens) {
  if (!wifiUp()) { failf("wi-fi down"); return String(); }

  HTTPClient http;
  if (!http.begin(tls(), kLlmUrl)) { failf("llm connect failed"); return String(); }
  http.addHeader("authorization", AAI_API_KEY);
  http.addHeader("content-type", "application/json");
  http.setTimeout(45000);

  String body;
  body.reserve(prompt.length() + 200);
  body += "{\"model\":\"";
  body += model;
  body += "\",\"max_tokens\":";
  body += max_tokens;
  body += ",\"messages\":[{\"role\":\"user\",\"content\":\"";
  body += jsonEscape(prompt);
  body += "\"}]}";

  const int rc = http.POST(body);
  if (rc != 200) {
    failf("llm http %d", rc);
    http.end();
    return String();
  }
  const String out = extractContent(http.getStream());
  http.end();
  logf("[llm] %u chars back", (unsigned)out.length());
  return out;
}
