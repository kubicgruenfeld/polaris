# labwc-polaris-hdr patches

Applied in order by `nix/packages/labwc-polaris-hdr/default.nix`.

Polaris HDR capture patch surface for the `headless_stream` runtime
(`labwc` + wlroots screencopy/ext-image-copy-capture, see
`docs/stream-paths.md`).

| # | File | Purpose | Drop when |
|---|------|---------|-----------|
| **01** | `01-headless-hdr-colorimetry.patch` | Sets `supported_primaries`/`supported_transfer_functions` (BT.2020 + ST2084 PQ) on wlroots headless outputs in `handle_new_output()`, so labwc's existing `output_supports_hdr()` / `<Hdr.core>` / `wlr_color_manager_v1` machinery — which only ever ran for real DRM outputs — also activates for the fake output Polaris streams from | wlroots' headless backend (`backend/headless/output.c`) populates these bitfields itself, e.g. from a caller-supplied capability descriptor |

## Why this exists

wlroots 0.20 already implements full HDR: the `wlr_color_management_v1`
protocol, BT.2020/PQ support in the Vulkan renderer, and DRM-backend EDID
parsing that flips `wlr_output.supported_primaries` /
`supported_transfer_functions` from the connector's real capabilities
(`backend/drm/util.c`). labwc's own HDR enable path
(`output_state_setup_hdr()` in `src/output.c`) is real and already wired to
all of that.

None of it runs on a headless output: `wlr_headless_add_output()`
(`backend/headless/output.c`) `calloc()`s the `wlr_output` and never touches
either bitfield, because there is no EDID to read for a display that does
not physically exist. `output_supports_hdr()` then evaluates
`0 & WLR_COLOR_NAMED_PRIMARIES_BT2020` as false unconditionally, and HDR
silently refuses to enable no matter what `rc.xml` says.

This is exactly the situation `headless_stream` is in: there is no real
monitor to probe, but the display *is* known — it's the one Polaris streams
to. Setting the capability bits in `handle_new_output()`, gated on
`wlr_output_is_headless()` (a pattern labwc already uses elsewhere, see
`src/output-virtual.c`), is the smallest change that lets the rest of
labwc's existing HDR machinery run unmodified.

## Required companion patch

Do not ship this patch without `nix/patches/wlroots/01-headless-backend-accept-render-format-and-image-description.patch`
(applied via `wlroots-polaris-hdr`, which `labwc-polaris-hdr` builds
against instead of stock `wlroots_0_20`). Setting `supported_primaries`/
`supported_transfer_functions` changes what labwc tries to commit on
*every* headless output reconfiguration, not only HDR ones — without the
wlroots patch, the stock headless backend rejects that commit outright and
breaks `headless_stream` output configuration entirely. See
`nix/patches/wlroots/README.md` for the full trace.

## What this patch does **not** do

It only unlocks compositor-side HDR (10-bit render format selection +
`wlr_color_manager_v1` advertising BT.2020/PQ to Wayland clients). It does
not touch:

- Requesting `Hdr.core` in the `rc.xml` Polaris generates for the private
  labwc instance (`src/platform/linux/stream_runtime_labwc.cpp` and
  friends). `LAB_RENDER_BIT_DEPTH_DEFAULT` does **not** auto-upgrade to
  10-bit: `output_state_setup_hdr()` only derives the target bit depth from
  the output's *current* `render_format` when depth is unset, and a fresh
  output starts 8-bit. `<Hdr.core>yes</Hdr.core>` (or equivalent) has to be
  requested explicitly for anything to attempt 10-bit at all.
- Forcing the Vulkan renderer (`WLR_RENDERER=vulkan`) for the private
  compositor process — wlroots' color-management implementation is
  Vulkan-only in 0.20.x (`render/vulkan/`, not `render/gles2/`).
- Capture-side changes in Polaris (`src/platform/linux/wayland.cpp`,
  `wlgrab_pixel_copy.h`, `wlgrab.cpp`), which still only negotiate 8bpc
  formats end to end and need a 10-bit path before any of this reaches an
  encoder.
- `wl::wlr_t::is_hdr()` / `get_hdr_metadata()`
  (`src/platform/linux/wlgrab.cpp`), which still unconditionally report no
  HDR regardless of what the compositor now supports.

## Checking the stack

```bash
scripts/check-nix-patches.py          # hunk headers, declarations — instant
```

`labwc-polaris-hdr` does not pin its own `fetchFromGitHub` source (it
patches nixpkgs' `labwc` derivation via `overrideAttrs`, currently pinned to
`0.20.1` / `refs/tags/0.20.1`), so `--apply` has nothing package-local to
fetch this patch against; `scripts/check-nix-patches.py` skips source
verification for it the same way it does for `polaris-stream`. Re-check the
hunk context by hand against whatever `labwc` version the pinned `nixpkgs`
input moves to.

## References

- wlroots `backend/drm/util.c` (`update_edid_colorimetry_info`, or
  equivalent) — the DRM-backend counterpart this patch mirrors for the
  headless backend.
- labwc `src/output.c` (`output_supports_hdr`, `output_state_setup_hdr`,
  `output_enable_hdr`) and `src/output-virtual.c`
  (`wlr_output_is_headless()` precedent).
