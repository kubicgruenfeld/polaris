# wlroots for Polaris — let the headless backend commit HDR output state.
#
# Companion to labwc-polaris-hdr: patching labwc alone to advertise HDR
# capability on headless outputs is not safe on its own, because it changes
# what labwc tries to commit on *every* headless output reconfiguration, not
# only HDR ones — see nix/patches/wlroots/README.md for the full trace. This
# patch widens backend/headless/output.c's SUPPORTED_OUTPUT_STATE so those
# commits are accepted instead of silently failing.
#
# Drop checklist: nix/patches/wlroots/README.md (grep POLARIS-UPSTREAM-REMOVE).
{ wlroots_0_20 }:

wlroots_0_20.overrideAttrs (old: {
  pname = "wlroots-polaris-hdr";

  patches = (old.patches or [ ]) ++ [
    # POLARIS-UPSTREAM-REMOVE when the headless backend accepts
    # WLR_OUTPUT_STATE_RENDER_FORMAT / WLR_OUTPUT_STATE_IMAGE_DESCRIPTION
    # itself.
    ../../patches/wlroots/01-headless-backend-accept-render-format-and-image-description.patch
  ];
})
