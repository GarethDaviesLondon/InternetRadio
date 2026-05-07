"""
Wire ccache into the PlatformIO build. With ccache installed and on
PATH, this prepends `ccache` to the C and C++ compiler invocations,
which means identical translation-unit compiles are served from the
cache instead of being re-run by GCC.

Setup on Windows (one of):
  scoop:        scoop install ccache
  chocolatey:   choco install ccache
  manual:       download ccache.exe from https://ccache.dev and put it
                somewhere on PATH (e.g. %USERPROFILE%\\bin\\ccache.exe).

The cache lives at %CCACHE_DIR% if set, otherwise the user-default
(%LOCALAPPDATA%\\ccache on Windows). Set CCACHE_MAXSIZE if you want a
specific limit; ccache auto-evicts.

If ccache isn't installed the script prints a notice and exits cleanly,
so the build still works on machines without it.
"""

import shutil
import os
Import("env")

# Bail quickly if ccache isn't on PATH -- the build should still work.
ccache = shutil.which("ccache")
if not ccache:
    print("ccache: not on PATH, skipping. (install via scoop/choco/manual)")
    Return()

# Quote the existing compiler paths so spaces in the toolchain dir don't
# break the wrapped command. SCons handles list-form CC/CXX correctly,
# but the simplest portable form is "ccache <quoted compiler>".
def _wrap(name):
    cur = env.subst(f"${name}")
    if not cur:
        return None
    return f'"{ccache}" "{cur}"'

env.Replace(
    CC  = _wrap("CC")  or env["CC"],
    CXX = _wrap("CXX") or env["CXX"],
)

# Reasonable defaults if the user hasn't already set them.
os.environ.setdefault("CCACHE_SLOPPINESS", "pch_defines,time_macros,locale")
os.environ.setdefault("CCACHE_COMPRESS",   "true")

print(f"ccache: enabled via {ccache} (dir: {os.environ.get('CCACHE_DIR', '<default>')})")
