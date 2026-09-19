# wlroots-polaris-hdr patches

Applied in order by `nix/packages/wlroots-polaris-hdr/default.nix`, which
`nix/packages/labwc-polaris-hdr/default.nix` builds labwc against in place
of nixpkgs' stock `wlroots_0_20`.

| # | File | Purpose | Drop when |
|---|------|---------|-----------|
| **01** | `01-headless-backend-accept-render-format-and-image-description.patch` | Adds `WLR_OUTPUT_STATE_RENDER_FORMAT` and `WLR_OUTPUT_STATE_IMAGE_DESCRIPTION` to the headless backend's `SUPPORTED_OUTPUT_STATE`, so `output_test()`/`output_commit()` in `backend/headless/output.c` accept those state fields instead of rejecting the whole commit | wlroots' headless backend accepts these state fields itself |

## Why this exists — and why it's required, not optional

This is the companion to `nix/patches/labwc/01-headless-hdr-colorimetry.patch`.
That patch alone is **not safe to ship without this one**: it makes
`wlr_output.supported_primaries`/`supported_transfer_functions` nonzero for
headless outputs, which changes labwc's behavior on *every* output
reconfiguration, not just HDR ones.

Trace: `configure_new_output()` in labwc's `src/output.c` calls
`output_state_setup_hdr(output, false)` for every output, headless included.
When HDR isn't actually requested/available, that function still ends by
calling `output_enable_hdr(output, &output->pending, /* enabled */ false, ...)`.
Its disable branch is:

```c
if (!enabled) {
    if (output->wlr_output->supported_primaries != 0 ||
            output->wlr_output->supported_transfer_functions != 0) {
        wlr_output_state_set_image_description(os, NULL);
    }
    return;
}
```

Before the labwc patch, `supported_primaries`/`supported_transfer_functions`
are always 0 for a headless output, so this branch never fires and
`WLR_OUTPUT_STATE_IMAGE_DESCRIPTION` never gets set. After the labwc patch,
that guard is true unconditionally, so **every** headless output
reconfiguration now sets `WLR_OUTPUT_STATE_IMAGE_DESCRIPTION` (to `NULL`,
disabling color management) on `output->pending`.

That state then gets committed via `lab_wlr_scene_output_commit()` right
after `configure_new_output()` adds the output to the layout. wlroots'
generic `wlr_output_test_state()` unlike `WLR_OUTPUT_STATE_MODE`/`ENABLED`
etc., has no "unchanged" fast path for `IMAGE_DESCRIPTION`
(`output_compare_state()` in `types/output/output.c` doesn't list it), so
the bit always reaches the backend's own `impl->test()`. The stock headless
backend's `output_test()` rejects any committed field outside
`SUPPORTED_OUTPUT_STATE`, which does not include `IMAGE_DESCRIPTION` (or
`RENDER_FORMAT`, needed for the HDR-*succeeds* path via
`output_set_render_format()`), so the whole commit — mode, enabled, buffer,
everything bundled into the same `wlr_output_state`, not just the color bits
— fails.

In short: applying the labwc patch by itself would silently break
`headless_stream` output configuration entirely (not just fail to get HDR),
the moment the compositor tries to reconfigure the headless output at all.
This patch closes that gap by teaching the headless backend to accept those
two state fields the same way `WLR_OUTPUT_STATE_MODE` already is — it has no
real hardware to reject them for.

## Checking the stack

```bash
scripts/check-nix-patches.py          # hunk headers, declarations — instant
```

Verified by hand: `patch -p1` applies cleanly (with the documented
leading-space-on-blank-context-line fuzz `scripts/check-nix-patches.py`'s
own docstring calls out) against the pinned `wlroots_0_20` `0.20.2` source,
and `nix build .#labwc-polaris-hdr` builds against the result.

## References

- wlroots `types/output/output.c` (`output_basic_test`, `output_compare_state`,
  `wlr_output_test_state`) — the generic layer that already validates
  `image_description` against `supported_primaries`/`supported_transfer_functions`
  once those are set; this patch only widens what the *headless* backend's
  own narrower `impl->test()` accepts.
- labwc `src/output.c` (`configure_new_output`, `output_state_setup_hdr`,
  `output_enable_hdr`, `output_set_render_format`).
