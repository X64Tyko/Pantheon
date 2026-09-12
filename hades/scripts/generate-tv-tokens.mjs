#!/usr/bin/env node
// Parses every --hds-* custom property out of src/index.css and emits a
// design-token export native clients (Android now, Roku eventually) read
// instead of hardcoding their own approximated colors — "styling comes from
// the manifest, not a per-client guess" is the whole point of this file.
// Deliberately mechanical: this is a regex over real declarations, not a CSS
// parser, because --hds-* values are always simple `name: value;` pairs (see
// index.css's own "Extended tokens" comment) — composite CSS (multi-layer
// shadows, multi-stop gradients) is intentionally never expressed as a
// single token, so there's nothing here that needs a real parser to unpack.
//
// Written to TWO places: public/tv-tokens.json (Hades' own static assets,
// unchanged from before) and ../kairos/assets/tv-tokens.json — the second
// copy exists because Kairos's Docker build context is scoped to kairos/
// only (see .github/workflows/docker-kairos.yml's `context: ./kairos`), so
// TvManifestService can't reach across to hades/public/ at build time. Both
// copies come from this one run against this one CSS file — there is
// deliberately only one generator, not two hand-synced ones.
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs'
import { fileURLToPath } from 'node:url'
import path from 'node:path'

const here      = path.dirname(fileURLToPath(import.meta.url))
const cssPath   = path.join(here, '../src/index.css')
const outPaths  = [
  path.join(here, '../public/tv-tokens.json'),
  path.join(here, '../../kairos/assets/tv-tokens.json'),
]

const css = readFileSync(cssPath, 'utf8')

// Matches `--hds-foo-bar: <value>;` anywhere in the file (not just inside a
// single :root block — index.css has more than one). Values never contain a
// literal `;` (CSS custom property values can't), so this is unambiguous.
const tokenRe = /--(hds-[a-zA-Z0-9-]+):\s*([^;]+);/g

const categoryOf = (name) => {
  if (name.startsWith('hds-radius-'))          return 'radii'
  if (name.startsWith('hds-space-'))           return 'spacing'
  if (name.startsWith('hds-font-size-'))       return 'fontSizes'
  if (name.startsWith('hds-font-'))            return 'fonts'
  if (name.startsWith('hds-transition-'))       return 'transitions'
  if (name.startsWith('hds-shadow'))           return 'shadows'
  if (name === 'hds-control-height' || name.startsWith('hds-tile-'))
    return 'sizing'
  return 'colors' // everything else in this file's token set is a color
}

// ── oklch()/rgba()/#hex → #RRGGBB[AA], for clients that can't parse CSS
//    color functions (Kotlin, BrightScript). Standard OKLab matrices —
//    https://bottosson.github.io/posts/oklab/ — same algorithm libraries
//    like culori implement, not a homebrewed approximation. Returns null for
//    anything not directly convertible (composite gradients, `var(...)`
//    references) — those keep only their raw `css` form.
function oklchToHex(l, c, h, alpha) {
  const hRad = (h * Math.PI) / 180
  const a = c * Math.cos(hRad)
  const b = c * Math.sin(hRad)

  const l_ = l + 0.3963377774 * a + 0.2158037573 * b
  const m_ = l - 0.1055613458 * a - 0.0638541728 * b
  const s_ = l - 0.0894841775 * a - 1.2914855480 * b

  const l3 = l_ ** 3, m3 = m_ ** 3, s3 = s_ ** 3

  const r    =  4.0767416621 * l3 - 3.3077115913 * m3 + 0.2309699292 * s3
  const g    = -1.2684380046 * l3 + 2.6097574011 * m3 - 0.3413193965 * s3
  const bLin = -0.0041960863 * l3 - 0.7034186147 * m3 + 1.7076147010 * s3

  return rgbToHex(r, g, bLin, alpha)
}

function gammaEncode(x) {
  x = Math.max(0, Math.min(1, x))
  return x <= 0.0031308 ? 12.92 * x : 1.055 * Math.pow(x, 1 / 2.4) - 0.055
}

function rgbToHex(rLin, gLin, bLin, alpha = 1) {
  const toByte = (x) => Math.round(gammaEncode(x) * 255)
  const bytes = [toByte(rLin), toByte(gLin), toByte(bLin)]
  if (alpha < 1) bytes.push(Math.round(Math.max(0, Math.min(1, alpha)) * 255))
  return '#' + bytes.map((v) => v.toString(16).padStart(2, '0')).join('')
}

// Parses a percentage-or-plain numeric CSS component; percentages for L/A
// are 0-100%, C/H tokens in this file are never expressed as percentages.
function parseComponent(raw, scale100) {
  if (raw.endsWith('%')) return (parseFloat(raw) / 100) * (scale100 ?? 1)
  return parseFloat(raw)
}

const oklchRe = /^oklch\(\s*([\d.]+%?)\s+([\d.]+%?)\s+([\d.]+)\s*(?:\/\s*([\d.]+%?)\s*)?\)$/
const rgbaRe  = /^rgba?\(\s*([\d.]+)\s*,\s*([\d.]+)\s*,\s*([\d.]+)\s*(?:,\s*([\d.]+)\s*)?\)$/
const hexRe   = /^#([0-9a-fA-F]{3}|[0-9a-fA-F]{6})$/

function toHex(cssValue) {
  const oklch = oklchRe.exec(cssValue)
  if (oklch) {
    const [, lRaw, cRaw, hRaw, aRaw] = oklch
    const l = parseComponent(lRaw, 1)
    const c = parseComponent(cRaw, 0.4) // oklch chroma has no standard %-basis; unused here (never % in this file)
    const h = parseFloat(hRaw)
    const alpha = aRaw !== undefined ? parseComponent(aRaw, 1) : 1
    return oklchToHex(l, c, h, alpha)
  }
  const rgba = rgbaRe.exec(cssValue)
  if (rgba) {
    const [, r, g, b, a] = rgba
    // rgbToHex expects linear 0-1 for gamma-encoding, but rgba()'s r/g/b are
    // already gamma-encoded 0-255 sRGB bytes — encode directly, no
    // OKLab/linear round-trip needed for a value that's already sRGB.
    const toByte = (v) => Math.max(0, Math.min(255, Math.round(parseFloat(v))))
    const bytes = [toByte(r), toByte(g), toByte(b)]
    if (a !== undefined) bytes.push(Math.round(Math.max(0, Math.min(1, parseFloat(a))) * 255))
    return '#' + bytes.map((v) => v.toString(16).padStart(2, '0')).join('')
  }
  const hex = hexRe.exec(cssValue)
  if (hex) {
    const h = hex[1]
    return '#' + (h.length === 3 ? h.split('').map((ch) => ch + ch).join('') : h)
  }
  return null // composite value (gradient, var() reference, shadow, ...) — no single color to derive
}

const tokens = { colors: {}, spacing: {}, radii: {}, shadows: {}, fonts: {}, fontSizes: {}, transitions: {}, sizing: {}, other: {} }

let match
while ((match = tokenRe.exec(css)) !== null) {
  const [, name, rawValue] = match
  const value = rawValue.trim()
  const category = categoryOf(name)
  const bucket = tokens[category] ? category : 'other'
  if (bucket === 'colors') {
    tokens.colors[name] = { css: value, hex: toHex(value) }
  } else {
    tokens[bucket][name] = value
  }
}

const totalTokens = Object.values(tokens).reduce((n, group) => n + Object.keys(group).length, 0)
if (totalTokens === 0) {
  console.error('[generate-tv-tokens] found zero --hds-* tokens in src/index.css — refusing to write an empty file')
  process.exit(1)
}

const colorsWithHex = Object.values(tokens.colors).filter((v) => v.hex !== null).length
const colorsTotal   = Object.keys(tokens.colors).length

const output = JSON.stringify({ version: 2, tokens }, null, 2) + '\n'
for (const outPath of outPaths) {
  mkdirSync(path.dirname(outPath), { recursive: true })
  writeFileSync(outPath, output)
}

console.log(
  `[generate-tv-tokens] wrote ${totalTokens} tokens (${colorsWithHex}/${colorsTotal} colors resolved to hex) to:\n` +
    outPaths.map((p) => `  - ${path.relative(process.cwd(), p)}`).join('\n'),
)
