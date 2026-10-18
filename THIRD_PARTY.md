# Upstream sources

## Waveshare board example

- Repository: https://github.com/waveshareteam/ESP32-S3-ePaper-3.97
- Revision: `9b12d40731a80213b927ee8a421cae4082952819`
- Source directory: `ESP-IDF/08_ESP32-S3_e-Paper-3.97/`
- Documentation: https://docs.waveshare.net/ESP32-S3-ePaper-3.97

`components/axpPower`, `components/epaper_port`, and `components/shtc3_bsp`
are reduced/adapted from that example. The I2C/SDMMC pin assignments, RTC
register mapping and power configuration also originate there. The application,
small bitmap renderer, USB time tool and documentation are new project code.

The inspected upstream root has no repository-wide license file. This private
project does not assert a blanket MIT license or relicense the Waveshare code.
Keep existing notices; clarify redistribution rights before making it public.

## XPowersLib

The vendored `components/axpPower/src/` files retain their upstream MIT license
and copyright notices (Lewis He / lewis he; see each file). They are included
as shipped with the Waveshare snapshot, not fetched from an unpinned release.

ESP-IDF is a separate build dependency, not vendored in this repository.
