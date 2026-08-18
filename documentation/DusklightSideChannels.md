# The `D3DMATERIAL9` side channels — the allocation table

`D3DRS_LIGHTING` is off in the Dusklight D3D9 backend, so nothing consumes a
D3D9 material and the whole `D3DMATERIAL9` struct is free for aurora to fill and
this fork to read. Several features do exactly that.

**This file is the allocation table, and it is checked by CI**
(`scripts/check_dusklight_invariants.py`). It is a file rather than a comment
because the map used to live in four places, and on 2026-08-05 a feature took
`Ambient.g`/`.b` and updated only some of them — leaving the canonical table
advertising both as spare, with nothing failing.

**What each row means, and the prose behind it, is aurora's**
`docs/dx9/remix-material-interface.md` §2. This one only records *that* a
channel is taken, in a form a script can read.

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
| `Power` | all three water facts packed: `tag * 100 + layer * 10 + role` | `rtx_dusklight_water.h` | 2026-08-11 |

## Spare — and closed to new features

**`Ambient.a` is the only field left, and you should not take it.** Since
2026-08-14 the transport for a new per-draw fact is `dusklightSetDrawMeta`, a
versioned export on this fork's own `d3d9.dll` fed by aurora's
`GX_AURORA_SET_DUSKLIGHT_DRAW_META` (`0x0058`), whose payload is a flags word —
**so a new per-draw fact is a new bit, not a new channel.** Add one to
`rtx_dusklight_drawmeta.h`, whose header comment (`:26-51`) argues the whole
thing: why not another channel, why the scarcity was never a real constraint,
the `getRtxOptionValue`/`GetProcAddress` precedent that makes it safe to add one
side at a time, and the size-versioning scheme for growing the struct.

Keep `Ambient.a` for something that genuinely has to ride the material struct
through the **capture** path. **And before claiming it, check the live `claude/*`
branches:** `claude/dusklight-remix-transparency-e7l766` has an unmerged claim on
it (a per-draw transparency class), and nothing automated can see an unmerged
branch.

**Take a channel only by adding its row above in the same commit.** That is what
puts the claim and the write in one diff, which is the only thing that reliably
stops the next feature taking a channel already in use.

### What the check enforces

`scripts/check_dusklight_invariants.py` runs **both** directions: a channel the
fork reads with no row here fails, and a row here that nothing reads fails. The
second is what a merge produces when a branch that forked before a channel
existed wins a conflict — which is what nearly happened to `Ambient.g`/`.b` on
2026-08-11, when the water branch would have reclaimed them. It also checks the
water packing formula against the one aurora states, because that is one contract
written in two repositories.

**What it cannot catch:** a channel that keeps being written with a *different*
meaning. Both directions pass and this table reads as true.

### Identity hash

**Every field listed above participates in
`LegacyMaterialData::computeIdentityHash` (`rtx_materials.cpp`), and that is not
optional.** Without it, two draws differing only in a side channel produce the
same hash, and `SceneManager`'s preserve path keeps handing an instance the
material it built from the other one — a stale material with no error anywhere.
`Power` was excluded on the stated grounds that nothing read it, right up until
water did, on a branch that never touched `rtx_materials.cpp` — so the comment
saying so stayed true-looking for a week. **Add the field to the hash in the same
commit that adds the row.**

## The rasterized path

`Ambient.g` and `Ambient.b` are the only channels read on the **rasterized** path
(`D3D9DeviceEx::BindTexture`) as well as the ray-traced one. Everything else here
is consumed during material resolution, which UI draws never reach — so a side
channel intended to affect the HUD has to be read in `BindTexture` too, not only
in `determineMaterialData`.
