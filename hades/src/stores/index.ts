import { ChannelStore } from './ChannelStore'
import { SourceStore }  from './SourceStore'
import { SystemStore }  from './SystemStore'

export type { LogEntry, ErrorToast } from './SystemStore'

export const sourceStore  = new SourceStore()
export const channelStore = new ChannelStore()
export const systemStore  = new SystemStore()

export { contentStore }  from './ContentStore'
export { statusStore }   from './StatusStore'
