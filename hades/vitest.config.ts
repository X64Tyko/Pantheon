import { defineConfig } from 'vitest/config'
import path from 'path'

export default defineConfig({
  resolve: {
    alias: {
      // Lets momus test files import with '@/...' instead of '../../../hades/src/...'
      '@': path.resolve(__dirname, './src'),
    },
  },
  test: {
    environment: 'node',
    include: ['../momus/hades/**/*.test.ts', '../momus/hades/**/*.test.tsx'],
  },
})
