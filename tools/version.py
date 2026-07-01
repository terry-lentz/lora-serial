"""Inject the firmware version as a -D FW_VERSION build flag.

PlatformIO pre-build hook (wired in via `extra_scripts = pre:tools/version.py`
in platformio.ini). The firmware version is the single source of truth for
"which release is this board running", and it comes straight from git so a
tagged release build and the firmware it produces always agree:

  - clean tagged build      -> the tag exactly (e.g. v0.2.0)
  - commits past the tag     -> v0.2.0-3-gabc1234 (tag + commits-since + hash)
  - uncommitted changes      -> ...-dirty
  - CI without git metadata  -> the release tag env (GITHUB_REF_NAME)
  - no git at all            -> "dev" (the fw_config.h default also covers this)

The value is reported by ATI (fw=...), AT+VER, and the USB boot banner. Releases
are cut by pushing a semver tag (git tag v0.2.0 && git push --tags); this makes
the flashed firmware self-identify with that exact tag.
"""
import os
import subprocess

Import("env")  # noqa: F821 - injected by the PlatformIO/SCons build runtime


def firmware_version():
    """Version string from git, falling back to the CI tag env, then "dev"."""
    try:
        described = subprocess.check_output(
            ["git", "describe", "--tags", "--always", "--dirty"],
            stderr=subprocess.DEVNULL,
        ).decode().strip()
        if described:
            return described
    except Exception:
        pass
    return os.environ.get("GITHUB_REF_NAME", "").strip() or "dev"


env.Append(CPPDEFINES=[("FW_VERSION", env.StringifyMacro(firmware_version()))])
