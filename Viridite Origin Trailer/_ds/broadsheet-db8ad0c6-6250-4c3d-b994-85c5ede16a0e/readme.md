# Broadsheet design system

Broadsheet is newsprint set for the web: near-black Source Serif 4 on paper white, with the process accents — cyan and magenta, completed by a print yellow in the press treatments — used small and deliberately, like spot color. Page structure uses no boxes and no dividers between sections; hierarchy comes from the serif scale and from negative space alone. The one place rules print is as front-page furniture — the landing's thick-thin head pair around its dateline rail, in full-strength ink. Images print as their own misregistered process plates (interface imagery can take the simpler halftone dot screen).

## How to use this

- Link the one stylesheet from every page — `<link rel="stylesheet" href="styles.css">` (adjust the relative path) — and take every color, font, spacing, radius and shadow from its variables (`var(--color-*)`, `var(--font-*)`, `var(--space-*)`, `var(--radius-*)`, `var(--shadow-*)`). Never hard-code a hex, a font name or a px value the tokens already carry.
- Build with the classes below rather than inventing parallel ones; the component pages are plain HTML, so view source and copy the markup.
- `templates/` holds starting points a consuming project can copy whole.
- The whole system was derived from `theme.json`. To change the look, edit the tokens at the top of `styles.css` — every page, the thumbnail and this guide read from them — and keep `theme.json` and the written guidance in step so they don't drift from what the CSS actually does.

## Direction

Left-aligned, asymmetric layouts. Flush-left headings; content hugs the left edge with whitespace on the right. Photographs take the print treatments: `.cmyk` prints an image as its four misregistered process plates (the showcase treatment — decks use it throughout), and `.halftone` is the simpler newsprint dot-screen for interface imagery. In decks, section dividers are the press sheet's proof margin — the page stays paper (this system shows no dark surfaces) and the break is carried by press furniture: a field of misregistered process halftone screens off the right of the sheet, the `.cmyk-num` plate numeral, the ten-step black-and-white wedge, the eight-patch color scale and a star target. Most slides anchor their first ink to one shared top-left spot, and true registration targets sit at the sheet edges.

## Color

A light ground (`--color-bg` #f3f2f2) with `--color-text` #201e1d and two accents — `--color-accent` #0088b0 and `--color-accent-2` #d6006c. Each role carries a 100–900 tonal ramp (`--color-neutral-100` … `--color-accent-2-900`) generated in OKLCH on a shared perceptual lightness scale, so the same step of any ramp has the same visual weight. Use the light steps (100–300) for tinted fills, hovers and subtle borders, 500 as the role's base, and the dark steps (700–900) for text on tinted fills and for pressed states; prefer ramp steps over ad-hoc `color-mix()`. For elevation use `--shadow-sm/md/lg` (already tuned to the ground) rather than ad-hoc box-shadows.

## Type

Source Serif 4 for headings over Source Serif 4 for body text, loaded as `--font-heading` / `--font-body`, with the true italic loaded at the body weight — pull quotes and emphasis take the serif's italic voice, never a synthesized oblique. Density 1.25× and radius 2px are already baked into the `--space-*` / `--radius-*` scales — use the variables, not raw numbers.

## Icons

Use Phosphor icons (https://phosphoricons.com), in the duotone weight throughout.

## Interaction states

Interactive states are themed, never browser defaults: give every interactive element a `:hover` tint and a pressed state from the accent ramp (one step past the base — `--color-accent-600` on a light ground, `--color-accent-400` on a dark one, or a `color-mix()` tint for outlined/ghost variants), and style keyboard focus with `:focus-visible { outline: 2px solid var(--color-accent); outline-offset: 2px; }` — never leave the default blue focus ring.

## Components

| Class | What it is | Shown in |
| --- | --- | --- |
| `.btn` with `.btn-primary`, `.btn-secondary`, `.btn-ghost`, `.btn-icon`, `.btn-block` | Actions — the primary is a solid accent fill | components/buttons.html |
| `.tag` with `.tag-accent`, `.tag-accent-2`, `.tag-neutral`, `.tag-outline` | Small labels tinted from the ramps | components/buttons.html |
| `.field` + `label`, `.input`, `.radio` + `.dot`, `.seg` + `.seg-opt` | Form fields and choices on native elements — no script | components/forms.html |
| `.card` with `.card-kicker`, `.card-title`, `.card-body`, `.card-meta`; `.elev-sm/md/lg` | Surface-filled content cards; elevation utilities | components/cards.html |
| `.nav` + `.nav-brand` | The header bar | components/navigation.html |
| `.table` | Data tables with themed header and row rules | components/table.html |
| `.dialog-backdrop` + `.dialog` (+ `.dialog-title/-body/-actions`) | A modal at the top elevation | components/dialog.html |
| `.hr` | A horizontal rule — present, but this system prefers whitespace; avoid it | — |
| `.cmyk` | The showcase image treatment — a photograph printed as its four misregistered process plates (decks use it throughout; the filter defs ship in `print-plates.js`, inlined in the compiled bundle template pages load — other pages add the one script tag beside the stylesheet) | foundations/image.html |
| `.cmyk-num` | Display figures as misregistered process plates over the white of the sheet | templates/deck/Deck.dc.html |
| `.halftone` | The simpler newsprint dot-screen, for interface imagery | foundations/image.html |

States are built in: hovers and pressed states come from the accent ramp, keyboard focus is the 2px accent `:focus-visible` ring, `::selection` is an accent tint, and disabled controls drop to 45% opacity. Don't restyle them per page. The accent-to-ground pair is tuned to at least 3:1 — enough for icons, large text and interface chrome, not for body copy — so for paragraph-size text in the accent use a deep ramp step (`--color-accent-700` on this ground) rather than the accent itself.

## Do

- Separate sections with whitespace, not dividers or cards.
- Set everything in the serif; let size, weight and measure do the organising.
- Use cyan for interactive elements and magenta as the rarer second spot color.
- Print photographs as their process plates with `.cmyk` (interface imagery can take the simpler `.halftone` dot screen).

## Don't

- Do not structure the page with rules, borders or boxes — the page is an open broadsheet. Reserve `.card` (the one boxed component) for genuinely discrete items such as listings, never for layout.
- Do not use both accents in the same small component.
- Do not tighten the airy spacing scale (density 1.25×).
- Do not introduce a sans-serif for UI chrome; the serif is the chrome.

## Files

- `styles.css` — the only stylesheet: the token sheet (`:root` variables, ramps, base type) plus the component layer. Link it from every page.
- `print-plates.js` — the separation filter defs (`#sep-c/m/y/k` and the compound `#sep-all`) that `.cmyk` prints with. Template pages get it through the compiled bundle (`_ds_bundle.js` inlines it); a page that links `styles.css` directly adds `<script src="print-plates.js"></script>` beside the stylesheet.
- `readme.md` — this guide.
- `theme.json` — the parameters these files were derived from (a machine-readable record of the theme).
- `thumbnail.html` — the project cover (brand mark + swatches).
- `foundations/type.html` — the type scale and the heading/body pairing at real sizes.
- `foundations/color.html` — color roles and the 100-900 tonal ramps, with usage notes.
- `foundations/layout.html` — the spacing scale, the grid and how edges are drawn.
- `foundations/icons.html` — the icon set at interface sizes, inline and in buttons.
- `foundations/image.html` — how photographs and figures are treated.
- `components/buttons.html` — buttons, icon buttons and tags in every variant and state.
- `components/forms.html` — text fields, radios and the segmented control on native elements.
- `components/cards.html` — content cards and the elevation steps.
- `components/navigation.html` — the header bar pattern.
- `components/table.html` — a data table with the themed header and row rules.
- `components/dialog.html` — a modal over its backdrop at the top elevation.
- `theme.html` — the theme's parameters rendered as a reference sheet.
- `templates/landing/` — a starter page consuming the system the intended way (`index.html`, its `ds-base.js` loader, and the vendored `image-slot.js` its photograph mounts).
