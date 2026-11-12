import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

// Relative base so the same build works from nginx, `vite preview`, or an Electron file:// load later.
export default defineConfig({
  base: './',
  plugins: [react()],
  server: { host: '127.0.0.1', port: 5173, strictPort: true },
  // One local bundle (editor + React + the embedded WebAssembly core, ~225 KB gzipped) is fine for a localhost tool.
  build: { chunkSizeWarningLimit: 1000 },
  test: { environment: 'node' },
});
