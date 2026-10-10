# Device Spec: CrowPanel Advanced 10.1" ESP32-P4 HMI

Elecrow CrowPanel Advanced 10.1-inch ESP32-P4 HMI AI display, 1024×600 IPS touch screen, Wi-Fi 6.

Source: [Elecrow product page](https://www.elecrow.com/crowpanel-advanced-10-1inch-esp32-p4-hmi-ai-display-1024x600-ips-touch-screen-wifi-6.html), retrieved 2026-10-10. Values are transcribed as published. See [Source discrepancies](#source-discrepancies) for apparent errors on the vendor page.

## Main Chip (ESP32-P4NRW32)

| Item | Value |
|---|---|
| CPU/SoC | ESP32-P4 |
| HP core | RISC-V 32-bit dual-core, up to 400 MHz |
| LP core | RISC-V 32-bit single-core, up to 40 MHz |
| System memory | 768 KB L2MEM (HP), 32 KB SRAM (LP), 8 KB TCM, 32 MB PSRAM |
| ROM | 128 KB (HP), 16 KB (LP) |
| Flash | 16 MB |
| Development environment | ESP-IDF, Arduino IDE |

## Screen

| Item | Value |
|---|---|
| Size | 10.1 in |
| Resolution | 1024×600 |
| Panel | IPS |
| Touch | Capacitive, single / 5-point |
| Viewing angle | 178° |
| Brightness | 400 cd/m² (typ.) |
| Color depth | 16.7M (8-bit) |

## Wireless (onboard antenna)

| Item | Value |
|---|---|
| Wi-Fi | 2.4 GHz Wi-Fi 6, 802.11 b/g/n |
| Bluetooth | Bluetooth 5.3 and BLE |
| Optional | Zigbee, LoRa, nRF2401, Matter, Thread, Wi-Fi HaLow |

## Interfaces and Functions

- **Interfaces:** USB 2.0, UART, I2C, GPIO female headers, SD card holder, battery socket, speaker jack, camera header, module female headers, etc.
- **Functions:** audio amplifier, battery charge management, USB-to-UART, microphones, dual speakers, etc.

### Speaker Connector

| Item | Value |
|---|---|
| Model | BX-PH 2.0-2PWT |
| Pins | 1×2P |
| Pitch | 2 mm |
| Dimensions (L×W×H) | 8 × 8.5 × 5.6 mm |
| Mounting | SMT right angle |

### Buttons and LEDs

| Control | Behavior |
|---|---|
| Reset button | Press to reset the device |
| Boot button | "Press and hold the power button to burn the program" (as published) |
| Power button | Switch on/off |
| PWR LED | Power on/off indication |
| CHG LED | Lithium battery charging status, low battery state |

## Physical and Power

| Item | Value |
|---|---|
| Mounting | Mounting holes all around (M3, 3.2 mm), embedded, shell assembly |
| Operating temperature | -20 to 70 °C |
| Storage temperature | -30 to 80 °C |
| Power input | 5 V / 2 A, via USB or UART terminal |
| Dimensions | 248 × 147 mm |

## Board Connectors

| Pin | Description | Connector |
|---|---|---|
| SPK | Audio output to speakers. Mainboard has a power amplifier. | PH2.0-2P |
| PWR | Power LED | — |
| RST | Reset button | — |
| BOOT | — | — |
| UART0-OUT | Serial link to logic modules (serial comms, printing) | HY2.0-4P |
| UART1-OUT | Serial link to logic modules (serial comms, printing) | HY2.0-4P |
| UART1-IN | — | HY2.0-4P |
| I2C-OUT | MCU-to-peripheral communication | HY2.0-4P |
| BAT | Lithium battery, with charging circuit | PH2.0-2P |

## Optional Wireless Modules

### ESP32-H2 Module

Pinout:

| Pin | Direction | Note |
|---|---|---|
| U1RXD | Input | Serial port 0 receive pin (as published) |
| U1TXD | Output | Serial port 0 receive pin (as published) |
| GPIO12 | I/O | GPIO |
| GPIO13 | I/O | GPIO |
| GPIO14 | I/O | GPIO |
| 3V3 | — | Power supply |
| GND | — | Ground reference |
| GPIO1 | I/O | GPIO |
| GPIO0 | I/O | GPIO |
| GPIO2 | I/O | GPIO |
| GPIO22 | I/O | GPIO |
| GPIO10 | I/O | GPIO |
| GPIO11 | I/O | GPIO |
| BOOT | — | Hold, then tap RST to enter flashing mode |
| RST | — | Restarts the program; also used when flashing |

Chip:

| Item | Value |
|---|---|
| Model | ESP32-H2FH4 |
| Flash | 4 MB Quad SPI |
| SRAM | 320 KB |
| ROM | 128 KB |
| LP memory | 4 KB |
| Ambient temperature | -40 to 105 °C |
| Voltage | 3.3 V |
| BLE channel center frequency | 2402–2480 MHz |
| RF TX power | -24.0 to 20.0 dBm |

Patch antenna (2.4–2.5 GHz):

| Item | Value |
|---|---|
| Peak gain | 4.33 dBi |
| Average gain | 4.0 dBi |
| Gain range | 3.59–4.33 dBi |
| Peak efficiency | 62.5% |
| Average efficiency | 57.5% |
| Efficiency range | 51.3–62.5% |

### nRF2401 Module

Pinout:

| Pin | Direction | Note |
|---|---|---|
| CE | Input | Module control |
| SCK | Output | SPI |
| MISO | Input | SPI |
| MOSI | Output | SPI |
| 3V3 | — | Power supply |
| GND | — | Ground reference |
| CSN | Input | SPI chip select |
| IRQ | Input | Interrupt, active low |

Chip (nRF24L01+):

- Worldwide 2.4 GHz ISM band
- 250 kbps, 1 Mbps, 2 Mbps air data rates
- 11.3 mA TX at 0 dBm
- 13.5 mA RX at 2 Mbps
- 900 nA power down
- On-chip voltage regulator
- 1.9–3.6 V supply

Patch antenna (2.4–2.5 GHz):

| Item | Value |
|---|---|
| Peak gain | 3.74 dBi |
| Average gain | 3.66 dBi |
| Gain range | 3.45–3.74 dBi |
| Peak efficiency | 58.9% |
| Average efficiency | 55.9% |
| Efficiency range | 53.0–58.9% |

### SX1262 (LoRa) Module

Pinout:

| Pin | Direction | Note |
|---|---|---|
| DIO1 | I/O | Configurable GPIO |
| SCK | Input | SPI |
| MISO | Input | SPI |
| MOSI | Output | SPI |
| 3V3 | — | Power supply |
| GND | — | Ground reference |
| NRESET | Output | Chip reset, active low |
| DIO2 | Input | RF switch: low = RX, high = TX |
| BUSY | Output | Status indication |
| NSS | Input | SPI chip select |
| DIO3 | I/O | Configurable GPIO |

Antenna:

| Item | Value |
|---|---|
| Gain | 3.5 dBi |
| VSWR | ≤ 1.9 |
| Input impedance | 50 Ω |
| Polarization | Vertical |
| Lightning protection | DC ground |
| Cable | RF1.13, 10 cm |
| Connector | IPEX-1 |
| Weight | 0.01 kg |
| Operating temperature | -40 to 85 °C |

## Source Discrepancies

The vendor page contains apparent errors. They are kept verbatim above and should be checked against the schematic before relying on them.

- **ESP32-H2 U1RXD/U1TXD.** Both are described as "Serial port 0 receiving pin", though the names indicate UART1 and U1TXD is an output.
- **Boot button.** The description mentions the power button. The H2 module's BOOT description (hold BOOT, tap RST) is the usual ESP flashing sequence.
- **Pin directions.** Several SPI and control pin directions (e.g. nRF2401 SCK as output, SX1262 NRESET and BUSY) look stated from inconsistent perspectives (host vs. module).
- **Wi-Fi.** "Wi-Fi 6" is listed alongside only 802.11 b/g/n; Wi-Fi 6 is 802.11ax.
