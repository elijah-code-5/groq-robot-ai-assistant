// Groq Robot Alarm Clock - MINIMAL version
// ESP32 + Freenove 4WD + Bluetooth Speaker
// Optimized for memory

#include <WiFi.h>
#include <WebServer.h>
#include <time.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "Freenove_4WD_Car_For_ESP32.h"

const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";
const char* GROQ_API_KEY = "gsk_YOUR_KEY_HERE";

#define GMT_OFFSET_S 18000
#define DRIVE_SPEED 2000
#define DRIVE_MS 2000
#define LOG_N 8

WebServer server(80);

bool driving = false;
unsigned long driveStart = 0;

String logLines[LOG_N];
int logN = 0;

void note(const String& s) {
  Serial.println(s);
  logLines[logN % LOG_N] = s;
  logN++;
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

String askGroq(const String& userText) {
  if (strlen(GROQ_API_KEY) < 10) return "No key";

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  
  if (!http.begin(client, "https://api.groq.com/openai/v1/chat/completions")) {
    return "Failed";
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + GROQ_API_KEY);

  String body = "{\"model\":\"llama-3.3-70b-versatile\",\"messages\":[";
  body += "{\"role\":\"system\",\"content\":\"Brief answers.\"},";
  body += "{\"role\":\"user\",\"content\":\"" + escapeJson(userText) + "\"}";
  body += "],\"max_tokens\":100}";

  int httpCode = http.POST(body);
  String payload = http.getString();
  http.end();

  if (httpCode != 200) return "Error";

  int start = payload.indexOf("\"content\":\"");
  if (start < 0) return "No response";

  start += 11;
  int end = payload.indexOf("\"", start);
  if (end < 0) return "Parse error";

  String result = payload.substring(start, end);
  String out = "";
  for (size_t i = 0; i < result.length(); i++) {
    if (result[i] == '\\' && i + 1 < result.length()) {
      char nxt = result[i + 1];
      if (nxt == 'n') out += ' ';
      else if (nxt == '"') out += '"';
      else out += nxt;
      i++;
    } else {
      out += result[i];
    }
  }
  return out;
}

void processCommand(const String& cmd) {
  String lower = cmd;
  lower.toLowerCase();

  if (lower.indexOf("forward") >= 0 || lower.indexOf("go") >= 0) {
    drive(DRIVE_SPEED);
    note("Forward");
    return;
  }
  if (lower.indexOf("back") >= 0) {
    drive(-DRIVE_SPEED);
    note("Back");
    return;
  }
  if (lower.indexOf("left") >= 0) {
    Motor_Move(-DRIVE_SPEED, -DRIVE_SPEED, DRIVE_SPEED, DRIVE_SPEED);
    driving = true;
    driveStart = millis();
    note("Left");
    return;
  }
  if (lower.indexOf("right") >= 0) {
    Motor_Move(DRIVE_SPEED, DRIVE_SPEED, -DRIVE_SPEED, -DRIVE_SPEED);
    driving = true;
    driveStart = millis();
    note("Right");
    return;
  }
  if (lower.indexOf("stop") >= 0) {
    motorStop();
    note("Stopped");
    return;
  }

  String ans = askGroq(cmd);
  if (ans.length() > 50) ans = ans.substring(0, 50);
  note("AI: " + ans);
}

void handleStatus() {
  struct tm t;
  char tb[24] = "...";
  if (getLocalTime(&t, 5)) strftime(tb, sizeof(tb), "%H:%M:%S", &t);

  String j = "{\"t\":\"" + String(tb) + "\",\"log\":[";
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
  String h = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  h += "<meta name='viewport' content='width=device-width'>";
  h += "<title>Robot</title><style>";
  h += "body{bg:#111;color:#aaa;font:14px sans-serif;margin:0;padding:10px}";
  h += ".card{bg:#222;border:1px solid #444;border-radius:8px;padding:10px;margin:8px 0}";
  h += "button{bg:#4af;color:#fff;border:0;border-radius:5px;padding:8px 12px;margin:2px;cursor:pointer}";
  h += "input{width:100%;padding:8px;bg:#0a0a0a;color:#aaa;border:1px solid #333;border-radius:4px;margin:4px 0}";
  h += "pre{bg:#0a0a0a;padding:8px;border-radius:4px;font-size:11px;color:#666;height:150px;overflow-y:auto}";
  h += "</style></head><body>";
  h += "<h2>Robot</h2>";
  h += "<div class='card'>Time: <b id='t'>--</b></div>";
  h += "<div class='card'><input type='text' id='cmd' placeholder='Command...'>";
  h += "<button onclick='send()'>Send</button></div>";
  h += "<div class='card'><button onclick='c(\"/d?a=f\")'>Fwd</button>";
  h += "<button onclick='c(\"/d?a=b\")'>Back</button>";
  h += "<button onclick='c(\"/d?a=l\")'>L</button>";
  h += "<button onclick='c(\"/d?a=r\")'>R</button>";
  h += "<button onclick='c(\"/d?a=s\")'>Stop</button></div>";
  h += "<div class='card'>Log:<pre id='log'></pre></div>";
  h += "<script>";
  h += "function c(u){fetch(u).catch(e=>{});}";
  h += "function send(){const v=document.getElementById('cmd').value;if(v)fetch('/v?q='+encodeURIComponent(v));document.getElementById('cmd').value='';}";
  h += "function p(){fetch('/s').then(r=>r.json()).then(d=>{document.getElementById('t').textContent=d.t;document.getElementById('log').textContent=d.log.join('\\n');}).catch(e=>{});}";
  h += "document.getElementById('cmd').onkeypress=e=>{if(e.key==='Enter')send();}; p(); setInterval(p, 2000);";
  h += "</script></body></html>";
  server.send(200, "text/html", h);
}

void handleVoice() {
  String q = server.arg("q");
  if (q.length() > 0) processCommand(q);
  server.send(200, "text/plain", "ok");
}

void handleDrive() {
  String a = server.arg("a");
  if (a == "f") drive(DRIVE_SPEED);
  else if (a == "b") drive(-DRIVE_SPEED);
  else if (a == "l") { Motor_Move(-DRIVE_SPEED, -DRIVE_SPEED, DRIVE_SPEED, DRIVE_SPEED); driving = true; driveStart = millis(); }
  else if (a == "r") { Motor_Move(DRIVE_SPEED, DRIVE_SPEED, -DRIVE_SPEED, -DRIVE_SPEED); driving = true; driveStart = millis(); }
  else motorStop();
  server.send(200, "text/plain", "ok");
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\nRobot starting...");

  PCA9685_Setup();
  motorStop();

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.print("WiFi");
  int t = 0;
  while (WiFi.status() != WL_CONNECTED && t < 30) {
    delay(500);
    Serial.print(".");
    t++;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
  }

  configTime(GMT_OFFSET_S, 0, "pool.ntp.org");

  server.on("/", handleRoot);
  server.on("/s", handleStatus);
  server.on("/v", handleVoice);
  server.on("/d", handleDrive);
  server.begin();

  note("Ready");
}

void loop() {
  server.handleClient();

  if (driving && millis() - driveStart > DRIVE_MS) {
    motorStop();
  }
}
