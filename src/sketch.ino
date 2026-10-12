#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Keypad.h>
#include <LiquidCrystal_I2C.h>
#include <Servo.h>

// ------------------------------------------------------------
// Smart Access Control System: RFID + PIN + simulated door latch
// Board: STM32 Blue Pill (STM32F103C8)
// Authorized Wokwi Blue Card UID: 01:02:03:04
// Default PIN: 2580
//
// DEBUG BUILD: setup() shows each boot step on the LCD and Serial.
// Set DEBUG_BOOT to 0 once everything works to skip the step pauses.
// ------------------------------------------------------------
#define DEBUG_BOOT 1

// MFRC522 uses SPI1: SCK=PA5, MISO=PA6, MOSI=PA7
constexpr uint8_t RFID_SS_PIN  = PA4;  // RC522 SDA/SS pin
constexpr uint8_t RFID_RST_PIN = PB1;

constexpr uint8_t SERVO_PIN = PA8;
constexpr uint8_t BUZZER_PIN = PB0;
constexpr uint8_t RED_LED_PIN = PA0;
constexpr uint8_t GREEN_LED_PIN = PA1;

// LCD uses the Blue Pill default I2C1 pins: PB6=SCL, PB7=SDA
LiquidCrystal_I2C lcd(0x27, 16, 2);
MFRC522 rfid(RFID_SS_PIN, RFID_RST_PIN);
Servo doorServo;

const byte ROWS = 4;
const byte COLS = 4;
char keyMap[ROWS][COLS] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};

// Wiring order: R1-R4 -> PB12-PB15; C1-C4 -> PB8-PB11
byte rowPins[ROWS] = {PB12, PB13, PB14, PB15};
byte colPins[COLS] = {PB8, PB9, PB10, PB11};
Keypad keypad = Keypad(makeKeymap(keyMap), rowPins, colPins, ROWS, COLS);

const byte AUTHORIZED_UID[] = {0x01, 0x02, 0x03, 0x04};
const String CORRECT_PIN = "2580";
const unsigned long CARD_AUTH_TIMEOUT_MS = 15000;
const unsigned long DOOR_OPEN_MS = 5000;
const unsigned long LOCKOUT_MS = 10000;
const byte MAX_PIN_FAILURES = 3;

String enteredPin = "";
bool cardVerified = false;
unsigned long cardVerifiedAt = 0;

bool doorOpen = false;
unsigned long doorOpenedAt = 0;

byte failedPinAttempts = 0;
bool isLockedOut = false;
unsigned long lockoutUntil = 0;

void showMessage(const String &line1, const String &line2) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(line1.substring(0, 16));
  lcd.setCursor(0, 1);
  lcd.print(line2.substring(0, 16));
}

void showIdleScreen() {
  showMessage("SMART ACCESS", "Scan RFID card");
}

// Shows a boot step on Serial and (in debug mode) on the LCD.
// The last step visible on the LCD tells you where startup got stuck.
void bootStep(const char *text) {
  Serial.print("[boot] ");
  Serial.println(text);
#if DEBUG_BOOT
  showMessage("BOOT", text);
  delay(400);
#endif
}

void beep(unsigned int frequency, unsigned int durationMs) {
  tone(BUZZER_PIN, frequency);
  delay(durationMs);
  noTone(BUZZER_PIN);
}

bool isAuthorizedCard() {
  if (rfid.uid.size != sizeof(AUTHORIZED_UID)) {
    return false;
  }
  for (byte i = 0; i < sizeof(AUTHORIZED_UID); i++) {
    if (rfid.uid.uidByte[i] != AUTHORIZED_UID[i]) {
      return false;
    }
  }
  return true;
}

void printCardUID() {
  Serial.print("Card UID: ");
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) Serial.print('0');
    Serial.print(rfid.uid.uidByte[i], HEX);
    if (i < rfid.uid.size - 1) Serial.print(':');
  }
  Serial.println();
}

void updatePinDisplay() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Enter PIN:");
  lcd.setCursor(0, 1);
  lcd.print("PIN: ");
  for (unsigned int i = 0; i < enteredPin.length(); i++) {
    lcd.print('*');
  }
  lcd.setCursor(0, 1);
}

void lockDoor() {
  doorServo.write(0);            // 0 degrees = locked position in this demo
  doorOpen = false;
  cardVerified = false;
  enteredPin = "";
  digitalWrite(RED_LED_PIN, HIGH);
  digitalWrite(GREEN_LED_PIN, LOW);
  showMessage("DOOR LOCKED", "Scan RFID card");
  Serial.println("Door relocked.");
  beep(900, 80);
  delay(500);
  showIdleScreen();
}

void unlockDoor() {
  doorOpen = true;
  doorOpenedAt = millis();
  cardVerified = false;
  enteredPin = "";
  failedPinAttempts = 0;

  doorServo.write(90);           // 90 degrees = unlocked position in this demo
  digitalWrite(RED_LED_PIN, LOW);
  digitalWrite(GREEN_LED_PIN, HIGH);
  showMessage("ACCESS GRANTED", "Door Unlocked");
  Serial.println("Access granted: RFID + PIN accepted.");
  beep(1500, 120);
}

void startLockout() {
  cardVerified = false;
  enteredPin = "";
  isLockedOut = true;
  lockoutUntil = millis() + LOCKOUT_MS;
  showMessage("SYSTEM LOCKED", "Wait 10 seconds");
  Serial.println("Too many incorrect PINs. Temporary lockout started.");
  for (byte i = 0; i < 3; i++) {
    beep(350, 150);
    delay(100);
  }
}

void rejectPin() {
  failedPinAttempts++;
  cardVerified = false;
  enteredPin = "";
  Serial.print("Incorrect PIN. Failed attempt ");
  Serial.println(failedPinAttempts);
  beep(300, 350);

  if (failedPinAttempts >= MAX_PIN_FAILURES) {
    startLockout();
  } else {
    showMessage("Incorrect PIN", "Scan card again");
  }
}

void handleKey(char key) {
  if (!cardVerified) {
    return; // PIN entry is enabled only after an authorized RFID card.
  }

  if (key >= '0' && key <= '9') {
    if (enteredPin.length() < 4) {
      enteredPin += key;
      updatePinDisplay();
    }
    return;
  }

  if (key == '*') {
    enteredPin = "";
    updatePinDisplay();
    return;
  }

  if (key == '#') {
    if (enteredPin.length() == 4 && enteredPin == CORRECT_PIN) {
      unlockDoor();
    } else {
      rejectPin();
    }
  }
  // A/B/C/D are intentionally ignored in PIN mode.
}

void setup() {
  Serial.begin(115200);
  Serial.println("[boot] start");

  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(RED_LED_PIN, OUTPUT);
  pinMode(GREEN_LED_PIN, OUTPUT);
  digitalWrite(RED_LED_PIN, HIGH);   // red = locked
  digitalWrite(GREEN_LED_PIN, LOW);
  noTone(BUZZER_PIN);

  doorServo.attach(SERVO_PIN);
  doorServo.write(0);

  Wire.begin();
  lcd.init();
  lcd.backlight();
  showMessage("SMART ACCESS", "Starting...");
  delay(300);
  bootStep("1 LCD+servo ok");

  // Keep the RC522 deselected before SPI starts
  pinMode(RFID_SS_PIN, OUTPUT);
  digitalWrite(RFID_SS_PIN, HIGH);
  SPI.begin();
  bootStep("2 SPI ok");

  rfid.PCD_Init();
  bootStep("3 RC522 init ok");

  // Read the chip version register directly (replaces PCD_DumpVersionToSerial)
  byte version = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  Serial.print("RC522 VersionReg: 0x");
  Serial.println(version, HEX);

  if (version == 0x00 || version == 0xFF) {
    // No reader answering on SPI: warn, but keep running instead of hanging
    Serial.println("WARNING: RC522 not responding. Check SPI wiring/part.");
    showMessage("RFID NOT FOUND", "Check wiring");
    delay(2000);
  } else {
    showMessage("RC522 found", "Ver 0x" + String(version, HEX));
    delay(500);
  }

  Serial.println("Smart Access Control System ready.");
  Serial.println("Use Wokwi Blue Card UID 01:02:03:04, then PIN 2580 and #.");
  delay(1000);
  showIdleScreen();
}

void loop() {
  unsigned long now = millis();

  if (isLockedOut) {
    if (now < lockoutUntil) {
      delay(50);
      return;
    }
    isLockedOut = false;
    failedPinAttempts = 0;
    Serial.println("Lockout ended.");
    showIdleScreen();
  }

  if (doorOpen) {
    if (now - doorOpenedAt >= DOOR_OPEN_MS) {
      lockDoor();
    }
    delay(50);
    return;
  }

  if (cardVerified && now - cardVerifiedAt >= CARD_AUTH_TIMEOUT_MS) {
    cardVerified = false;
    enteredPin = "";
    showMessage("Card timed out", "Scan RFID again");
    delay(700);
    showIdleScreen();
  }

  // RFID first factor: only active when awaiting a card
  if (!cardVerified) {
    if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
      printCardUID();
      bool permitted = isAuthorizedCard();
      rfid.PICC_HaltA();
      rfid.PCD_StopCrypto1();

      if (permitted) {
        cardVerified = true;
        cardVerifiedAt = millis();
        enteredPin = "";
        showMessage("RFID accepted", "Enter PIN then #");
        Serial.println("Authorized card. Waiting for PIN.");
        beep(1300, 100);
      } else {
        cardVerified = false;
        enteredPin = "";
        showMessage("Card denied", "Unknown RFID");
        Serial.println("Access denied: unknown RFID card.");
        beep(350, 180);
        delay(100);
        beep(350, 180);
        delay(700);
        showIdleScreen();
      }
      return;
    }
  } else {
    // PIN second factor: active only after card is verified
    char key = keypad.getKey();
    if (key != NO_KEY) {
      handleKey(key);
    }
  }

  // Small delay prevents the WebAssembly simulator from pegging CPU at 100%,
  // eliminating host computer lag and heating while keeping responsiveness instant.
  delay(20);
}
