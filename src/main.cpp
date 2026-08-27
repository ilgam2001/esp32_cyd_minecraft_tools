
#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <ArduinoJson.h>
#include <WebServer.h>
#include <Preferences.h>
#include <TJpg_Decoder.h>
#include <PNGdec.h>
#include <base64.hpp>
#include <HTTPClient.h>
#include <vector>

#include "ui_jpg.h"

namespace {
PNG faviconPng;

struct FaviconDrawState {
  TFT_eSPI *tft;
  int x;
  int y;
};

struct SkinDrawState {
  TFT_eSPI *tft;
  int x;
  int y;
  PNG *png;
};

String stripMinecraftFormatting(const String &text);

constexpr uint16_t SCREEN_W = 320;
constexpr uint16_t SCREEN_H = 240;
constexpr uint16_t DEFAULT_PORT = 25565;
constexpr char DEFAULT_WIFI_SSID[] = "YOUR_WIFI_SSID";
constexpr char DEFAULT_WIFI_PASSWORD[] = "YOUR_WIFI_PASSWORD";

TFT_eSPI tft = TFT_eSPI();
XPT2046_Touchscreen touch(TOUCH_CS, TOUCH_IRQ);
SPIClass touchSpi(HSPI);

struct Key {
  int x;
  int y;
  int w;
  int h;
  String label;
  char value;
  bool action;
};

struct ServerInfo {
  String host;
  int port;
  int ping;
  String version;
  String motd;
  int online;
  int maxPlayers;
  String players;
  String status;
};

struct FinderResultEntry {
  String host;
  int port;
  String status;
  String motd;
  int online;
  int maxPlayers;
  int ping;
};

enum class ScreenMode { MainMenu, Input, Info, Finder, Skin };

ScreenMode currentMode = ScreenMode::MainMenu;
String serverInput = "192.168.8.2:5565";
ServerInfo currentServer;
String lastMessage = "Open config page";
String finderHost = "192.168.8.2";
String finderStatus = "Not scanned";
String finderResult = "";
String skinNick = "Notch";
bool skinImageLoaded = false;
std::vector<uint8_t> skinPngBytes;
std::vector<FinderResultEntry> finderResults;
int finderSelectedIndex = 0;
int finderPortStart = 25565;
int finderPortEnd = 25610;
Preferences preferences;
WebServer server(80);
uint32_t lastPingRefreshMs = 0;

void saveServerInputToPreferences(const String &input) {
  preferences.begin("mc_server", false);
  preferences.putString("host", input);
  preferences.end();
}

void saveFinderRangeToPreferences(int startPort, int endPort) {
  preferences.begin("mc_server", false);
  preferences.putInt("finder_start", startPort);
  preferences.putInt("finder_end", endPort);
  preferences.end();
}

void saveSkinNickToPreferences(const String &nick) {
  preferences.begin("mc_server", false);
  preferences.putString("skin_nick", nick);
  preferences.end();
}

void loadServerInputFromPreferences() {
  preferences.begin("mc_server", true);
  String saved = preferences.getString("host", "192.168.8.2:5565");
  preferences.end();
  if (saved.length() > 0) {
    serverInput = saved;
  }
}

void loadFinderRangeFromPreferences() {
  preferences.begin("mc_server", true);
  finderPortStart = preferences.getInt("finder_start", 25565);
  finderPortEnd = preferences.getInt("finder_end", 25610);
  preferences.end();
  if (finderPortStart <= 0) finderPortStart = 25565;
  if (finderPortEnd <= 0) finderPortEnd = 25610;
  if (finderPortStart > finderPortEnd) {
    int tmp = finderPortStart;
    finderPortStart = finderPortEnd;
    finderPortEnd = tmp;
  }
}

void loadSkinNickFromPreferences() {
  preferences.begin("mc_server", true);
  String saved = preferences.getString("skin_nick", "Notch");
  preferences.end();
  if (saved.length() > 0) {
    skinNick = saved;
  }
}

String buildConfigHtml() {
  return String(
      "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>"
      "<title>MC Server Config</title><style>body{font-family:Arial,sans-serif;background:#101820;color:#f5f7fa;margin:0;padding:20px}"
      ".card{max-width:420px;margin:40px auto;background:#1f2c39;border-radius:16px;padding:24px;box-shadow:0 8px 22px rgba(0,0,0,.25)}"
      "h1{margin-top:0;color:#7ef29a;font-size:24px}label{display:block;margin:18px 0 8px;color:#dfe8f3}input{width:100%;box-sizing:border-box;"
      "padding:12px;border-radius:10px;border:1px solid #4e677d;background:#0d1520;color:#fff;font-size:18px}button{margin-top:20px;"
      "width:100%;padding:12px;border:0;border-radius:10px;background:#2d8cff;color:#fff;font-size:18px;font-weight:700}"
      "</style></head><body><div class='card'><h1>Server config</h1><form method='POST' action='/save'>"
      "<label>Host/IP</label><input name='host' value='" + serverInput.substring(0, serverInput.lastIndexOf(':') > 0 ? serverInput.lastIndexOf(':') : serverInput.length()) + "' required>"
      "<label>Port</label><input name='port' type='number' min='1' max='65535' value='" + String(serverInput.lastIndexOf(':') > 0 ? serverInput.substring(serverInput.lastIndexOf(':') + 1).toInt() : 5565) + "' required>"
      "<button type='submit'>Save</button></form></div></body></html>");
}

void handleConfigRoot() {
  server.send(200, "text/html", buildConfigHtml());
}

void handleConfigSave() {
  String host = server.arg("host");
  String port = server.arg("port");
  host.trim();
  port.trim();

  if (host.length() == 0 || port.length() == 0) {
    server.send(400, "text/plain", "Host and port required");
    return;
  }

  String candidate = host + ":" + port;
  serverInput = candidate;
  saveServerInputToPreferences(candidate);
  lastMessage = "Saved: " + candidate;
  server.send(200, "text/html", "<html><body style='background:#101820;color:#fff;font-family:Arial;padding:32px;'><h2>Saved</h2><p>" + candidate + "</p><p><a href='/' style='color:#7ef29a'>Back</a></p></body></html>");
}

bool connectToWiFi();
String urlEncode(const String &value);

void startConfigPortal() {
  if (WiFi.status() != WL_CONNECTED) {
    if (!connectToWiFi()) {
      lastMessage = "WiFi not connected";
      return;
    }
  }

  server.on("/", HTTP_GET, handleConfigRoot);
  server.on("/save", HTTP_POST, handleConfigSave);
  server.begin();

  IPAddress ip = WiFi.localIP();
  lastMessage = "Open http://" + ip.toString();
  currentMode = ScreenMode::Input;
}

bool tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
  if (y >= SCREEN_H) return 0;
  tft.pushImage(x, y, w, h, bitmap);
  return 1;
}

bool isPointInsideRect(int x, int y, int rx, int ry, int rw, int rh) {
  return x >= rx && x <= rx + rw && y >= ry && y <= ry + rh;
}

void drawMinecraftBackground() {
  TJpgDec.drawJpg(0, 0, ui_jpg, sizeof(ui_jpg));
  tft.fillScreen(TFT_DARKGREY);
  for (int y = 0; y < SCREEN_H; y += 16) {
    for (int x = 0; x < SCREEN_W; x += 16) {
      if ((x / 16 + y / 16) % 2 == 0) {
        tft.fillRect(x, y, 16, 16, TFT_DARKGREY);
      } else {
        tft.fillRect(x, y, 16, 16, TFT_LIGHTGREY);
      }
    }
  }

  tft.fillRect(0, 0, SCREEN_W, 18, TFT_BLACK);
  tft.fillRect(0, 18, SCREEN_W, 2, TFT_GREENYELLOW);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("Minecraft Server Info", 12, 2);
}

void drawButtonRect(int x, int y, int w, int h, const String &text, uint16_t fillColor, uint16_t borderColor) {
  tft.fillRoundRect(x, y, w, h, 6, fillColor);
  tft.drawRoundRect(x, y, w, h, 6, borderColor);
  tft.setTextColor(TFT_WHITE, fillColor);
  tft.setTextSize(2);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(text, x + w / 2, y + h / 2);
}

void drawInputBox() {
  tft.fillRoundRect(12, 30, 220, 52, 8, TFT_BLACK);
  tft.drawRoundRect(12, 30, 220, 52, 8, TFT_GREENYELLOW);
  tft.setTextColor(TFT_GREENYELLOW, TFT_BLACK);
  tft.setTextSize(2);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("Server IP:", 18, 34);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);
  String ip = serverInput;
  if (ip.length() > 22) {
    ip = ip.substring(0, 22) + "...";
  }
  tft.drawString(ip, 18, 62);

  drawButtonRect(240, 30, 68, 34, "Ping", TFT_BLUE, TFT_WHITE);
  drawButtonRect(240, 70, 68, 34, "Clear", TFT_RED, TFT_WHITE);
  drawButtonRect(240, 110, 68, 34, "Web", TFT_DARKGREEN, TFT_WHITE);
}

void drawInputScreen() {
  drawMinecraftBackground();
  drawInputBox();

  tft.fillRoundRect(12, 150, 296, 70, 8, TFT_BLACK);
  tft.drawRoundRect(12, 150, 296, 70, 8, TFT_GREENYELLOW);
  tft.setTextColor(TFT_GREENYELLOW, TFT_BLACK);
  tft.setTextSize(2);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("MOTD", 24, 160);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);
  String motd = currentServer.motd.length() > 0 ? currentServer.motd : "Minecraft server";
  motd.replace("\n", " ");
  if (motd.length() > 52) motd = motd.substring(0, 52) + "...";
  tft.drawString(motd, 24, 188);

  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setTextSize(1);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(lastMessage, 12, 126);
}

void drawServerIcon(int x, int y) {
  tft.fillRoundRect(x, y, 52, 52, 8, TFT_DARKGREEN);
  tft.fillRoundRect(x + 6, y + 6, 40, 40, 8, TFT_GREEN);
  tft.fillRect(x + 14, y + 20, 24, 24, TFT_BROWN);
  tft.fillRect(x + 18, y + 16, 16, 8, TFT_YELLOW);
  tft.drawRoundRect(x, y, 52, 52, 8, TFT_WHITE);
}

void drawInfoScreen() {
  drawMinecraftBackground();

  drawServerIcon(16, 28);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(currentServer.host, 80, 36);
  tft.setTextSize(1);
  tft.drawString("Version: " + currentServer.version, 80, 68);
  tft.drawString("Ping: " + String(currentServer.ping) + " ms", 80, 86);
  tft.drawString("Players: " + String(currentServer.online) + "/" + String(currentServer.maxPlayers), 80, 104);

  tft.fillRoundRect(14, 136, 292, 78, 8, TFT_BLACK);
  tft.drawRoundRect(14, 136, 292, 78, 8, TFT_GREENYELLOW);
  tft.setTextColor(TFT_GREENYELLOW, TFT_BLACK);
  tft.setTextSize(2);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("MOTD", 24, 146);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);
  String motd = currentServer.motd;
  motd.replace("\n", " ");
  if (motd.length() > 42) {
    motd = motd.substring(0, 42) + "...";
  }
  tft.drawString(motd, 24, 170);

  tft.fillRoundRect(14, 222, 292, 14, 8, TFT_NAVY);
  tft.drawRoundRect(14, 222, 292, 14, 8, TFT_WHITE);
  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.setTextSize(1);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(currentServer.players.length() > 0 ? currentServer.players : "No players listed", 20, 224);

  drawButtonRect(240, 26, 70, 24, "Back", TFT_RED, TFT_WHITE);
}

void clearServerInput() {
  serverInput = "";
}

bool readVarInt(WiFiClient &client, int &value, uint32_t timeoutMs = 3000) {
  value = 0;
  int pos = 0;
  uint32_t start = millis();
  while (true) {
    while (client.connected() && !client.available()) {
      if (millis() - start > timeoutMs) return false;
      delay(10);
    }
    if (!client.connected() && !client.available()) return false;

    uint8_t byte = client.read();
    value |= (byte & 0x7F) << (pos * 7);
    if ((byte & 0x80) == 0) {
      return true;
    }
    pos++;
    if (pos >= 5) return false;
  }
}

String readVarString(WiFiClient &client, uint32_t timeoutMs = 3000) {
  int len = 0;
  if (!readVarInt(client, len, timeoutMs)) return "";
  if (len < 0 || len > 65535) return "";

  uint32_t start = millis();
  char *buf = new char[len + 1];
  size_t bytesRead = 0;
  while (bytesRead < static_cast<size_t>(len)) {
    while (client.connected() && !client.available()) {
      if (millis() - start > timeoutMs) {
        delete[] buf;
        return "";
      }
      delay(10);
    }
    if (!client.connected() && !client.available()) {
      delete[] buf;
      return "";
    }
    buf[bytesRead++] = static_cast<char>(client.read());
  }
  buf[len] = '\0';
  String result(buf);
  delete[] buf;
  return result;
}

String stripMinecraftFormatting(const String &text) {
  String out;
  for (size_t i = 0; i < text.length(); ++i) {
    unsigned char c = static_cast<unsigned char>(text[i]);

    if (c == 0xC2 && i + 1 < text.length() && static_cast<unsigned char>(text[i + 1]) == 0xA7) {
      i += 1;
      continue;
    }
    if (c == 0xA7) {
      if (i + 1 < text.length()) {
        i += 1;
      }
      continue;
    }
    if (c == '\n' || c == '\r') {
      if (out.length() == 0 || out[out.length() - 1] != ' ') out += ' ';
      continue;
    }
    if (c < 0x20 || c == 0x7F) {
      continue;
    }
    out += static_cast<char>(c);
  }
  return out;
}

String readJsonText(const JsonVariantConst &value) {
  if (value.is<const char *>()) {
    return stripMinecraftFormatting(value.as<const char *>());
  }
  if (value.is<JsonObject>()) {
    String result;
    if (value["text"].is<const char *>()) {
      result += stripMinecraftFormatting(value["text"].as<const char *>());
    }
    JsonVariantConst extraValue = value["extra"];
    if (!extraValue.isNull() && extraValue.is<JsonArray>()) {
      JsonArrayConst extra = extraValue.as<JsonArrayConst>();
      for (JsonVariantConst v : extra) {
        String extraText = readJsonText(v);
        if (extraText.length() > 0) {
          if (result.length() > 0 && result[result.length() - 1] != ' ') result += ' ';
          result += extraText;
        }
      }
    }
    return result;
  }
  if (value.is<JsonArray>()) {
    String result;
    JsonArrayConst array = value.as<JsonArrayConst>();
    for (JsonVariantConst v : array) {
      String item = readJsonText(v);
      if (item.length() > 0) {
        if (result.length() > 0 && result[result.length() - 1] != ' ') result += ' ';
        result += item;
      }
    }
    return result;
  }
  return "";
}

void appendVarInt(std::vector<uint8_t> &out, int value) {
  int v = value;
  while (true) {
    if ((v & 0x7F) == v) {
      out.push_back(static_cast<uint8_t>(v));
      return;
    }
    out.push_back(static_cast<uint8_t>((v & 0x7F) | 0x80));
    v = v >> 7;
  }
}

void appendVarString(std::vector<uint8_t> &out, const String &value) {
  appendVarInt(out, value.length());
  for (size_t i = 0; i < value.length(); ++i) {
    out.push_back(static_cast<uint8_t>(value[i]));
  }
}

void appendU16(std::vector<uint8_t> &out, uint16_t value) {
  out.push_back((value >> 8) & 0xFF);
  out.push_back(value & 0xFF);
}

std::vector<uint8_t> makeStatusHandshake(const String &host, uint16_t port) {
  std::vector<uint8_t> payload;
  payload.push_back(0x00);  // packet id: handshake
  appendVarInt(payload, 47);
  appendVarString(payload, host);
  appendU16(payload, port);
  appendVarInt(payload, 1);  // status state

  std::vector<uint8_t> packet;
  appendVarInt(packet, payload.size());
  packet.insert(packet.end(), payload.begin(), payload.end());
  return packet;
}

std::vector<uint8_t> makeStatusRequest() {
  std::vector<uint8_t> payload;
  payload.push_back(0x00);  // packet id: request

  std::vector<uint8_t> packet;
  appendVarInt(packet, payload.size());
  packet.insert(packet.end(), payload.begin(), payload.end());
  return packet;
}

bool queryMinecraftServer(const String &input, ServerInfo &out) {
  String host = input;
  host.trim();
  if (host.length() == 0) return false;

  if (host.startsWith("http://")) host.remove(0, 7);
  if (host.startsWith("https://")) host.remove(0, 8);

  int port = DEFAULT_PORT;
  int pos = host.lastIndexOf(':');
  if (pos > 0 && host.indexOf(':') == pos) {
    String portStr = host.substring(pos + 1);
    if (portStr.length() > 0) port = portStr.toInt();
    host = host.substring(0, pos);
  }

  if (host.length() == 0) return false;

  IPAddress ip;
  bool resolved = false;
  if (ip.fromString(host)) {
    resolved = true;
  } else {
    if (WiFi.hostByName(host.c_str(), ip)) {
      resolved = true;
    } else {
      IPAddress fallbackDns(8, 8, 8, 8);
      if (WiFi.dnsIP(0) != INADDR_NONE) {
        fallbackDns = WiFi.dnsIP(0);
      }
      WiFi.config(WiFi.localIP(), WiFi.gatewayIP(), WiFi.subnetMask(), fallbackDns);
      if (WiFi.hostByName(host.c_str(), ip)) {
        resolved = true;
      }
    }
  }

  if (!resolved) {
    lastMessage = "DNS failed; use IP";
    out.status = "DNS failed";
    Serial.printf("DNS failed for %s\n", host.c_str());
    return false;
  }

  WiFiClient client;
  client.setTimeout(2000);
  uint32_t start = millis();
  if (!client.connect(ip, port)) {
    out.status = "Offline";
    return false;
  }

  // РџСЂРѕС‚РѕРєРѕР» Minecraft 1.7+: СЃРЅР°С‡Р°Р»Р° handshake, Р·Р°С‚РµРј request, Р·Р°С‚РµРј JSON-РѕС‚РІРµС‚.
  auto handshake = makeStatusHandshake(host, port);
  size_t written = client.write(handshake.data(), handshake.size());
  if (written != handshake.size()) {
    client.stop();
    out.status = "Write failed";
    return false;
  }

  delay(50);

  auto request = makeStatusRequest();
  written = client.write(request.data(), request.size());
  if (written != request.size()) {
    client.stop();
    out.status = "Request failed";
    return false;
  }

  int packetLen = 0;
  if (!readVarInt(client, packetLen, 5000) || packetLen <= 0) {
    client.stop();
    out.status = "No reply";
    return false;
  }

  int packetId = 0;
  if (!readVarInt(client, packetId, 5000) || packetId != 0x00) {
    client.stop();
    out.status = "Bad packet";
    return false;
  }

  String json = readVarString(client, 5000);
  client.stop();
  if (json.length() < 10) {
    out.status = "Empty response";
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, json);
  if (err) {
    out.status = "JSON parse failed";
    Serial.println("JSON error: " + String(err.c_str()));
    return false;
  }

  out.host = host;
  out.port = port;
  out.ping = millis() - start;

  out.version = stripMinecraftFormatting(doc["version"]["name"].as<String>());
  if (out.version.length() == 0) out.version = "Unknown";

  String motdText = "";
  if (doc["description"].is<JsonVariant>()) {
    motdText = readJsonText(doc["description"]);
  }
  if (motdText.length() == 0) {
    motdText = "Minecraft server";
  }
  out.motd = stripMinecraftFormatting(motdText);

  out.online = doc["players"]["online"].as<int>();
  out.maxPlayers = doc["players"]["max"].as<int>();
  out.players = "";

  JsonArray sample = doc["players"]["sample"].as<JsonArray>();
  if (!sample.isNull()) {
    for (JsonVariant v : sample) {
      String name = stripMinecraftFormatting(v["name"].as<String>());
      if (name.length() == 0) continue;
      if (out.players.length() > 0) out.players += ", ";
      out.players += name;
    }
  }
  if (out.players.length() == 0) {
    if (out.maxPlayers > 0) {
      out.players = String(out.online) + "/" + String(out.maxPlayers);
    } else {
      out.players = "No players shown";
    }
  }

  out.status = "Online";
  return true;
}

void renderCurrentScreen();

bool connectToWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(DEFAULT_WIFI_SSID, DEFAULT_WIFI_PASSWORD);
  Serial.println("Connecting to WiFi...");
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi connected: " + WiFi.localIP().toString());
    IPAddress gateway = WiFi.gatewayIP();
    if (gateway != INADDR_NONE) {
      Serial.printf("Gateway: %s\n", gateway.toString().c_str());
    }
    return true;
  }
  Serial.println("WiFi failed");
  return false;
}

void scanServerFinder() {
  String host = finderHost;
  host.trim();
  if (host.length() == 0) {
    host = serverInput;
  }

  int pos = host.lastIndexOf(':');
  if (pos > 0 && host.indexOf(':') == pos) {
    host = host.substring(0, pos);
  }
  if (host.length() == 0) {
    finderStatus = "Enter host/IP";
    finderResult = "";
    finderResults.clear();
    finderSelectedIndex = 0;
    return;
  }

  finderHost = host;
  finderResults.clear();
  finderSelectedIndex = 0;

  int portCount = finderPortEnd - finderPortStart + 1;
  if (portCount <= 0) {
    portCount = 1;
  }
  if (portCount > 256) {
    portCount = 256;
  }

  finderStatus = "Scanning " + String(finderPortStart) + "-" + String(finderPortEnd) + "...";
  finderResult = "";
  for (int port = finderPortStart; port <= finderPortEnd && port <= 65535; ++port) {
    String candidate = host + ":" + String(port);
    ServerInfo probe;
    if (queryMinecraftServer(candidate, probe)) {
      FinderResultEntry entry;
      entry.host = host;
      entry.port = port;
      entry.status = probe.status;
      entry.motd = probe.motd;
      entry.online = probe.online;
      entry.maxPlayers = probe.maxPlayers;
      entry.ping = probe.ping;
      finderResults.push_back(entry);
      if (finderResults.size() >= 32) {
        break;
      }
    }
  }

  if (finderResults.size() == 0) {
    finderStatus = "No server found on common ports";
    finderResult = "Try a custom port or use direct IP";
    return;
  }

  finderStatus = "Found " + String(finderResults.size()) + " server(s)";
  finderResult = finderResults[0].host + ":" + String(finderResults[0].port) + " | " + finderResults[0].motd;
  finderSelectedIndex = 0;
}

void handleTouchInput() {
  if (!touch.touched()) return;
  TS_Point p = touch.getPoint();
  int x = map(p.x, 300, 3900, 0, SCREEN_W);
  int y = map(p.y, 300, 3900, 0, SCREEN_H);

  if (currentMode == ScreenMode::MainMenu) {
    if (isPointInsideRect(x, y, 20, 70, 280, 30)) {
      currentMode = ScreenMode::Input;
      renderCurrentScreen();
      return;
    }
    if (isPointInsideRect(x, y, 20, 108, 280, 30)) {
      finderHost = (serverInput.indexOf(':') > 0) ? serverInput.substring(0, serverInput.lastIndexOf(':')) : serverInput;
      currentMode = ScreenMode::Finder;
      renderCurrentScreen();
      return;
    }
    if (isPointInsideRect(x, y, 20, 146, 280, 30)) {
      currentMode = ScreenMode::Skin;
      renderCurrentScreen();
      return;
    }
    if (isPointInsideRect(x, y, 20, 184, 280, 30)) {
      startUnifiedConfigPortal();
      renderCurrentScreen();
      return;
    }
    return;
  }

  if (currentMode == ScreenMode::Input) {
    if (isPointInsideRect(x, y, 228, 48, 74, 28)) {
      if (WiFi.status() != WL_CONNECTED && !connectToWiFi()) {
        lastMessage = "WiFi not connected";
        drawInputScreen();
        return;
      }
      if (queryMinecraftServer(serverInput, currentServer)) {
        currentMode = ScreenMode::Info;
        lastMessage = "Server online";
      } else {
        lastMessage = "Server offline / no reply";
        currentServer.status = "Offline";
      }
      renderCurrentScreen();
      return;
    }

    if (isPointInsideRect(x, y, 228, 82, 74, 28)) {
      startConfigPortal();
      renderCurrentScreen();
      return;
    }

    if (isPointInsideRect(x, y, 20, 196, 280, 28)) {
      currentMode = ScreenMode::MainMenu;
      renderCurrentScreen();
      return;
    }
    return;
  }

  if (currentMode == ScreenMode::Finder) {
    if (isPointInsideRect(x, y, 188, 58, 116, 24)) {
      scanServerFinder();
      renderCurrentScreen();
      return;
    }
    if (isPointInsideRect(x, y, 188, 88, 116, 24)) {
      startFinderConfigPortal();
      renderCurrentScreen();
      return;
    }
    if (isPointInsideRect(x, y, 12, 96, 82, 22)) {
      if (finderResults.size() > 0) {
        finderSelectedIndex = (finderSelectedIndex - 1 + finderResults.size()) % finderResults.size();
        finderResult = finderResults[finderSelectedIndex].host + ":" + String(finderResults[finderSelectedIndex].port) + " | " + finderResults[finderSelectedIndex].motd;
      }
      renderCurrentScreen();
      return;
    }
    if (isPointInsideRect(x, y, 100, 96, 82, 22)) {
      if (finderResults.size() > 0) {
        finderSelectedIndex = (finderSelectedIndex + 1) % finderResults.size();
        finderResult = finderResults[finderSelectedIndex].host + ":" + String(finderResults[finderSelectedIndex].port) + " | " + finderResults[finderSelectedIndex].motd;
      }
      renderCurrentScreen();
      return;
    }
    if (isPointInsideRect(x, y, 12, 124, 170, 22)) {
      currentMode = ScreenMode::MainMenu;
      renderCurrentScreen();
      return;
    }
    return;
  }

  if (currentMode == ScreenMode::Skin) {
    if (isPointInsideRect(x, y, 220, 58, 88, 26)) {
      skinStatus = "Fetching...";
      skinImageLoaded = false;
      if (loadSkinPreviewForNick()) {
        lastMessage = "Skin loaded";
      } else {
        lastMessage = "Skin not found";
      }
      renderCurrentScreen();
      return;
    }
    if (isPointInsideRect(x, y, 220, 90, 88, 26)) {
      startSkinConfigPortal();
      renderCurrentScreen();
      return;
    }
    if (isPointInsideRect(x, y, 220, 122, 88, 26)) {
      currentMode = ScreenMode::MainMenu;
      renderCurrentScreen();
      return;
    }
    return;
  }

  if (currentMode == ScreenMode::Info) {
    if (isPointInsideRect(x, y, 240, 26, 70, 24)) {
      currentMode = ScreenMode::MainMenu;
      renderCurrentScreen();
      return;
    }
  }
}

void setupDisplay() {
  tft.begin();
  tft.setRotation(1);
  tft.setSwapBytes(true);
  touchSpi.begin(25, 39, 32, TOUCH_CS);
  touch.begin(touchSpi);
  touch.setRotation(1);
  TJpgDec.setJpgScale(1);
  TJpgDec.setCallback(tft_output);
  tft.fillScreen(TFT_BLACK);
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
  Serial.printf("Display init ok. Touch SPI: SCK=25 MISO=39 MOSI=32 CS=%d IRQ=%d\n", TOUCH_CS, TOUCH_IRQ);
}

void renderCurrentScreen() {
  switch (currentMode) {
    case ScreenMode::MainMenu:
      drawMainMenuScreen();
      break;
    case ScreenMode::Finder:
      drawFinderScreen();
      break;
    case ScreenMode::Skin:
      drawSkinScreen();
      break;
    case ScreenMode::Info:
      drawInfoScreen();
      break;
    case ScreenMode::Input:
    default:
      drawInputScreen();
      break;
  }
}

void refreshServerPingIfNeeded() {
  if (currentMode != ScreenMode::Info || WiFi.status() != WL_CONNECTED) {
    return;
  }

  uint32_t now = millis();
  if (now - lastPingRefreshMs < 1000) {
    return;
  }
  lastPingRefreshMs = now;

  if (queryMinecraftServer(serverInput, currentServer)) {
    lastMessage = "Server online";
  } else {
    currentServer.status = "Offline";
    lastMessage = "Server offline / no reply";
  }
  renderCurrentScreen();
}
} // namespace

void setup() {
  Serial.begin(115200);
  loadServerInputFromPreferences();
  setupDisplay();
  if (!connectToWiFi()) {
    lastMessage = "WiFi failed";
  }
  currentMode = ScreenMode::MainMenu;
  renderCurrentScreen();
}

void loop() {
  server.handleClient();
  handleTouchInput();
  refreshServerPingIfNeeded();
  delay(20);
}
