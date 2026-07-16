import { FIELD_DEFS, RESOLUTIONS, DECADES, type FilterField, type FilterOp } from './filterFields'

// ─────────────────────────────────────────────────────────────────────────────
// The canon filter syntax — one text grammar that is simultaneously: what the
// visual rule-builder (PickerFilters.tsx) emits/edits, what a power user can
// type directly into the Library search bar, and (future, not built yet) what
// a smart playlist stores as its live definition. The rule-builder is a
// structured editor over this AST, not a separate parallel representation.
//
//   expr    := orExpr
//   orExpr  := andExpr ( ("OR"|"||") andExpr )*
//   andExpr := term ( ("AND"|"&&")? term )*      -- implicit AND between terms
//   term    := ("-"|"NOT") term | "(" expr ")" | clause | word
//   clause  := FIELD ":" (">=" | "<=" | ">" | "<")? VALUE
//   VALUE   := '"' ... '"' | bareword
//   word    := bareword                           -- folds into fuzzy free text
//
// FIELD names are FIELD_DEFS keys (PickerFilters.tsx) plus a couple aliases
// (see FIELD_ALIASES below). A `field:` prefix that doesn't match a known
// field/alias is treated as an ordinary word rather than a parse error —
// this syntax is meant to degrade gracefully for casual typing, not reject it.
// ─────────────────────────────────────────────────────────────────────────────

export interface ClauseNode {
  type:    'clause'
  field:   FilterField
  op:      FilterOp
  value:   string
}
export interface WordNode {
  type:    'word'
  value:   string
  negate?: boolean
}
export interface GroupNode {
  type:     'group'
  match:    'all' | 'any'
  children: FilterNode[]
  negate?:  boolean
}
export type FilterNode = ClauseNode | WordNode | GroupNode

const FIELD_ALIASES: Record<string, FilterField> = {
  rating: 'content_rating',
}

const FIELD_KEYS = new Set<string>([...Object.keys(FIELD_DEFS), ...Object.keys(FIELD_ALIASES)])

function resolveField(raw: string): FilterField | null {
  const key = raw.toLowerCase()
  if (key in FIELD_ALIASES) return FIELD_ALIASES[key]
  if ((FIELD_DEFS as Record<string, unknown>)[key]) return key as FilterField
  return null
}

// ─── Tokenizer ──────────────────────────────────────────────────────────────

type TokKind = 'lparen' | 'rparen' | 'and' | 'or' | 'not' | 'word'
interface Token { kind: TokKind; text: string; start: number; end: number }

function tokenize(src: string): Token[] {
  const toks: Token[] = []
  let i = 0
  const n = src.length
  while (i < n) {
    const c = src[i]
    if (c === ' ' || c === '\t' || c === '\n' || c === '\r') { i++; continue }
    if (c === '(') { toks.push({ kind: 'lparen', text: '(', start: i, end: i + 1 }); i++; continue }
    if (c === ')') { toks.push({ kind: 'rparen', text: ')', start: i, end: i + 1 }); i++; continue }

    // A bareword/word token — may embed a quoted value (field:"..."), so we
    // scan until whitespace/parens *outside* any quoted span, not just to
    // the next space.
    const start = i
    let inQuote = false
    let buf = ''
    while (i < n) {
      const ch = src[i]
      if (inQuote) {
        buf += ch
        if (ch === '"') inQuote = false
        i++
        continue
      }
      if (ch === '"') { inQuote = true; buf += ch; i++; continue }
      if (ch === ' ' || ch === '\t' || ch === '\n' || ch === '\r') break
      if (ch === '(' || ch === ')') break
      buf += ch
      i++
    }
    if (buf === '') { i++; continue } // stray char, skip defensively

    const upper = buf.toUpperCase()
    if (upper === 'AND' || upper === '&&') { toks.push({ kind: 'and', text: buf, start, end: i }); continue }
    if (upper === 'OR'  || upper === '||') { toks.push({ kind: 'or',  text: buf, start, end: i }); continue }
    if (upper === 'NOT') { toks.push({ kind: 'not', text: buf, start, end: i }); continue }
    toks.push({ kind: 'word', text: buf, start, end: i })
  }
  return toks
}

// ─── Parser (recursive descent over the token list) ────────────────────────

class Parser {
  constructor(private toks: Token[], private pos = 0) {}
  private peek(): Token | undefined { return this.toks[this.pos] }
  private next(): Token | undefined { return this.toks[this.pos++] }

  parseExpr(): FilterNode { return this.parseOr() }

  private parseOr(): FilterNode {
    const first = this.parseAnd()
    const children = [first]
    while (this.peek()?.kind === 'or') { this.next(); children.push(this.parseAnd()) }
    return children.length === 1 ? first : { type: 'group', match: 'any', children }
  }

  private parseAnd(): FilterNode {
    const first = this.parseTerm()
    const children = [first]
    for (;;) {
      const t = this.peek()
      if (!t) break
      if (t.kind === 'or' || t.kind === 'rparen') break
      if (t.kind === 'and') this.next() // explicit "AND" — just skip the keyword, still implicit-join below
      children.push(this.parseTerm())
    }
    return children.length === 1 ? first : { type: 'group', match: 'all', children }
  }

  private parseTerm(): FilterNode {
    const t = this.peek()
    if (!t) return { type: 'word', value: '' }

    if (t.kind === 'not') {
      this.next()
      const inner = this.parseTerm()
      return negateNode(inner)
    }
    if (t.kind === 'lparen') {
      this.next()
      const inner = this.parseExpr()
      if (this.peek()?.kind === 'rparen') this.next()
      return inner
    }
    // word token — may be "-prefixed", may be a field:value clause, else a
    // plain fuzzy word.
    this.next()
    let raw = t.text
    let negate = false
    if (raw.startsWith('-') && raw.length > 1) { negate = true; raw = raw.slice(1) }

    const clause = tryParseClause(raw)
    if (clause) return negate ? negateNode(clause) : clause

    const word: WordNode = { type: 'word', value: stripQuotes(raw) }
    if (negate) word.negate = true
    return word
  }
}

function negateNode(node: FilterNode): FilterNode {
  // Fold negation into the op itself for the two ops that have a natural
  // opposite, so `-genre:horror` compiles to a plain `is_not` clause instead
  // of a NOT(...) wrapper — cleaner SQL, and matches what the rule-builder's
  // own "is not"/"does not contain" options already produce.
  if (node.type === 'clause') {
    if (node.op === 'is')       return { ...node, op: 'is_not' }
    if (node.op === 'contains') return { ...node, op: 'does_not_contain' }
  }
  if (node.type === 'word') return { ...node, negate: !node.negate }
  if (node.type === 'group') return { ...node, negate: !node.negate }
  return node
}

const CMP_PREFIXES: [string, FilterOp][] = [['>=', 'gte'], ['<=', 'lte'], ['>', 'gt'], ['<', 'lt']]

// Attempts to read `raw` as FIELD:VALUE. Returns null (falls back to a plain
// word) if the prefix before the first top-level ':' isn't a known field —
// deliberately permissive, not a hard parse error.
function tryParseClause(raw: string): ClauseNode | null {
  const colon = raw.indexOf(':')
  if (colon <= 0) return null
  const fieldRaw = raw.slice(0, colon)
  const field = resolveField(fieldRaw)
  if (!field) return null

  let valuePart = raw.slice(colon + 1)
  const def = FIELD_DEFS[field]

  let op: FilterOp | null = null

  if (field === 'added') {
    // 'added' doesn't support generic lt/gt/gte/lte (its real ops are
    // in_last/before/after), so it's handled entirely separately from the
    // generic comparator-prefix loop below:
    //   added:<30d / 30d / 30   -> in_last, N days (unit-less = days)
    //   added:>2024-01-01       -> after 2024-01-01
    //   added:<2024-01-01       -> before 2024-01-01
    const hasCmp = valuePart.startsWith('<') || valuePart.startsWith('>')
    const body = hasCmp ? valuePart.slice(1) : valuePart
    const m = body.match(/^(\d+)([dwmy]?)$/i)
    if (m) {
      const n = parseInt(m[1], 10)
      const mult = m[2] ? { d: 1, w: 7, m: 30, y: 365 }[m[2].toLowerCase() as 'd'|'w'|'m'|'y'] : 1
      op = 'in_last'
      valuePart = String(n * mult)
    } else if (hasCmp) {
      op = valuePart.startsWith('>') ? 'after' : 'before'
      valuePart = body
    }
  }

  // Wildcard markers for contains/begins_with/ends_with — there's no
  // comparator-symbol equivalent for these (only gt/gte/lt/lte get one), so
  // without this they'd silently serialize as plain field:value and
  // round-trip back as the field's default op (usually 'is') instead —
  // e.g. picking "contains" in the rule builder would silently become an
  // exact match once sent to the API. Skipped for an explicitly quoted
  // value ('field:"*literal*"') — quoting means "take this exactly,
  // asterisks included," not "treat as a wildcard."
  if (!op && !valuePart.startsWith('"')) {
    const startsStar = valuePart.startsWith('*')
    const endsStar   = valuePart.length > 1 && valuePart.endsWith('*')
    if (startsStar && endsStar && def.ops.some(o => o.id === 'contains')) {
      op = 'contains'; valuePart = valuePart.slice(1, -1)
    } else if (endsStar && def.ops.some(o => o.id === 'begins_with')) {
      op = 'begins_with'; valuePart = valuePart.slice(0, -1)
    } else if (startsStar && def.ops.some(o => o.id === 'ends_with')) {
      op = 'ends_with'; valuePart = valuePart.slice(1)
    }
  }

  if (!op) {
    for (const [sym, id] of CMP_PREFIXES) {
      if (valuePart.startsWith(sym) && def.ops.some(o => o.id === id)) {
        op = id
        valuePart = valuePart.slice(sym.length)
        break
      }
    }
  }

  let value = stripQuotes(valuePart)

  if (!op) {
    // No comparator matched — fall back to the field's first/default
    // operator rather than silently dropping the clause (that's the exact
    // bug this syntax replaces).
    op = def.ops[0]?.id ?? 'is'
  } else if (!def.ops.some(o => o.id === op)) {
    op = def.ops[0]?.id ?? 'is'
  }

  if (field === 'resolution') {
    const match = RESOLUTIONS.find(r => r.toLowerCase() === value.toLowerCase())
    if (match) value = match
  }
  if (field === 'decade') {
    value = value.replace(/s$/i, '')
    if (!DECADES.some(d => d.replace(/s$/i, '') === value)) {
      // leave as-typed; backend range-check just won't match anything if bogus
    }
  }

  return { type: 'clause', field, op, value }
}

function stripQuotes(s: string): string {
  if (s.length >= 2 && s[0] === '"' && s[s.length - 1] === '"') return s.slice(1, -1)
  return s
}

export function parseFilterSyntax(text: string): FilterNode {
  const toks = tokenize(text ?? '')
  if (toks.length === 0) return { type: 'group', match: 'all', children: [] }
  return new Parser(toks).parseExpr()
}

// Count of recognized field:value clauses in a parsed tree (ignores bare
// fuzzy words) — used for the search bar's "N filters parsed" hint.
export function countClauses(node: FilterNode): number {
  if (node.type === 'clause') return 1
  if (node.type === 'word') return 0
  return node.children.reduce((n, c) => n + countClauses(c), 0)
}

// ─── Serializer ─────────────────────────────────────────────────────────────

const CMP_SYMBOL: Partial<Record<FilterOp, string>> = { gt: '>', gte: '>=', lt: '<', lte: '<=' }

function quoteIfNeeded(v: string): string {
  if (v === '') return '""'
  return /\s|[()"]/.test(v) ? `"${v.replace(/"/g, '\\"')}"` : v
}

// Ops with no comparator-symbol equivalent get a wildcard-marker
// representation instead (see tryParseClause's mirrored read side) — without
// this they'd serialize identically to plain field:value and silently
// round-trip back as the field's default op instead of the one actually
// picked in the rule builder.
function serializeClause(c: ClauseNode): string {
  const v = quoteIfNeeded(c.value)
  switch (c.op) {
    case 'contains':         return `${c.field}:*${v}*`
    case 'begins_with':      return `${c.field}:${v}*`
    case 'ends_with':        return `${c.field}:*${v}`
    case 'is_not':           return `-${c.field}:${v}`
    case 'does_not_contain': return `-${c.field}:*${v}*`
    default: {
      const sym = CMP_SYMBOL[c.op] ?? ''
      return `${c.field}:${sym}${v}`
    }
  }
}

export function serializeFilterSyntax(node: FilterNode): string {
  if (node.type === 'clause') return serializeClause(node)
  if (node.type === 'word') return `${node.negate ? '-' : ''}${quoteIfNeeded(node.value)}`
  const joiner = node.match === 'any' ? ' OR ' : ' AND '
  const inner = node.children.map(c => {
    const s = serializeFilterSyntax(c)
    return c.type === 'group' && c.children.length > 1 ? `(${s})` : s
  }).join(joiner)
  const wrapped = node.negate ? `NOT (${inner})` : inner
  return wrapped
}
