import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
import tailwindcss from '@tailwindcss/vite'

// Static-hostable build: relative asset paths so the dist/ folder can be
// dropped behind Caddy at any mount point, exactly like the old frontend.
export default defineConfig({
  base: './',
  plugins: [react(), tailwindcss()],
})
