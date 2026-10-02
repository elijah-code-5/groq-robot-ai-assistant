// ============================================================================
// Groq Robot Alarm Clock + AI Assistant
// ESP32-WROOM-32 + Freenove 4WD Car Kit + JBL Go 4 Bluetooth Speaker
// Full Arduino sketch with Groq API integration
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

// ========== CONFIGURATION ==========
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";
const char* BT_SPEAKER = "JBL Go 4";
const char* GROQ_API_KEY = "gsk_YOUR_KEY_HERE";
const char* GROQ_MODEL = "llama-3.3-70b-versatile";

#define REQUIRE_LOGIN 0
const char* WEB_USER = "admin";
const char* WEB_PASS = "change-me";

const long GMT_OFFSET_S = 18000; // UTC+5

const char* SEED_PODCAST_NAME = "Podcast 1";
const char* SEED_PODCAST_URL = "https://dcs-cached.megaphone.fm/ARML4940363433.mp3";

#define ENABLE_GROQ_TTS 1
#define ENABLE_TTS 0
const char* WIT_TOKEN = "";
const char* WIT_VOICE = "Rebecca";

#define STOP_BTN_PIN 0
#define BT_PREWARM_MIN 2
#define DRIVE_SPEED 2000
#define DRIVE_MS 2000
#define ALARM_MAX_MS (15UL * 60UL * 1000UL)

// ========== CONSTANTS ==========
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

enum Stage { IDLE, TTS, PODCAST, WAITBT };
Stage stage = IDLE;
int wantPod = -1;
bool gotAny = false;
unsigned long lastData = 0;
unsigned long alarmStart = 0;

unsigned long countdownEnd = 0;
bool timerActive = false;

String logLines[LOG_N];
int logN = 0;

struct Podcast { char name[32]; char url[256]; };
Podcast pods[MAX_PODCASTS];

enum { MODE_OFF = 0, MODE_ONCE, MODE_DAILY, MODE_WEEKDAYS, MODE_MON_SAT };
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

// ========== HELPER FUNCTIONS ==========
String toLower(const String& s) {
  String result = s;
  for (int i = 0; i < result.length(); i++) result[i] = tolower((unsigned char)result[i]);
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
    else if (c == '\t') out += "\\t";
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

// ========== MOTOR CONTROL ==========
void drive(int speed) {
  Motor_Move(speed, speed, speed, speed);
  driving = (speed != 0);
  driveStart = millis();
}

void motorStop() {
  Motor_Move(0, 0, 0, 0);
  driving = false;
}

void motorMove(int d1, int d2, int d3, int d4) {
  Motor_Move(d1, d2, d3, d4);
}

// ========== STORAGE FUNCTIONS ==========
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
    strlcpy(pods[0].name, SEED_PODCAST_NAME, sizeof(pods[0].name));
    strlcpy(pods[0].url, SEED_PODCAST_URL, sizeof(pods[0].url));
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

// ========== AUDIO SYSTEM ==========
int btConnState() {
  if (!audioReady || !a2dp) return 0;
  auto src = a2dp->source();
  if (src.is_connected()) return 1;
  return 0;
}

bool ensureAudio() {
  if (audioReady) return true;
  notef("Starting BT. Looking for '%s'. heap=%u", BT_SPEAKER, ESP.getFreeHeap());
  a2dp = new A2DPStream();
  auto cfg = a2dp->defaultConfig(TX_MODE);
  cfg.name = BT_SPEAKER;
  cfg.auto_reconnect = true;
  a2dp->begin(cfg);
  decoder = new EncodedAudioStream(a2dp, new MP3DecoderHelix());
  podUrl = new URLStream();
  podCopier = new StreamCopy(*decoder, *podUrl, 1024);
  audioReady = true;
  note("Bluetooth started, searching for speaker...");
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
  driving = false;
  note("Stopped");
}

void beginPodcastNow(int idx) {
  notef("Opening: %s (heap %u)", pods[idx].name, ESP.getFreeHeap());
  bool ok = podUrl->begin(pods[idx].url, "audio/mpeg");
  if (!ok && strncmp(pods[idx].url, "https://", 8) == 0) {
    String alt = String("http://") + (pods[idx].url + 8);
    note("https failed, trying http...");
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
  note("Stream opened, waiting for audio...");
}

void startPodcastStage(int idx) {
  if (idx < 0 || idx >= MAX_PODCASTS || strlen(pods[idx].url) < 8) {
    note("No URL in podcast slot");
    stopAll();
    return;
  }
  ensureAudio();
  pendingPod = idx;
  pendingSince = millis();
  stage = WAITBT;
  note("Waiting for speaker...");
}

void speakThenPlay(const String& text, int pod) {
  if (stage != IDLE) stopAll();
  wantPod = pod;
  alarmStart = millis();
  note("Voice: " + text);
  if (pod >= 0) startPodcastStage(pod);
}

void fireAlarm(int idx, const struct tm& t) {
  Alarm& a = alarms[idx];
  notef("ALARM #%d fired", idx + 1);
  if (a.roll) drive(DRIVE_SPEED);
  char tb[16];
  strftime(tb, sizeof(tb), "%I:%M %p", &t);
  String text = String("Good morning. Time is ") + tb + ". " + a.msg;
  speakThenPlay(text, a.podcast ? (int)a.podIdx : -1);
  if (a.mode == MODE_ONCE) {
    a.enabled = false;
    saveAlarms();
  }
}

// ========== GROQ API INTEGRATION ==========
String askGroq(const String& userText) {
  if (strlen(GROQ_API_KEY) < 10) {
    return "Groq API key missing";
  }

  String prompt = "You are a helpful robot assistant. Keep answers short and practical. User: " + userText;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, "https://api.groq.com/openai/v1/chat/completions")) {
    return "Could not connect to Groq.";
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + GROQ_API_KEY);

  String body = "{";
  body += "\"model\":\"" + String(GROQ_MODEL) + "\",";
  body += "\"messages\":[";
  body += "{\"role\":\"system\",\"content\":\"You are a robot assistant. Keep answers brief.\"},";
  body += "{\"role\":\"user\",\"content\":\"" + escapeJson(prompt) + "\"}";
  body += "],";
  body += "\"temperature\":0.7,";
  body += "\"max_tokens\":250";
  body += "}";

  int httpCode = http.POST(body);
  String payload = http.getString();
  http.end();

  if (httpCode != HTTP_CODE_OK) {
    notef("Groq HTTP %d", httpCode);
    return "Groq request failed";
  }

  int start = payload.indexOf("\"content\":\"");
  if (start < 0) return "No response";
  start += 11;

  int end = payload.indexOf("\"", start);
  if (end < 0) return "Malformed response";

  String result = payload.substring(start, end);
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

// ========== VOICE COMMAND PROCESSING ==========
void processVoiceCommand(const String& command) {
  String cmd = command;
  cmd.trim();
  String cmdLower = toLower(cmd);

  size_t pos = cmdLower.indexOf("hey robot");
  if (pos != -1) cmdLower = cmdLower.substring(pos + 10);
  pos = cmdLower.indexOf("ok robot");
  if (pos != -1) cmdLower = cmdLower.substring(pos + 9);
  cmdLower.trim();

  if (cmdLower.indexOf("go") != -1 || cmdLower.indexOf("forward") != -1) {
    drive(DRIVE_SPEED);
    note("Voice: Forward");
    return;
  }
  if (cmdLower.indexOf("back") != -1 || cmdLower.indexOf("backward") != -1) {
    drive(-DRIVE_SPEED);
    note("Voice: Backward");
    return;
  }
  if (cmdLower.indexOf("left") != -1) {
    Motor_Move(-DRIVE_SPEED, -DRIVE_SPEED, DRIVE_SPEED, DRIVE_SPEED);
    driving = true;
    driveStart = millis();
    note("Voice: Left");
    return;
  }
  if (cmdLower.indexOf("right") != -1) {
    Motor_Move(DRIVE_SPEED, DRIVE_SPEED, -DRIVE_SPEED, -DRIVE_SPEED);
    driving = true;
    driveStart = millis();
    note("Voice: Right");
    return;
  }
  if (cmdLower.indexOf("stop") != -1 || cmdLower.indexOf("halt") != -1) {
    stopAll();
    note("Voice: Stopped");
    return;
  }
  if (cmdLower.indexOf("play") != -1 && cmdLower.indexOf("podcast") != -1) {
    for (int i = 0; i < MAX_PODCASTS; i++) {
      if (strlen(pods[i].url) > 0) {
        String podName = toLower(String(pods[i].name));
        if (cmdLower.indexOf(podName) != -1 || cmdLower.indexOf(String(i+1)) != -1) {
          if (stage != IDLE) stopAll();
          startPodcastStage(i);
          notef("Voice: Podcast %d", i+1);
          return;
        }
      }
    }
    if (strlen(pods[0].url) > 0) {
      if (stage != IDLE) stopAll();
      startPodcastStage(0);
      note("Voice: Playing podcast");
    }
    return;
  }
  if (cmdLower.indexOf("what time") != -1) {
    struct tm t;
    if (getLocalTime(&t, 10)) {
      char tb[16];
      strftime(tb, sizeof(tb), "%I:%M %p", &t);
      note("Voice: Time is " + String(tb));
    } else {
      note("Voice: Time unavailable");
    }
    return;
  }

  // Send to Groq for general queries
  String answer = askGroq(command);
  note("AI: " + answer);
}

// ========== WEB INTERFACE ==========
bool auth() {
#if REQUIRE_LOGIN
  if (!server.authenticate(WEB_USER, WEB_PASS)) {
    server.requestAuthentication();
    return false;
  }
#endif
  return true;
}

String podOptions(int sel) {
  String o;
  for (int i = 0; i < MAX_PODCASTS; i++) {
    if (strlen(pods[i].url) < 8) continue;
    o += "<option value=" + String(i) + (i == sel ? " selected>" : ">") + esc(pods[i].name) + "</option>";
  }
  return o;
}

void redirectHome() {
  if (server.hasArg("x")) {
    server.send(200, "text/plain", "ok");
    return;
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

const char* stateName() {
  switch (stage) {
    case TTS: return "speaking";
    case PODCAST: return "playing";
    case WAITBT: return "connecting";
    default: return "idle";
  }
}

const char* speakerName() {
  if (!audioReady) return "off";
  int c = btConnState();
  return c == 1 ? "connected" : "searching";
}

void handleStatus() {
  struct tm t;
  char tb[24] = "syncing";
  if (getLocalTime(&t, 10)) strftime(tb, sizeof(tb), "%a %H:%M:%S", &t);
  String j = String("{\"time\":\"") + tb + "\",\"state\":\"" + stateName() +
             "\",\"speaker\":\"" + speakerName() +
             "\",\"heap\":" + String(ESP.getFreeHeap()) + ",\"log\":[";
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

const char PAGE_HEAD[] PROGMEM = R"rawliteral(<!doctype html><html data-theme="dark"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Robot Assistant v8</title>
<style>:root{--bg:#0f1420;--card:#192032;--text:#e6ebf5;--accent:#4f8cff;--ok:#2ecc71;--bad:#ff4d5e;--line:#2a3450;--input:#0f1626}
html[data-theme=light]{--bg:#f2f4f9;--card:#fff;--text:#1b2233;--accent:#2f6bff;--ok:#1a9c52;--bad:#d9303f;--line:#d7dcea;--input:#f7f8fc}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:16px/1.4 system-ui,sans-serif}.wrap{max-width:700px;margin:auto;padding:14px}
h1{font-size:20px;margin:0}h3{margin:0 0 10px;font-size:13px;color:#8d99b3;text-transform:uppercase}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:14px;margin:12px 0}
.chips{display:flex;flex-wrap:wrap;gap:6px}.chip{background:var(--input);border:1px solid var(--line);border-radius:999px;padding:4px 10px;font-size:13px}
button{background:var(--accent);color:#fff;border:0;border-radius:10px;padding:10px 14px;cursor:pointer}button.alt{background:var(--input);color:var(--text);border:1px solid var(--line)}
.stop{background:var(--bad);width:100%;font-size:20px;padding:14px;margin:12px 0 0}
.pad{display:grid;grid-template-columns:repeat(3,76px);gap:8px;justify-content:center}
.pad button{margin:0;font-size:28px;padding:0}
input,select{background:var(--input);color:var(--text);border:1px solid var(--line);border-radius:8px;padding:8px;width:100%;margin:4px 0}
#log{font:12px/1.6 monospace;color:#8d99b3;white-space:pre-wrap;word-break:break-word}
#voiceInput{width:100%;padding:10px;margin:4px 0}</style></head><body><div class="wrap">
<h1>🤖 Robot Assistant v8</h1>
<div class="card"><div class="chips"><span class="chip">Time <b id="t">--</b></span><span class="chip">State <b id="st">--</b></span>
<span class="chip">Speaker <b id="sp">--</b></span><span class="chip">Mem <b id="hp">--</b></span></div></div>)rawliteral";

const char PAGE_END[] PROGMEM = R"rawliteral(
<div class="card"><h3>Voice</h3>
<input type="text" id="voiceInput" placeholder="Ask anything..."><button onclick="sendVoice()">Send</button></div>

<div class="card"><h3>Drive</h3>
<div class="pad">
<span></span><button data-d="f">▲</button><span></span>
<button data-d="l">◀</button><button data-d="s" class="alt">■</button><button data-d="r">▶</button>
<span></span><button data-d="b">▼</button><span></span>
</div>
<button class="stop" id="stop">STOP</button></div>

<div class="card"><h3>Podcast</h3>
<select id="podSel"></select><button onclick="playPod()">Play</button></div>

<div class="card"><h3>Log</h3><div id="log">...</div></div></div>
<script>
const $=s=>document.querySelector(s);
function cmd(u){fetch(u+'&x=1').then(r=>r.text()).catch(e=>console.log(e))}
document.querySelectorAll('.pad button').forEach(b=>{const d=b.dataset.d;
b.addEventListener('pointerdown',e=>{e.preventDefault();cmd('/drive?d='+d)});
if(d!='s'){['pointerup','pointerleave'].forEach(ev=>b.addEventListener(ev,()=>cmd('/drive?d=s')))}});
$('#stop').onclick=()=>cmd('/stop');
function sendVoice(){const v=$('#voiceInput').value;if(v){fetch('/voice?cmd='+encodeURIComponent(v)+'&x=1').then(r=>r.text());$('#voiceInput').value=''}}
$('#voiceInput').addEventListener('keypress',e=>{if(e.key==='Enter'){e.preventDefault();sendVoice()}});
function poll(){fetch('/status').then(r=>r.json()).then(s=>{$('#t').textContent=s.time;$('#st').textContent=s.state;$('#sp').textContent=s.speaker;$('#hp').textContent=Math.round(s.heap/1024)+' KB';$('#log').textContent=s.log.join('\\n')})}
poll();setInterval(poll,2000);
function playPod(){const s=$('#podSel').value;if(s){cmd('/play?p='+s)}}
</script></body></html>)rawliteral";

void handleRoot() {
  if (!auth()) return;
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  server.sendContent(PAGE_HEAD);
  
  String pod = "<select id='podSel'>";
  for (int i = 0; i < MAX_PODCASTS; i++) {
    if (strlen(pods[i].url) > 0) {
      pod += "<option value=" + String(i) + ">" + esc(pods[i].name) + "</option>";
    }
  }
  pod += "</select>";
  server.sendContent(pod);
  
  server.sendContent(PAGE_END);
}

void handleDrive() {
  if (!auth()) return;
  String d = server.arg("d");
  if (d == "f") drive(DRIVE_SPEED);
  else if (d == "b") drive(-DRIVE_SPEED);
  else if (d == "l") { Motor_Move(-DRIVE_SPEED, -DRIVE_SPEED, DRIVE_SPEED, DRIVE_SPEED); driving=true; driveStart=millis(); }
  else if (d == "r") { Motor_Move(DRIVE_SPEED, DRIVE_SPEED, -DRIVE_SPEED, -DRIVE_SPEED); driving=true; driveStart=millis(); }
  else motorStop();
  redirectHome();
}

void handleStop() {
  if (!auth()) return;
  stopAll();
  timerActive = false;
  redirectHome();
}

void handleVoice() {
  if (!auth()) return;
  String cmd = server.arg("cmd");
  if (cmd.length() > 0) processVoiceCommand(cmd);
  server.send(200, "text/plain", "ok");
}

void handlePlay() {
  if (!auth()) return;
  int i = server.arg("p").toInt();
  if (stage != IDLE) stopAll();
  wantPod = -1;
  alarmStart = millis();
  startPodcastStage(i);
  redirectHome();
}

void handlePing() {
  server.send(200, "text/plain", "pong");
}

// ========== SETUP ==========
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== Groq Robot Alarm Clock + AI Assistant ===\n");

  pinMode(STOP_BTN_PIN, INPUT_PULLUP);

  Serial.println("[Motors] Init Freenove...");
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
  server.on("/ping", handlePing);
  server.on("/", handleRoot);
  server.on("/drive", handleDrive);
  server.on("/stop", handleStop);
  server.on("/voice", handleVoice);
  server.on("/play", handlePlay);

  server.begin();
  Serial.println("\n[Server] Started\n");
}

// ========== LOOP ==========
void loop() {
  server.handleClient();

  static unsigned long lastNet = 0;
  if (millis() - lastNet > 30000) {
    lastNet = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WiFi] Lost, reconnecting...");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS);
    }
  }

  static unsigned long btnDown = 0;
  if (digitalRead(STOP_BTN_PIN) == LOW) {
    if (!btnDown) btnDown = millis();
    if (millis() - btnDown > 60 && stage != IDLE) stopAll();
  } else btnDown = 0;

  if (stage == WAITBT) {
    int c = btConnState();
    if (c != 0 && pendingPod >= 0) {
      if (c == 1) note("Speaker connected");
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
    if (millis() - alarmStart > ALARM_MAX_MS) stopAll();
  }

  if (driving && millis() - driveStart > DRIVE_MS) {
    motorStop();
    driving = false;
  }

  if (timerActive && (long)(millis() - countdownEnd) >= 0) {
    timerActive = false;
    drive(DRIVE_SPEED);
    speakThenPlay("Timer done", -1);
  }

  static unsigned long lastCheck = 0;
  if (millis() - lastCheck > 1000) {
    lastCheck = millis();
    struct tm t;
    if (getLocalTime(&t, 10)) {
      if (!audioReady) {
        int nowMin = t.tm_hour * 60 + t.tm_min;
        for (int i = 0; i < MAX_ALARMS; i++) {
          Alarm& a = alarms[i];
          if (a.enabled && a.podcast && dayMatches(a.mode, t.tm_wday)) {
            int diff = a.hour * 60 + a.minute - nowMin;
            if (diff >= 0 && diff <= BT_PREWARM_MIN) {
              ensureAudio();
              break;
            }
          }
        }
      }
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
