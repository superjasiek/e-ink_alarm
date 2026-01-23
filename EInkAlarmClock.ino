#include <WiFi.h>
#include <time.h>
#include <GxEPD2_BW.h>
#include <U8g2_for_Adafruit_GFX.h>

// --- USTAWIENIA SIECI WIFI ---
const char* WIFI_SSID = "TWOJA_NAZWA_SIECI";
const char* WIFI_PASS = "TWOJE_HASLO";

// --- USTAWIENIA CZASU (NTP) ---
const char* NTP_SERVER = "pool.ntp.org";
const long  GMT_OFFSET_SEC = 3600; // Przesunięcie GMT w sekundach (np. 3600 dla UTC+1)
const int   DAYLIGHT_OFFSET_SEC = 3600; // Przesunięcie dla czasu letniego

// --- DEFINICJE PINÓW ---
// Przyciski
const int BUTTON_UP_PIN = 32;
const int BUTTON_DOWN_PIN = 33;
const int BUTTON_OK_PIN = 25;
// Głośnik
const int SPEAKER_PIN = 26;
// Wyświetlacz e-ink (SPI) - dostosuj do swojego podłączenia!
const int EPD_CS_PIN = 5;
const int EPD_DC_PIN = 17;
const int EPD_RST_PIN = 16;
const int EPD_BUSY_PIN = 4;

// --- OBIEKTY I ZMIENNE GLOBALNE ---
GxEPD2_BW<GxEPD2_290, GxEPD2_290::HEIGHT> display(GxEPD2_290(EPD_CS_PIN, EPD_DC_PIN, EPD_RST_PIN, EPD_BUSY_PIN));
U8G2_FOR_ADAFRUIT_GFX u8g2Fonts;

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


void setup() {
  // --- INICJALIZACJA KOMUNIKACJI ---
  Serial.begin(115200);
  Serial.println("Budzik E-Ink startuje...");

  // --- ŁĄCZENIE Z WIFI ---
  Serial.print("Laczenie z siecia ");
  Serial.println(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(500);
  }
  Serial.println("\nPolaczono z WiFi!");

  // --- SYNCHRONIZACJA CZASU (NTP) ---
  Serial.println("Pobieranie czasu z serwera NTP...");
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    Serial.println("Pobrano czas:");
    Serial.println(&timeinfo, "%A, %B %d %Y %H:%M:%S");
  } else {
    Serial.println("Nie udalo sie zsynchronizowac czasu.");
  }

  // --- INICJALIZACJA SPRZĘTU ---
  // Wyświetlacz
  display.init(115200);
  u8g2Fonts.begin(display);

  // Przyciski
  pinMode(BUTTON_UP_PIN, INPUT_PULLUP);
  pinMode(BUTTON_DOWN_PIN, INPUT_PULLUP);
  pinMode(BUTTON_OK_PIN, INPUT_PULLUP);

  // Głośnik
  pinMode(SPEAKER_PIN, OUTPUT);

  Serial.println("Inicjalizacja sprzetu zakonczona.");
}

void loop() {
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
  }

  if (isAlarmRinging) {
    // Generuj ton 1kHz
    tone(SPEAKER_PIN, 1000);
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

// --- FUNKCJE POMOCNICZE ---

void drawTimeScreen(struct tm &timeinfo) {
  char timeHourMin[6];
  char timeSec[4];
  char dateStr[16];

  strftime(timeHourMin, sizeof(timeHourMin), "%H:%M", &timeinfo);
  strftime(timeSec, sizeof(timeSec), ":%S", &timeinfo);
  strftime(dateStr, sizeof(dateStr), "%d.%m.%Y", &timeinfo);

  display.setPartialWindow(0, 0, display.width(), display.height());
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    u8g2Fonts.setFontMode(1);
    u8g2Fonts.setFontDirection(0);
    u8g2Fonts.setForegroundColor(GxEPD_BLACK);

    // Rysuj godzinę i minuty
    u8g2Fonts.setFont(u8g2_font_logisoso78_tn);
    u8g2Fonts.setCursor(5, 85);
    u8g2Fonts.print(timeHourMin);

    // Rysuj datę
    u8g2Fonts.setFont(u8g2_font_helvR14_tf);
    u8g2Fonts.setCursor(170, 30);
    u8g2Fonts.print(dateStr);

    // Rysuj status alarmu
    u8g2Fonts.setFont(u8g2_font_helvR14_tf);
    u8g2Fonts.setCursor(170, 60);
    char alarmStatus[15];
    sprintf(alarmStatus, "Alarm %02d:%02d", alarmHour, alarmMinute);
    u8g2Fonts.print(alarmStatus);
    u8g2Fonts.setCursor(170, 80);
    u8g2Fonts.print(isAlarmEnabled ? "(Wlaczony)" : "(Wylaczony)");

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
        drawSetAlarmScreen(buttonOk ? false : true);
        break;

      case STATE_SET_ALARM_MINUTE:
        if (buttonUp) alarmMinute = (alarmMinute + 1) % 60;
        if (buttonDown) alarmMinute = (alarmMinute - 1 + 60) % 60;
        if (buttonOk) {
          currentState = STATE_DISPLAY_TIME;
          forceScreenUpdate = true;
        }
        drawSetAlarmScreen(false);
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