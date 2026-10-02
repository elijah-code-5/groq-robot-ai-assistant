// ============================================================================
// Groq Robot Alarm Clock + AI Assistant
// ESP32-WROOM-32 + Freenove 4WD Car Kit + Bluetooth Speaker
// Clean, working Arduino sketch
// ============================================================================

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Wire.h>
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

// ========== CONFIGURATION ==========
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";
const char* BT_SPEAKER = "JBL Go 4";
const char* GROQ_API_KEY = "gsk_YOUR_KEY_HERE";
const char* GROQ_MODEL = "llama-3.3-70b-versatile";

#define GMT_OFFSET_S 18000
#define DRIVE_SPEED 2000
#define DRIVE_MS 2000
#define MAX_ALARMS 5
#define MAX_PODCASTS 10
#define LOG_N 10

// ========== GLOBALS ==========
WebServer server(80);
Preferences prefs;
bool driving = false;
unsigned long driveStart = 0;

A2DPStream* a2dp = nullptr;
URLStream* podUrl = nullptr;
EncodedAudioStream* decoder = nullptr;
StreamCopy* podCopier = nullptr;

bool audioReady = false;
int pendingPod = -1;
unsigned long pendingSince = 0;

enum Stage { IDLE, PODCAST, WAITBT };
Stage stage = IDLE;
bool gotAny = false;
unsigned long lastData = 0;

String logLines[LOG_N];
int logN = 0;

struct Podcast { 
  char name[32]; 
  char url[256]; 
};
Podcast pods[MAX_PODCASTS];

enum AlarmMode { MODE_OFF = 0, MODE_ONCE, MODE_DAILY, MODE_WEEKDAYS, MODE_MON_SAT };
const char* MODE_NAMES[] = { "Off", "Once", "Every day", "Mon-Fri", "Mon-Sat" };

struct Alarm {
  bool enabled;
  uint8_t hour, minute, mode;
  uint8_t podIdx;
  bool podcast, roll;
  char msg[48];
};
Alarm alarms[MAX_ALARMS];
int32_t lastFired[MAX_ALARMS];

// ========== HELPERS ==========
String toLower(const String& s) {
  String result = s;
  for (int i = 0; i < result.length(); i++) {
    result[i] = tolower((unsigned char)result[i]);
  }
  return result;
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

void note(const String& m) {
  Serial.println(m);
  logLines[logN % LOG_N] = m;
  logN++;
}

void notef(const char* fmt, ...) {
  char b[200];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof(b), fmt, ap);
  va_end(ap);
  note(String(b));
}

// ========== MOTOR ==========
void drive(int speed) {
  Motor_Move(speed, speed, speed, speed);
  driving = (speed != 0);
  driveStart = millis();
}

void motorStop() {
  Motor_Move(0, 0, 0, 0);
  driving = false;
}

// ========== STORAGE ==========
void savePods() {
  prefs.begin("pods", false);
  prefs.putBytes("p", pods, sizeof(pods));
  prefs.end();
  note("Podcasts saved");
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

void saveAlarms() {
  prefs.begin("alarms", false);
  prefs.putBytes("a", alarms, sizeof(alarms));
  prefs.end();
  note("Alarms saved");
}

void loadAlarms() {
  memset(alarms, 0, sizeof(alarms));
  prefs.begin("alarms", false);
  size_t n = prefs.getBytesLength("a");
  bool ok = (n == sizeof(alarms));
  if (ok) prefs.getBytes("a", alarms, sizeof(alarms));
  prefs.end();
  if (!ok) {
    alarms[0] = { true, 7, 30, MODE_DAILY, 0, true, true, "" };
    strlcpy(alarms[0].msg, "Time to wake up.", sizeof(alarms[0].msg));
    saveAlarms();
  }
  for (int i = 0; i < MAX_ALARMS; i++) lastFired[i] = -1;
}

bool dayMatches(uint8_t mode, int wday) {
  switch (mode) {
    case MODE_ONCE:
    case MODE_DAILY: return true;
    case MODE_WEEKDAYS: return wday >= 1 && wday <= 5;
    case MODE_MON_SAT: return wday >= 1 && wday <= 6;
    default: return false;
  }
}

// ========== AUDIO ==========
int btConnState() {
  if (!audioReady || !a2dp) return 0;
  auto src = a2dp->source();
  if (src.is_connected()) return 1;
  return 0;
}

bool ensureAudio() {
  if (audioReady) return true;
  notef("Starting BT. heap=%u", ESP.getFreeHeap());
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
  stage = IDLE;
  pendingPod = -1;
  motorStop();
  note("Stopped");
}

void beginPodcastNow(int idx) {
  notef("Opening: %s", pods[idx].name);
  bool ok = podUrl->begin(pods[idx].url, "audio/mpeg");
  if (!ok && strncmp(pods[idx].url, "https://", 8) == 0) {
    String alt = String("http://") + (pods[idx].url + 8);
    note("Trying http...");
    podUrl->end();
    ok = podUrl->begin(alt.c_str(), "audio/mpeg");
  }
  if (!ok) {
    note("Could not open podcast");
    stopAll();
    return;
  }
  decoder->begin();
  stage = PODCAST;
  gotAny = false;
  lastData = millis();
  note("Stream opened");
}

void startPodcastStage(int idx) {
  if (idx < 0 || idx >= MAX_PODCASTS || strlen(pods[idx].url) < 8) {
    note("No URL");
    stopAll();
    return;
  }
  ensureAudio();
  pendingPod = idx;
  pendingSince = millis();
  stage = WAITBT;
  note("Waiting for speaker...");
}

void fireAlarm(int idx, const struct tm& t) {
  Alarm& a = alarms[idx];
  notef("ALARM #%d", idx + 1);
  if (a.roll) drive(DRIVE_SPEED);
  char tb[16];
  strftime(tb, sizeof(tb), "%I:%M %p", &t);
  note(String("Good morning. Time is ") + tb + ". " + a.msg);
  if (a.podcast) startPodcastStage(a.podIdx);
  if (a.mode == MODE_ONCE) {
    a.enabled = false;
    saveAlarms();
  }
}

// ========== GROQ API ==========
String askGroq(const String& userText) {
  if (strlen(GROQ_API_KEY) < 10) return "No API key";

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  
  if (!http.begin(client, "https://api.groq.com/openai/v1/chat/completions")) {
    return "Connection failed";
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + GROQ_API_KEY);

  String body = "{\"model\":\"" + String(GROQ_MODEL) + "\",";
  body += "\"messages\":[";
  body += "{\"role\":\"system\",\"content\":\"You are a helpful robot assistant. Keep answers brief.\"},";
  body += "{\"role\":\"user\",\"content\":\"" + escapeJson(userText) + "\"}";
  body += "],\"temperature\":0.7,\"max_tokens\":200}";

  int httpCode = http.POST(body);
  String payload = http.getString();
  http.end();

  if (httpCode != 200) {
    notef("Groq HTTP %d", httpCode);
    return "API error";
  }

  int start = payload.indexOf("\"content\":\"");
  if (start < 0) return "No response";
  start += 11;

  int end = payload.indexOf("\"", start);
  if (end < 0) return "Parse error";

  String result = payload.substring(start, end);
  
  // Unescape JSON
  String clean = "";
  for (size_t i = 0; i < result.length(); i++) {
    if (result[i] == '\\' && i + 1 < result.length()) {
      char next = result[i + 1];
      if (next == 'n') clean += '\n';
      else if (next == '"') clean += '"';
      else if (next == '\\') clean += '\\';
      else clean += next;
      i++;
    } else {
      clean += result[i];
    }
  }
  clean.trim();
  return clean.length() > 0 ? clean : "No answer";
}

// ========== VOICE COMMANDS ==========
void processVoiceCommand(const String& command) {
  String cmd = command;
  cmd.trim();
  String cmdLower = toLower(cmd);

  if (cmdLower.indexOf("forward") >= 0 || cmdLower.indexOf("go") >= 0) {
    drive(DRIVE_SPEED);
    note("Voice: Forward");
    return;
  }
  if (cmdLower.indexOf("back") >= 0) {
    drive(-DRIVE_SPEED);
    note("Voice: Back");
    return;
  }
  if (cmdLower.indexOf("left") >= 0) {
    Motor_Move(-DRIVE_SPEED, -DRIVE_SPEED, DRIVE_SPEED, DRIVE_SPEED);
    driving = true;
    driveStart = millis();
    note("Voice: Left");
    return;
  }
  if (cmdLower.indexOf("right") >= 0) {
    Motor_Move(DRIVE_SPEED, DRIVE_SPEED, -DRIVE_SPEED, -DRIVE_SPEED);
    driving = true;
    driveStart = millis();
    note("Voice: Right");
    return;
  }
  if (cmdLower.indexOf("stop") >= 0) {
    stopAll();
    note("Voice: Stopped");
    return;
  }
  if (cmdLower.indexOf("podcast") >= 0 || cmdLower.indexOf("music") >= 0) {
    if (stage != IDLE) stopAll();
    startPodcastStage(0);
    note("Voice: Podcast");
    return;
  }

  String answer = askGroq(command);
  note("AI: " + answer);
}

// ========== WEB ==========
void handleStatus() {
  struct tm t;
  char tb[24] = "syncing";
  if (getLocalTime(&t, 10)) strftime(tb, sizeof(tb), "%a %H:%M:%S", &t);
  
  String j = "{\"time\":\"" + String(tb) + "\",\"state\":\"";
  j += (stage == PODCAST ? "playing" : "idle");
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
  String html = R"(
<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>Robot Assistant</title>
  <style>
    body { background: #0f1420; color: #e6ebf5; font: 16px system-ui; margin: 0; padding: 14px; }
    .wrap { max-width: 700px; margin: auto; }
    h1 { margin: 0; font-size: 20px; }
    .card { background: #192032; border: 1px solid #2a3450; border-radius: 14px; padding: 14px; margin: 12px 0; }
    button { background: #4f8cff; color: #fff; border: 0; border-radius: 10px; padding: 10px 14px; cursor: pointer; margin: 2px; }
    button.alt { background: #0f1626; color: #e6ebf5; border: 1px solid #2a3450; }
    .pad { display: grid; grid-template-columns: repeat(3, 76px); gap: 8px; justify-content: center; }
    .pad button { margin: 0; font-size: 28px; padding: 0; }
    input, select { background: #0f1626; color: #e6ebf5; border: 1px solid #2a3450; border-radius: 8px; padding: 8px; width: 100%; margin: 4px 0; }
    #log { font: 12px monospace; color: #8d99b3; white-space: pre-wrap; }
  </style>
</head>
<body>
  <div class="wrap">
    <h1>Robot Assistant</h1>
    <div class="card">
      <p>Time: <b id="t">--</b> | State: <b id="st">idle</b></p>
    </div>
    
    <div class="card">
      <h3>Voice</h3>
      <input type="text" id="cmd" placeholder="Ask anything...">
      <button onclick="sendCmd()">Send</button>
    </div>
    
    <div class="card">
      <h3>Drive</h3>
      <div class="pad">
        <span></span><button data-d="f">UP</button><span></span>
        <button data-d="l">L</button><button data-d="s" class="alt">STOP</button><button data-d="r">R</button>
        <span></span><button data-d="b">DN</button><span></span>
      </div>
    </div>
    
    <div class="card">
      <h3>Log</h3>
      <div id="log">...</div>
    </div>
  </div>
  
  <script>
    function cmd(u) { fetch(u + '&x=1').then(r => r.text()).catch(e => {}); }
    document.querySelectorAll('.pad button').forEach(b => {
      const d = b.dataset.d;
      b.addEventListener('pointerdown', e => { e.preventDefault(); cmd('/drive?d=' + d); });
      if (d != 's') {
        ['pointerup', 'pointerleave'].forEach(ev => b.addEventListener(ev, () => cmd('/drive?d=s')));
      }
    });
    
    function sendCmd() {
      const c = document.getElementById('cmd').value;
      if (c) {
        fetch('/voice?cmd=' + encodeURIComponent(c) + '&x=1').then(r => r.text());
        document.getElementById('cmd').value = '';
      }
    }
    
    function poll() {
      fetch('/status').then(r => r.json()).then(s => {
        document.getElementById('t').textContent = s.time;
        document.getElementById('st').textContent = s.state;
        document.getElementById('log').textContent = s.log.join('\n');
      }).catch(() => {});
    }
    
    document.getElementById('cmd').addEventListener('keypress', e => {
      if (e.key === 'Enter') { e.preventDefault(); sendCmd(); }
    });
    
    poll();
    setInterval(poll, 2000);
  </script>
</body>
</html>
  )";
  
  server.send(200, "text/html", html);
}

void handleDrive() {
  String d = server.arg("d");
  if (d == "f") drive(DRIVE_SPEED);
  else if (d == "b") drive(-DRIVE_SPEED);
  else if (d == "l") { Motor_Move(-DRIVE_SPEED, -DRIVE_SPEED, DRIVE_SPEED, DRIVE_SPEED); driving = true; driveStart = millis(); }
  else if (d == "r") { Motor_Move(DRIVE_SPEED, DRIVE_SPEED, -DRIVE_SPEED, -DRIVE_SPEED); driving = true; driveStart = millis(); }
  else motorStop();
  server.send(200, "text/plain", "ok");
}

void handleVoice() {
  String cmd = server.arg("cmd");
  if (cmd.length() > 0) processVoiceCommand(cmd);
  server.send(200, "text/plain", "ok");
}

// ========== SETUP ==========
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== Groq Robot Alarm Clock v9 ===\n");

  pinMode(0, INPUT_PULLUP);

  Serial.println("[Motors] Init...");
  PCA9685_Setup();
  motorStop();
  Serial.println("[Motors] Ready");

  loadAlarms();
  loadPods();

  WiFi.persistent(false);
  WiFi.setHostname("robot-alarm");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.print("[WiFi] Connecting");
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (millis() - t0 > 20000) {
      Serial.println("\nRetrying...");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS);
      t0 = millis();
    }
  }

  Serial.print("\n>>> IP: ");
  Serial.println(WiFi.localIP());

  configTime(GMT_OFFSET_S, 0, "pool.ntp.org", "time.google.com");
  MDNS.begin("alarm");

  server.on("/status", handleStatus);
  server.on("/", handleRoot);
  server.on("/drive", handleDrive);
  server.on("/voice", handleVoice);

  server.begin();
  Serial.println("[Server] Started\n");
}

// ========== LOOP ==========
void loop() {
  server.handleClient();

  static unsigned long lastNet = 0;
  if (millis() - lastNet > 30000) {
    lastNet = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WiFi] Reconnecting...");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS);
    }
  }

  if (stage == WAITBT) {
    int c = btConnState();
    if (c != 0 && pendingPod >= 0) {
      int i = pendingPod;
      pendingPod = -1;
      beginPodcastNow(i);
    } else if (millis() - pendingSince > 30000) {
      note("Speaker timeout");
      stopAll();
    }
  }

  if (audioReady && stage == PODCAST) {
    size_t n = podCopier->copy();
    if (n > 0) {
      lastData = millis();
      if (!gotAny) {
        gotAny = true;
        auto ai = decoder->audioInfo();
        notef("Audio: %u Hz, %u ch", (unsigned)ai.sample_rate, (unsigned)ai.channels);
      }
    }
    if (millis() - lastData > 8000) {
      stopAll();
    }
  }

  if (driving && millis() - driveStart > DRIVE_MS) {
    motorStop();
    driving = false;
  }

  static unsigned long lastCheck = 0;
  if (millis() - lastCheck > 1000) {
    lastCheck = millis();
    struct tm t;
    if (getLocalTime(&t, 10)) {
      int32_t stamp = ((t.tm_year * 366) + t.tm_yday) * 1440 + t.tm_hour * 60 + t.tm_min;
      for (int i = 0; i < MAX_ALARMS; i++) {
        Alarm& a = alarms[i];
        if (a.enabled && a.hour == t.tm_hour && a.minute == t.tm_min &&
            dayMatches(a.mode, t.tm_wday) && lastFired[i] != stamp) {
          lastFired[i] = stamp;
          fireAlarm(i, t);
          break;
        }
      }
    }
  }
}
