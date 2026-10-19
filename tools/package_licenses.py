#!/usr/bin/env python3
"""Include project/font/board-library notices in the firmware artifact."""
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
