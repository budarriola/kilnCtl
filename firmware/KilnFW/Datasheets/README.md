# KilnFW Datasheets

**Scope rule: this folder holds datasheets ONLY for ICs that have a
communication interface (I2C, SPI, UART).** Passive/analog parts (op-amps,
MOSFETs, optocouplers, relays, LDO regulators, etc.) are documented in the
main repo's `datasheets\` library (`..\..\datasheets\`), not here. Do not
re-add them to this folder — if a part doesn't talk I2C/SPI/UART to the
firmware, it doesn't belong in `KilnFW\Datasheets`.

Pre-existing files `pinout_converted.png` and
`S8ceb2c60b7c44c02852b1f573ec41dcdW.jpg_960x960q75.jpg_.avif` were already in
this folder before this pass and were left untouched (not evaluated against
the comm-interface rule since they aren't datasheets).

## Files

| File | Part | Interface | Used for | Source |
|---|---|---|---|---|
| `MAX31856.pdf` | MAX31856 | SPI | 3-channel thermocouple ADC/cold-junction-compensation front end (ThermocoupleBoard) | copied from repo `datasheets\ThermocoupleBoard_Sensor_Temperature\MAX31856.pdf` |
| `SX1509.pdf` | SX1509 | I2C | I2C GPIO expander (mainBoard) | copied from repo `datasheets\mainBoard_SX1509\SX1509.pdf` |
| `RaspberryPi_Pico.pdf` | Raspberry Pi Pico / RP2040 | UART | UART peer MCU across the ESP32-S3 <-> Pico isolation barrier | copied from repo `datasheets\MCU_Module\RaspberryPi_Pico.pdf` |
| `ILI9488.pdf` | ILI9488 | SPI | TFT display controller in the BIGTREETECH TFT35 SPI V2.1 480x320 panel | https://www.waveshare.com/w/upload/2/2d/ILI9488_Data_Sheet.pdf |
| `XPT2046.pdf` | XPT2046 | SPI | Resistive touch controller — downloaded per task request; see `notes\BIGTREETECH_TFT35_SPI_V2.1_notes.md` for an important caveat: BIGTREETECH's own docs say the TFT35 SPI V2.1's touch IC is actually **NS2009**, not XPT2046 | https://raw.githubusercontent.com/loboris/ESP32_TFT_library/master/Documents/XPT2046.pdf (mirror; see notes for direct-vendor attempts that failed with HTTP 403) |
| `ESP32-S3_datasheet.pdf` | ESP32-S3 | SPI/I2C/UART (host MCU) | Main firmware host MCU datasheet | https://www.espressif.com/sites/default/files/documentation/esp32-s3_datasheet_en.pdf |
| `ESP32-S3-DevKitC-1_user_guide.pdf` | ESP32-S3-DevKitC-1 | — (board/pinout guide) | Dev board pinout/user guide for the ESP32-S3 host | https://mm.digikey.com/Volume0/opasdata/d220001/medias/docus/3787/ESP32-S3-DevKitC-1.pdf (Espressif user guide, mirrored via DigiKey; official `docs.espressif.com` PDF path returned 404) |
| `4.0inch_SPI_Module_ST7796_MSP4030_MSP4031_V1.0_Keep/` (directory) | ST7796S (SPI display) + FT6336U (I2C capacitive touch) | SPI + I2C | LCDWIKI/Elecrow MSP4031 module — candidate second panel, see `../docs/DISPLAY_ST7796_PLAN.md`. Pruned vendor package (~11 MB of the original 394 MB): specs, `ST7796_Init.txt`, ST7796S/FT6336U datasheets, FT6336U register `.xlsx`, module schematic PDF, Altium library, user manual, and the `Demo_ESP32/` vendor reference sketches (not compiled — see `DISPLAY_ST7796_PLAN.md` §5). Committed `41bb192`. The original untracked/gitignored 394 MB `4.0inch_SPI_Module_ST7796_MSP4030_MSP4031_V1.0/` directory next to it is filesystem cleanup, not tracked here — see `DISPLAY_ST7796_PLAN.md` §11. | vendor package (LCDWIKI/Elecrow) |

## Removed per scope change (do NOT re-add)

These were copied in an earlier pass of this task and then deleted once the
"comm-interface only" rule was set. They remain available, untouched, in the
repo-root `datasheets\` library if ever needed for non-firmware (PCB/BOM)
work:

| Part | Why excluded | Still available at |
|---|---|---|
| TCMT1109 | Optocoupler — no digital comm interface | `datasheets\mainBoard_TCMT1109\TCMT1109.pdf` |
| EE2-12NUH | Relay — no digital comm interface | `datasheets\mainBoard_Relay\EE2-12NUH.pdf` |
| AD8542 | Op-amp — no digital comm interface | `datasheets\mainBoard_AD8542\AD8542.pdf` |
| BSS138P | MOSFET — no digital comm interface | `datasheets\mainBoard_BSS138P\` (not present in local library; was downloaded fresh to `assets.nexperia.com` this pass, then deleted) |
| LT1962-3.3 | Linear LDO regulator — no digital comm interface | `datasheets\ThermocoupleBoard_Regulator_Linear\LT1962-3.3.pdf` |

## notes/

- `notes\BIGTREETECH_TFT35_SPI_V2.1_notes.md` — research notes on the
  BIGTREETECH TFT35 SPI V2.1 module: 10-pin connector pinout (with
  confidence level and caveats), logic voltage, controller ICs (including
  the ILI9488-vs-touch-IC discrepancy noted above), and backlight control.
  All claims are sourced with URLs; anything uncertain is flagged as such
  rather than guessed.

## Missing / not obtained

None. All targeted files (7 copied + ILI9488, XPT2046, ESP32-S3 datasheet,
ESP32-S3-DevKitC-1 user guide) downloaded successfully and verified as valid
PDFs (`%PDF` header, all well over 100 KB). BSS138P was also downloaded
successfully before being removed per the scope change above — no retries
were needed for it.
