// Groq Robot Alarm Clock + AI Assistant
// ESP32 + Freenove 4WD + Bluetooth Speaker
// Simple, working version

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <time.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include "AudioTools.h"
#include "AudioTools/AudioCodecs/CodecMP3Helix.h"
#include "Freenove_4WD_Car_For_ESP32.h"

#if __has_include("AudioTools/Communication/A2DPStream.h")
  #include "AudioTools/Communication/A2DPStream.h"
#elif __has_include("AudioTools/AudioLibs/A2DPStream.h")
  #include "AudioTools/AudioLibs/A2DPStream.h"
#endif

#if __has_include("AudioTools/Communication/AudioHttp.h")
  #include "AudioTools/Communication/AudioHttp.h"
#elif __has_include("AudioTools/AudioLibs/AudioHttp.h")
  #include "AudioTools/AudioLibs/AudioHttp.h"
#endif

const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";
const char* BT_SPEAKER = "JBL Go 4";
const char* GROQ_API_KEY = "gsk_YOUR_KEY_HERE";
const char* GROQ_MODEL = "llama-3.3-70b-versatile";

#define GMT_OFFSET_S 18000
#define DRIVE_SPEED 2000
#define DRIVE_MS 2000
#define MAX_PODCASTS 10
#define LOG_N 10

WebServer server(80);
Preferences prefs;

bool driving = false;
unsigned long driveStart = 0;

A2DPStream* a2dp = nullptr;
URLStream* podUrl = nullptr;
EncodedAudioStream* decoder = nullptr;
StreamCopy* podCopier = nullptr;

bool audioReady = false;
bool gotAny = false;
unsigned long lastData = 0;
int stage = 0;

String logLines[LOG_N];
int logN = 0;

struct Podcast { 
  char name[32]; 
  char url[256]; 
};
Podcast pods[MAX_PODCASTS];

String toLower(const String& s) {
  String r = s;
  for (int i = 0; i < r.length(); i++) r[i] = tolower((unsigned char)r[i]);
  return r;
}

String esc(const char* s) {
  String o;
  for (; *s; s++) {
    if (*s == '<') o += "&lt;";
    else if (*s == '>') o += "&gt;";
    else if (*s == '&') o += "&amp;";
    else if (*s == '"') o += "&quot;";
    else o += *s;
  }
  return o;
}

String escapeJson(const String& input) {
  String out;
  for (size_t i = 0; i < input.length(); i++) {
    char c = input[i];
    if (c == '\\') out += "\\\\";
    else if (c == '"') out += "\\\"";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else out += c;
  }
  return out;
}

void note(const String& s) {
  Serial.println(s);
  logLines[logN % LOG_N] = s;
  logN++;
}

void notef(const char* fmt, ...) {
  char buf[200];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  note(String(buf));
}

void drive(int speed) {
  Motor_Move(speed, speed, speed, speed);
  driving = (speed != 0);
  driveStart = millis();
}

void motorStop() {
  Motor_Move(0, 0, 0, 0);
  driving = false;
}

void savePods() {
  prefs.begin("pods", false);
  prefs.putBytes("p", pods, sizeof(pods));
  prefs.end();
}

void loadPods() {
  memset(pods, 0, sizeof(pods));
  prefs.begin("pods", false);
  bool ok = (prefs.getBytesLength("p") == sizeof(pods));
  if (ok) prefs.getBytes("p", pods, sizeof(pods));
  prefs.end();

  if (!ok) {
    strlcpy(pods[0].name, "Podcast 1", sizeof(pods[0].name));
    strlcpy(pods[0].url, "https://dcs-cached.megaphone.fm/ARML4940363433.mp3", sizeof(pods[0].url));
    savePods();
  }
}

int btConnState() {
  if (!audioReady || !a2dp) return 0;
  auto src = a2dp->source();
  return src.is_connected() ? 1 : 0;
}

bool ensureAudio() {
  if (audioReady) return true;

  a2dp = new A2DPStream();
  auto cfg = a2dp->defaultConfig(TX_MODE);
  cfg.name = BT_SPEAKER;
  cfg.auto_reconnect = true;
  a2dp->begin(cfg);

  decoder = new EncodedAudioStream(a2dp, new MP3DecoderHelix());
  podUrl = new URLStream();
  podCopier = new StreamCopy(*decoder, *podUrl, 1024);
  audioReady = true;
  return true;
}

void stopAll() {
  if (audioReady) {
    if (decoder) decoder->end();
    if (podUrl) podUrl->end();
  }
  stage = 0;
  motorStop();
  note("Stopped");
}

void startPodcastStage(int idx) {
  if (idx < 0 || idx >= MAX_PODCASTS || strlen(pods[idx].url) < 8) {
    note("No podcast URL");
    return;
  }
  ensureAudio();
  if (!podUrl->begin(pods[idx].url, "audio/mpeg")) {
    note("Podcast open failed");
    return;
  }
  decoder->begin();
  stage = 1;
  gotAny = false;
  lastData = millis();
  note("Playing podcast");
}

String askGroq(const String& userText) {
  if (strlen(GROQ_API_KEY) < 10) return "Groq key missing";

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, "https://api.groq.com/openai/v1/chat/completions")) {
    return "Connection failed";
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + GROQ_API_KEY);

  String body = "{";
  body += "\"model\":\"" + String(GROQ_MODEL) + "\",";
  body += "\"messages\":[";
  body += "{\"role\":\"system\",\"content\":\"You are a helpful robot assistant. Keep answers short.\"},";
  body += "{\"role\":\"user\",\"content\":\"" + escapeJson(userText) + "\"}";
  body += "],";
  body += "\"temperature\":0.7,";
  body += "\"max_tokens\":200";
  body += "}";

  int httpCode = http.POST(body);
  String payload = http.getString();
  http.end();

  if (httpCode != 200) {
    return "Groq request failed";
  }

  int start = payload.indexOf("\"content\":\"");
  if (start < 0) return "No response from Groq";

  start += 11;
  int end = payload.indexOf("\"", start);
  if (end < 0) return "Malformed response";

  String result = payload.substring(start, end);
  String out = "";
  for (size_t i = 0; i < result.length(); i++) {
    if (result[i] == '\\' && i + 1 < result.length()) {
      char nxt = result[i + 1];
      if (nxt == 'n') out += '\n';
      else if (nxt == '"') out += '"';
      else if (nxt == '\\') out += '\\';
      else out += nxt;
      i++;
    } else {
      out += result[i];
    }
  }
  out.trim();
  return out;
}

void processVoiceCommand(const String& command) {
  String cmd = command;
  cmd.trim();
  String lower = toLower(cmd);

  if (lower.indexOf("forward") >= 0 || lower.indexOf("go") >= 0) {
    drive(DRIVE_SPEED);
    note("Moving forward");
    return;
  }

  if (lower.indexOf("back") >= 0 || lower.indexOf("backward") >= 0) {
    drive(-DRIVE_SPEED);
    note("Moving backward");
    return;
  }

  if (lower.indexOf("left") >= 0) {
    Motor_Move(-DRIVE_SPEED, -DRIVE_SPEED, DRIVE_SPEED, DRIVE_SPEED);
    driving = true;
    driveStart = millis();
    note("Turning left");
    return;
  }

  if (lower.indexOf("right") >= 0) {
    Motor_Move(DRIVE_SPEED, DRIVE_SPEED, -DRIVE_SPEED, -DRIVE_SPEED);
    driving = true;
    driveStart = millis();
    note("Turning right");
    return;
  }

  if (lower.indexOf("stop") >= 0) {
    stopAll();
    note("Stopped");
    return;
  }

  if (lower.indexOf("podcast") >= 0 || lower.indexOf("music") >= 0) {
    if (stage != 0) stopAll();
    startPodcastStage(0);
    return;
  }

  String answer = askGroq(command);
  note("AI: " + answer);
}

void handleStatus() {
  struct tm t;
  char tb[24] = "syncing";
  if (getLocalTime(&t, 10)) strftime(tb, sizeof(tb), "%a %H:%M:%S", &t);

  String j = "{\"time\":\"" + String(tb) + "\",\"state\":\"";
  j += (stage == 1 ? "playing" : "idle");
  j += "\",\"log\":[";
  int n = logN < LOG_N ? logN : LOG_N;
  for (int k = 1; k <= n; k++) {
    String m = logLines[(logN - k) % LOG_N];
    m.replace("\"", "'");
    if (k > 1) j += ",";
    j += "\"" + m + "\"";
  }
  j += "]}";
  server.send(200, "application/json", j);
}

void handleRoot() {
  String html = "<!doctype html><html><head>";
  html += "<meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>Robot Assistant</title>";
  html += "<style>";
  html += "body { background: #0f1420; color: #e6ebf5; font: 16px system-ui; margin: 0; padding: 14px; }";
  html += ".wrap { max-width: 700px; margin: auto; }";
  html += "h1 { margin: 0 0 10px 0; }";
  html += ".card { background: #192032; border: 1px solid #2a3450; border-radius: 14px; padding: 14px; margin: 12px 0; }";
  html += "button { background: #4f8cff; color: white; border: 0; border-radius: 10px; padding: 10px 14px; cursor: pointer; margin: 2px; }";
  html += "input { width: 100%; padding: 10px; border: 1px solid #2a3450; background: #0f1626; color: white; border-radius: 8px; margin: 4px 0; font-size: 16px; }";
  html += "pre { background: #0f1626; padding: 10px; border-radius: 8px; overflow-x: auto; font-size: 12px; color: #8d99b3; }";
  html += "</style></head><body>";
  html += "<div class='wrap'>";
  html += "<h1>Robot Assistant</h1>";
  html += "<div class='card'>";
  html += "<p>Time: <b id='t'>--</b> | State: <b id='st'>idle</b></p>";
  html += "</div>";
  html += "<div class='card'>";
  html += "<h3>Voice</h3>";
  html += "<input type='text' id='cmd' placeholder='Ask or command...' />";
  html += "<button onclick='sendCmd()'>Send</button>";
  html += "</div>";
  html += "<div class='card'>";
  html += "<h3>Drive</h3>";
  html += "<button onclick='cmd(\"/drive?d=f\")'>Forward</button>";
  html += "<button onclick='cmd(\"/drive?d=b\")'>Backward</button>";
  html += "<button onclick='cmd(\"/drive?d=l\")'>Left</button>";
  html += "<button onclick='cmd(\"/drive?d=r\")'>Right</button>";
  html += "<button onclick='cmd(\"/drive?d=s\")'>Stop</button>";
  html += "</div>";
  html += "<div class='card'>";
  html += "<h3>Activity Log</h3>";
  html += "<pre id='log'>...</pre>";
  html += "</div>";
  html += "</div>";
  html += "<script>";
  html += "function cmd(u) { fetch(u + '&x=1').then(r => r.text()).catch(()=>{}); }";
  html += "function sendCmd() {";
  html += "  const v = document.getElementById('cmd').value.trim();";
  html += "  if (!v) return;";
  html += "  fetch('/voice?cmd=' + encodeURIComponent(v) + '&x=1').then(r => r.text());";
  html += "  document.getElementById('cmd').value = '';";
  html += "}";
  html += "function poll() {";
  html += "  fetch('/status').then(r => r.json()).then(s => {";
  html += "    document.getElementById('t').textContent = s.time;";
  html += "    document.getElementById('st').textContent = s.state;";
  html += "    document.getElementById('log').textContent = s.log.join('\\n') || '(nothing)';";
  html += "  }).catch(()=>{});";
  html += "}";
  html += "document.getElementById('cmd').addEventListener('keypress', e => { if (e.key === 'Enter') sendCmd(); });";
  html += "poll(); setInterval(poll, 2000);";
  html += "</script>";
  html += "</body></html>";
  
  server.send(200, "text/html", html);
}

void handleVoice() {
  String cmd = server.arg("cmd");
  if (cmd.length() > 0) processVoiceCommand(cmd);
  server.send(200, "text/plain", "ok");
}

void handleDrive() {
  String d = server.arg("d");
  if (d == "f") drive(DRIVE_SPEED);
  else if (d == "b") drive(-DRIVE_SPEED);
  else if (d == "l") {
    Motor_Move(-DRIVE_SPEED, -DRIVE_SPEED, DRIVE_SPEED, DRIVE_SPEED);
    driving = true;
    driveStart = millis();
  }
  else if (d == "r") {
    Motor_Move(DRIVE_SPEED, DRIVE_SPEED, -DRIVE_SPEED, -DRIVE_SPEED);
    driving = true;
    driveStart = millis();
  }
  else motorStop();
  server.send(200, "text/plain", "ok");
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== Groq Robot Alarm Clock ===\n");

  PCA9685_Setup();
  motorStop();
  Serial.println("Motors ready");

  loadPods();

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.print("WiFi connecting");
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 40) {
    delay(500);
    Serial.print(".");
    tries++;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi failed");
  }

  configTime(GMT_OFFSET_S, 0, "pool.ntp.org", "time.google.com");

  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/voice", handleVoice);
  server.on("/drive", handleDrive);
  server.begin();
  
  Serial.println("Server started");
}

void loop() {
  server.handleClient();

  if (driving && millis() - driveStart > DRIVE_MS) {
    motorStop();
    driving = false;
  }

  if (audioReady && stage == 1) {
    size_t n = podCopier->copy();
    if (n > 0) lastData = millis();
    if (millis() - lastData > 8000) {
      stopAll();
    }
  }
}
