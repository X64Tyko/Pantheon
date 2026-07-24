import type { ScraperSearchResult } from '../../api/types'
import { MediaDetailHero } from './MediaDetailHero'
import { LibraryDetailActions, PlayAction, WatchTogetherAction } from './LibraryDetailActions'
import { LibraryAdminPanel } from './LibraryAdminPanel'

interface MediaDetailProps {
  id?:              string
  content_type?:    'show' | 'movie'
  discoverResult?:  ScraperSearchResult
  onClose:          () => void
  onViewInLibrary?: (id: string, content_type: 'show' | 'movie') => void
}

export function MediaDetail({ id, content_type, discoverResult, onClose, onViewInLibrary }: MediaDetailProps) {
  return (
    <MediaDetailHero
      id={id}
      content_type={content_type}
      discoverResult={discoverResult}
      onBack={onClose}
      playButton={<>
        <PlayAction id={id} content_type={content_type} discoverResult={discoverResult} />
        <WatchTogetherAction id={id} content_type={content_type} discoverResult={discoverResult} />
      </>}
      actions={media => <LibraryDetailActions id={id} content_type={content_type} discoverResult={discoverResult} onViewInLibrary={onViewInLibrary} media={media} />}
      afterShelves={id && content_type && !discoverResult ? <LibraryAdminPanel id={id} content_type={content_type} /> : undefined}
    />
  )
}
