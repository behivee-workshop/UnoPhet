As built · Arduino Uno R4

# UnoBrass

A **Prophet-5-style analog pad** — two detuned saws through a resonant filter, two independent envelopes per voice, six voices, played live off four macro pots and one button. Built, flashed, and running.

## 01 The voice

Two sawtooth oscillators per voice, detuned a few cents apart, summed into one resonant lowpass filter. Unlike the original plan, the filter doesn't borrow the amplitude envelope's shape — it runs its own, independent ADSR. The filter opens faster and closes sooner than the amp; brightness arrives before full volume and mellows out before the note's gone. That's the one thing a real Prophet-5 has that a single shared envelope never could.

Each of the 6 voices also carries a small fixed pitch and cutoff offset — not random per note, just a baked-in personality per voice slot, so a chord doesn't sound like six identical copies stacked. That's the "alive" quality a one-voice synth structurally can't have.

amp envelope — slower attack, holds longer filter envelope — opens fast, closes early

## 02 Four controls

Each pot moves two related parameters together instead of one knob per raw value — a bigger perceptual move per turn, and fewer knobs to fight each other.

A1Shape

Scales *both* envelopes' attack and release together — short/percussive at one end, slow cinematic swell at the other — while keeping the filter-leads-amp relationship fixed.

A2Brightness

Filter cutoff *and* filter-envelope amount move together — dark and mellow at one end, open and punchy at the other.

A3Grit

Resonance *and* oscillator detune width together — clean and narrow to harsh and wide. Updates live, even on notes already held.

A4Weight

Sub-oscillator level *and* a slight downward nudge on cutoff as it rises — more bass without going boomy. Also live on held notes.

D2Sub

Button, toggles the sub-oscillator on or off. A4 sets how loud it is once it's on — the button is the switch, the pot is the level.

## 03 At a glance

|  | UnoBrass |
| --- | --- |
| MCU | RA4M1 (Cortex-M4) @ 48 MHz |
| Audio out | built-in 12-bit DAC, A0 + small RC/cap cleanup |
| Sample rate | 32768 Hz |
| Voice | 2× detuned saw → resonant lowpass, independent filter + amp envelopes |
| Polyphony | 6 notes, voice-stealing pool, per-voice pitch/cutoff drift |
| Controls | 4 macro pots (A1–A4), 1 button (D2) |
| MIDI in | opto-free jack+resistor → `Serial1` RX @ 31250 baud, omni |
| Firmware | Mozzi 2.0.2, internal-DAC mode |

## 04 As a Eurorack panel

A layout idea, not an electrical redesign — a real CV-compatible module would need ±5V-handling input stages, bipolar power instead of USB 5V, and jacks rated for patch cables, none of which this does today. But the control surface itself — 4 knobs, 1 button, MIDI in, audio out — maps directly onto a 12HP module face.

Panel layout concept — 12HP, 4 knobs in reading order (Shape, Brightness, Grit, Weight), the Sub button, MIDI in and audio out at the base.

## 05 Not doing this round

**Out of scope unless you ask:** actual CV/gate handling for a real Eurorack build, SysEx/patch editor, pitch bend, aftertouch, saved/recalled settings, ensemble/chorus (still no spare pin or pot for it).

Built, flashed, running on the bench. The panel above is a layout idea, not a build order.