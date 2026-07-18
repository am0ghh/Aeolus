# Hardware additions — IR booster, light sensing, temp sensing

Planning notes for three hardware additions to Aeolus (ESP32-C3). No firmware
changes yet — wiring only. Code will be added once hardware is in place.

## 1. PN2222 IR booster (low-side switch + LED bank)

`IrSender.begin(IR_SEND_PIN)` currently drives GPIO5 straight into an IR LED,
capped at the GPIO's own current limit (~20mA safe / 40mA abs max) — that's
the range bottleneck. The transistor moves LED current off the GPIO entirely;
the GPIO just switches the transistor's base.

Power source: **5V available** (USB VBUS pin on the devkit).

### Wiring (low-side NPN switch)

Two separate current loops meet at the transistor — easiest to read as a
connection list rather than a box diagram:

| From | Connects to |
|---|---|
| 5V rail | Resistor R1 (10Ω) — pin 1 |
| R1 — pin 2 | LED1 anode (+, longer leg) |
| LED1 cathode (−) | LED2 anode (+) |
| LED2 cathode (−) | PN2222 **Collector** |
| PN2222 **Emitter** | GND (same ground as the ESP32) |
| GPIO5 | Resistor R2 (220Ω) — pin 1 |
| R2 — pin 2 | PN2222 **Base** |

As a single-line trace of each path:

```
Power path (high current, ~200mA):
  5V ──[R1 10Ω]──▶ LED1(+ ──▶ −) ──▶ LED2(+ ──▶ −) ──▶ [Collector]

Control path (low current, ~12mA):
  GPIO5 ──[R2 220Ω]──▶ [Base]

Return path (shared):
  [Emitter] ──▶ GND
```

`[Collector]`, `[Base]`, `[Emitter]` are the three legs of the same PN2222 —
the transistor is the single point where all three lines join. Nothing else
on the board ties Collector, Base, and Emitter together directly.

- **LED config:** 2x IR LEDs **in series**, one shared 10Ω resistor. At 5V
  this leaves ~2.2V across the resistor after 2×Vf (~1.3V each) + transistor
  Vce_sat (~0.2V), giving ~200mA through both LEDs — current is automatically
  balanced in series, no per-LED resistor needed. (3 in series at 5V leaves
  too little headroom and gets sensitive to Vf tolerance — stick with 2.)
- **Base resistor:** 220Ω from GPIO5 gives ~12mA base current — plenty to
  hard-saturate a PN2222 at 200mA collector current (forced gain ~17x), while
  staying well under GPIO limits. IR bursts are brief/low-duty, so sustained
  GPIO heating isn't a concern.
- **Resistor power rating:** use ½W even though average dissipation is tiny
  (low duty cycle) — cheap insurance against a stuck/looping send.
- **Scaling further:** if 2 LEDs isn't enough range, add a second series-pair
  branch (own 10Ω resistor) off the same 5V rail and the same collector node —
  two independent 2-LED strings in parallel, each self-balanced, combined
  ~400mA. Still within PN2222's 600mA abs max collector rating but closer to
  the edge — try the single string first.
- **Why not reuse the existing 100Ω resistor:** the 100Ω currently in the
  circuit was sized for the *old* direct-GPIO-drive setup — GPIO5 (3.3V) →
  100Ω → LED → GND gives `(3.3 - 1.3Vf) / 100Ω ≈ 20mA`, the safe ceiling for a
  GPIO. Once the transistor is driving the LEDs from 5V instead, 100Ω only
  gives `(5 - 2.6Vf - 0.2Vce) / 100Ω ≈ 22mA` — almost no improvement, since
  it defeats the point of bypassing the GPIO's current limit. Dropping to
  10Ω at 5V gives ~220mA. If a 10Ω isn't on hand, equal resistors in
  parallel work as a stand-in (R/n for n resistors): 4× 100Ω in parallel ≈
  25Ω (~88mA) is a reasonable stopgap; 10× 100Ω in parallel ≈ 10Ω (~220mA)
  matches the target exactly but is impractical to build. A real 10–22Ω
  resistor is cheap and worth sourcing directly.
- **Pinout warning:** PN2222 TO-92 pinout (E-B-C) varies by
  manufacturer/clone. Verify with a multimeter diode-test or datasheet before
  wiring — reversing E/C won't always destroy it but will misbehave, and
  reversing B can pull current straight from GPIO into the LED rail.

This is a pure wiring change — `IrSender.begin(5)` keeps working unmodified
since GPIO5 still just outputs the same logic-level carrier signal.

## 2. Photoresistor (LDR) → OLED brightness

Free up an ADC pin (avoid GPIO2/8/9, the strapping pins). ESP32-C3 ADC1
channels: GPIO0/1/2/3/4. GPIO3 is taken by `IR_RECEIVE_PIN`, so use **GPIO4**
for the LDR.

### Voltage divider

```
3.3V ── LDR ──┬── GPIO4 (ADC1_CH4)
              │
           [10kΩ] ── GND
```

With the LDR on top: bright room → LDR resistance drops → more voltage at the
ADC node (higher reading in light, lower in dark — intuitive mapping). 10kΩ
fixed resistor is a reasonable starting point since most common LDRs (e.g.
GL5528) sit around 10kΩ under normal indoor lighting — gives good ADC swing
across the room's actual light range. Measure the specific LDR's resistance
in the room (lights on vs off) with a multimeter; tune the fixed resistor
toward that value if the ADC range ends up too compressed.

No extra hardware needed for the brightness side — the SSD1306 has a
software contrast register (0–255), so once there's an analog reading,
dimming is just an I2C command (`display.dim()` or a direct contrast write).
That part lands in code later.

## 3. Thermistor (NTC) → temperature trigger

Replaces the old `fans` project's DHT11. Use another free ADC1 pin, e.g.
**GPIO0** or **GPIO1**.

### Voltage divider

```
3.3V ── [Rfixed] ──┬── GPIO0 (ADC1_CH0)
                    │
                 Thermistor ── GND
```

Match `Rfixed` to the thermistor's nominal 25°C resistance (commonly 10kΩ for
generic breadboard NTCs marked "MF52" — these typically use β≈3950).
Conversion later uses the Beta equation against the divider-derived
resistance.

**Before wiring, confirm (or measure) on the actual thermistor:**
- Its nominal resistance at 25°C (often printed/marked, e.g. "103" = 10kΩ)
- Its Beta/B-coefficient (datasheet value; 3950 is the common default for
  generic 10k NTCs if unmarked)

If neither is findable, a multimeter resistance reading at a known room
temperature is enough to back-calculate a usable Beta later.

## Pin map after all three additions

| Function | Pin | Notes |
|---|---|---|
| IR TX (to transistor base) | GPIO5 | unchanged |
| IR RX | GPIO3 | unchanged |
| OLED SDA / SCL | GPIO7 / GPIO6 | unchanged |
| LDR (light) | GPIO4 | ADC1_CH4 |
| Thermistor | GPIO0 | ADC1_CH0 |
| *(spare ADC)* | GPIO1 | ADC1_CH1, free for later |

## Next step

Once wired up, add to `main.cpp`:
- ADC reads for LDR + thermistor
- Brightness mapping (LDR reading → SSD1306 contrast)
- Temperature-trigger logic (carrying over the hysteresis + minimum-runtime
  pattern from the `fans` project)
