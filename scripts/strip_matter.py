"""
Strip esp-matter / connectedhomeip include paths from the PlatformIO build.

Background:
  pioarduino's recent framework packages bundle esp-matter (Matter /
  connectedhomeip) headers under
      framework-arduinoespressif32-libs/esp32s3/include/espressif__esp_matter/
          connectedhomeip/connectedhomeip/src/platform/ESP32/nimble/
  and Arduino's newlib/platform_include/pthread.h does
      #include_next <pthread.h>
  That `#include_next` then lands in the Matter shim's pthread.h. On
  Windows the path is long enough (195+ chars, doubled with the nested
  `connectedhomeip/connectedhomeip` dir) that the compiler fails with
  `Invalid argument` even with the LongPathsEnabled registry flag set,
  because the xtensa GCC binary doesn't declare long-path awareness in
  its manifest.

  We don't use Matter, so removing those include directories from the
  build's CPPPATH (and any explicit -I flags) is safe.

  If you're on Linux/macOS this script is still a no-op unless esp-matter
  headers actually appear in CPPPATH, so it's fine to leave enabled on
  every platform.
"""

Import("env")  # type: ignore  # PlatformIO injects this at script time


def _is_matter(item) -> bool:
    s = str(item).lower()
    return "esp_matter" in s or "connectedhomeip" in s or "esp-matter" in s


def _filter_list(items):
    return [i for i in items if not _is_matter(i)]


# Main: CPPPATH is the canonical list of include directories.
env.Replace(CPPPATH=_filter_list(env.get("CPPPATH", [])))

# Defensive: some frameworks push -I flags directly into CCFLAGS /
# CXXFLAGS instead of CPPPATH. Strip "-Ixxx" entries (and the two-token
# "-I xxx" form) that reference Matter.
def _filter_flag_list(flags):
    out = []
    skip = False
    for f in flags:
        if skip:
            skip = False
            continue
        s = str(f)
        if s == "-I":
            # Two-token form: next entry is the path. Drop both if Matter.
            skip = False
            out.append(f)
            continue
        if s.startswith("-I") and _is_matter(s[2:]):
            continue
        out.append(f)
    return out


for key in ("CCFLAGS", "CXXFLAGS", "CPPFLAGS", "ASFLAGS"):
    env.Replace(**{key: _filter_flag_list(env.get(key, []))})

# Report how many paths we removed so build log makes this visible.
print("strip_matter: filtered esp-matter / connectedhomeip from include path")
