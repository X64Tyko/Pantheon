import { defineConfig } from 'vitest/config'
import path from 'path'
import {appVersion} from './scripts/app-version.mjs'

export default defineConfig({
    define: {
        __APP_VERSION__: JSON.stringify(appVersion),
    },
  resolve: {
    alias: {
      // Lets momus test files import with '@/...' instead of '../../../hades/src/...'
      '@': path.resolve(__dirname, './src'),
    },
  },
  test: {
    environment: 'node',
    setupFiles: ['../momus/hades/setup.ts'],
    include: ['../momus/hades/**/*.test.ts', '../momus/hades/**/*.test.tsx'],
  },
})
