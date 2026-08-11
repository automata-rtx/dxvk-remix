# The `D3DMATERIAL9` side channels — the allocation table

`D3DRS_LIGHTING` is off in the Dusklight D3D9 backend, so nothing consumes a
D3D9 material and the whole `D3DMATERIAL9` struct is free for aurora to fill and
this fork to read. Several features do exactly that.

**This file is the allocation table, and it is checked by CI**
(`scripts/check_dusklight_invariants.py`). Every channel the fork reads must
have a row here; a channel with no row fails the build.

**Why it is a file rather than a comment.** The map previously lived in four
places — aurora's `remix-material-interface.md` §2, a comment in
`rtx_dusklight_emissive.h`, aurora's `dx9_internal.hpp`, and this fork's
`CLAUDE.md`. On 2026-08-05 a feature took `Ambient.g` and `Ambient.b` and
updated some of them, leaving the canonical §2 table still advertising those two
as spare. Nothing failed; the next feature to want a side channel would simply
have taken one that was already in use, and the collision would have surfaced as
a material bug nowhere near either change.

**What each row means, and the prose behind it, is aurora's**
`docs/dx9/remix-material-interface.md` §2. That document explains *why* each
channel carries what it carries. This one only records *that* it is taken, in a
form a script can read.

## Allocated

| Channel | Carries | Read in | Since |
| :-- | :-- | :-- | :-- |
| `Specular.r` | the vertex stream is authored material colour, not baked lighting | `d3d9_rtx_utils.cpp` | 2026-08-04 |
| `Specular.g` | aurora evaluated a presentable colour for this draw | `rtx_dusklight_emissive.h` | 2026-08-05 |
| `Specular.b` | this material has a colour of its own | `rtx_dusklight_emissive.h` | 2026-08-05 |
| `Specular.a` | this material is self-lit | `rtx_dusklight_emissive.h` | 2026-08-04 |
| `Emissive.r` `Emissive.g` `Emissive.b` | the colour the surface presents | `rtx_instance_manager.cpp` | 2026-08-04 |
| `Emissive.a` | the self-illumination evidence score — **reported, not used** | — | 2026-08-04 |
| `Diffuse.r` `Diffuse.g` `Diffuse.b` | the ramp endpoint TFACTOR does not hold | `rtx_instance_manager.cpp` | 2026-08-04 |
| `Diffuse.a` | this material is a two-colour ramp | `rtx_instance_manager.cpp` | 2026-08-04 |
| `Ambient.r` | which ramp endpoint TFACTOR holds | `rtx_dusklight_emissive.h` | 2026-08-04 |
| `Ambient.g` | 1-based HD texture replacement index, 0 for none | `rtx_dusklight_texrep.cpp` | 2026-08-05 |
| `Ambient.b` | the D3D9 stage `Ambient.g` refers to | `rtx_dusklight_texrep.cpp` | 2026-08-05 |

## Spare

`Ambient.a`, and `Power` — **and both are already claimed by branches that have
not merged. Audited 2026-08-11: there is nothing genuinely free.**

| Nominally spare | Claimed by | Also claimed by |
| :-- | :-- | :-- |
| `Ambient.a` | `claude/water-rendering-investigation-7baezw` (MAxx water tag) | `claude/dusklight-remix-transparency-e7l766` (draw class) |
| `Power` | `claude/water-rendering-investigation-7baezw` (water layer) | — |

`Ambient.a` is claimed **twice over**, by two branches neither of which can see
the other. A feature that wants a side channel now has to pack into an existing
one or find another transport — and should say which before it is written.

**`Ambient.g` and `Ambient.b` are also contested.** The water branch forked
before 2026-08-05, so its `set_remix_material` has no `texRepIndex`/`texRepStage`
parameters and writes water flags into both channels; on this side of the fork
that branch does not merely reclaim them, it is **missing
`rtx_dusklight_texrep.{h,cpp}` from its tree entirely.** Its merge conflicts in
aurora's `dx9_internal.hpp`, and the obvious resolution — take the newer,
self-consistent, well-commented side — deletes HD texture packs from both repos
without either the conflict or the invariants scripts naming them.

Full audit, including the GX FIFO subcommand space (where **four** live branches
have each taken `0x0053`) and the recommended merge procedure for the water
branch: `aurora-ao/docs/dx9/in-flight-allocation.md`.

**Take a channel only by adding its row above in the same commit** — the CI
check enforces the reverse direction (a read with no row), but nothing can
enforce that two branches do not take the same spare channel simultaneously. If
you are adding a side channel while another branch is in flight, say so; this
file is where that collision is visible.

## The rasterized path

`Ambient.g` and `Ambient.b` are the only channels read on the **rasterized**
path (`D3D9DeviceEx::BindTexture`) as well as the ray-traced one. Everything
else here is consumed during material resolution, which UI draws never reach —
so a side channel intended to affect the HUD has to be read in `BindTexture`
too, not only in `determineMaterialData`.
