# Fork rebase notes

This fork carries generic Remix enhancements on top of upstream `dxvk-remix`, and it is meant to be
rebased onto upstream regularly. Everything here exists to keep that rebase from turning into a merge
session.

## The rule

**Substance goes in new files. Upstream files get hooks, not code.**

A new file cannot conflict with anything — upstream has never heard of it. A modified line in an
upstream file conflicts the moment upstream touches the same region. So each feature lives in its own
`.cpp`/`.h` pair, and the upstream files it has to reach into get the smallest possible call into that
pair.

Practical form of the rule:

- **Insert, never modify.** A hunk that only adds lines survives a rebase whenever the lines around it
  survive. A hunk that rewrites an existing line conflicts as soon as upstream rewrites that same
  line — and, worse, `git` will happily take one side and leave the feature silently half-applied.
  In particular: do not change an existing function signature to thread a new argument through. Have
  the callee read what it needs, or add a second entry point.
- **Bind behaviour to the thing it belongs to, not to a call site.** A hook that has to be called in
  the right place is a hook a future upstream commit can forget: add a caller of the function you
  hooked *next to* and the new path silently runs without it, with no conflict to warn anyone. Put the
  behaviour inside the operation it modifies.
- **Do not restate an existing condition.** Hang new UI and new behaviour inside the branch that
  already decides the thing, rather than writing a fresh `if` that says the same thing in different
  words. A second copy drifts away from the first during a rebase without any conflict to warn you.
- **Additive defaults.** Every new option defaults to the value that reproduces upstream behaviour
  exactly, so a config that never mentions the option renders identically.
- **Never hand-edit `RtxOptions.md`; regenerate it.** The repo-root `RtxOptions.md` is generated, and
  `tests/rtx/unit/test_documentation.cpp` diffs it line by line against what the runtime's
  `writeMarkdownDocumentation` emits, so a new `RTX_OPTION` that is missing from it fails the default
  test suite. It is also the most rebase-hostile file in the tree — roughly one upstream commit in
  fifteen regenerates the whole thing. Do not merge it by hand: on a conflict take upstream's copy
  wholesale (`git checkout --theirs RtxOptions.md`) and regenerate by running Remix once with
  `DXVK_DOCUMENTATION_WRITE_RTX_OPTIONS_MD=1`. The generator is the only correct author of that file,
  and it reads the descriptions straight out of the `RTX_OPTION` declarations, which live in this
  fork's own files.
- **One line in `meson.build` per new file.** The `dxvk_src` list is alphabetical, so a new entry lands
  next to its own name rather than at a hot spot. Do not normalise trailing whitespace on the lines
  around it — that turns a pure insertion into a permanent modified-line conflict.

Where a change genuinely cannot be additive, take the conflict knowingly and record it here so the next
rebase knows what to look for. There are two such places today:

- **`.github/workflows/build.yml` trigger lists.** Adding a branch to a YAML inline list rewrites the
  line. It is deliberate: this is CI policy the fork owns, and if upstream changes its own triggers we
  want the conflict rather than a silent merge. The fork's version builds `claude/**` and
  `remix-auto-ver` on push, so a branch is compiled as soon as it is pushed instead of only once a
  pull request is opened against `main`.
- **The USD packaging, below.** That one is temporary and must be deleted rather than merged.

## USD packaging — DROP THIS, DO NOT MERGE IT

Upstream `673dc2a` moved USD onto `open_usd@25.11+...-nopython`. That package is on neither public
packman remote, so packman falls through to its third remote, NVGTL, and stops with:

```
packman(ERROR): The environment variable 'NVM_GTLAPI_TOKEN' must be set to use NVGTL
```

which is an NVIDIA-internal credential. Dependency fetch is the first thing meson does, so **every**
build of this fork failed before a single file was compiled, whatever the branch contained.

The fork therefore repoints USD at `usd.py311.windows-x86_64.stock.{release,debug}@0.25.11-gl.18041+...`,
which a public runner can fetch, keeping USD at 25.11. Verified by probing both against a public
runner: the `open_usd` packages come back blocked, the stock ones and `python@3.11.10+nv1` come back
public. The `-gl.` in that version string looks internal and is not.

Because the stock build is Python-enabled, pxr's headers reach for `pyconfig.h`, so the Python include
and link paths `673dc2a` removed have to come back. **Three subprojects fetch USD independently and
each needs them** — miss one and it fails on its own:

| Subproject | Its manifest | Where its paths live |
| :-- | :-- | :-- |
| root build | `packman-external.xml` | `external-build/meson.build`, `meson.build` |
| schema plugins | `src/usd-plugins/packman-external.xml` | `src/usd-plugins/meson.build` |
| Hydra test renderer | `tests/rtx/apps/HydraTestRender/packman-external.xml` | `tests/rtx/apps/HydraTestRender/meson.build` |

**Delete this the moment `open_usd` is published on a public remote** — re-check by pulling those two
package names on a runner. It is not a fix to keep and merge forward; it is a hold against an upstream
packaging change that public forks cannot consume. It touches files upstream actively edits, so it will
conflict on rebase, and that conflict is the reminder to check whether it can go.

Only the packaging half of `673dc2a` is reversed. Its GetRenderStats warmup API, sdrMdl plugin and
schema/lssusd rework are all still here, because none of them depends on how USD was packaged.

## Marking the hooks

Every line this fork inserts into an upstream source file carries a `// FORK: <feature>` marker, so the
complete set of hooks is one grep away and a rebase can be audited by counting:

```
git grep -c "FORK: dlss-render-preset"
```

If a count drops after a rebase, or a file in the feature's table below stops appearing, a hunk was
dropped or a conflict was resolved the wrong way. New files carry no marker — the whole file is the
fork's — and neither do `meson.build` entries, which name the new file already.

## DLSS render presets

Remix exposes the DLSS quality mode but not the render preset — the network itself — so without this
the choice is NGX's, and revisable over the air. Two options, each defaulting to `Default`, so nothing
moves until someone chooses:

| Option | Feature | Values |
| :-- | :-- | :-- |
| `rtx.dlss.renderPreset` | DLSS super resolution | E / J / K / L / M |
| `rtx.rayreconstruction.renderPresetOverride` | DLSS Ray Reconstruction | D / E / F |

Everything the feature owns is in `src/dxvk/rtx_render/rtx_dlss_render_preset.{h,cpp}`: both enums,
both options, both dropdowns, and the one predicate the greyed-out controls ask. The upstream files
below carry **inserted lines only — no line of upstream code is modified or deleted.**

| File | Hook |
| :-- | :-- |
| `rtx_dlss.h` | includes the new header; one `mPrevRenderPreset` member |
| `rtx_dlss.cpp` | two lines in `dispatch`, so the feature is rebuilt when the preset moves |
| `rtx_ngx_wrapper.cpp` | inside `NGXDLSSContext::initialize`, the five `DLSS.Hint.Render.Preset.*` params. **Written unconditionally, including for 0** — `m_parameters` is created once per context and outlives every feature, so a skipped write leaves the previous preset in place and `Default` stops working. The option is read here rather than passed in, which is what keeps the hint bound to feature creation: `NGXDLSSContext::initialize`'s signature is untouched, and a future upstream caller of it cannot get a feature carrying somebody else's preset |
| `rtx_ray_reconstruction.h` | one `m_prevRenderPresetOverride` member |
| `rtx_ray_reconstruction.cpp` | two lines in `dispatch`; one `if` after the existing `dlssdModel` ternary; `BeginDisabled`/`EndDisabled` around the existing Transformer Model D checkbox |
| `dxvk_imgui.cpp` | includes the new header; one `showDlssRenderPresetCombo()` call in each of the two existing DLSS branches; `BeginDisabled`/`EndDisabled` around the existing Ray Reconstruction Model combo |
| `rtx_user_menu.cpp` | one `showDlssRenderPresetCombo()` call in the `UpscalerType::DLSS` case |
| `RtxOptions.md` | the two generated rows for the new options. Regenerate rather than merge — see the rule above |
| `meson.build` | the two `dxvk_src` entries for the new files |

`rtx_ngx_wrapper.h` is deliberately not touched.

Two things a rebase must not "tidy":

**The preset values are numbers, not `NVSDK_NGX_*_Hint_Render_Preset_*` enumerators.** The preset is
resolved by the installed DLSS runtime, not by the header we compile against, so a runtime newer than
our SDK can honour a preset its header calls unused — which is exactly how Ray Reconstruction preset F
is reachable. And the NGX SDK is not in this tree; packman fetches it (`ngx_sdk_dldn`,
`packman-external.xml`), so an enumerator the pinned copy lacks is a Windows build failure invisible
from a Linux checkout. The pinned copy is provably older than NVIDIA's published header:
`rtx_ray_reconstruction.cpp` still compiles `NVSDK_NGX_RayReconstruction_Hint_Render_Preset_A`, which
the published header removed.

**The Ray Reconstruction override supersedes two existing controls.** Ray Reconstruction Model
(CNN/Transformer) and Transformer Model D are only a two-bit spelling of the same preset choice, so
while an override is set they do nothing, and both are greyed out. `Default` keeps the old ternary
exactly as it was. If upstream ever gives those two settings a second job, the greying-out is the part
that needs revisiting.

### Behaviour worth knowing before reporting a bug

- **Both options carry `RtxOptionFlags::UserSetting`**, because both dropdowns are drawn in the
  simplified player menu, and that menu's Save Settings button saves only the user layer. Without the
  flag an edit lands in `rtx.conf`, the button stays dark, and the player's choice is gone at the next
  launch. Every control beside them there — `upscalerType`, `enableRayReconstruction`, `qualityDLSS`,
  `rtx.rayreconstruction.model` — carries the same flag.
- **The grey-out lags by one frame.** Option writes from ImGui widgets are deferred to the end of the
  frame, and the Ray Reconstruction Model combo is drawn *before* the render preset dropdown in both
  menus, so choosing a non-`Default` preset greys the model combo on the next frame, not the current
  one. Same for the Transformer Model D checkbox.
- **The player menu greys the whole Upscaling block unless DLSS Preset is Custom.** The render preset
  dropdown is inside that block, so a player has to pick Custom before it is reachable. That is
  upstream's behaviour for every control in the block, not something this feature added.
- **Only the Ray Reconstruction Model combo gets the "Overridden by the Ray Reconstruction Render
  Preset." line.** The Transformer Model D checkbox is greyed with no explanation, because it sits
  under Denoising in the developer overlay where the preset dropdown is not on screen to point at.
