#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_BME280.h>
#include <Adafruit_BMP280.h>

// Sensor-Test: Taster, LDR, Bodenfeuchte, BME/BMP280 -> Serial Monitor / Plotter
// Ausgabeformat "name:wert name:wert ..." -> direkt plottbar (Arduino Serial Plotter, Teleplot)

// ---------- Pins ----------
const int PIN_BUTTON   = 2;   // Taster gegen GND (Präsenz-Ersatz)
const int PIN_MOISTURE = A0;  // Pin 14, Feuchtesensor AOUT
const int PIN_LDR      = A1;  // Pin 15, Spannungsteiler LDR / 10k

// ---------- BME/BMP280 ----------
// Beide Chips sehen gleich aus, aber die Bibliotheken prüfen die Chip-ID.
// Darum lesen wir die ID selbst aus (Register 0xD0) und nehmen dann die passende Bibliothek.
const uint8_t CHIP_ID_BME280 = 0x60;
const uint8_t CHIP_ID_BMP280 = 0x58;

Adafruit_BME280 bme;
Adafruit_BMP280 bmp;
uint8_t envAddr = 0;    // gefundene I2C-Adresse (0x76 oder 0x77), 0 = nichts gefunden
uint8_t envChipId = 0;  // gelesene Chip-ID

// Liest ein Byte aus einem Register eines I2C-Geräts. Gibt 0 zurück, wenn niemand antwortet.
uint8_t readRegister(uint8_t addr, uint8_t reg) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission() != 0) return 0;  // != 0 -> kein ACK, Gerät nicht da
  Wire.requestFrom(addr, (uint8_t)1);
  return Wire.available() ? Wire.read() : 0;
}

void setupEnvSensor() {
  Wire.begin();  // Teensy 4.1: SDA = 18, SCL = 19

  // Beide möglichen Adressen probieren (SDO an GND -> 0x76, an 3V -> 0x77)
  const uint8_t candidates[] = {0x76, 0x77};
  for (uint8_t addr : candidates) {
    uint8_t id = readRegister(addr, 0xD0);
    if (id != 0) {
      envAddr = addr;
      envChipId = id;
      break;
    }
  }

  if (envAddr == 0) {
    Serial.println("# BME/BMP280: nichts gefunden auf 0x76/0x77 -> Verkabelung (SDA 18, SCL 19, CSB an 3V) prüfen");
    return;
  }

  Serial.printf("# Gefunden auf 0x%02X, Chip-ID 0x%02X -> ", envAddr, envChipId);
  if (envChipId == CHIP_ID_BME280) {
    Serial.println("BME280 (mit Luftfeuchte)");
    if (!bme.begin(envAddr)) Serial.println("# bme.begin() fehlgeschlagen");
  } else if (envChipId == CHIP_ID_BMP280) {
    Serial.println("BMP280 (ohne Luftfeuchte)");
    if (!bmp.begin(envAddr, envChipId)) Serial.println("# bmp.begin() fehlgeschlagen");
  } else {
    Serial.println("unbekannter Chip");
    envAddr = 0;
  }
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {}  // bis zu 3 s warten, damit der Monitor den Anfang mitbekommt

  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(LED_BUILTIN, OUTPUT);

  Serial.println("# Sensor-Test startet");
  setupEnvSensor();
}

void loop() {
  // Taster: INPUT_PULLUP -> gedrückt = LOW. Wir drehen es um, damit 1 = "jemand da".
  int presence = digitalRead(PIN_BUTTON) == LOW ? 1 : 0;
  digitalWrite(LED_BUILTIN, presence);  // LED zeigt Präsenz direkt an

  int light    = analogRead(PIN_LDR);       // 0..1023, heller = höher (LDR oben an 3V)
  int moisture = analogRead(PIN_MOISTURE);  // 0..1023, NASSER = NIEDRIGER

  Serial.printf("praesenz:%d licht:%d feuchte:%d", presence, light, moisture);

  if (envChipId == CHIP_ID_BME280 && envAddr) {
    Serial.printf(" temp:%.1f luftfeuchte:%.1f druck:%.1f",
                  bme.readTemperature(), bme.readHumidity(), bme.readPressure() / 100.0f);
  } else if (envChipId == CHIP_ID_BMP280 && envAddr) {
    Serial.printf(" temp:%.1f druck:%.1f",
                  bmp.readTemperature(), bmp.readPressure() / 100.0f);
  }
  Serial.println();

  delay(200);  // 5 Messungen pro Sekunde
}
