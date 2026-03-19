import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
import tailwindcss from '@tailwindcss/vite'

export default defineConfig({
  plugins: [react(), tailwindcss()],
  build: {
    rolldownOptions: {
      output: {
        codeSplitting: {
          groups: [
            { name: 'vendor-react', test: /node_modules[\\/](react|react-dom|react-router)/ },
            { name: 'vendor-highlight', test: /node_modules[\\/](highlight\.js|rehype-highlight)/ },
          ],
        },
      },
    },
  },
  server: {
    proxy: {
      '/api': 'http://localhost:18080',
    },
  },
})
