/*
  Mood & Memory Assistant - Hybrid (Auto + Menu)
  Arduino Nano

  Hardware:
    LCD I2C: SDA->A4, SCL->A5, VCC->5V, GND->GND  (address usually 0x27; change if 0x3F)
    RTC DS3231: SDA->A4, SCL->A5, VCC->5V, GND->GND
    RFID RC522: SS->D10, SCK->D13, MOSI->D11, MISO->D12, RST->D9, VCC->3.3V, GND->GND
    Buzzer: D8 -> buzzer -> GND
    Buttons (INPUT_PULLUP, other leg -> GND):
      D2 = Memory, D3 = Meds, D4 = Exercise, D5 = SOS
*/

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <RTClib.h>
#include <SPI.h>
#include <MFRC522.h>

// ---------- Pins ----------
#define BUZZER_PIN      8
#define BTN_MEMORY_PIN  2
#define BTN_MED_PIN     3
#define BTN_EX_PIN      4
#define BTN_SOS_PIN     5
#define RST_PIN         9
#define SS_PIN          10

// ---------- Objects ----------
LiquidCrystal_I2C lcd(0x27, 16, 2);   // change 0x27 -> 0x3F if needed
RTC_DS3231 rtc;
MFRC522 rfid(SS_PIN, RST_PIN);

// ---------- Debounce ----------
const unsigned long DEBOUNCE_MS = 200;
unsigned long lastDebounceTime[4] = {0,0,0,0};

// ---------- Menu/Idle ----------
unsigned long lastActivity = 0;
const unsigned long IDLE_MENU_MS = 15000UL;

// ---------- RFID known items ----------
struct KnownItem {
  const char* uidHex;     // uppercase, no spaces
  const char* name;
  char lastScanned[9];    // "HH:MM:SS"
};
KnownItem knownItems[] = {
  {"6C433E06", "Keys",         "00:00:00"},
  {"05F7A904", "Medicine Box", "00:00:00"},
  {"26B0A304", "Glasses",      "00:00:00"}
};
const int KNOWN_COUNT = sizeof(knownItems)/sizeof(knownItems[0]);

// ---------- Affirmations ----------
const char* const AFFS[] = {
  "You are strong!",
  "Proud of you!",
  "Well done!",
  "Keep healthy!",
  "One step at a time."
};
const uint8_t AFF_COUNT = sizeof(AFFS)/sizeof(AFFS[0]);

// ---------- Medicines (realistic small dataset) ----------
struct MedItem {
  const char* name;
  uint8_t hour;     // 24h
  uint8_t minute;
  bool pending;     // needs ack
  int lastAckYMD;   // YYYYMMDD when acknowledged (to avoid repeat same day)
};
MedItem meds[] = {
  {"BP Tablet",   8,  0,  false, 0},
  {"Vitamin D",   13, 30, false, 0},
  {"Insulin",     18, 0,  false, 0},
  {"Calcium",     22, 0,  false, 0}
};
const int MED_COUNT = sizeof(meds)/sizeof(meds[0]);

// Schedule checks
unsigned long lastScheduleCheck = 0;
const unsigned long SCHEDULE_CHECK_MS = 5000UL; // check often so “same minute” match works

// Hydration auto nudge
int lastHydrationHour = -1;

// ---------- Helpers ----------
void beepMs(int ms) {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(ms);
  digitalWrite(BUZZER_PIN, LOW);
}
void shortBeep() { beepMs(100); }
void longBeep()  { beepMs(300); }

void uidToHex(const MFRC522::Uid &uid, char *out) {
  // out must be >= uid.size*2 + 1
  uint8_t p = 0;
  for (byte i = 0; i < uid.size; i++) {
    uint8_t b = uid.uidByte[i];
    uint8_t hi = (b >> 4) & 0x0F;
    uint8_t lo = b & 0x0F;
    out[p++] = (hi < 10) ? ('0' + hi) : ('A' + (hi - 10));
    out[p++] = (lo < 10) ? ('0' + lo) : ('A' + (lo - 10));
  }
  out[p] = '\0';
}

void ymdNow(const DateTime& t, int &ymd) {
  ymd = (t.year()*10000) + (t.month()*100) + t.day();
}

void timeToStr(const DateTime& t, char* buf /* len>=9 */) {
  sprintf(buf, "%02d:%02d:%02d", t.hour(), t.minute(), t.second());
}

// ---------- LCD ----------
void lcdMenu() {
  lcd.clear();
  lcd.setCursor(0,0); lcd.print("1:Memory 2:Meds");
  lcd.setCursor(0,1); lcd.print("3:Exerc  4:SOS ");
}

void lcdStartup() {
  DateTime now = rtc.now();
  const char* days[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
  char line1[17]; char line2[17];
  int w = now.dayOfTheWeek(); // 0=Sun
  snprintf(line1, sizeof(line1), "%s %04d-%02d-%02d", days[w], now.year(), now.month(), now.day());
  snprintf(line2, sizeof(line2), "%02d:%02d:%02d", now.hour(), now.minute(), now.second());
  lcd.clear();
  lcd.setCursor(0,0); lcd.print(line1);
  lcd.setCursor(0,1); lcd.print(line2);
  delay(1800);
  lcdMenu();
}

void showTwo(const char* l1, const char* l2, unsigned int ms=1500) {
  lcd.clear();
  lcd.setCursor(0,0); lcd.print(l1);
  lcd.setCursor(0,1); lcd.print(l2);
  delay(ms);
}

// ---------- Buttons ----------
bool readBtnOnce(int pin, int idx) {
  if (digitalRead(pin) == LOW) {
    unsigned long now = millis();
    if (now - lastDebounceTime[idx] > DEBOUNCE_MS) {
      lastDebounceTime[idx] = now;
      lastActivity = now;
      shortBeep();
      return true;
    }
  }
  return false;
}

// ---------- MEMORY (RFID) ----------
void memoryPromptScan() {
  showTwo("Memory Help", "Show tag near", 1000);
  // when user presses Memory, we fall back to the same real-time behavior as auto
}

void handleRFID_Auto() {
  // If a tag is near, read it, map to item, show info and keep buzzer ON while near
  if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
    char uid[32]; uidToHex(rfid.uid, uid);

    int idx = -1;
    for (int i = 0; i < KNOWN_COUNT; i++) {
      if (strcasecmp(uid, knownItems[i].uidHex) == 0) { idx = i; break; }
    }

    DateTime now = rtc.now();
    char tbuf[9]; timeToStr(now, tbuf);

    if (idx >= 0) {
      snprintf(knownItems[idx].lastScanned, sizeof(knownItems[idx].lastScanned), "%s", tbuf);
      lcd.clear();
      lcd.setCursor(0,0); lcd.print("Found: "); lcd.print(knownItems[idx].name);
      lcd.setCursor(0,1); lcd.print("Last: ");  lcd.print(knownItems[idx].lastScanned);
    } else {
      lcd.clear();
      lcd.setCursor(0,0); lcd.print("Unknown Tag");
      lcd.setCursor(0,1); lcd.print("UID: "); lcd.print(uid);
    }

    // Continuous buzzer while tag is present
    digitalWrite(BUZZER_PIN, HIGH);
    // Stay here while the same/any tag is present
    unsigned long lastSeen = millis();
    while (true) {
      // If still present, refresh lastSeen
      if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
        lastSeen = millis(); // tag still near
      }
      // If no tag seen for ~300ms, consider removed
      if (millis() - lastSeen > 300) break;
      // Allow SOS escape
      if (digitalRead(BTN_SOS_PIN) == LOW) break;
      delay(20);
    }
    digitalWrite(BUZZER_PIN, LOW);

    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();

    // Return to menu
    lcdMenu();
  }
}

// ---------- MEDICINE ----------
void startMedPending(int i) {
  meds[i].pending = true;
}

void checkMedicinesAuto() {
  DateTime now = rtc.now();
  int ymd; ymdNow(now, ymd);

  // Hourly hydration nudge (at minute 0)
  if (now.minute() == 0 && lastHydrationHour != now.hour()) {
    lastHydrationHour = now.hour();
    // brief nudge; non-blocking-ish
    lcd.clear();
    lcd.setCursor(0,0); lcd.print("Hydration");
    lcd.setCursor(0,1); lcd.print("Take a sip!  ");
    shortBeep(); delay(200); shortBeep();
    delay(800);
    lcdMenu();
  }

  // Trigger meds when time matches and not ACKed today
  for (int i = 0; i < MED_COUNT; i++) {
    if (!meds[i].pending) {
      if (now.hour() == meds[i].hour && now.minute() == meds[i].minute) {
        if (meds[i].lastAckYMD != ymd) {
          startMedPending(i);
        }
      }
    }
  }

  // If any pending, buzz and prompt until user confirms via Btn2
  for (int i = 0; i < MED_COUNT; i++) {
    if (meds[i].pending) {
      lcd.clear();
      lcd.setCursor(0,0); lcd.print("MED DUE:");
      lcd.setCursor(9,0); lcd.print(meds[i].hour < 10 ? "0" : ""); lcd.print(meds[i].hour);
      lcd.print(":"); lcd.print(meds[i].minute < 10 ? "0" : ""); lcd.print(meds[i].minute);
      lcd.setCursor(0,1); lcd.print(meds[i].name);
      // buzz pattern
      longBeep();
      delay(500);
      break; // only show the first pending
    }
  }
}

void confirmFirstPendingMed() {
  for (int i = 0; i < MED_COUNT; i++) {
    if (meds[i].pending) {
      meds[i].pending = false;
      DateTime now = rtc.now();
      int ymd; ymdNow(now, ymd);
      meds[i].lastAckYMD = ymd;
      // affirmation
      uint8_t a = random(0, AFF_COUNT);
      lcd.clear();
      lcd.setCursor(0,0); lcd.print("Taken: ");
      lcd.print(meds[i].name);
      lcd.setCursor(0,1); lcd.print(AFFS[a]);
      shortBeep(); delay(150); shortBeep();
      delay(1200);
      lcdMenu();
      return;
    }
  }
  // If nothing pending, show next
  DateTime now = rtc.now();
  int nextIdx = -1;
  for (int i = 0; i < MED_COUNT; i++) {
    if (now.hour() < meds[i].hour || (now.hour() == meds[i].hour && now.minute() < meds[i].minute)) {
      nextIdx = i; break;
    }
  }
  if (nextIdx == -1) nextIdx = 0; // next day first
  lcd.clear();
  lcd.setCursor(0,0); lcd.print("Next Med:");
  lcd.setCursor(0,1);
  lcd.print(meds[nextIdx].name);
  lcd.print(" @");
  lcd.print(meds[nextIdx].hour < 10 ? "0" : ""); lcd.print(meds[nextIdx].hour);
  lcd.print(":");
  lcd.print(meds[nextIdx].minute < 10 ? "0" : ""); lcd.print(meds[nextIdx].minute);
  delay(1400);
  lcdMenu();
}

// ---------- EXERCISE ----------
void exerciseFlow() {
  // Guided 4-5-4 with buzzer cues + hydration nudge
  lcd.clear();
  lcd.setCursor(0,0); lcd.print("Breathing 4-5-4");
  lcd.setCursor(0,1); lcd.print("Cycle x2");
  delay(800);

  for (uint8_t c=0; c<2; c++) {
    // Inhale 4s
    lcd.clear();
    lcd.setCursor(0,0); lcd.print("Inhale...");
    lcd.setCursor(0,1); lcd.print("4 sec");
    shortBeep();
    delay(4000);

    // Hold 5s
    lcd.clear();
    lcd.setCursor(0,0); lcd.print("Hold...");
    lcd.setCursor(0,1); lcd.print("5 sec");
    delay(5000);

    // Exhale 4s
    lcd.clear();
    lcd.setCursor(0,0); lcd.print("Exhale...");
    lcd.setCursor(0,1); lcd.print("4 sec");
    longBeep();
    delay(4000);
  }

  lcd.clear();
  lcd.setCursor(0,0); lcd.print("Well done!");
  lcd.setCursor(0,1); lcd.print("Hydration: sip");
  shortBeep(); delay(200); shortBeep();
  delay(1200);
  lcdMenu();
}

// ---------- SOS ----------
void sosFlow() {
  lcd.clear();
  lcd.setCursor(0,0); lcd.print("!!! SOS ALERT !!!");
  lcd.setCursor(0,1); lcd.print("Sending Signal..");
  delay(600);

  unsigned long start = millis();
  const unsigned long SOS_DURATION = 5000; // 5 seconds

  while (millis() - start < SOS_DURATION) {
    // S (dot dot dot)
    for (int i=0;i<1;i++){ shortBeep(); delay(200); }
    delay(300);
    // O (dash dash dash)
    for (int i=0;i<1;i++){ longBeep(); delay(200); }
    delay(300);
    // S
    for (int i=0;i<1;i++){ shortBeep(); delay(200); }
    delay(300);
  }

  lcd.clear();
  lcd.setCursor(0,0); lcd.print("SOS Ended");
  lcd.setCursor(0,1); lcd.print("Stay safe");
  delay(1500);
  lcdMenu();
}

// ---------- Setup ----------
void setup() {
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  pinMode(BTN_MEMORY_PIN, INPUT_PULLUP);
  pinMode(BTN_MED_PIN,    INPUT_PULLUP);
  pinMode(BTN_EX_PIN,     INPUT_PULLUP);
  pinMode(BTN_SOS_PIN,    INPUT_PULLUP);

  lcd.init(); lcd.backlight();
  SPI.begin();
  rfid.PCD_Init();
  Serial.begin(9600);
  delay(50);
  randomSeed(analogRead(A0));

  if (!rtc.begin()) {
    showTwo("RTC not found", "Check wiring", 2500);
  } else if (rtc.lostPower()) {
    showTwo("RTC lost power", "Set time", 1800);
  }

  lcdStartup();
  lastActivity = millis();
}

// ---------- Loop ----------
void loop() {
  // --- Auto behaviors ---
  handleRFID_Auto();  // continuous buzzer while tag near
  if (millis() - lastScheduleCheck >= SCHEDULE_CHECK_MS) {
    lastScheduleCheck = millis();
    checkMedicinesAuto();
  }

  // --- Menu-driven buttons ---
  if (readBtnOnce(BTN_MEMORY_PIN, 0)) {
    memoryPromptScan();
  }
  if (readBtnOnce(BTN_MED_PIN, 1)) {
    confirmFirstPendingMed();
  }
  if (readBtnOnce(BTN_EX_PIN, 2)) {
    exerciseFlow();
  }
  // SOS is held action (no debounce edge)
  if (digitalRead(BTN_SOS_PIN) == LOW) {
    unsigned long now = millis();
    if (now - lastDebounceTime[3] > DEBOUNCE_MS) {
      lastDebounceTime[3] = now;
      sosFlow();
    }
  }

  // --- Idle menu refresh (optional) ---
  if (millis() - lastActivity > IDLE_MENU_MS) {
    lcdMenu();
    lastActivity = millis();
  }

  delay(20);
}