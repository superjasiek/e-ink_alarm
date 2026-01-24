#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WebServer.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <time.h>
#include <GxEPD2_BW.h>
#include <Fonts/FreeMonoBold12pt7b.h>
#include <Fonts/FreeMonoBold9pt7b.h>
#include <Fonts/FreeMono9pt7b.h>

// --- USTAWIENIA CZASU (NTP) ---
const char* NTP_SERVER = "pool.ntp.org";
const long  GMT_OFFSET_SEC = 3600; // Przesunięcie GMT w sekundach (np. 3600 dla UTC+1)
const int   DAYLIGHT_OFFSET_SEC = 3600; // Przesunięcie dla czasu letniego

// --- DEFINICJE PINÓW ---
// Przyciski
const int BUTTON_UP_PIN = 4;
const int BUTTON_DOWN_PIN = 17;
const int BUTTON_OK_PIN = 25;
// Głośnik
const int SPEAKER_PIN = 26;
// Wyświetlacz e-ink (SPI) - dostosuj do swojego podłączenia!
const int EPD_CS_PIN = 22;
const int EPD_DC_PIN = 27;
const int EPD_RST_PIN = 33;
const int EPD_BUSY_PIN = 32;

// --- OBIEKTY I ZMIENNE GLOBALNE ---
WebServer server(80);
Preferences preferences;
GxEPD2_BW<GxEPD2_290_T94_V2, GxEPD2_290_T94_V2::HEIGHT> display(GxEPD2_290_T94_V2(EPD_CS_PIN, EPD_DC_PIN, EPD_RST_PIN, EPD_BUSY_PIN)
);

// Konfiguracja (ładowana z pamięci NVS)
long gmtOffsetSeconds = 3600;
int selectedRingtone = 1;

// Stany aplikacji (maszyna stanów)
enum AppState {
  STATE_DISPLAY_TIME,
  STATE_SET_ALARM_HOUR,
  STATE_SET_ALARM_MINUTE,
  STATE_ALARM_RINGING
};
AppState currentState = STATE_DISPLAY_TIME;

// Zmienne alarmu
int alarmHours[7] = {6, 6, 6, 6, 6, 6, 6};
int alarmMinutes[7] = {30, 30, 30, 30, 30, 30, 30};
bool alarmDays[7] = {true, true, true, true, true, true, true};
bool isAlarmEnabled = true;
bool isAlarmRinging = false;
bool forceScreenUpdate = false;

// Zmienne pogody
float currentTemp = 0.0;
int weatherCode = 0;
unsigned long lastWeatherFetch = 0;
float cityLat = 49.75;
float cityLon = 18.63;
String cityName = "Cieszyn";

// Prototypy funkcji
void saveConfiguration();
void loadConfiguration();
String buildRootPage();
void handleSave();
void playRingtone(int ringtone, bool reset = false);
void drawTimeScreen(struct tm &timeinfo);
void handleButtons();
void drawSetAlarmScreen(bool isSettingHour);
void fetchWeather();
String getWeatherDesc(int code);
long getMinutesToNextAlarm(struct tm &now);


void setup() {
  // --- INICJALIZACJA KOMUNIKACJI ---
  Serial.begin(115200);
  Serial.println("Budzik E-Ink startuje...");

  // Wczytaj konfigurację z pamięci
  loadConfiguration();

  // --- ŁĄCZENIE Z WIFI ---
  WiFiManager wifiManager;
  if (!wifiManager.autoConnect("BudzikE-Ink-Config")) {
    Serial.println("Nie udalo sie polaczyc i osiagnieto limit czasu.");
    ESP.restart();
  }
  Serial.println("Polaczono z WiFi!");

  // --- SYNCHRONIZACJA CZASU (NTP) ---
  Serial.println("Pobieranie czasu z serwera NTP...");
  configTime(gmtOffsetSeconds, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    Serial.println("Pobrano czas:");
    Serial.println(&timeinfo, "%A, %B %d %Y %H:%M:%S");
  } else {
    Serial.println("Nie udalo sie zsynchronizowac czasu.");
  }

  // --- INICJALIZACJA SPRZĘTU ---
  // Wyświetlacz
  display.init(115200, true, 2, false);
  display.setRotation(1);
  display.setTextColor(GxEPD_BLACK);

  // Przyciski
  pinMode(BUTTON_UP_PIN, INPUT_PULLUP);
  pinMode(BUTTON_DOWN_PIN, INPUT_PULLUP);
  pinMode(BUTTON_OK_PIN, INPUT_PULLUP);

  // Głośnik
  pinMode(SPEAKER_PIN, OUTPUT);

  Serial.println("Inicjalizacja sprzetu zakonczona.");

  // --- SERWER WWW ---
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html", buildRootPage());
  });
  server.on("/save", HTTP_POST, handleSave);
  server.begin();
  Serial.println("Serwer WWW uruchomiony.");
}

void loop() {
  server.handleClient(); // Obsługa żądań HTTP

  static int lastMinute = -1;
  static bool alarmCheckedForThisMinute = false;
  struct tm timeinfo;

  handleButtons();

  if (!getLocalTime(&timeinfo)) {
    Serial.println("Blad odczytu czasu lokalnego");
    return;
  }

  // Pobieranie pogody co 30 minut
  if (millis() - lastWeatherFetch > 1800000 || lastWeatherFetch == 0) {
    fetchWeather();
  }

  // Zresetuj flagę sprawdzania alarmu, gdy minuta się zmieni
  if (timeinfo.tm_min != lastMinute) {
    alarmCheckedForThisMinute = false;
  }

  // Logika alarmu - sprawdzana raz na minutę, uwzględniając wybrane dni
  if (isAlarmEnabled && alarmDays[timeinfo.tm_wday] && !isAlarmRinging && !alarmCheckedForThisMinute &&
      timeinfo.tm_hour == alarmHours[timeinfo.tm_wday] && timeinfo.tm_min == alarmMinutes[timeinfo.tm_wday]) {
    isAlarmRinging = true;
    currentState = STATE_ALARM_RINGING;
    alarmCheckedForThisMinute = true;
    playRingtone(selectedRingtone, true); // Zresetuj melodię
  }

  if (isAlarmRinging) {
    playRingtone(selectedRingtone);
  }

  // Odświeżanie wyświetlacza
  if ((timeinfo.tm_min != lastMinute && currentState == STATE_DISPLAY_TIME) || forceScreenUpdate) {
    lastMinute = timeinfo.tm_min;

    // Pełne odświeżanie co godzinę, inaczej częściowe
    if (timeinfo.tm_min == 0 || forceScreenUpdate) {
      display.setFullWindow();
    } else {
      display.setPartialWindow(0, 0, display.width(), display.height());
    }

    // Wybierz odpowiedni ekran do narysowania
    if (currentState == STATE_SET_ALARM_HOUR) {
      drawSetAlarmScreen(true);
    } else if (currentState == STATE_SET_ALARM_MINUTE) {
      drawSetAlarmScreen(false);
    } else {
      drawTimeScreen(timeinfo);
    }

    forceScreenUpdate = false; // Zresetuj flagę
  }

  delay(200); // Główne opóźnienie pętli, aby nie obciążać procesora
}

// --- FUNKCJE OBSŁUGI PAMIĘCI ---

void saveConfiguration() {
  preferences.begin("alarm-clock", false);
  preferences.putBytes("alarmHours", alarmHours, 7 * sizeof(int));
  preferences.putBytes("alarmMins", alarmMinutes, 7 * sizeof(int));
  preferences.putBool("isAlarmEnabled", isAlarmEnabled);
  preferences.putLong("gmtOffset", gmtOffsetSeconds);
  preferences.putInt("ringtone", selectedRingtone);
  preferences.putFloat("lat", cityLat);
  preferences.putFloat("lon", cityLon);
  preferences.putString("cityName", cityName);

  uint8_t dayMask = 0;
  for (int i = 0; i < 7; i++) {
    if (alarmDays[i]) dayMask |= (1 << i);
  }
  preferences.putUChar("alarmDays", dayMask);

  preferences.end();
  Serial.println("Konfiguracja zapisana.");
}

void loadConfiguration() {
  preferences.begin("alarm-clock", true);
  preferences.getBytes("alarmHours", alarmHours, 7 * sizeof(int));
  preferences.getBytes("alarmMins", alarmMinutes, 7 * sizeof(int));
  isAlarmEnabled = preferences.getBool("isAlarmEnabled", true);
  gmtOffsetSeconds = preferences.getLong("gmtOffset", 3600);
  selectedRingtone = preferences.getInt("ringtone", 1);
  cityLat = preferences.getFloat("lat", 49.75);
  cityLon = preferences.getFloat("lon", 18.63);
  cityName = preferences.getString("cityName", "Cieszyn");

  uint8_t dayMask = preferences.getUChar("alarmDays", 0x7F); // Domyślnie wszystkie dni
  for (int i = 0; i < 7; i++) {
    alarmDays[i] = (dayMask & (1 << i));
  }

  preferences.end();
  Serial.println("Konfiguracja wczytana.");
}

// --- DYNAMICZNA STRONA WWW ---
String buildRootPage() {
  String page = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'><style>";
  page += "body{font-family:Arial,sans-serif}.container{max-width:400px;margin:auto;padding:20px}.form-group{margin-bottom:15px}label{display:block;margin-bottom:5px}input[type=number],select{width:100%;padding:8px;box-sizing:border-box}.btn{background-color:#4CAF50;color:#fff;padding:10px 15px;border:none;cursor:pointer;width:100%}.day-row{display:flex;align-items:center;margin-bottom:5px}.day-row input[type=number]{width:60px;margin-left:10px}";
  page += "</style></head><body><div class='container'><h2>Konfiguracja Budzika E-Ink</h2>";
  page += "<form action='/save' method='POST'>";

  page += "<h3>Ustawienia Alarmu</h3>";
  const char* dayLabels[] = {"Niedziela", "Poniedzialek", "Wtorek", "Sroda", "Czwartek", "Piatek", "Sobota"};
  for (int i = 0; i < 7; i++) {
    page += "<div class='day-row'>";
    page += "<input type='checkbox' name='day" + String(i) + "' value='1'" + (alarmDays[i] ? " checked" : "") + "> ";
    page += "<label style='width:100px;margin-bottom:0;'>" + String(dayLabels[i]) + ":</label>";
    page += "<input type='number' name='h" + String(i) + "' min='0' max='23' value='" + String(alarmHours[i]) + "'>";
    page += "<span>:</span>";
    page += "<input type='number' name='m" + String(i) + "' min='0' max='59' value='" + String(alarmMinutes[i]) + "'>";
    page += "</div>";
  }

  page += "<h3>Lokalizacja (Pogoda)</h3>";
  page += "<div class='form-group'><label>Miasto:</label><input type='text' name='city' value='" + cityName + "'></div>";
  page += "<div class='form-group'><label>Szerokosc (Lat):</label><input type='text' name='lat' value='" + String(cityLat, 4) + "'></div>";
  page += "<div class='form-group'><label>Dlugosc (Lon):</label><input type='text' name='lon' value='" + String(cityLon, 4) + "'></div>";

  page += "<div class='form-group'><label for='timezone'>Strefa czasowa:</label><select id='timezone' name='timezone'>";
  for (int i = -12; i <= 12; i++) {
    page += "<option value='" + String(i * 3600) + "'";
    if (i * 3600 == gmtOffsetSeconds) {
      page += " selected";
    }
    page += String(">UTC") + (i >= 0 ? "+" : "") + String(i) + "</option>";
  }
  page += "</select></div>";

  page += "<div class='form-group'><label for='ringtone'>Dzwonek:</label><select id='ringtone' name='ringtone'>";
  page += "<option value='1'" + String(selectedRingtone == 1 ? " selected" : "") + ">Standardowy</option>";
  page += "<option value='2'" + String(selectedRingtone == 2 ? " selected" : "") + ">Melodia 1</option>";
  page += "<option value='3'" + String(selectedRingtone == 3 ? " selected" : "") + ">Melodia 2</option>";
  page += "<option value='4'" + String(selectedRingtone == 4 ? " selected" : "") + ">Super Mario</option>";
  page += "</select></div>";

  page += "<button type='submit' class='btn'>Zapisz</button></form></div></body></html>";
  return page;
}

// --- FUNKCJE OBSŁUGI SERWERA WWW ---

void handleSave() {
  gmtOffsetSeconds = server.arg("timezone").toInt();
  selectedRingtone = server.arg("ringtone").toInt();
  cityName = server.arg("city");
  cityLat = server.arg("lat").toFloat();
  cityLon = server.arg("lon").toFloat();

  for (int i = 0; i < 7; i++) {
    alarmDays[i] = server.hasArg("day" + String(i));
    alarmHours[i] = server.arg("h" + String(i)).toInt();
    alarmMinutes[i] = server.arg("m" + String(i)).toInt();
  }

  saveConfiguration();

  server.send(200, "text/plain", "Ustawienia zapisane. Budzik zostanie zrestartowany.");
  delay(1000);
  ESP.restart();
}

// --- FUNKCJE DZWONKA ---

// Definicje nut
#define NOTE_E6  1319
#define NOTE_G6  1568
#define NOTE_A6  1760
#define NOTE_AS6 1865
#define NOTE_B6  1976
#define NOTE_C7  2093
#define NOTE_D7  2349
#define NOTE_E7  2637
#define NOTE_F7  2794
#define NOTE_G7  3136
#define NOTE_A7  3520

#define NOTE_G3  196
#define NOTE_A3  220
#define NOTE_B3  247
#define NOTE_C4  262

void playRingtone(int ringtone, bool reset) {
  static int melody_pos = 0;
  static unsigned long last_note_time = 0;

  if (reset) {
    melody_pos = 0;
    last_note_time = 0;
  }

  // Melodia 1 (prosta)
  int melody1_notes[] = { NOTE_C4, NOTE_G3, NOTE_G3, NOTE_A3, NOTE_G3, 0, NOTE_B3, NOTE_C4 };
  int melody1_durations[] = { 4, 8, 8, 4, 4, 4, 4, 4 };

  // Melodia 2 (inna)
  int melody2_notes[] = { NOTE_A3, NOTE_B3, NOTE_C4, 0, NOTE_A3, NOTE_B3, NOTE_C4, 0 };
  int melody2_durations[] = { 8, 8, 8, 8, 8, 8, 8, 8 };

  // Melodia 3 (Super Mario) - bazowa sekwencja
  static const int mario_notes[] = {
    NOTE_E7, NOTE_E7, 0, NOTE_E7, 0, NOTE_C7, NOTE_E7, 0, NOTE_G7, 0, 0, 0, NOTE_G6, 0, 0, 0,
    NOTE_C7, 0, 0, NOTE_G6, 0, 0, NOTE_E6, 0, 0, NOTE_A6, 0, NOTE_B6, 0, NOTE_AS6, NOTE_A6, 0,
    NOTE_G6, NOTE_E7, NOTE_G7, NOTE_A7, 0, NOTE_F7, NOTE_G7, 0, NOTE_E7, 0, NOTE_C7, NOTE_D7, NOTE_B6, 0, 0
  };
  static const int mario_durations[] = {
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    9, 9, 9, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12
  };

  switch (ringtone) {
    case 1: // Dzwonek standardowy
      tone(SPEAKER_PIN, 1000);
      break;
    case 2: // Melodia 1
      if (millis() > last_note_time + (1000 / melody1_durations[melody_pos])) {
        int note = melody1_notes[melody_pos];
        if (note == 0) noTone(SPEAKER_PIN);
        else tone(SPEAKER_PIN, note);

        last_note_time = millis();
        melody_pos++;
        if (melody_pos >= sizeof(melody1_notes)/sizeof(int)) melody_pos = 0; // Zapętl
      }
      break;
    case 3: // Melodia 2
       if (millis() > last_note_time + (1000 / melody2_durations[melody_pos])) {
        int note = melody2_notes[melody_pos];
        if (note == 0) noTone(SPEAKER_PIN);
        else tone(SPEAKER_PIN, note);

        last_note_time = millis();
        melody_pos++;
        if (melody_pos >= sizeof(melody2_notes)/sizeof(int)) melody_pos = 0; // Zapętl
      }
      break;
    case 4: // Super Mario
      if (millis() > last_note_time + (1000 / mario_durations[melody_pos])) {
        int note = mario_notes[melody_pos];
        if (note == 0) noTone(SPEAKER_PIN);
        else tone(SPEAKER_PIN, note);

        last_note_time = millis();
        melody_pos++;
        if (melody_pos >= sizeof(mario_notes)/sizeof(int)) melody_pos = 0; // Zapętl
      }
      break;
  }
}


// --- FUNKCJE POMOCNICZE ---

void drawTimeScreen(struct tm &timeinfo) {
  char timeHourMin[6];
  strftime(timeHourMin, sizeof(timeHourMin), "%H:%M", &timeinfo);

  long minsToAlarm = getMinutesToNextAlarm(timeinfo);
  String alarmRemaining = (minsToAlarm >= 0) ?
    "Alarm za: " + String(minsToAlarm / 60) + "h " + String(minsToAlarm % 60) + "m" : "Brak alarmu";

  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);

    // 1. Pogoda u góry
    display.setFont(&FreeMono9pt7b);
    String weatherStr = cityName + ": " + String(currentTemp, 1) + "C, " + getWeatherDesc(weatherCode);
    display.setCursor(10, 20);
    display.print(weatherStr);

    // 2. Główna godzina (wyśrodkowana)
    display.setFont(&FreeMonoBold12pt7b);
    display.setTextSize(3);
    int16_t tbx, tby; uint16_t tbw, tbh;
    display.getTextBounds(timeHourMin, 0, 0, &tbx, &tby, &tbw, &tbh);
    display.setCursor((display.width() - tbw) / 2, 85);
    display.print(timeHourMin);
    display.setTextSize(1); // Powrót do standardowego rozmiaru

    // 3. Lewy dolny róg - status alarmu
    display.setFont(&FreeMono9pt7b);
    display.setCursor(10, 122);
    if (isAlarmEnabled) {
      display.print("[!] "); // Zastępczy glyph dla dzwonka
      char alarmTime[10];
      sprintf(alarmTime, "%02d:%02d", alarmHours[timeinfo.tm_wday], alarmMinutes[timeinfo.tm_wday]);
      display.print(alarmTime);
    } else {
      display.print("OFF");
    }

    // 4. Prawy dolny róg - czas do alarmu
    display.getTextBounds(alarmRemaining.c_str(), 0, 0, &tbx, &tby, &tbw, &tbh);
    display.setCursor(display.width() - tbw - 10, 122);
    display.print(alarmRemaining);

  } while (display.nextPage());
}


void handleButtons() {
  static unsigned long lastButtonPress = 0;
  if (millis() - lastButtonPress < 250) { // Prosty debouncing
    return;
  }

  bool buttonUp = digitalRead(BUTTON_UP_PIN) == LOW;
  bool buttonDown = digitalRead(BUTTON_DOWN_PIN) == LOW;
  bool buttonOk = digitalRead(BUTTON_OK_PIN) == LOW;

  if (buttonUp || buttonDown || buttonOk) {
    lastButtonPress = millis();

    switch (currentState) {
      case STATE_DISPLAY_TIME:
        if (buttonOk) {
          currentState = STATE_SET_ALARM_HOUR;
          forceScreenUpdate = true;
        } else if (buttonUp) {
          isAlarmEnabled = !isAlarmEnabled;
          forceScreenUpdate = true;
        }
        break;

      case STATE_SET_ALARM_HOUR:
        {
          struct tm now;
          getLocalTime(&now);
          if (buttonUp) alarmHours[now.tm_wday] = (alarmHours[now.tm_wday] + 1) % 24;
          if (buttonDown) alarmHours[now.tm_wday] = (alarmHours[now.tm_wday] - 1 + 24) % 24;
          if (buttonOk) currentState = STATE_SET_ALARM_MINUTE;
        }
        forceScreenUpdate = true;
        break;

      case STATE_SET_ALARM_MINUTE:
        {
          struct tm now;
          getLocalTime(&now);
          if (buttonUp) alarmMinutes[now.tm_wday] = (alarmMinutes[now.tm_wday] + 1) % 60;
          if (buttonDown) alarmMinutes[now.tm_wday] = (alarmMinutes[now.tm_wday] - 1 + 60) % 60;
          if (buttonOk) {
            currentState = STATE_DISPLAY_TIME;
            saveConfiguration();
          }
        }
        forceScreenUpdate = true;
        break;

      case STATE_ALARM_RINGING:
        if (buttonUp || buttonDown || buttonOk) {
          isAlarmRinging = false;
          noTone(SPEAKER_PIN);
          currentState = STATE_DISPLAY_TIME;
        }
        break;
    }
  }
}

void drawSetAlarmScreen(bool isSettingHour) {
  struct tm now;
  getLocalTime(&now);
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setFont(&FreeMonoBold12pt7b);
    display.setCursor(10, 30);
    display.print("Ustawianie alarmu");

    char alarmTimeStr[6];
    sprintf(alarmTimeStr, "%02d:%02d", alarmHours[now.tm_wday], alarmMinutes[now.tm_wday]);

    display.setTextSize(3);
    int16_t tbx, tby; uint16_t tbw, tbh;
    display.getTextBounds(alarmTimeStr, 0, 0, &tbx, &tby, &tbw, &tbh);
    display.setCursor(30, 90);
    display.print(alarmTimeStr);
    display.setTextSize(1);

    if (isSettingHour) {
      display.fillRect(30, 95, 80, 5, GxEPD_BLACK);
    } else {
      display.fillRect(135, 95, 80, 5, GxEPD_BLACK);
    }

  } while (display.nextPage());
}
// --- FUNKCJE POGODY I LOGIKI ---

void fetchWeather() {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    String url = "https://api.open-meteo.com/v1/forecast?latitude=" + String(cityLat, 4) + "&longitude=" + String(cityLon, 4) + "&current_weather=true";
    http.begin(url);
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
      String payload = http.getString();
      StaticJsonDocument<512> doc;
      DeserializationError error = deserializeJson(doc, payload);
      if (!error) {
        currentTemp = doc["current_weather"]["temperature"];
        weatherCode = doc["current_weather"]["weathercode"];
        lastWeatherFetch = millis();
        Serial.println("Pogoda zaktualizowana.");
      }
    }
    http.end();
  }
}

String getWeatherDesc(int code) {
  switch (code) {
    case 0: return "Bezchmurnie";
    case 1: case 2: case 3: return "Zachmurzenie";
    case 45: case 48: return "Mgla";
    case 51: case 53: case 55: return "Mzywka";
    case 61: case 63: case 65: return "Deszcz";
    case 71: case 73: case 75: return "Snieg";
    case 95: return "Burza";
    default: return "Inna";
  }
}

long getMinutesToNextAlarm(struct tm &now) {
  if (!isAlarmEnabled) return -1;
  int currentDay = now.tm_wday;
  int currentTotalMins = now.tm_hour * 60 + now.tm_min;

  for (int i = 0; i < 7; i++) {
    int dayIdx = (currentDay + i) % 7;
    if (alarmDays[dayIdx]) {
      int alarmTotalMins = alarmHours[dayIdx] * 60 + alarmMinutes[dayIdx];
      if (i == 0) {
        if (alarmTotalMins > currentTotalMins) return alarmTotalMins - currentTotalMins;
      } else {
        return i * 1440 + alarmTotalMins - currentTotalMins;
      }
    }
  }

  // Sprawdź jeszcze raz dzisiejszy alarm na za tydzień, jeśli jest jedynym aktywnym
  if (alarmDays[currentDay]) {
     int alarmTotalMins = alarmHours[currentDay] * 60 + alarmMinutes[currentDay];
     return 7 * 1440 + alarmTotalMins - currentTotalMins;
  }

  return -1;
}
