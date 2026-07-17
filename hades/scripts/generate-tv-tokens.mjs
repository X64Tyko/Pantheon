#!/usr/bin/env node
// Parses every --hds-* custom property out of src/index.css and emits
// public/tv-tokens.json — the design-token export native clients (and
// eventually Roku) read to match Hades' actual visual rhythm, not just its
// color palette. Deliberately mechanical: this is a regex over real
// declarations, not a CSS parser, because --hds-* values are always simple
// `name: value;` pairs (see index.css's own "Extended tokens" comment) —
// composite CSS (multi-layer shadows, multi-stop gradients) is intentionally
// never expressed as a single token, so there's nothing here that needs a
// real parser to unpack.
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs'
import { fileURLToPath } from 'node:url'
import path from 'node:path'

const here    = path.dirname(fileURLToPath(import.meta.url))
const cssPath = path.join(here, '../src/index.css')
const outPath = path.join(here, '../public/tv-tokens.json')

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

const tokens = { colors: {}, spacing: {}, radii: {}, shadows: {}, fonts: {}, fontSizes: {}, transitions: {}, sizing: {}, other: {} }

let match
while ((match = tokenRe.exec(css)) !== null) {
  const [, name, rawValue] = match
  const value = rawValue.trim()
  const category = tokens[categoryOf(name)] ? categoryOf(name) : 'other'
  tokens[category][name] = value
}

const totalTokens = Object.values(tokens).reduce((n, group) => n + Object.keys(group).length, 0)
if (totalTokens === 0) {
  console.error('[generate-tv-tokens] found zero --hds-* tokens in src/index.css — refusing to write an empty file')
  process.exit(1)
}

mkdirSync(path.dirname(outPath), { recursive: true })
writeFileSync(outPath, JSON.stringify({ version: 1, tokens }, null, 2) + '\n')

console.log(`[generate-tv-tokens] wrote ${totalTokens} tokens to ${path.relative(process.cwd(), outPath)}`)
