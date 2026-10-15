# tools/ — the reverse-engineering bench

All scripts here are PSCapture's own tooling (no Sony code). They are the
instruments used to produce the findings in `docs/REVERSE_ENGINEERING.md`.

- `dumplib/`      payload: extracts the system libraries from YOUR console
- `sprx.py`       sprx (ELF) inspection/extraction
- `callsites.py`  find RIP-relative call sites to an address
- `scan_refs.py`  scan a disassembly for references to a target
- `whoimports.py`, `stubmap.py`  import/stub mapping across libraries
- `p010png.py`, `nv12png.py`     deterministic renderers of captured frames
- `flat.py`, `asmwin.py`        misc binary helpers

Usage examples are in the headers of each file.
