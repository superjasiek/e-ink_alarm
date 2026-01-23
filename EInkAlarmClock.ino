#include <WiFi.h>
#include <WebServer.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <time.h>
#include <GxEPD2_BW.h>
#include <U8g2_for_Adafruit_GFX.h>
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
U8G2_FOR_ADAFRUIT_GFX u8g2Fonts;

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
int alarmHour = 6;
int alarmMinute = 30;
bool isAlarmEnabled = true;
bool isAlarmRinging = false;
bool forceScreenUpdate = false;

// Prototypy funkcji
void saveConfiguration();
void loadConfiguration();
String buildRootPage();
void handleSave();
void playRingtone(int ringtone, bool reset = false);
void drawTimeScreen(struct tm &timeinfo);
void handleButtons();
void drawSetAlarmScreen(bool isSettingHour);


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
  u8g2Fonts.begin(display);

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

  // Zresetuj flagę sprawdzania alarmu, gdy minuta się zmieni
  if (timeinfo.tm_min != lastMinute) {
    alarmCheckedForThisMinute = false;
  }

  // Logika alarmu - sprawdzana raz na minutę
  if (isAlarmEnabled && !isAlarmRinging && !alarmCheckedForThisMinute && timeinfo.tm_hour == alarmHour && timeinfo.tm_min == alarmMinute) {
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
  preferences.putInt("alarmHour", alarmHour);
  preferences.putInt("alarmMinute", alarmMinute);
  preferences.putBool("isAlarmEnabled", isAlarmEnabled);
  preferences.putLong("gmtOffset", gmtOffsetSeconds);
  preferences.putInt("ringtone", selectedRingtone);
  preferences.end();
  Serial.println("Konfiguracja zapisana.");
}

void loadConfiguration() {
  preferences.begin("alarm-clock", true);
  alarmHour = preferences.getInt("alarmHour", 6);
  alarmMinute = preferences.getInt("alarmMinute", 30);
  isAlarmEnabled = preferences.getBool("isAlarmEnabled", true);
  gmtOffsetSeconds = preferences.getLong("gmtOffset", 3600);
  selectedRingtone = preferences.getInt("ringtone", 1);
  preferences.end();
  Serial.println("Konfiguracja wczytana.");
}

// --- DYNAMICZNA STRONA WWW ---
String buildRootPage() {
  String page = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'><style>";
  page += "body{font-family:Arial,sans-serif}.container{max-width:400px;margin:auto;padding:20px}.form-group{margin-bottom:15px}label{display:block;margin-bottom:5px}input[type=number],select{width:100%;padding:8px;box-sizing:border-box}.btn{background-color:#4CAF50;color:#fff;padding:10px 15px;border:none;cursor:pointer;width:100%}";
  page += "</style></head><body><div class='container'><h2>Konfiguracja Budzika E-Ink</h2>";
  page += "<form action='/save' method='POST'>";

  page += "<div class='form-group'><label for='alarm_hour'>Godzina alarmu:</label><input type='number' id='alarm_hour' name='alarm_hour' min='0' max='23' value='" + String(alarmHour) + "' required></div>";
  page += "<div class='form-group'><label for='alarm_minute'>Minuta alarmu:</label><input type='number' id='alarm_minute' name='alarm_minute' min='0' max='59' value='" + String(alarmMinute) + "' required></div>";

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
  page += "</select></div>";

  page += "<button type='submit' class='btn'>Zapisz</button></form></div></body></html>";
  return page;
}

// --- FUNKCJE OBSŁUGI SERWERA WWW ---

void handleSave() {
  alarmHour = server.arg("alarm_hour").toInt();
  alarmMinute = server.arg("alarm_minute").toInt();
  gmtOffsetSeconds = server.arg("timezone").toInt();
  selectedRingtone = server.arg("ringtone").toInt();

  saveConfiguration();

  server.send(200, "text/plain", "Ustawienia zapisane. Budzik zostanie zrestartowany.");
  delay(1000);
  ESP.restart();
}

// --- FUNKCJE DZWONKA ---

// Definicje nut
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
  }
}


// --- FUNKCJE POMOCNICZE ---

void drawTimeScreen(struct tm &timeinfo) {
  char timeHourMin[6];
  char dateStr[16];
  const char* dayNames[] = {"Niedziela", "Poniedziałek", "Wtorek", "Środa", "Czwartek", "Piątek", "Sobota"};

  strftime(timeHourMin, sizeof(timeHourMin), "%H:%M", &timeinfo);
  strftime(dateStr, sizeof(dateStr), "%d.%m.%Y", &timeinfo);

  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    u8g2Fonts.setFontMode(1);
    u8g2Fonts.setFontDirection(0);
    u8g2Fonts.setForegroundColor(GxEPD_BLACK);
    u8g2Fonts.setBackgroundColor(GxEPD_WHITE);

    // 1. Duża godzina na środku
    u8g2Fonts.setFont(u8g2_font_logisoso78_tn);
    int16_t tw = u8g2Fonts.getUTF8Width(timeHourMin);
    u8g2Fonts.setCursor((display.width() - tw) / 2, 85);
    u8g2Fonts.print(timeHourMin);

    // 2. Lewy dolny róg - informacja o alarmie
    u8g2Fonts.setFont(u8g2_font_helvR14_tf);
    u8g2Fonts.setCursor(10, 120);
    if (isAlarmEnabled) {
      u8g2Fonts.print("ALARM: ");
      char alarmTime[10];
      sprintf(alarmTime, "%02d:%02d", alarmHour, alarmMinute);
      u8g2Fonts.print(alarmTime);
    } else {
      u8g2Fonts.print("ALARM OFF");
    }

    // 3. Prawy dolny róg - data i słownie dzień tygodnia
    u8g2Fonts.setFont(u8g2_font_helvR12_tf);
    String footerRight = String(dateStr) + " " + dayNames[timeinfo.tm_wday];
    int16_t trw = u8g2Fonts.getUTF8Width(footerRight.c_str());
    u8g2Fonts.setCursor(display.width() - trw - 10, 120);
    u8g2Fonts.print(footerRight);

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
        if (buttonUp) alarmHour = (alarmHour + 1) % 24;
        if (buttonDown) alarmHour = (alarmHour - 1 + 24) % 24;
        if (buttonOk) currentState = STATE_SET_ALARM_MINUTE;
        forceScreenUpdate = true; // Odśwież ekran
        break;

      case STATE_SET_ALARM_MINUTE:
        if (buttonUp) alarmMinute = (alarmMinute + 1) % 60;
        if (buttonDown) alarmMinute = (alarmMinute - 1 + 60) % 60;
        if (buttonOk) {
          currentState = STATE_DISPLAY_TIME;
          saveConfiguration(); // Zapisz zmiany
        }
        forceScreenUpdate = true; // Odśwież ekran
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
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    u8g2Fonts.setFontMode(1);
    u8g2Fonts.setFontDirection(0);
    u8g2Fonts.setForegroundColor(GxEPD_BLACK);
    u8g2Fonts.setBackgroundColor(GxEPD_WHITE);

    u8g2Fonts.setFont(u8g2_font_helvR14_tf);
    u8g2Fonts.setCursor(10, 30);
    u8g2Fonts.print("Ustawianie alarmu");

    char alarmTimeStr[6];
    sprintf(alarmTimeStr, "%02d:%02d", alarmHour, alarmMinute);
    u8g2Fonts.setFont(u8g2_font_logisoso58_tn);
    u8g2Fonts.setCursor(30, 90);
    u8g2Fonts.print(alarmTimeStr);

    // Podkreślenie wybranej części (godziny lub minuty)
    if (isSettingHour) {
      display.fillRect(30, 95, 80, 5, GxEPD_BLACK);
    } else {
      display.fillRect(135, 95, 80, 5, GxEPD_BLACK);
    }

  } while (display.nextPage());
}
