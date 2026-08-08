# Reference implementations

Files here are **not built**. `meson.build` lists sources explicitly and
`scripts-common/compile_shaders.py` only picks up `.slang`, so nothing in this directory
reaches a compiler. They are kept in tree so the shader ports next door can be diffed against
their source of truth rather than against memory.

## `gt7_tone_mapping.cpp`

Polyphony Digital's sample implementation of the GT7 tone mapping operator, v1.0 (2025-08-10),
MIT licensed, presented at the SIGGRAPH 2025 shading course. Ported to Slang in
`../gt7.slangh`.

Provenance, since it matters here: the file was retrieved by the repository owner from
`https://blog.selfshadow.com/publications/s2025-shading-course/pdi/supplemental/gt7_tone_mapping.cpp`
and passed through an Apple Pages document, so it was treated as untrusted until verified. It
compiles clean under `g++ -std=c++17 -Wall` (with `std::powf`/`std::expf` mapped to their global
equivalents, which libstdc++ does not put in `std`) and its built-in test harness reproduces
self-consistent output across the SDR and 1000/4000/10000 nit HDR cases. The Slang port was
additionally validated against those numbers to a maximum deviation of 4e-3.

The only edit made to the retrieved text was stripping a single trailing `*` left by the
document round trip.
