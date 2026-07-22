import { describe, it, expect } from 'vitest'
import { FilterTreeStore } from '@/components/media/filterTree'
import { toFilterString } from '@/components/media/filterQuery'

// splitFromFilterString backs the "type it directly, or build it visually,
// both combine" editor (PlaylistPage.tsx's smart-playlist filter box +
// rule builder) — the critical property under test is round-tripping: the
// rule-builder-representable part must load into the tree, and whatever it
// can't represent (bare words, negated groups) must come back as leftover
// text rather than being silently dropped, so a load-then-save cycle never
// loses part of what was previously typed.

describe('FilterTreeStore.splitFromFilterString', () => {
  it('is a no-op on an empty string', () => {
    const tree = new FilterTreeStore()
    const leftover = tree.splitFromFilterString('')
    expect(leftover).toBe('')
    expect(tree.items).toEqual([])
  })

  it('loads a plain clause entirely into the rule builder, no leftover text', () => {
    const tree = new FilterTreeStore()
    const leftover = tree.splitFromFilterString('genre:Horror')
    expect(leftover).toBe('')
    expect(tree.items).toHaveLength(1)
    expect(tree.items[0]).toMatchObject({ kind: 'rule', field: 'genre', op: 'is', value: 'Horror' })
  })

  it('keeps a bare fuzzy word as leftover text, not silently dropped', () => {
    const tree = new FilterTreeStore()
    const leftover = tree.splitFromFilterString('genre:Horror unicorn')
    expect(tree.items).toHaveLength(1) // just the clause
    expect(leftover).toBe('unicorn')   // the bare word survives as text
  })

  it('round-trips a clause + a bare word through load -> save without loss', () => {
    const tree = new FilterTreeStore()
    const original = 'genre:Horror unicorn'
    const leftover = tree.splitFromFilterString(original)
    const recombined = [toFilterString(tree), leftover].filter(Boolean).join(' ')
    // Re-parsing the recombined string must still find the same clause and
    // the same fuzzy word — nothing lost across the round trip.
    const tree2 = new FilterTreeStore()
    const leftover2 = tree2.splitFromFilterString(recombined)
    expect(tree2.items).toHaveLength(1)
    expect(tree2.items[0]).toMatchObject({ field: 'genre', value: 'Horror' })
    expect(leftover2).toBe('unicorn')
  })

  it('keeps a negated group as leftover text (rule builder cannot represent negation)', () => {
    const tree = new FilterTreeStore()
    const leftover = tree.splitFromFilterString('NOT (genre:Horror OR genre:Comedy)')
    expect(tree.items).toHaveLength(0)
    expect(leftover.startsWith('NOT')).toBe(true)
  })

  it('loads a one-level group into the rule builder with no leftover', () => {
    const tree = new FilterTreeStore()
    const leftover = tree.splitFromFilterString('(genre:Horror AND year:>2015) OR studio:A24')
    expect(leftover).toBe('')
    expect(tree.items.some(i => i.kind === 'group')).toBe(true)
    expect(tree.items.some(i => i.kind === 'rule' && i.field === 'studio')).toBe(true)
  })
})
