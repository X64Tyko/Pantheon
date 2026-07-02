import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
import tailwindcss from '@tailwindcss/vite'

export default defineConfig({
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
    // chunk is lazily loaded — player/PlayerPage.tsx (hls.js + friends) is
    // React.lazy()'d in App.tsx specifically so its ~540kb only downloads
    // when someone opens the player, not on every page load. Bumped just
    // above that chunk so the warning still fires for anything genuinely
    // bloating the *eager* bundle.
    chunkSizeWarningLimit: 600,
    rollupOptions: {
      output: {
        manualChunks: {
          vendor: ['react', 'react-dom', 'react-router-dom'],
          state:  ['mobx', 'mobx-react-lite'],
        },
      },
    },
  }
})
