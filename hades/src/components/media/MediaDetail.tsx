import type { ScraperSearchResult } from '../../api/types'
import { MediaDetailHero } from './MediaDetailHero'
import { LibraryDetailActions } from './LibraryDetailActions'
import { LibraryAdminPanel } from './LibraryAdminPanel'

interface MediaDetailProps {
  id?:             string
  content_type?:   'show' | 'movie'
  discoverResult?: ScraperSearchResult
  onClose:         () => void
}

export function MediaDetail({ id, content_type, discoverResult, onClose }: MediaDetailProps) {
  return (
    <MediaDetailHero
      id={id}
      content_type={content_type}
      discoverResult={discoverResult}
      onBack={onClose}
      actions={<LibraryDetailActions id={id} content_type={content_type} discoverResult={discoverResult} />}
      afterShelves={id && content_type && !discoverResult ? <LibraryAdminPanel id={id} content_type={content_type} /> : undefined}
    />
  )
}
