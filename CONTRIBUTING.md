# Contributing to WinSquish

## Build and test

```bat
build.bat       # produces build\winsquish.exe
```

Requires Visual Studio with the C++ workload; the script finds it via
`vswhere` automatically. No dependencies beyond the Win32 SDK — the SQUISH
library is vendored in `squish\` and statically linked.

There is no automated test suite yet. Before sending a change, exercise the
affected paths by hand at minimum:

- compress a file and extract it back, confirming the result is
  byte-identical (extraction is checksum-verified by libsquish);
- if you touched the worker/progress code, run it on a file large enough to
  see the progress bar move (SQUISH runs at ~0.5–0.7 MB/s per core);
- if you touched registration, run `--register`, check the entries appear in
  Explorer's right-click menu (any file, and a `.sq` file), then
  `--unregister` and confirm they're gone.

## Vendored libsquish

`squish\squish.c` and `squish\squish.h` are copied unmodified from
[paigejulianne/squish](https://github.com/paigejulianne/squish). **Don't
patch them here** — fix bugs upstream and re-vendor. WinSquish-side
workarounds belong in `src\winsquish.cpp`.

## Code style

- Match the existing style in the file you're editing (brace placement,
  naming, comment style) rather than introducing a new one.
- Keep the build clean at `/W4`.
- Pure Win32 only: no MFC/ATL, no third-party libraries.
- All UI code is Unicode (`W` APIs); paths handed to libsquish's `char*`
  file helpers must be UTF-8 (the app manifest sets the active code page).
- Registry writes stay under `HKCU\Software\Classes` — nothing that needs
  elevation.

## Copyright headers

New source files should carry the same GPLv3 header used in the existing
files (see `src\winsquish.cpp` for the exact text).

## Submitting changes

1. Fork the repo and create a branch off `main`.
2. Make your change and verify it as described above.
3. Ensure `build.bat` succeeds with no new warnings.
4. Open a pull request describing the change.

By contributing, you agree your contributions are licensed under the
project's [GPLv3 license](LICENSE).
