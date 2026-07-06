# Pre-build hook: make the S140 v7 linker script available to the framework.
#
# The Adafruit nRF52 core only ships nrf52840_s140_v6.ld (SoftDevice v6.1.1,
# app @ 0x26000). The L1's resident SoftDevice is S140 7.3.0, whose app region
# starts at 0x27000 — linking at v6's 0x26000 lands inside the SoftDevice and
# faults at boot. Our board def (boards/seeed_wio_tracker_L1.json) sets
# build.arduino.ldscript = nrf52840_s140_v7.ld; the framework builder looks for
# that name in <framework>/cores/nRF5/linker/. We ship the file in linker/ and
# copy it into place here so a fresh checkout (or a framework reinstall) builds
# cleanly without any manual step.
Import("env")  # noqa: F821  (injected by PlatformIO/SCons)

import os
import shutil

fw = env.PioPlatform().get_package_dir("framework-arduinoadafruitnrf52")
if fw:
    dst = os.path.join(fw, "cores", "nRF5", "linker", "nrf52840_s140_v7.ld")
    src = os.path.join(env.subst("$PROJECT_DIR"), "linker",
                       "nrf52840_s140_v7.ld")
    if os.path.exists(src) and not os.path.exists(dst):
        shutil.copyfile(src, dst)
        print("install_ldscript: installed nrf52840_s140_v7.ld into framework")
