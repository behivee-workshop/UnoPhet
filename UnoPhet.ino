/*
  UnoPhet - Prophet-5-style polyphonic pad. Mozzi + MIDI.
  Arduino Uno R4 (Minima/WiFi) - uses the board's built-in 12-bit DAC on A0.

  Audio out : A0 (Mozzi's internal-DAC mode on Renesas, 12-bit, 32768 Hz)
  MIDI in   : RX (pin 0), via MIDI Library - same simple jack+resistor circuit
              as UnoSaw, no opto-isolation, no MIDI thru/out
  Voice     : 2x detuned sawtooth -> resonant lowpass filter. Two INDEPENDENT
              envelopes per voice (amp + filter, separate shapes), plus a
              small fixed per-voice pitch/cutoff drift so the 6 voices don't
              all agree perfectly. 6-voice pool, oldest-voice-steals.

  Four MACRO pots - each one moves two related parameters together, not a
  raw 1:1 knob-per-parameter layout:

  A1 Shape      - envelope times: short/percussive <-> slow cinematic swell.
                  Scales amp attack/release AND filter attack/release
                  together, keeping the filter-leads-amp relationship fixed.
  A2 Brightness - filter cutoff AND filter-envelope amount together: dark
                  and mellow <-> open and punchy.
  A3 Grit       - resonance AND oscillator detune width together: clean and
                  narrow <-> harsh and wide. Updates live on held notes.
  A4 Weight     - sub-oscillator level (when D2 is on) AND a slight downward
                  nudge on cutoff as it rises, so more bass doesn't also mean
                  boomy top end. Updates live on held notes.

  D2 : button, toggles the sub-oscillator on/off (A4 sets its level once on)
*/

// Overall output trim. 1.0 = unity, lower it if the signal is too hot.
// Converted to fixed-point once in setup() - UnoSaw's history here: redoing
// this float multiply every audio sample (instead of precomputing it) was
// what caused its "helicopter" warble, so don't move this math into updateAudio().
float masterVolume = 0.4f;
int16_t masterVolumeQ8;

#define MOZZI_CONTROL_RATE 128
#define MOZZI_AUDIO_RATE 32768
#include <Mozzi.h>
#include <Oscil.h>
#include <ADSR.h>
#include <ResonantFilter.h>
#include <tables/saw2048_int8.h>
#include <tables/sin2048_int8.h>

#include <MIDI.h>
// MIDI_CREATE_DEFAULT_INSTANCE() would bind to Serial here - on the R4 that's
// the USB-to-computer port, not the RX pin, because the R4 core doesn't define
// USBCON (which the MIDI library normally checks to pick Serial1 on native-USB
// boards). Bind to Serial1 explicitly so it actually listens on the RX pin.
MIDI_CREATE_INSTANCE(HardwareSerial, Serial1, MIDI);
// Omni by default (listens on all channels). UnoSaw pins to one channel with
// MIDI.begin(2) instead - swap the line in setup() below if you want that here too.

#define NUM_VOICES 6

// ---- macro pot ranges ----
#define AMP_ATTACK_MIN_MS   15
#define AMP_ATTACK_MAX_MS   1500
#define AMP_RELEASE_MIN_MS  60
#define AMP_RELEASE_MAX_MS  2500
#define FILTER_ATTACK_RATIO  0.34f // filter attack = amp attack * this - stays shorter, same shape at any Shape setting
#define FILTER_RELEASE_RATIO 0.56f

#define AMP_DECAY_MS    120 // fixed, not part of Shape - the short dip into sustain, same as UnoSaw
#define FILTER_DECAY_MS 250 // fixed
#define SUSTAIN_HOLD_MS 20000 // fixed "held open" - note-off always arrives first in practice

#define CUTOFF_MIN 2000UL
#define CUTOFF_MAX 28000UL
#define FILTER_ENV_AMOUNT_MIN 8000UL
#define FILTER_ENV_AMOUNT_MAX 40000UL

#define RESONANCE_MIN 8000
#define RESONANCE_MAX 58000
#define DETUNE_MIN_CENTS 3
#define DETUNE_MAX_CENTS 14

#define SUB_LEVEL_MIN 30
#define SUB_LEVEL_MAX 255
#define WEIGHT_DARKEN_MAX 3000UL // max cutoff reduction at full Weight

#define SUB_BUTTON_PIN 2
#define DEBOUNCE_MS 30

// Small fixed per-voice drift on top of whatever the pots are doing - real
// analog polysynths never have their voices perfectly agree, and that's a
// big part of why they sound "alive". Not randomized per-note, just a fixed
// personality baked into each of the 6 voice slots.
const int8_t VOICE_DETUNE_DRIFT_CENTS[NUM_VOICES] = { -3, 2, -1, 4, -2, 1 };
const int16_t VOICE_CUTOFF_DRIFT[NUM_VOICES]      = { -1400, 900, -600, 1300, -900, 500 };

Oscil<SAW2048_NUM_CELLS, MOZZI_AUDIO_RATE> osc1[NUM_VOICES];
Oscil<SAW2048_NUM_CELLS, MOZZI_AUDIO_RATE> osc2[NUM_VOICES];
Oscil<SIN2048_NUM_CELLS, MOZZI_AUDIO_RATE> subOsc[NUM_VOICES];
ADSR<MOZZI_CONTROL_RATE, MOZZI_AUDIO_RATE> env[NUM_VOICES];       // amplitude
ADSR<MOZZI_CONTROL_RATE, MOZZI_AUDIO_RATE> filterEnv[NUM_VOICES]; // cutoff, independent shape
LowPassFilter16 filt[NUM_VOICES]; // ResonantFilter<LOWPASS, uint16_t>

byte voiceNote[NUM_VOICES];
unsigned long voiceStartTime[NUM_VOICES];
uint8_t lastEnvLevel[NUM_VOICES];       // amp env, cached each audio sample
uint8_t lastFilterEnvLevel[NUM_VOICES]; // filter env, cached each audio sample

float voiceDriftRatio[NUM_VOICES];  // pow() done once in setup(), this drift is fixed
float voiceBaseFreq[NUM_VOICES];    // pow() done once per note-on, cached until the next one

// live macro values, updated in updateControl(), read in updateAudio()/noteOn()
uint16_t liveCutoff = CUTOFF_MIN;
uint16_t liveFilterEnvAmount = FILTER_ENV_AMOUNT_MIN;
uint16_t liveResonance = RESONANCE_MIN;
int liveDetuneCents = DETUNE_MIN_CENTS;
float detuneRatioUp = 1.0f;   // pow(2, liveDetuneCents/1200) - recomputed once per control
float detuneRatioDown = 1.0f; // tick in updateControl(), not per voice - see note there
uint8_t liveSubLevel = SUB_LEVEL_MIN;
uint16_t liveWeightDarken = 0;
uint8_t subLevel255 = 0; // liveSubLevel when enabled, else 0

unsigned long ledOffAt = 0;

bool subEnabled = false;
bool lastButtonReading = HIGH; // INPUT_PULLUP idles HIGH, press pulls LOW
unsigned long lastDebounceTime = 0;

float noteFreq(byte note) {
  return 440.0f * pow(2.0f, (note - 69) / 12.0f);
}

// (Re)sets a voice's oscillator frequencies from its cached base pitch and the
// current (already-computed) Grit detune ratios. No pow() in here on purpose -
// this runs every control tick for every playing voice so Grit can move live
// on held notes, and 6 voices x 3 pow() calls x 128/sec was enough transcendental
// math to occasionally blow the control-tick budget and glitch the audio.
void updateVoicePitch(byte v) {
  osc1[v].setFreq(voiceBaseFreq[v] * detuneRatioDown);
  osc2[v].setFreq(voiceBaseFreq[v] * detuneRatioUp);
  subOsc[v].setFreq(voiceBaseFreq[v] * 0.5f);
}

int8_t findVoice() {
  for (byte i = 0; i < NUM_VOICES; i++) {
    if (!env[i].playing()) return i;
  }
  // all busy: steal the oldest
  byte oldest = 0;
  unsigned long oldestTime = voiceStartTime[0];
  for (byte i = 1; i < NUM_VOICES; i++) {
    if (voiceStartTime[i] < oldestTime) { oldest = i; oldestTime = voiceStartTime[i]; }
  }
  return oldest;
}

void noteOn(byte channel, byte note, byte velocity) {
  if (velocity == 0) { noteOff(channel, note, velocity); return; }

  // if this note is already sounding, retrigger that same voice instead of
  // stacking a second one on it - two voices on one note would need two
  // note-offs to fully silence, and only one is ever coming.
  byte v = NUM_VOICES;
  for (byte i = 0; i < NUM_VOICES; i++) {
    if (voiceNote[i] == note && env[i].playing()) { v = i; break; }
  }
  if (v == NUM_VOICES) v = findVoice();

  voiceNote[v] = note;
  voiceStartTime[v] = millis();
  voiceBaseFreq[v] = noteFreq(note) * voiceDriftRatio[v]; // one pow() per note-on, not per tick
  updateVoicePitch(v);

  env[v].noteOn(true);       // reset=true for a clean retrigger even when stealing a voice
  filterEnv[v].noteOn(true);

  digitalWrite(13, HIGH);
  ledOffAt = millis() + 30;
}

void noteOff(byte channel, byte note, byte velocity) {
  for (byte i = 0; i < NUM_VOICES; i++) {
    if (voiceNote[i] == note && env[i].playing()) {
      env[i].noteOff();
      filterEnv[i].noteOff();
      return; // release the first match only
    }
  }
}

void setup() {
  pinMode(13, OUTPUT);
  pinMode(SUB_BUTTON_PIN, INPUT_PULLUP);

  masterVolumeQ8 = (int16_t)(masterVolume * 256);

  MIDI.setHandleNoteOn(noteOn);
  MIDI.setHandleNoteOff(noteOff);
  MIDI.begin(MIDI_CHANNEL_OMNI);
  MIDI.turnThruOff(); // no pass-through needed, and it corrupts the RX line otherwise

  for (byte i = 0; i < NUM_VOICES; i++) {
    osc1[i].setTable(SAW2048_DATA);
    osc2[i].setTable(SAW2048_DATA);
    subOsc[i].setTable(SIN2048_DATA);

    env[i].setADLevels(255, 200);
    env[i].setDecayTime(AMP_DECAY_MS);
    env[i].setSustainTime(SUSTAIN_HOLD_MS);

    filterEnv[i].setADLevels(255, 140); // settles to a dimmer-than-peak sustained brightness
    filterEnv[i].setDecayTime(FILTER_DECAY_MS);
    filterEnv[i].setSustainTime(SUSTAIN_HOLD_MS);

    voiceNote[i] = 255; // no note
    voiceDriftRatio[i] = pow(2.0f, VOICE_DETUNE_DRIFT_CENTS[i] / 1200.0f); // fixed, compute once
  }

  startMozzi();
}

void updateControl() {
  while (MIDI.read()) {} // drain everything pending, not just one message per tick
  if (millis() > ledOffAt) digitalWrite(13, LOW);

  // ---- Shape: amp + filter envelope times, together, filter always shorter ----
  uint16_t shapeRaw = mozziAnalogRead<16>(A1);
  unsigned int ampAttackMs = map(shapeRaw, 0, 65535, AMP_ATTACK_MIN_MS, AMP_ATTACK_MAX_MS);
  unsigned int ampReleaseMs = map(shapeRaw, 0, 65535, AMP_RELEASE_MIN_MS, AMP_RELEASE_MAX_MS);
  unsigned int filterAttackMs = max(5, (int)(ampAttackMs * FILTER_ATTACK_RATIO));
  unsigned int filterReleaseMs = max(5, (int)(ampReleaseMs * FILTER_RELEASE_RATIO));

  // ---- Brightness: cutoff + filter-env amount, together ----
  uint16_t brightRaw = mozziAnalogRead<16>(A2);
  liveCutoff = (uint16_t)map((uint32_t)brightRaw, 0UL, 65535UL, CUTOFF_MIN, CUTOFF_MAX);
  liveFilterEnvAmount = (uint16_t)map((uint32_t)brightRaw, 0UL, 65535UL, FILTER_ENV_AMOUNT_MIN, FILTER_ENV_AMOUNT_MAX);

  // ---- Grit: resonance + detune width, together, live on held notes ----
  uint16_t gritRaw = mozziAnalogRead<16>(A3);
  liveResonance = (uint16_t)map(gritRaw, 0, 65535, RESONANCE_MIN, RESONANCE_MAX);
  liveDetuneCents = map(gritRaw, 0, 65535, DETUNE_MIN_CENTS, DETUNE_MAX_CENTS);

  // One pow() here instead of up to 18 (6 voices x 3) down in updateVoicePitch(),
  // and skipped entirely if Grit hasn't actually moved since the last tick.
  static int lastDetuneCentsForRatio = -1000; // sentinel, forces the first calc
  if (liveDetuneCents != lastDetuneCentsForRatio) {
    detuneRatioUp = pow(2.0f, (float)liveDetuneCents / 1200.0f);
    detuneRatioDown = 1.0f / detuneRatioUp;
    lastDetuneCentsForRatio = liveDetuneCents;
  }

  // ---- Weight: sub level + cutoff darken, together, live on held notes ----
  uint16_t weightRaw = mozziAnalogRead<16>(A4);
  liveSubLevel = (uint8_t)map(weightRaw, 0, 65535, SUB_LEVEL_MIN, SUB_LEVEL_MAX);
  liveWeightDarken = (uint16_t)map((uint32_t)weightRaw, 0UL, 65535UL, 0UL, WEIGHT_DARKEN_MAX);

  bool reading = digitalRead(SUB_BUTTON_PIN);
  if (reading != lastButtonReading) lastDebounceTime = millis();
  if (millis() - lastDebounceTime > DEBOUNCE_MS) {
    static bool stableState = HIGH;
    if (reading != stableState) {
      stableState = reading;
      if (stableState == LOW) subEnabled = !subEnabled; // toggle on press
    }
  }
  lastButtonReading = reading;
  subLevel255 = subEnabled ? liveSubLevel : 0;

  for (byte i = 0; i < NUM_VOICES; i++) {
    env[i].setAttackTime(ampAttackMs);
    env[i].setReleaseTime(ampReleaseMs);
    env[i].update();

    filterEnv[i].setAttackTime(filterAttackMs);
    filterEnv[i].setReleaseTime(filterReleaseMs);
    filterEnv[i].update();

    if (env[i].playing()) updateVoicePitch(i); // Grit's detune width moves live

    uint32_t envBoost = ((uint32_t)lastFilterEnvLevel[i] * liveFilterEnvAmount) >> 8;
    int32_t cutoffForVoice = (int32_t)liveCutoff + VOICE_CUTOFF_DRIFT[i] - (int32_t)liveWeightDarken + (int32_t)envBoost;
    filt[i].setCutoffFreqAndResonance((uint16_t)constrain(cutoffForVoice, 0L, 65535L), liveResonance);
  }
}

AudioOutput updateAudio() {
  int32_t mixOut = 0;

  for (byte i = 0; i < NUM_VOICES; i++) {
    if (!env[i].playing()) continue;

    int8_t osc = (osc1[i].next() + osc2[i].next()) >> 1;
    int32_t filtered = filt[i].next(osc);

    uint8_t envVal = env[i].next();
    lastEnvLevel[i] = envVal;
    lastFilterEnvLevel[i] = filterEnv[i].next(); // must tick every sample for correct timing

    int32_t voiceOut = (filtered * (int32_t)envVal) >> 8;

    if (subLevel255 > 0) {
      int8_t sub = subOsc[i].next();
      voiceOut += ((int32_t)sub * envVal * subLevel255) >> 16;
    }

    mixOut += voiceOut;
  }

  mixOut = (mixOut * masterVolumeQ8) >> 8; // precomputed int - no float in this hot path
  mixOut = constrain(mixOut, -2047, 2047);
  return MonoOutput::fromNBit(12, mixOut);
}

void loop() {
  audioHook();
}
