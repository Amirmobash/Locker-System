#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <SoftwareSerial.h>
#include <LittleFS.h>
#include <ThreeWire.h>
#include <RtcDS1302.h>

const byte openRelayPin = D1;
const byte closeRelayPin = D4;

const byte rfidRxPin = D5;
const byte rfidTxPin = D2;

const byte rtcClockPin = D6;
const byte rtcDataPin = D7;
const byte rtcResetPin = D0;

const byte relayOn = HIGH;
const byte relayOff = LOW;

const int maxCards = 5;
const int maxLogs = 20;

const char* wifiName = "RFID-QS-1";
const char* wifiPassword = "12345678";

String masterCard = "0209010E1C80B32903";
String emergencyCode = "1234";

SoftwareSerial rfid(rfidRxPin, rfidTxPin);
ESP8266WebServer server(80);

ThreeWire rtcWire(rtcDataPin, rtcClockPin, rtcResetPin);
RtcDS1302<ThreeWire> rtc(rtcWire);

struct Card {
  String uid;
  String name;
};

Card cards[maxCards];
String logs[maxLogs];

int cardCount = 0;
int logCount = 0;

bool closeNext = false;
bool learningMode = false;

String newCardUid = "";
String statusText = "Ready";
String lastAction = "No action yet";

unsigned long lastCardTime = 0;

String cleanUid(String uid) {
  uid.trim();
  uid.toUpperCase();
  uid.replace(" ", "");
  uid.replace("-", "");
  uid.replace(":", "");
  uid.replace("\r", "");
  uid.replace("\n", "");
  return uid;
}

String byteToHex(byte value) {
  String text = String(value, HEX);

  if (text.length() < 2) {
    text = "0" + text;
  }

  text.toUpperCase();
  return text;
}

String decodeUrl(String text) {
  text.replace("+", " ");

  String result = "";

  for (unsigned int i = 0; i < text.length(); i++) {
    if (text[i] == '%' && i + 2 < text.length()) {
      char hex[3] = { text[i + 1], text[i + 2], 0 };
      result += char(strtol(hex, NULL, 16));
      i += 2;
    } else {
      result += text[i];
    }
  }

  return result;
}

String safeText(String text) {
  text.replace("&", "&amp;");
  text.replace("<", "&lt;");
  text.replace(">", "&gt;");
  text.replace("\"", "&quot;");
  return text;
}

void turnRelaysOff() {
  digitalWrite(openRelayPin, relayOff);
  digitalWrite(closeRelayPin, relayOff);
}

void pulseRelay(byte pin) {
  turnRelaysOff();
  delay(80);

  digitalWrite(pin, relayOn);
  delay(700);

  digitalWrite(pin, relayOff);
  delay(80);

  turnRelaysOff();
}

String currentTime() {
  RtcDateTime now = rtc.GetDateTime();

  if (!now.IsValid()) {
    return "00.00.0000 00:00:00";
  }

  char text[25];

  sprintf(
    text,
    "%02u.%02u.%04u %02u:%02u:%02u",
    now.Day(),
    now.Month(),
    now.Year(),
    now.Hour(),
    now.Minute(),
    now.Second()
  );

  return String(text);
}

void startClock() {
  rtc.Begin();
  rtc.SetIsWriteProtected(false);

  if (!rtc.GetIsRunning()) {
    rtc.SetIsRunning(true);
  }
}

void saveSettings() {
  File file = LittleFS.open("/settings.txt", "w");

  if (!file) {
    return;
  }

  file.println(cleanUid(masterCard));
  file.println(emergencyCode);
  file.println(closeNext ? "1" : "0");

  file.close();
}

void loadSettings() {
  if (!LittleFS.exists("/settings.txt")) {
    saveSettings();
    return;
  }

  File file = LittleFS.open("/settings.txt", "r");

  if (!file) {
    return;
  }

  masterCard = cleanUid(file.readStringUntil('\n'));

  emergencyCode = file.readStringUntil('\n');
  emergencyCode.trim();

  String relayState = file.readStringUntil('\n');
  relayState.trim();

  closeNext = relayState == "1";

  file.close();
}

void saveCards() {
  File file = LittleFS.open("/cards.txt", "w");

  if (!file) {
    return;
  }

  for (int i = 0; i < cardCount; i++) {
    file.println(cleanUid(cards[i].uid) + ";" + cards[i].name);
  }

  file.close();
}

void loadCards() {
  cardCount = 0;

  if (!LittleFS.exists("/cards.txt")) {
    return;
  }

  File file = LittleFS.open("/cards.txt", "r");

  if (!file) {
    return;
  }

  while (file.available() && cardCount < maxCards) {
    String line = file.readStringUntil('\n');
    line.trim();

    int split = line.indexOf(';');

    if (split > 0) {
      cards[cardCount].uid = cleanUid(line.substring(0, split));
      cards[cardCount].name = line.substring(split + 1);
      cards[cardCount].name.trim();
      cardCount++;
    }
  }

  file.close();
}

void saveLogs() {
  File file = LittleFS.open("/logs.txt", "w");

  if (!file) {
    return;
  }

  for (int i = 0; i < logCount; i++) {
    file.println(logs[i]);
  }

  file.close();
}

void loadLogs() {
  logCount = 0;

  if (!LittleFS.exists("/logs.txt")) {
    return;
  }

  File file = LittleFS.open("/logs.txt", "r");

  if (!file) {
    return;
  }

  while (file.available() && logCount < maxLogs) {
    String line = file.readStringUntil('\n');
    line.trim();

    if (line.length() > 0) {
      logs[logCount] = line;
      logCount++;
    }
  }

  file.close();
}

void addLog(String text) {
  for (int i = maxLogs - 1; i > 0; i--) {
    logs[i] = logs[i - 1];
  }

  logs[0] = currentTime() + " - " + text;

  if (logCount < maxLogs) {
    logCount++;
  }

  lastAction = text;
  saveLogs();
}

int findCard(String uid) {
  uid = cleanUid(uid);

  for (int i = 0; i < cardCount; i++) {
    if (cleanUid(cards[i].uid) == uid) {
      return i;
    }
  }

  return -1;
}

String readCard() {
  static bool reading = false;
  static byte data[20];
  static int length = 0;
  static unsigned long lastByteTime = 0;

  while (rfid.available()) {
    byte value = rfid.read();
    lastByteTime = millis();

    if (!reading) {
      if (value == 0x02) {
        reading = true;
        length = 0;
        data[length] = value;
        length++;
      }

      continue;
    }

    if (length < 20) {
      data[length] = value;
      length++;
    }

    if (value == 0x03) {
      String uid = "";

      for (int i = 0; i < length; i++) {
        uid += byteToHex(data[i]);
      }

      uid = cleanUid(uid);

      reading = false;
      length = 0;

      while (rfid.available()) {
        rfid.read();
      }

      return uid;
    }
  }

  if (reading && millis() - lastByteTime > 300) {
    reading = false;
    length = 0;
  }

  return "";
}

void openOrClose(String user) {
  if (!closeNext) {
    statusText = "Opening locker";
    pulseRelay(openRelayPin);
    addLog(user + " opened the locker");
  } else {
    statusText = "Closing locker";
    pulseRelay(closeRelayPin);
    addLog(user + " closed the locker");
  }

  closeNext = !closeNext;
  saveSettings();

  statusText = "Ready";
}

void handleCard(String uid) {
  if (uid.length() < 4) {
    return;
  }

  if (millis() - lastCardTime < 1800) {
    return;
  }

  lastCardTime = millis();
  uid = cleanUid(uid);

  Serial.println("----------");
  Serial.print("Card UID: ");
  Serial.println(uid);

  if (uid == cleanUid(masterCard)) {
    learningMode = true;
    newCardUid = "";
    statusText = "Master card accepted. Scan a new card.";
    addLog("Master card accepted");
    Serial.println("Master card accepted");
    return;
  }

  if (learningMode) {
    learningMode = false;
    newCardUid = uid;
    statusText = "New card found. Enter a name in the web page.";
    addLog("New card is waiting for a name");
    Serial.println("New card found");
    return;
  }

  int cardIndex = findCard(uid);

  if (cardIndex >= 0) {
    openOrClose(cards[cardIndex].name);
  } else {
    statusText = "Unknown card";
    addLog("Unknown card: " + uid);
    Serial.println("Unknown card");
  }
}

String pageStart(String title) {
  String page = "<!DOCTYPE html><html lang='en'><head><meta charset='UTF-8'>";
  page += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  page += "<title>" + title + "</title>";
  page += "<style>";
  page += "body{font-family:Arial;background:#10141c;color:#eee;margin:0;padding:16px}";
  page += "h2{margin-top:0}";
  page += ".box{background:#1d2533;padding:14px;margin:12px 0;border-radius:14px;box-shadow:0 2px 12px #0005}";
  page += ".ok{color:#43f28f}";
  page += ".warn{color:#ffd166}";
  page += ".bad{color:#ff6961}";
  page += "a{color:#59c7ff;text-decoration:none}";
  page += "input,button{font-size:17px;padding:10px;margin:6px 0;border-radius:9px;border:0}";
  page += "input{width:94%;background:#0f1722;color:#eee;border:1px solid #40516b}";
  page += "button{background:#2f9bff;color:white;font-weight:bold}";
  page += "small{color:#aaa}";
  page += ".row{border-bottom:1px solid #334;padding:8px 0}";
  page += "</style>";
  page += "<meta http-equiv='refresh' content='8'>";
  page += "</head><body><h2>Locker Control</h2>";

  return page;
}

String pageEnd() {
  return "</body></html>";
}

void homePage() {
  String page = pageStart("Locker Control");

  page += "<div class='box'>";
  page += "<h3>Status</h3>";
  page += "<p class='ok'>" + safeText(statusText) + "</p>";
  page += "<small>Last action: " + safeText(lastAction) + "</small><br>";
  page += "<small>Time: " + currentTime() + "</small>";
  page += "</div>";

  if (newCardUid.length() > 0) {
    page += "<div class='box'>";
    page += "<h3>Save New Card</h3>";
    page += "<p>UID: <b>" + newCardUid + "</b></p>";
    page += "<form action='/savecard' method='POST'>";
    page += "<input name='name' placeholder='Enter a name' required>";
    page += "<button>Save Card</button>";
    page += "</form>";
    page += "</div>";
  } else if (learningMode) {
    page += "<div class='box warn'>";
    page += "<h3>Learning Mode</h3>";
    page += "<p>Please scan the new card.</p>";
    page += "</div>";
  }

  page += "<div class='box'>";
  page += "<h3>Emergency Code</h3>";
  page += "<form action='/code' method='POST'>";
  page += "<input name='code' placeholder='Enter code'>";
  page += "<button>Open / Close</button>";
  page += "</form>";
  page += "</div>";

  page += "<div class='box'>";
  page += "<h3>Menu</h3>";
  page += "<p><a href='/cards'>Manage Cards</a></p>";
  page += "<p><a href='/logs'>Last 20 Actions</a></p>";
  page += "<p><a href='/settings'>Settings</a></p>";
  page += "<p><a href='/time'>Set Time</a></p>";
  page += "</div>";

  page += "<div class='box'>";
  page += "<h3>Last Actions</h3>";

  for (int i = 0; i < logCount; i++) {
    page += "<div class='row'>" + safeText(logs[i]) + "</div>";
  }

  page += "</div>";
  page += pageEnd();

  server.send(200, "text/html", page);
}

void codePage() {
  String code = server.arg("code");
  code.trim();

  if (code == emergencyCode) {
    openOrClose("Emergency code");
  } else {
    statusText = "Wrong emergency code";
    addLog("Wrong emergency code");
  }

  server.sendHeader("Location", "/");
  server.send(303);
}

void saveCardPage() {
  if (newCardUid.length() == 0) {
    server.sendHeader("Location", "/");
    server.send(303);
    return;
  }

  String name = decodeUrl(server.arg("name"));
  name.trim();

  if (name.length() == 0) {
    name = "User";
  }

  int cardIndex = findCard(newCardUid);

  if (cardIndex >= 0) {
    cards[cardIndex].name = name;
  } else if (cardCount < maxCards) {
    cards[cardCount].uid = cleanUid(newCardUid);
    cards[cardCount].name = name;
    cardCount++;
  } else {
    addLog("Card limit reached");
    newCardUid = "";
    server.sendHeader("Location", "/cards");
    server.send(303);
    return;
  }

  addLog("Card saved: " + name);

  newCardUid = "";

  saveCards();

  server.sendHeader("Location", "/cards");
  server.send(303);
}

void cardsPage() {
  String page = pageStart("Manage Cards");

  page += "<div class='box'>";
  page += "<h3>Manage Cards</h3>";
  page += "<p>Maximum 5 cards. To add a card, scan the master card first. Then scan the new card.</p>";
  page += "<p>Saved cards: " + String(cardCount) + " / " + String(maxCards) + "</p>";

  for (int i = 0; i < cardCount; i++) {
    page += "<div class='row'>";
    page += "<b>" + safeText(cards[i].name) + "</b><br>";
    page += "<small>UID: " + cards[i].uid + "</small><br>";
    page += "<form action='/rename' method='POST'>";
    page += "<input type='hidden' name='id' value='" + String(i) + "'>";
    page += "<input name='name' placeholder='New name'>";
    page += "<button>Rename</button>";
    page += "</form>";
    page += "<a class='bad' href='/delete?id=" + String(i) + "'>Delete</a>";
    page += "</div>";
  }

  page += "<p><a href='/'>Back</a></p>";
  page += "</div>";
  page += pageEnd();

  server.send(200, "text/html", page);
}

void renameCardPage() {
  int id = server.arg("id").toInt();

  String name = decodeUrl(server.arg("name"));
  name.trim();

  if (id >= 0 && id < cardCount && name.length() > 0) {
    cards[id].name = name;
    saveCards();
    addLog("Card renamed: " + name);
  }

  server.sendHeader("Location", "/cards");
  server.send(303);
}

void deleteCardPage() {
  int id = server.arg("id").toInt();

  if (id >= 0 && id < cardCount) {
    addLog("Card deleted: " + cards[id].name);

    for (int i = id; i < cardCount - 1; i++) {
      cards[i] = cards[i + 1];
    }

    cardCount--;
    saveCards();
  }

  server.sendHeader("Location", "/cards");
  server.send(303);
}

void logsPage() {
  String page = pageStart("Last 20 Actions");

  page += "<div class='box'>";
  page += "<h3>Last 20 Actions</h3>";

  for (int i = 0; i < logCount; i++) {
    page += "<div class='row'>" + safeText(logs[i]) + "</div>";
  }

  page += "<p><a href='/clearlogs'>Clear Logs</a></p>";
  page += "<p><a href='/'>Back</a></p>";
  page += "</div>";
  page += pageEnd();

  server.send(200, "text/html", page);
}

void clearLogsPage() {
  logCount = 0;
  saveLogs();
  addLog("Logs cleared");

  server.sendHeader("Location", "/logs");
  server.send(303);
}

void settingsPage() {
  String page = pageStart("Settings");

  page += "<div class='box'>";
  page += "<h3>Settings</h3>";

  page += "<p><b>Master card:</b><br>";
  page += "<small>" + masterCard + "</small></p>";

  page += "<form action='/setmaster' method='POST'>";
  page += "<input name='uid' placeholder='New master UID'>";
  page += "<button>Save Master Card</button>";
  page += "</form>";

  page += "<p><b>Emergency code:</b> " + safeText(emergencyCode) + "</p>";

  page += "<form action='/setcode' method='POST'>";
  page += "<input name='code' placeholder='New code'>";
  page += "<button>Save Code</button>";
  page += "</form>";

  page += "<p><a href='/time'>Set Time</a></p>";
  page += "<p><a href='/'>Back</a></p>";

  page += "</div>";
  page += pageEnd();

  server.send(200, "text/html", page);
}

void setMasterPage() {
  String uid = cleanUid(server.arg("uid"));

  if (uid.length() >= 4) {
    masterCard = uid;
    saveSettings();
    addLog("Master card changed");
  }

  server.sendHeader("Location", "/settings");
  server.send(303);
}

void setCodePage() {
  String code = server.arg("code");
  code.trim();

  if (code.length() > 0) {
    emergencyCode = code;
    saveSettings();
    addLog("Emergency code changed");
  }

  server.sendHeader("Location", "/settings");
  server.send(303);
}

void timePage() {
  RtcDateTime now = rtc.GetDateTime();

  String page = pageStart("Set Time");

  page += "<div class='box'>";
  page += "<h3>Set Time</h3>";
  page += "<p>Current RTC time: <b>" + currentTime() + "</b></p>";

  page += "<form action='/settime' method='POST'>";
  page += "<input name='year' type='number' min='2024' max='2099' placeholder='Year' value='" + String(now.IsValid() ? now.Year() : 2026) + "'>";
  page += "<input name='month' type='number' min='1' max='12' placeholder='Month' value='" + String(now.IsValid() ? now.Month() : 1) + "'>";
  page += "<input name='day' type='number' min='1' max='31' placeholder='Day' value='" + String(now.IsValid() ? now.Day() : 1) + "'>";
  page += "<input name='hour' type='number' min='0' max='23' placeholder='Hour' value='" + String(now.IsValid() ? now.Hour() : 12) + "'>";
  page += "<input name='minute' type='number' min='0' max='59' placeholder='Minute' value='" + String(now.IsValid() ? now.Minute() : 0) + "'>";
  page += "<input name='second' type='number' min='0' max='59' placeholder='Second' value='0'>";
  page += "<button>Save Time</button>";
  page += "</form>";

  page += "<p><a href='/settings'>Back</a></p>";
  page += "</div>";
  page += pageEnd();

  server.send(200, "text/html", page);
}

void setTimePage() {
  int year = server.arg("year").toInt();
  int month = server.arg("month").toInt();
  int day = server.arg("day").toInt();
  int hour = server.arg("hour").toInt();
  int minute = server.arg("minute").toInt();
  int second = server.arg("second").toInt();

  bool validTime =
    year >= 2024 && year <= 2099 &&
    month >= 1 && month <= 12 &&
    day >= 1 && day <= 31 &&
    hour >= 0 && hour <= 23 &&
    minute >= 0 && minute <= 59 &&
    second >= 0 && second <= 59;

  if (validTime) {
    rtc.SetIsWriteProtected(false);
    rtc.SetIsRunning(true);
    rtc.SetDateTime(RtcDateTime(year, month, day, hour, minute, second));
    addLog("Time changed");
  }

  server.sendHeader("Location", "/time");
  server.send(303);
}

void setup() {
  pinMode(openRelayPin, OUTPUT);
  pinMode(closeRelayPin, OUTPUT);

  turnRelaysOff();

  Serial.begin(115200);
  delay(800);

  Serial.println();
  Serial.println("Locker system is starting");

  rfid.begin(9600);

  if (!LittleFS.begin()) {
    Serial.println("LittleFS error");
  }

  loadSettings();
  loadCards();
  loadLogs();

  startClock();
  turnRelaysOff();

  WiFi.mode(WIFI_AP);

  bool wifiStarted = WiFi.softAP(wifiName, wifiPassword);

  Serial.print("WiFi started: ");
  Serial.println(wifiStarted ? "yes" : "no");

  Serial.print("WiFi name: ");
  Serial.println(wifiName);

  Serial.print("WiFi password: ");
  Serial.println(wifiPassword);

  Serial.print("IP address: ");
  Serial.println(WiFi.softAPIP());

  server.on("/", homePage);
  server.on("/code", HTTP_POST, codePage);
  server.on("/savecard", HTTP_POST, saveCardPage);
  server.on("/cards", cardsPage);
  server.on("/rename", HTTP_POST, renameCardPage);
  server.on("/delete", deleteCardPage);
  server.on("/logs", logsPage);
  server.on("/clearlogs", clearLogsPage);
  server.on("/settings", settingsPage);
  server.on("/setmaster", HTTP_POST, setMasterPage);
  server.on("/setcode", HTTP_POST, setCodePage);
  server.on("/time", timePage);
  server.on("/settime", HTTP_POST, setTimePage);

  server.begin();

  addLog("System started");
  turnRelaysOff();

  Serial.println("System is ready");
}

void loop() {
  server.handleClient();

  String uid = readCard();

  if (uid.length() > 0) {
    handleCard(uid);
  }
}
