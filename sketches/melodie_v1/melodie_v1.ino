#include <Arduino.h>
#include <Audio.h>
#include <Wire.h>
#include <Adafruit_BMP280.h>
#include <Adafruit_BME280.h>

/*
  Personifizierter Blumentopf – Prototyp (Team 01, UbiComp WS 26/27)

  Idee: Die Pflanze klingt nur, wenn jemand vorbeikommt. Der Klang spiegelt
  ihren Zustand (Bodenfeuchte) und die Beziehung ("oft da, gießt aber nicht").

  Ablauf:
    Sensoren -> Durst (0..1), Licht, Besuche -> Zustand -> Stimmung (Mood)
    Präsenz-Ereignis -> generative Melodie aus der Stimmung -> USB-Audio

  Klangdesign nach Juslin & Laukka 2003:
    fröhlich = schneller, höher, heller, aufsteigend, Dur
    traurig  = langsamer, tiefer, dumpfer, fallend, Moll, weicher Einsatz
  "Persönlichkeit" = immer gleiche Grundtonart (A), gleiche Klangfarbe
  (Dreieck + leicht verstimmter Sinus), Pentatonik. Zufall gegen Gewöhnung.

  Serial-Befehle (für Demo und Hörtest, ohne echten Feuchtesensor):
    p = Präsenz auslösen        d = Erde trocken simulieren
    w = gießen simulieren       a = zurück auf echten Sensor
    1..4 = Stimmung direkt abspielen (zufrieden/durstig/vernachlässigt/gegossen)
    r = Besuchszähler zurücksetzen   s = Status   h = Hilfe
*/

// ======================= Pins =======================
const int PIN_PRESENCE = 2;   // Taster gegen GND (später PIR, z. B. AM312)
const int PIN_MOISTURE = A0;  // Pin 14
const int PIN_LDR      = A1;  // Pin 15

// ======================= Kalibrierung =======================
// TODO: Mit dem Sensor-Test messen (Sensor in Luft = trocken, in Wasser = nass)
const int MOISTURE_RAW_DRY = 600;   // Rohwert, wenn Erde trocken
const int MOISTURE_RAW_WET = 300;   // Rohwert, wenn frisch gegossen
const int LIGHT_RAW_DARK   = 40;    // Rohwert, ab dem es "Nacht" ist
const int LIGHT_RAW_BRIGHT = 400;   // Rohwert für hellen Raum

// ======================= Verhalten =======================
const float THIRST_ON          = 0.55;  // ab hier durstig ...
const float THIRST_OFF         = 0.45;  // ... und erst hier wieder zufrieden (Hysterese)
const int   NEGLECT_VISITS     = 3;     // so viele Besuche ohne Gießen -> "vernachlässigt"
const float WATERING_DROP      = 0.3;   // Durst fällt in 5 s um so viel -> gegossen
const uint32_t PRESENCE_COOLDOWN_MS = 4000;  // Mindestabstand zwischen zwei Begrüßungen

// ======================= Audio-Graph =======================
//  osc1 ─┐
//        ├─ oscMix ─ env ─ filter(LP) ─┬────────────── outMix ─ USB L/R
//  osc2 ─┘                             └─ reverb ─────┘
AudioSynthWaveform       osc1;
AudioSynthWaveform       osc2;
AudioMixer4              oscMix;
AudioEffectEnvelope      env;
AudioFilterStateVariable filter;
AudioEffectFreeverb      reverb;
AudioMixer4              outMix;
AudioOutputUSB           usbOut;

AudioConnection pc1(osc1, 0, oscMix, 0);
AudioConnection pc2(osc2, 0, oscMix, 1);
AudioConnection pc3(oscMix, 0, env, 0);
AudioConnection pc4(env, 0, filter, 0);
AudioConnection pc5(filter, 0, outMix, 0);  // trocken
AudioConnection pc6(filter, 0, reverb, 0);
AudioConnection pc7(reverb, 0, outMix, 1);  // Hall
AudioConnection pc8(outMix, 0, usbOut, 0);
AudioConnection pc9(outMix, 0, usbOut, 1);

// ======================= Stimmungen =======================
const int MAJOR_PENT[] = {0, 2, 4, 7, 9};
const int MINOR_PENT[] = {0, 3, 5, 7, 10};

struct Mood {
  const char* name;
  const int*  scale;
  int   scaleLen;
  int   rootMidi;     // tiefster Ton (MIDI), 69 = A4 = 440 Hz
  int   minNotes, maxNotes;
  int   noteMs;       // Grunddauer pro Ton = Tempo
  float contour;      // -1 = fallend ... +1 = steigend
  float cutoffHz;     // Tiefpass: hoch = hell, niedrig = dumpf
  float detuneCents;  // Verstimmung osc2: mehr = "schief", unruhig
  int   attackMs;     // weicher oder harter Toneinsatz
  float level;        // Lautstärke
};

enum State { ZUFRIEDEN = 0, DURSTIG = 1, VERNACHLAESSIGT = 2, GEGOSSEN = 3 };

const Mood MOODS[] = {
  //  name              scale       len root  notes  ms    contour cutoff  detune att level
  { "zufrieden",       MAJOR_PENT, 5,  69,   4, 6,  180,  +0.3,   3500,    4,   10, 0.50 },
  { "durstig",         MINOR_PENT, 5,  57,   3, 4,  380,  -0.5,   1200,    8,   50, 0.45 },
  { "vernachlaessigt", MINOR_PENT, 5,  57,   3, 4,  560,  -0.9,    650,   25,   90, 0.40 },
  { "gegossen",        MAJOR_PENT, 5,  69,   6, 8,  110,  +0.9,   5000,    3,    5, 0.50 },
};

// ======================= Melodie-Player =======================
struct Note { float freq; uint16_t ms; };
Note     melody[12];
int      melodyLen = 0;
int      melodyPos = 0;
bool     playing   = false;
bool     noteOn    = false;
uint32_t nextEventAt = 0;
float    detuneFactor = 1.0;

float midiToFreq(int m) { return 440.0f * powf(2.0f, (m - 69) / 12.0f); }

// Erzeugt eine neue, zufällige Melodie für die Stimmung: Zufallsweg über die Tonleiter,
// der je nach "contour" eher steigt oder fällt.
void generateMelody(const Mood& m) {
  const int steps = m.scaleLen * 2;  // zwei Oktaven
  melodyLen = random(m.minNotes, m.maxNotes + 1);

  // steigende Melodien starten unten, fallende oben
  int pos = m.contour >= 0 ? random(0, steps / 2) : random(steps / 2, steps);

  for (int i = 0; i < melodyLen; i++) {
    int midi = m.rootMidi + 12 * (pos / m.scaleLen) + m.scale[pos % m.scaleLen];
    melody[i].freq = midiToFreq(midi);

    // Rhythmus: meist Grunddauer, manchmal halb oder doppelt, letzter Ton lang
    int r = random(10);
    float factor = r < 2 ? 0.5f : (r < 8 ? 1.0f : 2.0f);
    if (i == melodyLen - 1) factor = 2.5f;
    melody[i].ms = m.noteMs * factor;

    // nächster Schritt: mit Wahrscheinlichkeit |contour| in Konturrichtung, sonst frei
    int step;
    if (random(100) < fabsf(m.contour) * 70) step = (m.contour > 0 ? 1 : -1) * random(1, 3);
    else                                     step = random(-2, 3);
    pos = constrain(pos + step, 0, steps - 1);
  }
}

void startMood(State s, float volume) {
  const Mood& m = MOODS[s];
  generateMelody(m);

  detuneFactor = powf(2.0f, m.detuneCents / 1200.0f);
  filter.frequency(m.cutoffHz);
  env.attack(m.attackMs);
  env.release(m.noteMs * 1.2f);
  osc1.amplitude(m.level * volume);
  osc2.amplitude(m.level * volume * 0.6f);

  melodyPos = 0;
  noteOn = false;
  playing = true;
  nextEventAt = millis();

  Serial.printf("# spiele '%s' (%d Töne, Lautstärke %.2f)\n", m.name, melodyLen, volume);
}

// Nicht-blockierend: wird in jedem loop() aufgerufen, damit Sensoren weiterlaufen.
void updatePlayer() {
  if (!playing || millis() < nextEventAt) return;

  if (noteOn) {                     // aktueller Ton zu Ende -> loslassen, kurze Pause
    env.noteOff();
    noteOn = false;
    nextEventAt = millis() + melody[melodyPos].ms / 4;
    melodyPos++;
    if (melodyPos >= melodyLen) playing = false;
  } else {                          // nächsten Ton starten
    float f = melody[melodyPos].freq;
    osc1.frequency(f);
    osc2.frequency(f * detuneFactor);
    env.noteOn();
    noteOn = true;
    nextEventAt = millis() + melody[melodyPos].ms * 3 / 4;
  }
}

// ======================= Sensoren & Zustand =======================
Adafruit_BMP280 bmp;
Adafruit_BME280 bme;
uint8_t envChipId = 0;
uint8_t envAddr = 0;

float thirst = 0.0;            // 0 = nass, 1 = trocken (geglättet)
float overrideThirst = -1;     // >= 0: simulierter Wert statt Sensor
float light = 0.5;             // 0 = dunkel, 1 = hell
float temperature = NAN;
int   visitsWhileThirsty = 0;
State state = ZUFRIEDEN;

float thirstHistory[5];        // Durst der letzten 5 Sekunden (für Gieß-Erkennung)
int   historyIdx = 0;

uint8_t readRegister(uint8_t addr, uint8_t reg) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission() != 0) return 0;
  Wire.requestFrom(addr, (uint8_t)1);
  return Wire.available() ? Wire.read() : 0;
}

void setupEnvSensor() {
  Wire.begin();
  const uint8_t candidates[] = {0x76, 0x77};
  for (uint8_t addr : candidates) {
    uint8_t id = readRegister(addr, 0xD0);
    if (id == 0x58 && bmp.begin(addr, id)) { envChipId = id; envAddr = addr; break; }
    if (id == 0x60 && bme.begin(addr))     { envChipId = id; envAddr = addr; break; }
  }
  if (envAddr) Serial.printf("# Umweltsensor: %s auf 0x%02X\n", envChipId == 0x60 ? "BME280" : "BMP280", envAddr);
  else         Serial.println("# Umweltsensor: nicht gefunden (läuft auch ohne)");
}

void readSensors() {
  int rawMoist = analogRead(PIN_MOISTURE);
  float t = (float)(rawMoist - MOISTURE_RAW_WET) / (MOISTURE_RAW_DRY - MOISTURE_RAW_WET);
  t = constrain(t, 0.0f, 1.0f);
  if (overrideThirst >= 0) t = overrideThirst;
  thirst = overrideThirst >= 0 ? t : thirst * 0.9f + t * 0.1f;  // Sensor glätten, Simulation sofort

  float l = (float)(analogRead(PIN_LDR) - LIGHT_RAW_DARK) / (LIGHT_RAW_BRIGHT - LIGHT_RAW_DARK);
  light = constrain(l, 0.0f, 1.0f);

  if (envChipId == 0x58)      temperature = bmp.readTemperature();
  else if (envChipId == 0x60) temperature = bme.readTemperature();
}

State computeState() {
  bool thirsty = (state == ZUFRIEDEN) ? thirst > THIRST_ON : thirst > THIRST_OFF;
  if (!thirsty) return ZUFRIEDEN;
  if (visitsWhileThirsty >= NEGLECT_VISITS) return VERNACHLAESSIGT;
  return DURSTIG;
}

// Nachts leiser (dezent!), tagsüber normal
float currentVolume() { return 0.3f + 0.7f * light; }

// ======================= Ereignisse =======================
void onPresence() {
  if (thirst > THIRST_OFF) visitsWhileThirsty++;  // jemand war da, Pflanze ist durstig
  state = computeState();
  Serial.printf("# Präsenz! Zustand: %s, Besuche ohne Gießen: %d\n", MOODS[state].name, visitsWhileThirsty);
  startMood(state, currentVolume());
}

void onWatered() {
  visitsWhileThirsty = 0;
  Serial.println("# Gegossen! Danke :)");
  startMood(GEGOSSEN, currentVolume());
}

void checkPresence() {
  static bool wasPresent = false;
  static uint32_t lastTrigger = 0;
  bool present = digitalRead(PIN_PRESENCE) == LOW;
  digitalWrite(LED_BUILTIN, present);
  if (present && !wasPresent && millis() - lastTrigger > PRESENCE_COOLDOWN_MS) {
    lastTrigger = millis();
    onPresence();
  }
  wasPresent = present;
}

void checkWatering() {
  static uint32_t lastSample = 0;
  if (millis() - lastSample < 1000) return;
  lastSample = millis();

  float before = thirstHistory[historyIdx];  // ältester Wert (vor 5 s)
  thirstHistory[historyIdx] = thirst;
  historyIdx = (historyIdx + 1) % 5;

  if (before - thirst > WATERING_DROP) {
    for (float& h : thirstHistory) h = thirst;  // nicht mehrfach auslösen
    onWatered();
  }
}

void printStatus() {
  Serial.printf("# Zustand: %s | Durst %.2f%s | Licht %.2f | Temp %.1f °C | Besuche ohne Gießen %d\n",
                MOODS[computeState()].name, thirst, overrideThirst >= 0 ? " (simuliert)" : "",
                light, temperature, visitsWhileThirsty);
}

void printHelp() {
  Serial.println("# p=Präsenz  d=trocken  w=gießen  a=echter Sensor  1-4=Stimmung  r=Reset  s=Status");
}

void handleSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    switch (c) {
      case 'p': onPresence(); break;
      case 'd': overrideThirst = 1.0; Serial.println("# simuliert: trocken"); break;
      case 'w': overrideThirst = 0.0; Serial.println("# simuliert: gegossen"); break;
      case 'a': overrideThirst = -1;  Serial.println("# echter Feuchtesensor"); break;
      case '1': case '2': case '3': case '4': startMood((State)(c - '1'), currentVolume()); break;
      case 'r': visitsWhileThirsty = 0; Serial.println("# Besuche zurückgesetzt"); break;
      case 's': printStatus(); break;
      case 'h': printHelp(); break;
    }
  }
}

// ======================= Setup / Loop =======================
void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {}

  pinMode(PIN_PRESENCE, INPUT_PULLUP);
  pinMode(LED_BUILTIN, OUTPUT);
  randomSeed(analogRead(A2) ^ micros());  // offener Pin = Rauschen = Zufall

  AudioMemory(30);

  // Klangfarbe = Persönlichkeit: weiches Dreieck + leicht verstimmter Sinus
  osc1.begin(0.5, 440, WAVEFORM_TRIANGLE);
  osc2.begin(0.3, 440, WAVEFORM_SINE);
  oscMix.gain(0, 0.6);
  oscMix.gain(1, 0.4);
  env.attack(10);
  env.hold(0);
  env.decay(150);
  env.sustain(0.6);
  env.release(300);
  filter.resonance(0.9);
  reverb.roomsize(0.6);
  reverb.damping(0.5);
  outMix.gain(0, 0.7);  // trocken
  outMix.gain(1, 0.3);  // Hall

  setupEnvSensor();
  for (int i = 0; i < 50; i++) { readSensors(); delay(5); }  // Glättung einschwingen lassen
  for (float& h : thirstHistory) h = thirst;

  Serial.println("# Blumentopf bereit.");
  printHelp();
}

void loop() {
  static uint32_t lastSensor = 0, lastStatus = 0;

  handleSerial();
  checkPresence();
  updatePlayer();

  if (millis() - lastSensor >= 100) {
    lastSensor = millis();
    readSensors();
    checkWatering();
    state = computeState();
  }

  if (millis() - lastStatus >= 10000) {
    lastStatus = millis();
    printStatus();
  }
}
