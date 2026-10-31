# Brand assets

Vector sources for the Quilla mark. The geometry is the U-shaped cross-section
of a hull: a waterline cuts across it, one bilge keel runs off each side, and a
single keel node sits at the bottom centre — the node is the signature-
verification gate (unverified firmware is not admitted).

The design is symmetric and square-composed, so it survives an app-icon crop,
and "U + waterline + bilge keels + node" is a specific combination rather than
a generic geometric shape.

| File | Size | Use |
| --- | --- | --- |
| `quilla-mark.svg` | 512 × 512, transparent | README header, slides, any place that needs the mark alone |
| `quilla-logo.svg` | 720 × 160, transparent | Light backgrounds: site header, print, e-mail signature |
| `quilla-logo-dark.svg` | 720 × 160, transparent | Dark backgrounds (same lockup, light wordmark) |
| `quilla-mark-512.png` | 512 × 512, dark plate | Square avatar use — GitHub organisation / user avatar |
| `quilla-social-preview.png` | 1280 × 630 | GitHub repository social preview (link cards on social platforms) |
| `quilla-favicon.svg` | 64 × 64, dark disc | Favicon / small icon; bilge keels dropped and the frame thickened so it stays legible down to 16 px |

`quilla-demo.gif` next to these files is the flashing demo used by the README.

## Sizing rules

| Context | Use |
| --- | --- |
| ≥ 32 px | `quilla-mark.svg` (full mark, bilge keels included) |
| ≤ 24 px | `quilla-favicon.svg` (reduced mark, no bilge keels, heavier frame) |
| Clear space | ≥ 1/4 of the mark width on all four sides |
| Minimum size | full mark ≥ 32 px on screen / 8 mm in print |

Do not stretch, rotate, add strokes, shadows or 3-D effects, or recolour to a
different hue family.

## Where the two rasterised assets are used

GitHub does not read either of these from the repository — both are uploaded
once through the web UI by a maintainer:

1. **Organisation avatar** — organisation *Settings → Profile picture*, upload
   `quilla-mark-512.png`. The mark is inset so it survives GitHub's circular crop.
2. **Repository social preview** — repository *Settings → Social preview*,
   upload `quilla-social-preview.png` (1280 × 630; GitHub recommends 640 × 320
   minimum, this asset is 2× that for high-DPI link cards).

The README uses `quilla-mark.svg` directly, so it tracks the vector source and
stays crisp on any display.

## Colour

The mark uses one gradient across all variants:

| Stop | Value |
| --- | --- |
| 0 | `#34d399` |
| 0.55 | `#22d3ee` |
| 1 | `#8b5cf6` |

Single-colour reproduction (silkscreen, laser etching, one-colour print):
render the mark in `#0b1220` on light stock, or `#eaf4ff` on dark stock. Do not
recolour the gradient to a different hue family.

The wordmark is set in the system UI sans (Segoe UI / Helvetica Neue / Inter,
with Arial fallback) so the lockup stays dependency-free. For typeset material
where exact metrics matter, set the wordmark in the application's own typeface
and keep the mark as supplied.

## Licence

The Quilla code is distributed under the 3-clause BSD licence (see `LICENSE.md`),
which covers the source in this repository. The name **Quilla**, the wordmark and
the mark above identify the project; the source licence does not grant permission
to use them as the branding of a derivative product or service.
