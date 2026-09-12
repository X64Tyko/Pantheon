// Shared ISO 639-1 language list for the "preferred_language" / display-
// language selectors (library-level in SourcesPage, per-item override in
// LibraryAdminPanel). Kept to languages the wired-up scrapers (TMDB, TVDB,
// Trakt) actually have broad translation coverage for — not an exhaustive
// ISO 639-1 list.
export const LANGUAGE_OPTIONS: { value: string; label: string }[] = [
    {value: 'en', label: 'English'},
    {value: 'ja', label: 'Japanese'},
    {value: 'ko', label: 'Korean'},
    {value: 'zh', label: 'Chinese'},
    {value: 'fr', label: 'French'},
    {value: 'de', label: 'German'},
    {value: 'es', label: 'Spanish'},
    {value: 'it', label: 'Italian'},
    {value: 'pt', label: 'Portuguese'},
    {value: 'ru', label: 'Russian'},
]
