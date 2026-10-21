#!/usr/bin/env python3
"""Include project/font/board-library/SDK FatFs notices in the artifact."""
from pathlib import Path
import os
import shutil

root = Path(__file__).resolve().parent.parent
out = root / "build/licenses"
out.mkdir(parents=True, exist_ok=True)
for source in ("LICENSE", "THIRD_PARTY.md", "tools/OFL-NotoSansSC.txt"):
    shutil.copyfile(root / source, out / Path(source).name)
notices = []
for source in ("components/epaper_port/epaper_port.c",
               "components/axpPower/src/XPowersAXP2101.tpp",
               "components/axpPower/src/XPowersCommon.tpp",
               "components/axpPower/src/XPowersLibInterface.cpp"):
    text = (root / source).read_text()
    notices.append(source + "\n" + text[:text.index("*/") + 2])
(out / "BOARD-LIBRARY-NOTICES.txt").write_text("\n\n".join(notices) + "\n")
idf = Path(os.environ["IDF_PATH"])
for name in ("LICENSE", "NOTICE"):
    if (idf / name).is_file():
        shutil.copyfile(idf / name, out / ("ESP-IDF-" + name))

# FatFs has its own permissive ChaN license, not ESP-IDF's Apache-2.0.
# Read the actual build SDK, not a vendored or independently downloaded copy.
# Keep all leading comments (ffunicode.c has several before its license).
sdk_notices = []
for source in ("components/fatfs/src/ff.c",
               "components/fatfs/src/ffunicode.c",
               "components/fatfs/port/freertos/ffsystem.c",
               "components/fatfs/diskio/diskio.c",
               "components/fatfs/diskio/diskio_impl.h",
               "components/fatfs/diskio/diskio_sdmmc.c",
               "components/fatfs/diskio/diskio_sdmmc.h",
               "components/sdmmc/sdmmc_cmd.c",
               "components/esp_driver_sdmmc/src/sdmmc_host.c"):
    text = (idf / source).read_text()
    # Both C sources and headers begin code at a preprocessor directive.
    # Missing files/directives must fail packaging instead of losing notices.
    banner = text[:text.index("\n#")].strip()
    if not banner:
        raise ValueError(f"Missing SDK license banner: {source}")
    sdk_notices.append(source + "\n" + banner)
(out / "FATFS-SDMMC-NOTICES.txt").write_text("\n\n".join(sdk_notices) + "\n")
