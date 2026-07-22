import { makeAutoObservable } from 'mobx'
import { FIELD_DEFS, type FilterField, type FilterOp } from './filterFields'
import { parseFilterSyntax, serializeFilterSyntax, type ClauseNode, type FilterNode } from './filterSyntax'

let _id = 0
const nextId = () => String(++_id)

export interface FilterRuleItem { kind: 'rule'; id: string; field: FilterField; op: FilterOp; value: string }
// One level of grouping — enough to express Plex-style "(X AND Y) OR Z"
// without a fully general recursive tree UI. filterSyntax.ts's compiler
// already supports arbitrary nesting for text typed directly into the
// search bar; this just bounds how deep the *visual* builder goes.
export interface FilterGroupItem { kind: 'group'; id: string; match: 'all' | 'any'; rules: FilterRuleItem[] }
export type FilterItem = FilterRuleItem | FilterGroupItem

function blankRule(): FilterRuleItem {
  return { kind: 'rule', id: nextId(), field: 'genre', op: 'is', value: '' }
}

function clauseToRule(c: ClauseNode): FilterRuleItem {
  return { kind: 'rule', id: nextId(), field: c.field, op: c.op, value: c.value }
}

// Reverse of toFilterString (filterQuery.ts) — converts a parsed canon-syntax
// AST back into rule-builder items. Bounded to what the visual builder can
// actually represent (one level of grouping, clauses only): bare fuzzy words
// and negated groups have no rule-builder equivalent and are dropped rather
// than approximated. Used to seed the panel from a Home shelf's own `filter`
// string (see HomePage.tsx's shelf defs) so "Continue in Library" lands with
// the shelf's criteria visibly editable, not just silently applied.
function astToItems(node: FilterNode): FilterItem[] {
  const top = node.type === 'group' && !node.negate ? node.children : [node]
  const items: FilterItem[] = []
  for (const child of top) {
    if (child.type === 'clause') items.push(clauseToRule(child))
    else if (child.type === 'group' && !child.negate) {
      const rules = child.children.filter((c): c is ClauseNode => c.type === 'clause').map(clauseToRule)
      if (rules.length > 0) items.push({ kind: 'group', id: nextId(), match: child.match, rules })
    }
  }
  return items
}

// Shared filter rule-builder state — one instance per page that embeds
// PickerFilters (Library, Playlists, Filler Lists, channel content picker).
// Replaces each page's own copy-pasted filterRules/filterMatch state + add/
// update/remove methods, which is what let the "only 'is' rules, only 6
// fields" bug (see components/media/filterQuery.ts) drift out of sync
// across all 4 surfaces in the first place.
export class FilterTreeStore {
  open: boolean = false
  match: 'all' | 'any' = 'all'
  items: FilterItem[] = []

  constructor() { makeAutoObservable(this) }

  get isEmpty() { return this.items.length === 0 }

  // Flattened view for callers that only care about "every rule regardless
  // of grouping" (e.g. finding a single preset 'library'/'genre' value).
  get allRules(): FilterRuleItem[] {
    return this.items.flatMap(it => it.kind === 'rule' ? [it] : it.rules)
  }

  toggleOpen() { this.open = !this.open }
  setMatch(m: 'all' | 'any') { this.match = m }

  addRule() { this.items.push(blankRule()) }
  addGroup() { this.items.push({ kind: 'group', id: nextId(), match: 'all', rules: [blankRule()] }) }

  removeItem(id: string) { this.items = this.items.filter(it => it.id !== id) }

  updateRule(id: string, patch: Partial<Pick<FilterRuleItem, 'field' | 'op' | 'value'>>) {
    const rule = this.items.find((it): it is FilterRuleItem => it.kind === 'rule' && it.id === id)
      ?? this.items.flatMap(it => it.kind === 'group' ? it.rules : []).find(r => r.id === id)
    if (!rule) return
    if (patch.field !== undefined && patch.field !== rule.field) {
      rule.field = patch.field
      rule.op    = FIELD_DEFS[patch.field].ops[0].id
      rule.value = ''
      return
    }
    if (patch.op    !== undefined) rule.op    = patch.op
    if (patch.value !== undefined) rule.value = patch.value
  }

  setGroupMatch(groupId: string, match: 'all' | 'any') {
    const g = this.items.find(it => it.id === groupId)
    if (g?.kind === 'group') g.match = match
  }
  addRuleToGroup(groupId: string) {
    const g = this.items.find(it => it.id === groupId)
    if (g?.kind === 'group') g.rules.push(blankRule())
  }
  removeRuleFromGroup(groupId: string, ruleId: string) {
    const g = this.items.find(it => it.id === groupId)
    if (g?.kind === 'group') g.rules = g.rules.filter(r => r.id !== ruleId)
  }

  reset() { this.items = []; this.open = false; this.match = 'all' }

  // For Home shelf tiles that land on Library with a specific tag/value
  // already filtering the view — replaces every existing item with one
  // rule, opens the panel so it's immediately visible/editable.
  setSingleRule(field: FilterField, value: string) {
    this.items = [{ kind: 'rule', id: nextId(), field, op: 'is', value }]
    this.open  = true
  }

  // Same idea as setSingleRule, but for a whole canon filter-syntax string
  // (see astToItems above) — a Home shelf's `filter` def can express more
  // than one field/AND-group, not just a single field:value pair. A no-op on
  // an empty string (nothing to seed, tree stays whatever it already was).
  setFromFilterString(text: string) {
    if (!text.trim()) return
    const ast = parseFilterSyntax(text)
    this.match = ast.type === 'group' && !ast.negate ? ast.match : 'all'
    this.items = astToItems(ast)
    this.open  = true
  }

  // Same idea as setFromFilterString, but for a "rule builder + companion
  // free-text box, in tandem" editor (see PlaylistPage.tsx's smart-playlist
  // editor) — the part the rule builder can represent (clauses, one level of
  // grouping) is loaded into the tree as usual; whatever it can't (bare
  // fuzzy words, negated groups) is re-serialized and returned as leftover
  // text instead of silently dropped, so re-saving never loses what the
  // user had typed directly. A no-op (returns '') on an empty string.
  splitFromFilterString(text: string): string {
    if (!text.trim()) return ''
    const ast = parseFilterSyntax(text)
    const top: FilterNode[] = ast.type === 'group' && !ast.negate ? ast.children : [ast]
    const structured: FilterNode[] = []
    const leftover: FilterNode[] = []
    for (const child of top) {
      if (child.type === 'clause') structured.push(child)
      else if (child.type === 'group' && !child.negate) structured.push(child)
      else leftover.push(child)
    }
    const match = ast.type === 'group' && !ast.negate ? ast.match : 'all'
    this.match = match
    this.items = astToItems({ type: 'group', match, children: structured })
    this.open  = true
    if (leftover.length === 0) return ''
    return serializeFilterSyntax(leftover.length === 1 ? leftover[0] : { type: 'group', match, children: leftover })
  }
}
