import { readdir, readFile } from 'node:fs/promises'
import { extname, join, relative } from 'node:path'

const distDir = new URL('../dist/', import.meta.url)

async function listFiles(directory) {
  const entries = await readdir(directory, { withFileTypes: true })
  const files = await Promise.all(entries.map(async (entry) => {
    const path = new URL(`${entry.name}${entry.isDirectory() ? '/' : ''}`, directory)
    return entry.isDirectory() ? listFiles(path) : [path]
  }))
  return files.flat()
}

const files = await listFiles(distDir)
const relativeFiles = files.map((file) => relative(distDir.pathname, file.pathname))

if (files.length !== 1 || relativeFiles[0] !== 'index.html') {
  throw new Error(`Expected only dist/index.html, found: ${relativeFiles.join(', ') || 'nothing'}`)
}

const html = await readFile(files[0], 'utf8')
const forbiddenReferences = [
  /<(?:script|link)\b[^>]*(?:src|href)\s*=\s*["'][^"']+["']/iu,
  /\b(?:https?:\/\/|\/\/localhost\b|localhost:)/iu,
  /\b(?:src|href)\s*=\s*["'](?!data:|#)[^"']+["']/iu,
  /\burl\(\s*["']?(?!data:|#)[^)"']+/iu,
  /@import\s+(?:url\()?\s*["']?(?!data:)[^;"')]+/iu,
  /\b(?:fetch|WebSocket|EventSource)\s*\(/u,
]

for (const pattern of forbiddenReferences) {
  if (pattern.test(html)) {
    throw new Error(`Generated HTML contains a forbidden external reference or network call: ${pattern}`)
  }
}

if (extname(files[0].pathname) !== '.html' || !html.includes('<!doctype html>')) {
  throw new Error('Generated artifact is not a complete HTML document')
}

console.log(`Verified self-contained artifact: ${join('dist', 'index.html')} (${html.length} UTF-8 characters)`)
