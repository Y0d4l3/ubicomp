#include <Arduino.h>
#include <Audio.h>
#include <Wire.h>
#include <Adafruit_BMP280.h>
#include <Adafruit_BME280.h>

/*
  Personifizierter Blumentopf – Prototyp v2: fließende Klanglandschaft
  (Team 01, UbiComp WS 26/27)

  Statt einzelner Melodietöne: eine sich ständig langsam verändernde Klangfläche,
  inspiriert von Eirik Brandals Klangskulpturen ("Imaginary Landscapes").
  Sie blendet ein, wenn jemand vorbeikommt, und klingt aus, wenn niemand mehr da ist.

  Schichten:
    1. Drone   – 4 Stimmen, gleiten langsam (Portamento) zwischen Akkordtönen,
                 schwellen einzeln an und ab (LFOs) -> "atmende" Fläche
    2. Atem    – gefiltertes rosa Rauschen, das langsam an- und abschwillt (Wind/Atem)
    3. Tropfen – vereinzelte gezupfte Saiten (Karplus-Strong = physical modelling)
    alles durch einen langsam wandernden Tiefpass und viel Hall

  Zustand der Pflanze -> Stimmung (nach Juslin & Laukka 2003):
    zufrieden       hell, Dur, lebendige Schwebungen, einige Tropfen
    durstig         tiefer, Moll, dunkler, langsamer, mehr Atem
    vernachlässigt  noch tiefer und dunkler, verstimmt (Schwebung), "seufzende" Abwärts-Glides
    gegossen        aufsteigende Glides, hell, viele Tropfen (Wasser!)

  Serial-Befehle:
    p = Präsenz (Begegnung starten/verlängern)
    d = trocken simulieren   w = gießen simulieren   a = echter Feuchtesensor
    1..4 = Stimmung erzwingen + abspielen (Hörtest)   0 = Stimmung wieder automatisch
    r = Besuche zurücksetzen   s = Status   h = Hilfe
*/

// ======================= Pins =======================
const int PIN_PRESENCE = 2;   // Taster gegen GND (später PIR, z. B. AM312)
const int PIN_MOISTURE = A0;  // Pin 14
const int PIN_LDR      = A1;  // Pin 15

// ======================= Kalibrierung =======================
// TODO: Mit sketches/sensor_test messen (Luft = trocken, Wasserglas = nass)
const int MOISTURE_RAW_DRY = 600;
const int MOISTURE_RAW_WET = 300;
const int LIGHT_RAW_DARK   = 40;
const int LIGHT_RAW_BRIGHT = 400;

// ======================= Verhalten =======================
const float THIRST_ON       = 0.55;  // ab hier durstig ...
const float THIRST_OFF      = 0.45;  // ... erst hier wieder zufrieden (Hysterese)
const int   NEGLECT_VISITS  = 3;     // Besuche ohne Gießen -> vernachlässigt
const float WATERING_DROP   = 0.3;   // Durst fällt in 5 s um so viel -> gegossen

const uint32_t SESSION_MS   = 15000; // so lange klingt es nach einem kurzen Vorbeigehen
const uint32_t LINGER_MS    = 6000;  // Nachklang, solange Präsenz anhält (für PIR)
const uint32_t FADE_IN_MS   = 2500;
const uint32_t FADE_OUT_MS  = 5000;
const uint32_t WATERED_MS   = 10000; // so lange bleibt die "gegossen"-Stimmung

// ======================= Audio-Graph =======================
//  voice0..3 ─ voiceMix ─ droneFilter(LP) ─┐
//  noise ─ noiseFilter(BP) ────────────────┼─ mainMix ─ fade ─┬────────── outMix ─ USB L/R
//  pluck ──────────────────────────────────┘                  └─ reverb ─┘
const int NUM_VOICES = 4;
AudioSynthWaveform       voice[NUM_VOICES];
AudioMixer4              voiceMix;
AudioFilterStateVariable droneFilter;
AudioSynthNoisePink      noise;
AudioFilterStateVariable noiseFilter;
AudioSynthKarplusStrong  pluck;
AudioMixer4              mainMix;
AudioEffectFade          fade;
AudioEffectFreeverb      reverb;
AudioMixer4              outMix;
AudioOutputUSB           usbOut;

AudioConnection pc0(voice[0], 0, voiceMix, 0);
AudioConnection pc1(voice[1], 0, voiceMix, 1);
AudioConnection pc2(voice[2], 0, voiceMix, 2);
AudioConnection pc3(voice[3], 0, voiceMix, 3);
AudioConnection pc4(voiceMix, 0, droneFilter, 0);
AudioConnection pc5(droneFilter, 0, mainMix, 0);   // Tiefpass-Ausgang
AudioConnection pc6(noise, 0, noiseFilter, 0);
AudioConnection pc7(noiseFilter, 1, mainMix, 1);   // Bandpass-Ausgang
AudioConnection pc8(pluck, 0, mainMix, 2);
AudioConnection pc9(mainMix, 0, fade, 0);
AudioConnection pc10(fade, 0, outMix, 0);          // trocken
AudioConnection pc11(fade, 0, reverb, 0);
AudioConnection pc12(reverb, 0, outMix, 1);        // Hall
AudioConnection pc13(outMix, 0, usbOut, 0);
AudioConnection pc14(outMix, 0, usbOut, 1);

// ======================= Stimmungen =======================
// Alle Stimmungen in A -> wiedererkennbare "Stimme" der Pflanze
const int CHORD_HAPPY[]   = {0, 4, 7, 9, 12, 14, 16, 19};  // A-Dur-Pentatonik, offen
const int CHORD_THIRSTY[] = {0, 7, 12, 15, 19, 22};        // a-Moll, tiefer
const int CHORD_NEGLECT[] = {0, 7, 12, 15, 17, 22};        // Moll mit Quarte, reibt mehr
const int CHORD_WATERED[] = {0, 4, 7, 12, 14, 16, 19, 24}; // hell, weit

struct Mood {
  const char* name;
  const int*  chord;
  int   chordLen;
  int   rootMidi;       // 45 = A2 (110 Hz), 57 = A3 (220 Hz)
  float glideMs;        // wie träge die Tonhöhe gleitet
  int   changeMinMs;    // wie oft eine Stimme zu einem neuen Ton wandert
  int   changeMaxMs;
  float bendCents;      // neuer Ton startet so viel daneben: + = von oben (seufzen), - = von unten (aufblühen)
  float swellHz;        // Tempo des An- und Abschwellens
  float cutoffLo;       // Tiefpass wandert zwischen lo und hi: Helligkeit
  float cutoffHi;
  float detuneCents;    // Schwebung zwischen den Stimmen: mehr = unruhiger
  float noiseLevel;     // Atem/Wind
  float pluckPerSec;    // Tropfen pro Sekunde (Durchschnitt)
  float level;          // Lautstärke der Drone
};

enum State { ZUFRIEDEN = 0, DURSTIG = 1, VERNACHLAESSIGT = 2, GEGOSSEN = 3 };

const Mood MOODS[] = {
  // name              chord          len root glide  chMin chMax  bend  swell  cutLo cutHi  detune noise pluck level
  { "zufrieden",       CHORD_HAPPY,   8,  57,  600,  2500, 5000,    0,  0.20,  900, 3000,   4,  0.10, 0.25, 0.22 },
  { "durstig",         CHORD_THIRSTY, 6,  45, 1500,  4000, 8000,  100,  0.10,  400, 1200,   8,  0.25, 0.08, 0.22 },
  { "vernachlaessigt", CHORD_NEGLECT, 6,  45, 2500,  5000, 9000,  250,  0.06,  250,  700,  20,  0.35, 0.03, 0.20 },
  { "gegossen",        CHORD_WATERED, 8,  57,  400,   800, 2000, -300,  0.35, 1500, 5000,   3,  0.08, 1.50, 0.22 },
};

// ======================= Klang-Engine =======================
struct Voice {
  float freq;          // aktuelle Frequenz (gleitet zum Ziel)
  float target;        // Zielfrequenz
  uint32_t nextChange; // wann diese Stimme den nächsten Ton wählt
  float lfoPhase;      // für das An- und Abschwellen
  float lfoRate;       // jede Stimme etwas anders -> lebendig
};
Voice voices[NUM_VOICES];

const Mood* mood = &MOODS[ZUFRIEDEN];
bool  soundActive = false;
float masterVolume = 1.0;
float filterPhase = 0, breathPhase = 0;

float midiToFreq(float m) { return 440.0f * powf(2.0f, (m - 69.0f) / 12.0f); }
float centsToFactor(float c) { return powf(2.0f, c / 1200.0f); }
float randf(float lo, float hi) { return lo + (hi - lo) * random(10000) / 10000.0f; }

// Neuen Zielton für Stimme i wählen. Stimme 0 bleibt im Bass (Grundton/Quinte).
void pickNewNote(int i, bool bend) {
  const Mood& m = *mood;
  int offset = (i == 0) ? m.chord[random(0, 2)] : m.chord[random(1, m.chordLen)];
  voices[i].target = midiToFreq(m.rootMidi + offset);
  if (bend) voices[i].freq = voices[i].target * centsToFactor(m.bendCents);  // Seufzer/Aufblühen
  voices[i].nextChange = millis() + random(m.changeMinMs, m.changeMaxMs);
}

void setMood(State s) {
  if (mood == &MOODS[s]) return;
  mood = &MOODS[s];
  // Stimmen wandern nacheinander in die neue Stimmung -> fließender Übergang statt Umschalten
  for (int i = 0; i < NUM_VOICES; i++) {
    voices[i].nextChange = millis() + random(0, 2000);
    voices[i].lfoRate = mood->swellHz * randf(0.7, 1.3);
  }
  Serial.printf("# Stimmung -> %s\n", mood->name);
}

// Wird alle 10 ms aufgerufen: Glides, LFOs, Filterfahrt, Tropfen
void updateSound(float dt) {
  const Mood& m = *mood;
  uint32_t now = millis();
  float glideK = 1.0f - expf(-dt * 1000.0f / m.glideMs);

  AudioNoInterrupts();  // alle Parameter gleichzeitig ändern
  for (int i = 0; i < NUM_VOICES; i++) {
    Voice& v = voices[i];
    if ((int32_t)(now - v.nextChange) >= 0) pickNewNote(i, true);

    v.freq += (v.target - v.freq) * glideK;
    v.lfoPhase += TWO_PI * v.lfoRate * dt;
    if (v.lfoPhase > TWO_PI) v.lfoPhase -= TWO_PI;

    float swell = 0.35f + 0.65f * (0.5f + 0.5f * sinf(v.lfoPhase));
    float weight = (i == 0) ? 1.0f : 0.8f;
    float detune = (i % 2) ? centsToFactor(m.detuneCents) : centsToFactor(-m.detuneCents * 0.5f);
    voice[i].frequency(v.freq * detune);
    voice[i].amplitude(m.level * weight * swell);
  }

  // Tiefpass wandert langsam -> Klangfarbe "atmet"
  filterPhase += TWO_PI * 0.05f * dt;
  float sweep = 0.5f + 0.5f * sinf(filterPhase);
  droneFilter.frequency(m.cutoffLo + (m.cutoffHi - m.cutoffLo) * sweep);

  // Atem: Rauschen schwillt an und ab, Bandpass folgt
  breathPhase += TWO_PI * 0.12f * dt;
  float breath = powf(0.5f + 0.5f * sinf(breathPhase), 2.0f);
  noise.amplitude(m.noiseLevel * breath);
  noiseFilter.frequency(600 + 1800 * breath);
  AudioInterrupts();

  // Tropfen: zufällig, im Schnitt pluckPerSec pro Sekunde
  if (randf(0, 1) < m.pluckPerSec * dt) {
    int offset = m.chord[random(0, m.chordLen)] + 12;
    pluck.noteOn(midiToFreq(m.rootMidi + offset), randf(0.2, 0.5));
  }
}

// ======================= Sensoren & Zustand =======================
Adafruit_BMP280 bmp;
Adafruit_BME280 bme;
uint8_t envChipId = 0;
uint8_t envAddr = 0;

float thirst = 0.0;          // 0 = nass, 1 = trocken (geglättet)
float overrideThirst = -1;   // >= 0: simulierter Wert
float light = 0.5;           // 0 = dunkel, 1 = hell
float temperature = NAN;
int   visitsWhileThirsty = 0;
State state = ZUFRIEDEN;
int   forcedMood = -1;       // für Hörtest: feste Stimmung
uint32_t sessionUntil = 0;   // bis wann klingt es
uint32_t wateredUntil = 0;   // bis wann "gegossen"

float thirstHistory[5];
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
  float t = (float)(analogRead(PIN_MOISTURE) - MOISTURE_RAW_WET) / (MOISTURE_RAW_DRY - MOISTURE_RAW_WET);
  t = constrain(t, 0.0f, 1.0f);
  thirst = overrideThirst >= 0 ? overrideThirst : thirst * 0.9f + t * 0.1f;

  float l = (float)(analogRead(PIN_LDR) - LIGHT_RAW_DARK) / (LIGHT_RAW_BRIGHT - LIGHT_RAW_DARK);
  light = constrain(l, 0.0f, 1.0f);

  if (envChipId == 0x58)      temperature = bmp.readTemperature();
  else if (envChipId == 0x60) temperature = bme.readTemperature();
}

State computeState() {
  if (millis() < wateredUntil) return GEGOSSEN;
  bool wasThirsty = (state == DURSTIG || state == VERNACHLAESSIGT);
  bool thirsty = wasThirsty ? thirst > THIRST_OFF : thirst > THIRST_ON;
  if (!thirsty) return ZUFRIEDEN;
  if (visitsWhileThirsty >= NEGLECT_VISITS) return VERNACHLAESSIGT;
  return DURSTIG;
}

// Nachts leiser (dezent!)
void updateVolume() {
  float v = 0.3f + 0.7f * light;
  outMix.gain(0, 0.6f * v);  // trocken
  outMix.gain(1, 0.5f * v);  // Hall
}

// ======================= Begegnungen =======================
void startOrExtendSession(uint32_t durationMs) {
  uint32_t until = millis() + durationMs;
  if (until > sessionUntil) sessionUntil = until;
  if (!soundActive) {
    soundActive = true;
    for (int i = 0; i < NUM_VOICES; i++) pickNewNote(i, false);
    fade.fadeIn(FADE_IN_MS);
    Serial.printf("# Klang an (%s)\n", mood->name);
  }
}

void onPresenceStart() {
  if (thirst > THIRST_OFF) visitsWhileThirsty++;  // jemand war da, Pflanze ist durstig
  state = computeState();
  if (forcedMood < 0) setMood(state);
  Serial.printf("# Präsenz! Zustand: %s, Besuche ohne Gießen: %d\n", MOODS[state].name, visitsWhileThirsty);
  startOrExtendSession(SESSION_MS);
}

void onWatered() {
  visitsWhileThirsty = 0;
  wateredUntil = millis() + WATERED_MS;
  Serial.println("# Gegossen! Danke :)");
  if (forcedMood < 0) setMood(GEGOSSEN);
  startOrExtendSession(WATERED_MS);
}

void checkPresence() {
  static bool wasPresent = false;
  bool present = digitalRead(PIN_PRESENCE) == LOW;
  digitalWrite(LED_BUILTIN, present);
  if (present && !wasPresent && !soundActive) onPresenceStart();  // neue Begegnung
  if (present) startOrExtendSession(LINGER_MS);                   // jemand bleibt -> weiterklingen
  wasPresent = present;
}

void checkSessionEnd() {
  if (soundActive && (int32_t)(millis() - sessionUntil) >= 0) {
    soundActive = false;
    fade.fadeOut(FADE_OUT_MS);
    Serial.println("# Klang aus");
  }
}

void checkWatering() {
  static uint32_t lastSample = 0;
  if (millis() - lastSample < 1000) return;
  lastSample = millis();

  float before = thirstHistory[historyIdx];  // Wert vor 5 s
  thirstHistory[historyIdx] = thirst;
  historyIdx = (historyIdx + 1) % 5;

  if (before - thirst > WATERING_DROP) {
    for (float& h : thirstHistory) h = thirst;
    onWatered();
  }
}

void printStatus() {
  Serial.printf("# Zustand: %s | Stimmung: %s%s | Durst %.2f%s | Licht %.2f | Temp %.1f °C | Besuche ohne Gießen %d | Klang %s\n",
                MOODS[state].name, mood->name, forcedMood >= 0 ? " (erzwungen)" : "",
                thirst, overrideThirst >= 0 ? " (simuliert)" : "",
                light, temperature, visitsWhileThirsty, soundActive ? "an" : "aus");
}

void printHelp() {
  Serial.println("# p=Präsenz  d=trocken  w=gießen  a=echter Sensor  1-4=Stimmung erzwingen  0=automatisch  r=Reset  s=Status");
}

void handleSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    switch (c) {
      case 'p': onPresenceStart(); break;
      case 'd': overrideThirst = 1.0; Serial.println("# simuliert: trocken"); break;
      case 'w': overrideThirst = 0.0; Serial.println("# simuliert: gegossen"); break;
      case 'a': overrideThirst = -1;  Serial.println("# echter Feuchtesensor"); break;
      case '1': case '2': case '3': case '4':
        forcedMood = c - '1';
        setMood((State)forcedMood);
        startOrExtendSession(SESSION_MS);
        break;
      case '0': forcedMood = -1; Serial.println("# Stimmung automatisch"); break;
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
  randomSeed(analogRead(A2) ^ micros());

  AudioMemory(40);

  // Klangfarbe der Pflanze: Bass als Dreieck (wärmer), oben Sinus (weich)
  for (int i = 0; i < NUM_VOICES; i++) {
    voice[i].begin(0, 220, i == 0 ? WAVEFORM_TRIANGLE : WAVEFORM_SINE);
    voiceMix.gain(i, 0.5);
    voices[i] = { 220, 220, 0, randf(0, TWO_PI), mood->swellHz * randf(0.7, 1.3) };
  }
  droneFilter.resonance(1.2);
  noise.amplitude(0);
  noiseFilter.resonance(2.0);
  mainMix.gain(0, 0.8);   // Drone
  mainMix.gain(1, 0.5);   // Atem
  mainMix.gain(2, 0.35);  // Tropfen
  reverb.roomsize(0.85);
  reverb.damping(0.4);
  fade.fadeOut(1);        // stumm starten

  setupEnvSensor();
  for (int i = 0; i < 50; i++) { readSensors(); delay(5); }
  for (float& h : thirstHistory) h = thirst;
  state = computeState();
  setMood(state);
  updateVolume();

  Serial.println("# Blumentopf v2 (Klanglandschaft) bereit.");
  printHelp();
}

void loop() {
  static uint32_t lastSound = 0, lastSensor = 0, lastStatus = 0;
  uint32_t now = millis();

  handleSerial();
  checkPresence();

  if (now - lastSound >= 10) {
    float dt = (now - lastSound) / 1000.0f;
    lastSound = now;
    updateSound(dt);
  }

  if (now - lastSensor >= 100) {
    lastSensor = now;
    readSensors();
    checkWatering();
    state = computeState();
    if (forcedMood < 0) setMood(state);
    updateVolume();
    checkSessionEnd();
  }

  if (now - lastStatus >= 10000) {
    lastStatus = now;
    printStatus();
  }
}
