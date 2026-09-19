# labwc for Polaris — headless HDR capability bits (polaris#hdr-headless).
#
# nixpkgs' labwc (currently 0.20.1) already implements real HDR: the
# color-management-v1 protocol, BT.2020/PQ Vulkan rendering, and the
# <Hdr.core> config path. None of it ever runs for the wlroots *headless*
# backend, because wlr_headless_add_output() calloc()s its wlr_output and
# never sets supported_primaries/supported_transfer_functions — there is no
# EDID to read for a fake output. This is exactly the `headless_stream`
# runtime's output. See nix/patches/labwc/README.md for the full story.
#
# Unlike gamescope-polaris, this does not chase a newer upstream rev: the
# nixpkgs-pinned labwc already has everything this patch needs, so it is
# applied directly on top via overrideAttrs.
#
# Drop checklist: nix/patches/labwc/README.md (grep POLARIS-UPSTREAM-REMOVE).
{ labwc }:

labwc.overrideAttrs (old: {
  pname = "labwc-polaris-hdr";

  patches = (old.patches or [ ]) ++ [
    # POLARIS-UPSTREAM-REMOVE when wlroots' headless backend sets HDR
    # capability bits itself (e.g. from a caller-supplied descriptor).
    ../../patches/labwc/01-headless-hdr-colorimetry.patch
  ];
})
