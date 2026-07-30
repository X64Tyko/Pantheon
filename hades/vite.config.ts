import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
import tailwindcss from '@tailwindcss/vite'
import {appVersion} from './scripts/app-version.mjs'

export default defineConfig({
    define: {
        __APP_VERSION__: JSON.stringify(appVersion),
    },
  plugins: [react(), tailwindcss()],
  server: {
    host: true,
    port: 5173,
    proxy: {
      '/api': {
        target: 'http://localhost:8000',  // dev: route through Hermes
        changeOrigin: true,
      },
      '/stream': {
        target: 'http://localhost:8000',
        changeOrigin: true,
      },
    }
  },
  build: {
    outDir: './dist',
    emptyOutDir: true,
    // The default 500kb warning is a raw byte-size check that doesn't know a
    // chunk is lazily loaded — player/PlayerPage.tsx is React.lazy()'d
    // in App.tsx. We've moved hls.js to a separate 'player' manual chunk,
    // and the core application 'index' is now ~600kb.
    chunkSizeWarningLimit: 800,
    rollupOptions: {
      output: {
        manualChunks: {
          vendor: ['react', 'react-dom', 'react-router-dom'],
          state:  ['mobx', 'mobx-react-lite'],
          player: ['hls.js'],
        },
      },
    },
  }
})
