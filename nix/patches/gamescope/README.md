# gamescope-polaris patches

Applied in order by `nix/packages/gamescope-polaris/default.nix`.

Polaris HDR capture patch surface for gamescope PipeWire streaming.

| # | File | Purpose | Drop when |
|---|------|---------|-----------|
| **10** | `10-pipewire-offer-10-bit-BT2020-PQ.patch` | Offer SPA 10-bit BT.2020/PQ formats; paint switches on negotiated format | Upstream HDR PW formats land |
| **11** | `11-pipewire-composite-cursor.patch` | Optional `--pipewire-composite-cursor` | Upstream cursor composite lands |
| **12** | `12-polaris-stamp-version-polhdrN.patch` | Banner `+polhdr2` capability stamp | Functional patches upstream; version floors suffice |
| 02 | `02-headless-hdr-colorimetry.patch` | Headless real SDR vs HDR EDID/expose | Proven redundant with 10 on headless |
| 03 | `03-pipewire-prefer-dmabuf.patch` | Advertise DmaBuf\|MemFd\|MemPtr | Portal path no longer needs multi-type |
| **13** | `13-accept-wlroots-color-management.patch` | Accept a wlroots compositor's colour management so a nested gamescope can offer HDR to the game it hosts | gamescope stops demanding features its HDR10 path never uses |
| **06** | `06-prefer-discrete-gpu-2217.patch` | Headless prefers discrete GPU if unpinned | **[#2217](https://github.com/ValveSoftware/gamescope/pull/2217)** merges |

## Superseded (kept in `archive/`)

Retired patches move to `archive/`; they are history, not candidates. A patch
sitting next to the live ones without being applied reads as live, so
`scripts/check-nix-patches.py` treats that as an error.

| Old | Why gone |
|-----|----------|
| `01-pipewire-xbgr-210le-2270.patch` | Superseded by **10** (full 10-bit PQ offer + paint) |
| `04-pipewire-color-mgmt.patch` | Folded into **10** paint path |
| `07-paint-pipewire-eotf-pq.patch` | Folded into **10** |

## Checking the stack

```bash
scripts/check-nix-patches.py          # hunk headers, declarations — instant
scripts/check-nix-patches.py --apply  # + apply to the pinned gamescope rev
```

The order above is the order `default.nix` declares, and the order matters:
`patchPhase` stops at the first failure, so a patch that does not apply takes
every patch after it with it. `patch -p1` is what the check and the build both
use — `git apply` refuses the line offsets a drifted upstream produces.

## Capability stamp

```text
+polhdr1  10-bit BT.2020/PQ capture formats
+polhdr2  …and --pipewire-composite-cursor
```

```bash
gamescope --version   # expect +polhdr2
rg -n 'POLARIS-UPSTREAM-REMOVE|polhdr' nix/patches/gamescope/
```

## References

- ValveSoftware/gamescope#2270, #2217, #2126

## 13 — why gamescope refused HDR under labwc

`CWaylandBackend::SupportsColorManagement()` accepts either the legacy frog
protocol or `wp_color_manager_v1` with `bSupportsGamescopeColorManagement`.
wlroots implements the standard protocol and not frog, so everything rested on
that flag — and it demanded six features:

```
PARAMETRIC, SET_PRIMARIES, SET_MASTERING_DISPLAY_PRIMARIES,
EXTENDED_TARGET_VOLUME, SET_LUMINANCES, WINDOWS_SCRGB
```

The HDR10 path uses none of the middle four. It builds its image description
from `set_primaries_named(BT2020)` + `set_tf_named(ST2084_PQ)`, both of which
wlroots implements. Worse, wlroots *asserts* that a compositor cannot advertise
`set_primaries`, `set_tf_power`, `set_luminances`, `extended_target_volume` or
`windows_scrgb` (`wlr_color_manager_v1_create()`), and its handlers post
`"not supported"` protocol errors — so no wlroots compositor can ever satisfy
the gate, and labwc could not be patched into satisfying it.

Measured on an RTX 4070 Ti Super with labwc driving a BT.2020/PQ headless
output: gamescope received the PQ image description correctly
(`uMaxLum: 10000, uRefLum: 203`, against wlroots' sRGB default of 80/80) and
still reported `bExposeHDRSupport: false`, because the gate had already failed
and the assignment was skipped. Everything it needed had arrived.

So the gate is relaxed to what HDR10 actually uses, and the two call sites that
genuinely need the other features are guarded instead:

- the scRGB image description is only created when `WINDOWS_SCRGB` is
  advertised. It is dead weight otherwise: `GetWPImageDescription()` has no
  callers, and surfaces are tagged from `m_pCurrentImageDescription`, built per
  commit.
- the explicit `set_primaries` request, used only for gamut expansion when
  `wayland_hdr10_saturation_scale != 1.0` (default 1.0), falls back to the named
  primaries when the compositor does not advertise `SET_PRIMARIES`. Sending it
  regardless would be a protocol error, which kills gamescope.

The cost is that gamescope has no scRGB colourspace against such a compositor.
HDR10 is unaffected, which is what a DX11/DX12 title under Proton uses.
