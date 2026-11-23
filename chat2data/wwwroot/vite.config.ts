import tailwindcss from "@tailwindcss/vite"
import react from "@vitejs/plugin-react"
import { defineConfig, loadEnv } from "vite"

export default defineConfig(({ mode }) => {
  // 开发期网关地址通过环境变量 GATEWAY_URL 配置，生产环境同源访问。
  const env = loadEnv(mode, process.cwd(), "")
  const gateway = env.GATEWAY_URL || "http://127.0.0.1:9000"
  return {
    // 相对路径保证从网关挂载点下可正常加载资源。
    base: "./",
    plugins: [react(), tailwindcss()],
    build: {
      // 网关挂载 wwwroot/dist，源码和依赖保留在 wwwroot 内。
      outDir: "dist",
    },
    server: {
      proxy: { "/api": gateway, "/health": gateway },
    },
  }
})
