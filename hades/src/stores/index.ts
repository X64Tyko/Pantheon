import { ChannelStore } from './ChannelStore'
import { SourceStore }  from './SourceStore'
import { SystemStore }  from './SystemStore'
import { UserStore }    from './UserStore'

export type { LogEntry, ErrorToast } from './SystemStore'

export const sourceStore  = new SourceStore()
export const channelStore = new ChannelStore()
export const systemStore  = new SystemStore()
export const userStore    = new UserStore()

export { statusStore }   from './StatusStore'
