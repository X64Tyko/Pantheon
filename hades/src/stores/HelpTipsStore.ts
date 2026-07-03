import { makeAutoObservable } from 'mobx'

const STORAGE_KEY = 'hds-help-tips-enabled'

// Pure client display preference (which "?" help affordances render at all,
// app-wide) — localStorage-backed like LibraryStore's density/sidebarOpen,
// not a Kairos server setting; there's nothing here worth syncing across
// devices or users.
class HelpTipsStore {
  enabled: boolean = localStorage.getItem(STORAGE_KEY) !== 'false' // default: on

  constructor() { makeAutoObservable(this) }

  setEnabled(v: boolean) {
    this.enabled = v
    localStorage.setItem(STORAGE_KEY, String(v))
  }
}

export const helpTipsStore = new HelpTipsStore()
