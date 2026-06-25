#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <SoftwareSerial.h>
#include <LittleFS.h>
#include <ThreeWire.h>
#include <RtcDS1302.h>

#define RELAY1 D1
#define RELAY2 D2

#define RFID_RX D5
#define RFID_TX D0

#define RTC_CLK D6
#define RTC_DAT D7
#define RTC_RST D8

#define RELAY_ACTIVE_LOW false

#define MAX_CARDS 5
#define MAX_LOGS 20

const char* WIFI_SSID = "Locker-System";
const char* WIFI_PASS = "12345678";

String masterCard = "0209010E1C80B32903";
String emergencyCode = "123456789012";

SoftwareSerial RFID(RFID_RX, RFID_TX);
ESP8266WebServer server(80);

ThreeWire myWire(RTC_DAT, RTC_CLK, RTC_RST);
RtcDS1302<ThreeWire> Rtc(myWire);

struct Card {
  String uid;
  String name;
};

Card cards[MAX_CARDS];
int cardCount = 0;
String logs[MAX_LOGS];
int logCount = 0;

bool nextRelay = false;
bool learnMode = false;
String pendingCard = "";
String statusText = "Bereit";
String lastAction = "Keine Aktion";
unsigned long lastCardTime = 0;

int relayOnLevel() { return RELAY_ACTIVE_LOW ? LOW : HIGH; }
int relayOffLevel() { return RELAY_ACTIVE_LOW ? HIGH : LOW; }

void relaysOff() {
  digitalWrite(RELAY1, relayOffLevel());
  digitalWrite(RELAY2, relayOffLevel());
}

String nowTime() {
  RtcDateTime now = Rtc.GetDateTime();
  if (!now.IsValid()) return "00.00.0000 00:00:00";
  char buf[25];
  sprintf(buf, "%02u.%02u.%04u %02u:%02u:%02u", now.Day(), now.Month(), now.Year(), now.Hour(), now.Minute(), now.Second());
  return String(buf);
}

void setupRtc() {
  Rtc.Begin();
  if (!Rtc.GetIsRunning()) Rtc.SetIsRunning(true);
}

void saveConfig() {
  File f = LittleFS.open("/config.txt", "w");
  if (!f) return;
  f.println(masterCard);
  f.println(emergencyCode);
  f.println(nextRelay ? "1" : "0");
  f.close();
}

void loadConfig() {
  if (!LittleFS.exists("/config.txt")) { saveConfig(); return; }
  File f = LittleFS.open("/config.txt", "r");
  if (!f) return;
  masterCard = f.readStringUntil('\n'); masterCard.trim(); masterCard.toUpperCase();
  emergencyCode = f.readStringUntil('\n'); emergencyCode.trim();
  String nr = f.readStringUntil('\n'); nr.trim(); nextRelay = (nr == "1");
  f.close();
}

void saveCards() {
  File f = LittleFS.open("/cards.txt", "w");
  if (!f) return;
  for (int i = 0; i < cardCount; i++) f.println(cards[i].uid + ";" + cards[i].name);
  f.close();
}

void loadCards() {
  cardCount = 0;
  if (!LittleFS.exists("/cards.txt")) return;
  File f = LittleFS.open("/cards.txt", "r");
  if (!f) return;
  while (f.available() && cardCount < MAX_CARDS) {
    String line = f.readStringUntil('\n'); line.trim();
    int p = line.indexOf(';');
    if (p > 0) {
      cards[cardCount].uid = line.substring(0, p); cards[cardCount].uid.toUpperCase();
      cards[cardCount].name = line.substring(p + 1);
      cardCount++;
    }
  }
  f.close();
}

void saveLogs() {
  File f = LittleFS.open("/logs.txt", "w");
  if (!f) return;
  for (int i = 0; i < logCount; i++) f.println(logs[i]);
  f.close();
}

void loadLogs() {
  logCount = 0;
  if (!LittleFS.exists("/logs.txt")) return;
  File f = LittleFS.open("/logs.txt", "r");
  if (!f) return;
  while (f.available() && logCount < MAX_LOGS) {
    String line = f.readStringUntil('\n'); line.trim();
    if (line.length() > 0) logs[logCount++] = line;
  }
  f.close();
}

void addLog(String text) {
  for (int i = MAX_LOGS - 1; i > 0; i--) logs[i] = logs[i - 1];
  logs[0] = nowTime() + " - " + text;
  if (logCount < MAX_LOGS) logCount++;
  lastAction = text;
  saveLogs();
}

int findCard(String uid) {
  uid.toUpperCase();
  for (int i = 0; i < cardCount; i++) if (cards[i].uid == uid) return i;
  return -1;
}

String urlDecode(String s) {
  s.replace("+", " ");
  String out = "";
  for (unsigned int i = 0; i < s.length(); i++) {
    if (s[i] == '%' && i + 2 < s.length()) {
      char hex[3] = { s[i+1], s[i+2], 0 };
      out += char(strtol(hex, NULL, 16));
      i += 2;
    } else out += s[i];
  }
  return out;
}

String htmlEscape(String s) {
  s.replace("&", "&amp;"); s.replace("<", "&lt;"); s.replace(">", "&gt;"); s.replace("\"", "&quot;");
  return s;
}

String readRFID() {
  if (!RFID.available()) return "";

  byte data[20];
  int len = 0;
  unsigned long t = millis();
  while (millis() - t < 220) {
    while (RFID.available() && len < 20) data[len++] = RFID.read();
  }
  if (len == 0) return "";

  String uid = "";
  for (int i = 0; i < len; i++) {
    if (data[i] < 16) uid += "0";
    uid += String(data[i], HEX);
  }
  uid.toUpperCase();
  return uid;
}

void triggerRelay(String user) {
  if (!nextRelay) {
    statusText = "Schrank wird geöffnet...";
    digitalWrite(RELAY1, relayOnLevel());
    delay(700);
    digitalWrite(RELAY1, relayOffLevel());
    addLog(user + " - Relais 1 / Öffnen");
    Serial.println("Relais 1 aktiviert");
  } else {
    statusText = "Schrank wird geschlossen...";
    digitalWrite(RELAY2, relayOnLevel());
    delay(700);
    digitalWrite(RELAY2, relayOffLevel());
    addLog(user + " - Relais 2 / Schließen");
    Serial.println("Relais 2 aktiviert");
  }
  nextRelay = !nextRelay;
  saveConfig();
  statusText = "Bereit";
}

void handleRFID(String uid) {
  if (uid.length() < 4) return;
  if (millis() - lastCardTime < 1800) return;
  lastCardTime = millis();

  Serial.print("RFID UID HEX: "); Serial.println(uid);

  if (uid == masterCard) {
    learnMode = true;
    pendingCard = "";
    statusText = "Masterkarte erkannt - neue Karte scannen";
    addLog("Masterkarte erkannt");
    Serial.println("Masterkarte erkannt");
    return;
  }

  if (learnMode) {
    pendingCard = uid;
    learnMode = false;
    statusText = "Neue Karte erkannt - Name im Web eingeben";
    addLog("Neue Karte wartet auf Namen: " + uid);
    Serial.println("Neue Karte erkannt. Name im Web eingeben.");
    return;
  }

  int idx = findCard(uid);
  if (idx >= 0) {
    Serial.print("Benutzer: "); Serial.println(cards[idx].name);
    triggerRelay(cards[idx].name);
  } else {
    statusText = "Unbekannte Karte";
    addLog("Unbekannte Karte: " + uid);
    Serial.println("Unbekannte Karte");
  }
}

String pageStart(String title) {
  String h = "<!DOCTYPE html><html lang='de'><head><meta charset='UTF-8'>";
  h += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  h += "<title>" + title + "</title>";
  h += "<style>body{font-family:Arial;background:#10141c;color:#eee;margin:0;padding:16px}h2{margin-top:0}.box{background:#1d2533;padding:14px;margin:12px 0;border-radius:14px;box-shadow:0 2px 12px #0005}.ok{color:#43f28f}.warn{color:#ffd166}.bad{color:#ff6961}a{color:#59c7ff;text-decoration:none}input,button{font-size:17px;padding:10px;margin:6px 0;border-radius:9px;border:0}input{width:94%;background:#0f1722;color:#eee;border:1px solid #40516b}button{background:#2f9bff;color:white;font-weight:bold}small{color:#aaa}.row{border-bottom:1px solid #334; padding:8px 0}</style>";
  h += "<meta http-equiv='refresh' content='8'>";
  h += "</head><body><h2>Schranksteuerung</h2>";
  return h;
}
String pageEnd(){ return "</body></html>"; }

void handleRoot() {
  String h = pageStart("Schranksteuerung");
  h += "<div class='box'><h3>Status</h3><p class='ok'>" + htmlEscape(statusText) + "</p><small>Letzte Aktion: " + htmlEscape(lastAction) + "</small></div>";

  if (pendingCard.length() > 0) {
    h += "<div class='box'><h3>Neue Karte speichern</h3><p>UID: <b>" + pendingCard + "</b></p>";
    h += "<form action='/savecard' method='POST'><input name='name' placeholder='Name eingeben' required><button>Karte speichern</button></form></div>";
  } else if (learnMode) {
    h += "<div class='box warn'><h3>Lernmodus aktiv</h3><p>Bitte neue Karte scannen.</p></div>";
  }

  h += "<div class='box'><h3>Notfallcode</h3><form action='/code' method='POST'><input name='code' maxlength='12' placeholder='12-stelliger Code'><button>Öffnen / Schließen</button></form></div>";

  h += "<div class='box'><h3>Menü</h3><p><a href='/cards'>Karten verwalten</a></p><p><a href='/logs'>Letzte 20 Aktionen</a></p><p><a href='/settings'>Einstellungen</a></p></div>";

  h += "<div class='box'><h3>Letzte Aktionen</h3>";
  for (int i = 0; i < logCount; i++) h += "<div class='row'>" + htmlEscape(logs[i]) + "</div>";
  h += "</div>" + pageEnd();
  server.send(200, "text/html", h);
}

void handleCode() {
  String code = server.arg("code"); code.trim();
  if (code == emergencyCode) triggerRelay("Notfallcode");
  else { statusText = "Falscher Notfallcode"; addLog("Falscher Notfallcode"); }
  server.sendHeader("Location", "/"); server.send(303);
}

void handleSaveCard() {
  if (pendingCard.length() == 0) { server.sendHeader("Location", "/"); server.send(303); return; }
  String name = urlDecode(server.arg("name")); name.trim(); if (name.length() == 0) name = "Benutzer";
  int idx = findCard(pendingCard);
  if (idx >= 0) cards[idx].name = name;
  else if (cardCount < MAX_CARDS) { cards[cardCount].uid = pendingCard; cards[cardCount].name = name; cardCount++; }
  else { addLog("Kartenlimit erreicht"); pendingCard = ""; server.sendHeader("Location", "/cards"); server.send(303); return; }
  addLog("Karte gespeichert: " + name);
  pendingCard = "";
  saveCards();
  server.sendHeader("Location", "/cards"); server.send(303);
}

void handleCards() {
  String h = pageStart("Karten verwalten");
  h += "<div class='box'><h3>Karten verwalten</h3><p>Maximal 5 Karten. Zum Hinzufügen zuerst Masterkarte scannen, dann neue Karte scannen.</p>";
  h += "<p>Gespeichert: " + String(cardCount) + " / " + String(MAX_CARDS) + "</p>";
  for (int i = 0; i < cardCount; i++) {
    h += "<div class='row'><b>" + htmlEscape(cards[i].name) + "</b><br><small>UID: " + cards[i].uid + "</small><br>";
    h += "<form action='/rename' method='POST'><input type='hidden' name='id' value='" + String(i) + "'><input name='name' placeholder='Neuer Name'><button>Umbenennen</button></form>";
    h += "<a class='bad' href='/delete?id=" + String(i) + "'>Löschen</a></div>";
  }
  h += "<p><a href='/'>Zurück</a></p></div>" + pageEnd();
  server.send(200, "text/html", h);
}

void handleRename() {
  int id = server.arg("id").toInt();
  String name = urlDecode(server.arg("name")); name.trim();
  if (id >= 0 && id < cardCount && name.length() > 0) { cards[id].name = name; saveCards(); addLog("Karte umbenannt: " + name); }
  server.sendHeader("Location", "/cards"); server.send(303);
}

void handleDelete() {
  int id = server.arg("id").toInt();
  if (id >= 0 && id < cardCount) {
    addLog("Karte gelöscht: " + cards[id].name);
    for (int i = id; i < cardCount - 1; i++) cards[i] = cards[i+1];
    cardCount--; saveCards();
  }
  server.sendHeader("Location", "/cards"); server.send(303);
}

void handleLogs() {
  String h = pageStart("Letzte 20 Aktionen");
  h += "<div class='box'><h3>Letzte 20 Aktionen</h3>";
  for (int i = 0; i < logCount; i++) h += "<div class='row'>" + htmlEscape(logs[i]) + "</div>";
  h += "<p><a href='/clearlogs'>Logs löschen</a></p><p><a href='/'>Zurück</a></p></div>" + pageEnd();
  server.send(200, "text/html", h);
}

void handleClearLogs() {
  logCount = 0; saveLogs(); addLog("Logs gelöscht");
  server.sendHeader("Location", "/logs"); server.send(303);
}

void handleSettings() {
  String h = pageStart("Einstellungen");
  h += "<div class='box'><h3>Einstellungen</h3>";
  h += "<p><b>Masterkarte:</b><br><small>" + masterCard + "</small></p>";
  h += "<form action='/setmaster' method='POST'><input name='uid' placeholder='Neue Master UID HEX'><button>Masterkarte speichern</button></form>";
  h += "<p><b>Notfallcode:</b> " + emergencyCode + "</p>";
  h += "<form action='/setcode' method='POST'><input name='code' maxlength='12' placeholder='Neuer 12-stelliger Code'><button>Code speichern</button></form>";
  h += "<p><a href='/'>Zurück</a></p></div>" + pageEnd();
  server.send(200, "text/html", h);
}

void handleSetMaster() {
  String uid = server.arg("uid"); uid.trim(); uid.toUpperCase();
  if (uid.length() >= 4) { masterCard = uid; saveConfig(); addLog("Masterkarte geändert"); }
  server.sendHeader("Location", "/settings"); server.send(303);
}

void handleSetCode() {
  String code = server.arg("code"); code.trim();
  if (code.length() == 12) { emergencyCode = code; saveConfig(); addLog("Notfallcode geändert"); }
  server.sendHeader("Location", "/settings"); server.send(303);
}

void setup() {
  digitalWrite(RELAY1, relayOffLevel());
  digitalWrite(RELAY2, relayOffLevel());
  pinMode(RELAY1, OUTPUT);
  pinMode(RELAY2, OUTPUT);
  relaysOff();

  Serial.begin(115200);
  delay(800);
  Serial.println(); Serial.println("Locker-System startet...");

  RFID.begin(9600);
  if (!LittleFS.begin()) Serial.println("LittleFS Fehler");
  loadConfig(); loadCards(); loadLogs(); setupRtc();

  WiFi.mode(WIFI_AP);
  WiFi.softAP(WIFI_SSID, WIFI_PASS);
  Serial.println("WiFi gestartet");
  Serial.print("SSID: "); Serial.println(WIFI_SSID);
  Serial.print("Passwort: "); Serial.println(WIFI_PASS);
  Serial.println("Adresse: http://192.168.4.1");
  Serial.println("RFID UID wird hier angezeigt.");

  server.on("/", handleRoot);
  server.on("/code", HTTP_POST, handleCode);
  server.on("/savecard", HTTP_POST, handleSaveCard);
  server.on("/cards", handleCards);
  server.on("/rename", HTTP_POST, handleRename);
  server.on("/delete", handleDelete);
  server.on("/logs", handleLogs);
  server.on("/clearlogs", handleClearLogs);
  server.on("/settings", handleSettings);
  server.on("/setmaster", HTTP_POST, handleSetMaster);
  server.on("/setcode", HTTP_POST, handleSetCode);
  server.begin();

  addLog("System gestartet");
}

void loop() {
  server.handleClient();
  String uid = readRFID();
  if (uid.length() > 0) handleRFID(uid);
}
