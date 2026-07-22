// Field/operator registry for the filter rule-builder — a dependency-free
// leaf module so both the UI (PickerFilters.tsx) and the canon-syntax
// parser/compiler (filterSyntax.ts, filterTree.ts) can import it without a
// circular dependency.

export type FilterField =
  | 'library' | 'playlist' | 'source' | 'title' | 'genre' | 'tag' | 'year' | 'content_rating' | 'studio'
  | 'director' | 'actor' | 'writer' | 'country'
  | 'collection' | 'network' | 'label' | 'resolution' | 'decade'
  | 'audience_rating' | 'duration' | 'added'
  | 'audio_language' | 'subtitle_language'

export type FilterOp =
  | 'is' | 'is_not'
  | 'contains' | 'does_not_contain' | 'begins_with' | 'ends_with'
  | 'gt' | 'gte' | 'lt' | 'lte'
  | 'before' | 'after' | 'in_last'

export type ValueType = 'text' | 'number' | 'days' | 'resolution' | 'decade' | 'library' | 'source' | 'playlist'

export const RESOLUTIONS = ['4K', '1080p', '720p', 'SD']
export const DECADES     = ['2020s', '2010s', '2000s', '1990s', '1980s', '1970s', '1960s', '1950s', '1940s', '1930s']

export type FieldDef = { label: string; valueType: ValueType; ops: { id: FilterOp; label: string }[] }

const TEXT_OPS: { id: FilterOp; label: string }[] = [
  { id: 'is',              label: 'is' },
  { id: 'is_not',          label: 'is not' },
  { id: 'contains',        label: 'contains' },
  { id: 'does_not_contain',label: 'does not contain' },
]

const FULL_TEXT_OPS: { id: FilterOp; label: string }[] = [
  { id: 'contains',        label: 'contains' },
  { id: 'does_not_contain',label: 'does not contain' },
  { id: 'begins_with',     label: 'begins with' },
  { id: 'ends_with',       label: 'ends with' },
  { id: 'is',              label: 'is' },
  { id: 'is_not',          label: 'is not' },
]

// critic_rating deliberately isn't a field — no critic-score data source
// exists anywhere in Pantheon (TMDB's vote_average is already
// audience_rating). A real critic-score integration (Rotten Tomatoes/
// Metacritic/etc.) is a separate future scraper, not something to fake here.
export const FIELD_DEFS: Record<FilterField, FieldDef> = {
  library:         { label: 'Library',         valueType: 'library',    ops: [{ id: 'is',      label: 'is' }] },
  playlist:        { label: 'Playlist',        valueType: 'playlist',   ops: [{ id: 'is',      label: 'is' }] },
  source:          { label: 'Source',          valueType: 'source',     ops: [{ id: 'is', label: 'is' }, { id: 'is_not', label: 'is not' }] },
  title:           { label: 'Title',           valueType: 'text',       ops: FULL_TEXT_OPS },
  genre:           { label: 'Genre',           valueType: 'text',       ops: TEXT_OPS },
  tag:             { label: 'Tag',              valueType: 'text',       ops: TEXT_OPS },
  year:            { label: 'Year',            valueType: 'number',     ops: [{ id: 'is', label: 'is' }, { id: 'lt', label: 'is before' }, { id: 'gt', label: 'is after' }] },
  content_rating:  { label: 'Content Rating',  valueType: 'text',       ops: TEXT_OPS },
  studio:          { label: 'Studio',          valueType: 'text',       ops: FULL_TEXT_OPS },
  director:        { label: 'Director',        valueType: 'text',       ops: TEXT_OPS },
  actor:           { label: 'Actor',           valueType: 'text',       ops: TEXT_OPS },
  writer:          { label: 'Writer',          valueType: 'text',       ops: TEXT_OPS },
  country:         { label: 'Country',         valueType: 'text',       ops: TEXT_OPS },
  collection:      { label: 'Collection',      valueType: 'text',       ops: TEXT_OPS },
  network:         { label: 'Network',         valueType: 'text',       ops: TEXT_OPS },
  label:           { label: 'Label',           valueType: 'text',       ops: TEXT_OPS },
  audio_language:    { label: 'Audio Language',    valueType: 'text', ops: TEXT_OPS },
  subtitle_language: { label: 'Subtitle Language', valueType: 'text', ops: TEXT_OPS },
  resolution:      { label: 'Resolution',      valueType: 'resolution', ops: [{ id: 'is', label: 'is' }, { id: 'is_not', label: 'is not' }] },
  decade:          { label: 'Decade',          valueType: 'decade',     ops: [{ id: 'is', label: 'is' }] },
  audience_rating: { label: 'Audience Rating', valueType: 'number',     ops: [{ id: 'gte', label: 'is at least' }, { id: 'lte', label: 'is at most' }, { id: 'gt', label: 'is greater than' }, { id: 'lt', label: 'is less than' }] },
  duration:        { label: 'Duration (mins)', valueType: 'number',     ops: [{ id: 'gte', label: 'is at least' }, { id: 'lte', label: 'is at most' }, { id: 'gt', label: 'is greater than' }, { id: 'lt', label: 'is less than' }] },
  added:           { label: 'Date Added',      valueType: 'days',       ops: [{ id: 'in_last', label: 'in the last' }, { id: 'before', label: 'before' }, { id: 'after', label: 'after' }] },
}
