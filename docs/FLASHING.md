# Flashing MOTO-CTRL firmware

This board programs over **UART0 only** — there is no USB port, and no
auto-reset circuit (the RC/DTR-RTS reset network a typical ESP32 dev board
has). You need a separate 3.3V USB-to-serial adapter and you enter the
bootloader manually with two buttons. This guide covers both the initial
flash of a bare board and reflashing an already-running one over UART; for
day-to-day firmware updates once the board is paired to the app, use OTA
instead (see "OTA vs. UART" below).

Read [`DISCLAIMER.md`](../DISCLAIMER.md) before flashing a board that's
wired into a motorcycle. Bench-test any new firmware build before riding
with it.

## What you need

- A 3.3V-logic USB-to-serial adapter (e.g. an FTDI FT232RL board, a
  CP2102/CH340 adapter, or similar) set to **3.3V**, not 5V. The board's
  UART pins are not 5V-tolerant.
- Three jumper wires.
- A way to power the board: either bench 12V (10–15V, see
  [`hardware/PINOUT.md`](../hardware/PINOUT.md)) or the motorcycle's own
  12V feed. **Do not power the board from the USB adapter's 5V/3V3 pin** —
  the board runs from its own 12V input, not from UART.
- The [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/index.html)
  toolchain (for `idf.py flash`), or just `esptool.py` if you already have a
  built `.bin` and don't want the full toolchain.

## Wiring the UART adapter

The board exposes a 3-pin header (H1) for programming — see
[`hardware/PINOUT.md`](../hardware/PINOUT.md)'s "System / programming"
table:

| H1 pin | Signal | ESP32 GPIO | Connect to adapter |
|---|---|---|---|
| 1 | TX (board → adapter) | GPIO43 / TXD0 | adapter **RX** |
| 2 | RX (adapter → board) | GPIO44 / RXD0 | adapter **TX** |
| 3 | GND | — | adapter **GND** |

TX/RX are crossed, as with any serial link: the board's TX goes to the
adapter's RX, and vice versa. Do not connect the adapter's power pin to the
board at all.

## Entering the bootloader (manual BOOT/EN sequence)

Because there's no auto-reset circuit, you put the chip into its UART
download mode by hand, using the two buttons next to H1
([`hardware/PINOUT.md`](../hardware/PINOUT.md)'s BOOT/SW2 and RESET/SW1):

1. Power the board (bench supply or vehicle 12V) and connect the UART
   adapter to your computer, but don't start a flash yet.
2. **Press and hold BOOT (SW2).**
3. While still holding BOOT, **press and release RESET (SW1)** once.
4. **Release BOOT.**
5. The chip is now in UART download mode, waiting for `esptool`/`idf.py` to
   talk to it. Run your flash command now (see below) — if it times out
   waiting for a device, repeat the sequence.

> **Don't confuse this with the factory-reset gesture.** BOOT is also the
> physical factory-reset control. Same button, two different things, told
> apart entirely by *when* you press it:
>
> - Holding BOOT **through** a power-up or reset (i.e. before and during
>   step 3 above) puts the ROM into UART download mode. The application
>   firmware never runs at all until you power-cycle without holding BOOT.
>   This is what you want for flashing.
> - Pressing BOOT **within 5 seconds after** the board powers up, and then
>   holding it for 10 seconds, triggers a factory reset — see
>   [Factory reset](#factory-reset) below.
>
> The distinction is not arbitrary: BOOT is GPIO0, an ESP32-S3 strapping
> pin. Its level when reset releases is what selects download mode, so a
> factory reset can only ever be a gesture the firmware sees *after* it has
> started running.

## Factory reset

Wipes the paired phone keys, the output/input configuration and the
immobilizer's lock state and cheat-code, returning the board to a
re-enrollable factory state. Board calibration survives — it describes this
physical board's sense lines, not the owner's settings.

**No computer required.**

1. Power the board up normally, with BOOT **not** pressed. (If you hold it
   through power-up you get UART download mode instead — see the callout
   above.)
2. **Within 5 seconds**, press and hold **BOOT (SW2)**.
3. Keep holding for **10 seconds**.
4. All twelve outputs blink together six times. That is the confirmation —
   this board has no dedicated status LED, so the pattern is deliberately one
   no configured lighting function ever produces.
5. The board erases and reboots itself, coming up factory-fresh.

Releasing BOOT early cancels, and you can press again while the 5-second
window is still open. Once that window closes the firmware stops watching
until the next boot, so a stuck or shorted button can never wipe a bike's
configuration mid-ride.

If you have UART access anyway, erasing the NVS partition does the same
thing without the timing, and leaves the firmware in place:

```sh
python -m esptool --chip esp32s3 -p /dev/ttyUSB0 erase_region 0x9000 0x8000
```

## Flashing

With the toolchain set up and the chip in download mode from the steps
above:

```sh
. $IDF_PATH/export.sh
cd firmware
idf.py set-target esp32s3   # first time only, or after a clean
idf.py -p <serial-port> flash
```

`<serial-port>` is your adapter's device path (e.g. `/dev/tty.usbserial-*`
on macOS, `/dev/ttyUSB0` on Linux, `COM*` on Windows). If `idf.py` can't
find the chip, it's almost always the BOOT/RESET sequence above not landing
right — retry it.

To watch boot/log output afterward (once the app is running normally, not
in download mode):

```sh
idf.py -p <serial-port> monitor
```

If you only have a prebuilt `.bin` and don't want the full ESP-IDF
toolchain, `esptool.py write_flash` works the same way — see
`esptool`'s own docs for the exact partition offsets, which `idf.py flash`
handles for you automatically from `firmware/partitions.csv`.

## OTA vs. UART

Once a board has been flashed once and paired to the app, routine firmware
updates should go over BLE OTA (see [`docs/PROTOCOL.md`](PROTOCOL.md) §10
and the app's Firmware Update screen) — no cables, no manual bootloader
dance, and it's the path release builds are actually signed and shipped
for (`tools/sign-firmware.py`). UART flashing is for:

- The very first flash of a bare/blank board.
- Recovery, if a board is unresponsive over BLE (e.g. a corrupted image
  outside what OTA's A/B rollback covers, or BLE itself never came up).
- Development, where you're iterating on firmware locally and don't want to
  go through a signed release each time.

## Verifying after flashing

Before trusting a freshly flashed board on a bike, work through
[`docs/HARDWARE_TESTING.md`](HARDWARE_TESTING.md) on the bench — power,
outputs, inputs, BLE pairing, and the immobilizer, in that order, with
nothing connected to the motorcycle yet.
