# Third-party sources and hardware references

## Licensed e-paper controller reference

The public ESP-IDF display port is based on Waveshare's explicitly licensed
3.97-inch controller implementation:

- Repository: https://github.com/waveshareteam/e-Paper
- Revision: `a794fbc39656b0f93938d1ffb3fdc77eaed9e9fc`
- File: `E-paper_Separate_Program/3in97_e-Paper/Arduino_R4/EPD_3in97.cpp`
- License: permissive MIT-style grant in the source header.

`components/epaper_port/epaper_port.c` retains the Waveshare team attribution,
permission notice and disclaimer. Its ESP-IDF SPI/GPIO transport and board
power control are project code. Earlier prototype snapshots remain in the
preserved Git history; the root MIT license must not be interpreted as
retroactively relicensing third-party code without an explicit upstream grant.

## XPowersLib

`components/axpPower/src/` is vendored from the board example snapshot below.
These files carry their own MIT license and copyright notices (Lewis He /
lewis he), which are retained unchanged. `axp_prot.cpp` is the project board
wrapper around these APIs; its voltage and charging values are hardware data.

## Hardware references

- Board: https://docs.waveshare.net/ESP32-S3-ePaper-3.97
- Board example reference: https://github.com/waveshareteam/ESP32-S3-ePaper-3.97
  at `9b12d40731a80213b927ee8a421cae4082952819`.
- SHTC3 commands, timing, CRC and conversion formulas:
  https://sensirion.com/products/catalog/SHTC3
- PCF85063 register map: NXP PCF85063A datasheet.

Pin assignments, peripheral addresses and register values are hardware facts.
The public application, SHTC3 driver, RTC scheduling, I2C/SD glue, renderer and
web UI are project code. No repository-wide license is assumed for the board
example; it is cited as a hardware reference, not relicensed wholesale.

## Noto Sans SC bitmap font

`main/ui_font.c` contains 24×24 glyphs generated from Noto Sans SC,
covering printable ASCII, common GB2312 Han characters and CJK punctuation.
A shared 3-bit coverage-class resource maps to monochrome, linear four-gray,
and contour-matched four-gray. These classes are not additional panel colors.
`main/clock_font.c` contains native-size 200px time and 40px reading glyphs
with 2-bit coverage from the same font; no low-resolution bitmap is enlarged.
It is a SIL Open Font License 1.1 derivative, not covered by the root MIT grant.
The full license is retained in `tools/OFL-NotoSansSC.txt`.

- Source: https://github.com/notofonts/noto-cjk/blob/main/Sans/Variable/TTF/Subset/NotoSansSC-VF.ttf
- Input SHA-256: `d68bafcb48a2707749396aa12bbbd833cb70401f3a9a689fd2902c7e0d295964`
- Reproducers: `tools/generate_ui_font.cjs` and `tools/generate_clock_font.cjs`;
  the full input TTF is not vendored.
- The derived bitmap resources are named the epaper-clock UI and clock fonts.

## ESP-IDF provisioning protocol

ESP-IDF v5.5.2 is a separate build dependency (Apache-2.0), not vendored here.
BLE uses its `wifi_provisioning` / `protocomm` Security1 implementation.
The Pages client implements the public protobuf wire protocol and Security1
handshake; any additional source attribution is recorded alongside that client.

Project code is MIT licensed; third-party notices take precedence for their files.
