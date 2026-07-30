import {existsSync, readFileSync} from 'node:fs'
import {fileURLToPath} from 'node:url'
import {resolve, dirname} from 'node:path'

// Single source of truth for the app version (see VERSION at repo root) —
// bump that file only. It lives one level up in the monorepo, but the
// Docker build flattens hades/ into the same directory as VERSION (see
// hades/Dockerfile), so check both locations.
const scriptsDir = dirname(fileURLToPath(import.meta.url))
const versionFile = [resolve(scriptsDir, '../../VERSION'), resolve(scriptsDir, '../VERSION')]
    .find(existsSync)

if (!versionFile) {
    throw new Error('VERSION file not found — expected at repo root')
}

export const appVersion = readFileSync(versionFile, 'utf-8').trim()
