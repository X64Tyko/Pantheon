import { serializeFilterSyntax, type GroupNode, type FilterNode } from './filterSyntax'
import type { FilterTreeStore, FilterRuleItem } from './filterTree'

// Single shared translation from a FilterTreeStore (rule-builder state — see
// filterTree.ts) into the canon filter syntax string sent to the API —
// replaces the 4 near-identical, mostly-dead `isRules = filterRules
// .filter(r => r.op === 'is' ...)` blocks that used to live in LibraryStore,
// channel/store.ts, PlaylistPage.tsx, and FillerPage.tsx (only 6 of 16
// fields, only the 'is' operator, filterMatch/group nesting never read).
// Every rule/op/group the rule-builder can produce now round-trips through
// here, including 'library' (see FilterExpr.cpp's matching compileClause
// branch) — it composes fine alongside the separate library_ids multi-select
// scoping (SourceSwitcher.tsx pills), rather than needing to be excluded in
// favor of it.
//
// Rules with an empty value are skipped (an in-progress, not-yet-filled-in
// row shouldn't affect the query). An empty tree serializes to ''.
export function toFilterString(tree: Pick<FilterTreeStore, 'items' | 'match'>): string {
  const ruleNode = (r: FilterRuleItem): FilterNode => ({ type: 'clause', field: r.field, op: r.op, value: r.value })

  const children: FilterNode[] = []
  for (const item of tree.items) {
    if (item.kind === 'rule') {
      if (item.value.trim() === '') continue
      children.push(ruleNode(item))
    } else {
      const groupRules = item.rules.filter(r => r.value.trim() !== '')
      if (groupRules.length === 0) continue
      children.push({ type: 'group', match: item.match, children: groupRules.map(ruleNode) })
    }
  }
  if (children.length === 0) return ''
  const top: GroupNode = { type: 'group', match: tree.match, children }
  return serializeFilterSyntax(top)
}
