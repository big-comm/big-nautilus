#!/usr/bin/env python3

import os
from pathlib import Path
import shutil
import sys


build_dir = Path(sys.argv[1])
locale_dir = Path(sys.argv[2])
languages = sys.argv[3].split(",")
# Optional, so the existing terminal invocation keeps working unchanged.
domain = sys.argv[4] if len(sys.argv) > 4 else "nautilus-terminal"
prefix = sys.argv[5] if len(sys.argv) > 5 else ""
install_prefix = Path(os.environ["MESON_INSTALL_DESTDIR_PREFIX"])

if locale_dir.is_absolute():
    locale_dir = locale_dir.relative_to(os.environ["MESON_INSTALL_PREFIX"])

for language in languages:
    destination = install_prefix / locale_dir / language / "LC_MESSAGES"
    destination.mkdir(parents=True, exist_ok=True)
    shutil.copy2(build_dir / f"{prefix}{language}.mo",
                 destination / f"{domain}.mo")
