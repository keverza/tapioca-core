import { svelte } from '@sveltejs/vite-plugin-svelte'
import { defineConfig } from 'vite'
import { viteSingleFile } from 'vite-plugin-singlefile'

export default defineConfig({
  plugins: [
    {
      name: 'tapioca-development-csp',
      apply: 'serve',
      transformIndexHtml(html) {
        return html.replace(
          "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; img-src data: blob:; connect-src 'none'",
          "default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; img-src data: blob:; connect-src 'self' ws:",
        )
      },
    },
    svelte(),
    viteSingleFile(),
  ],
  build: {
    assetsInlineLimit: Number.MAX_SAFE_INTEGER,
    cssCodeSplit: false,
    modulePreload: false,
    sourcemap: false,
    target: 'chrome111',
  },
})
