# MOTO-CTRL Hardware Pinout — Board "Integrated V2" (schematic 2026-07-18)

This file is the firmware ↔ hardware contract. All pin definitions in firmware must
come from a single board-config header generated from this table. Source of truth:
`hardware/releases/v1/` schematic.

MCU: ESP32-S3-WROOM-1-N4. UART0 programming only (no USB — IO19/IO20 are
intentionally unconnected). Manual boot entry: hold BOOT (SW2, IO0) and pulse
RESET (SW1, EN).

## PROFET control (6× BTS7008-2EPA, U2–U7)

Each PROFET has two channels: IN0 → OUTx (odd), IN1 → OUTy (even).
All PROFET logic lines go through 4.7kΩ series resistors.

| Device | Signal      | ESP32 GPIO | Drives      |
|--------|-------------|------------|-------------|
| U2     | PROFET_IN1  | GPIO47     | OUT1        |
| U2     | PROFET_IN2  | GPIO14     | OUT2        |
| U2     | PROFET_DEN1 | GPIO21     | U2 diag en  |
| U3     | PROFET_IN3  | GPIO13     | OUT3        |
| U3     | PROFET_IN4  | GPIO11     | OUT4        |
| U3     | PROFET_DEN2 | GPIO12     | U3 diag en  |
| U4     | PROFET_IN5  | GPIO10     | OUT5        |
| U4     | PROFET_IN6  | GPIO3  ⚠   | OUT6        |
| U4     | PROFET_DEN3 | GPIO46 ⚠   | U4 diag en  |
| U5     | PROFET_IN7  | GPIO8      | OUT7        |
| U5     | PROFET_IN8  | GPIO17     | OUT8        |
| U5     | PROFET_DEN4 | GPIO18     | U5 diag en  |
| U6     | PROFET_IN9  | GPIO16     | OUT9        |
| U6     | PROFET_IN10 | GPIO7      | OUT10       |
| U6     | PROFET_DEN5 | GPIO15     | U6 diag en  |
| U7     | PROFET_IN11 | GPIO6      | OUT11       |
| U7     | PROFET_IN12 | GPIO4      | OUT12       |
| U7     | PROFET_DEN6 | GPIO5      | U7 diag en  |

Shared diagnostic lines:

| Signal       | ESP32 GPIO | Notes                                              |
|--------------|------------|----------------------------------------------------|
| PROFET_DSEL  | GPIO48     | Channel select for IS mux, shared across U2–U7     |
| PROFET_IS    | GPIO9      | ADC1_CH8. Sense: 2kΩ (R3) to GND, RC 2.2kΩ/1nF     |

⚠ Strapping pins: GPIO3 (JTAG source) and GPIO46 (ROM log) are used as outputs to
PROFET IN6/DEN3. Both are pulled inactive by the PROFET input structure at boot and
the assignment is verified safe on this hardware — but firmware must never enable
internal pullups on these two pins before boot completes, and must drive them low
during early init.

## Diagnostics readout procedure

Only one PROFET may have DEN high at a time. To read a channel current:
set DSEL (0 = OUT-odd/IN0 channel, 1 = OUT-even/IN1 channel), raise that device's
DENx, settle, read ADC on GPIO9, lower DENx. Round-robin all 12 channels.
I_load ≈ (V_IS / 2000Ω) × kILIS (see BTS7008-2EPA datasheet; calibrate per board).

## Handlebar buttons (via CN2)

Active-low. Each input: 2.2kΩ series + 10kΩ pullup to 3V3 + 10nF to GND.

| Signal | ESP32 GPIO |
|--------|------------|
| BTN1   | GPIO35     |
| BTN2   | GPIO36     |
| BTN3   | GPIO37     |
| BTN4   | GPIO38     |
| BTN5   | GPIO39     |
| BTN6   | GPIO40     |
| BTN7   | GPIO41     |
| BTN8   | GPIO42     |

## Analog

| Signal     | ESP32 GPIO | ADC channel | Notes                                     |
|------------|------------|-------------|-------------------------------------------|
| VSENSE_BAT | GPIO1      | ADC1_CH0    | 1MΩ/100kΩ divider, ratio 0.0909. Use 12dB attenuation. Filter cap C5 100nF. Calibrate offset/gain per board. |
| PROFET_IS  | GPIO9      | ADC1_CH8    | See diagnostics section                   |

## System / programming

| Function   | Pin              | Notes                                        |
|------------|------------------|----------------------------------------------|
| UART TX    | TXD0 (GPIO43)    | 3-pin header H1: 1=TX, 2=RX, 3=GND (3.3V logic, no auto-reset) |
| UART RX    | RXD0 (GPIO44)    |                                              |
| BOOT       | GPIO0 (SW2)      | Also usable at runtime as the factory-reset button (hold at power-on) |
| RESET      | EN (SW1)         | 10kΩ pullup + 1µF                            |

## Reserved / unconnected

| Pin    | Status                                                        |
|--------|---------------------------------------------------------------|
| GPIO2  | Spare, ADC1-capable, routed to no net. Reserved: NTC board temp / 5V monitor / aux analog. Stub a driver. |
| GPIO19 | NC (USB D-; USB intentionally unused)                         |
| GPIO20 | NC (USB D+)                                                   |
| GPIO45 | NC (strapping, VDD_SPI — correct to leave unconnected)        |

## External connectors

CN1 (KF142V-5.08-12P, output terminal): pin N = OUTN for N = 1…12.
CN2 (KF142V-5.08-8P, button input terminal): reverse order — pin N = BTN(9−N).
  Pin 1 = BTN8, pin 2 = BTN7, pin 3 = BTN6, pin 4 = BTN5,
  pin 5 = BTN4, pin 6 = BTN3, pin 7 = BTN2, pin 8 = BTN1.
  (Verified by the board designer against the layout.)
Power: 4× M3 ring-terminal pads (U11–U14) — main +12V and GND, dual feed.
LEDs: LED1–LED12 mirror OUT1–OUT12 (2.2kΩ from each OUT rail); LED13 = 3V3 power
  indicator (4.7kΩ, R40).