import tailwindcss from "@tailwindcss/vite"
import react from "@vitejs/plugin-react"
import { defineConfig } from "vite"

export default defineConfig({
  plugins: [react(), tailwindcss()],
  build: {
    // 网关挂载 wwwroot/dist，源码和依赖保留在 wwwroot 内。
    outDir: "dist",
  },
  server: {
    proxy: { "/api": "http://127.0.0.1:9000" },
  },
})
