# Tapioca Notebook UI

This is the browser-only workspace for the embedded notebook experiment. Its
production output is one self-contained `dist/index.html`; do not edit that
generated file or load it from disk at runtime.

## Browser Iteration

Install the pinned dependencies once with `npm ci`, then run `npm run dev` from
this directory. Vite prints the local preview URL and reloads it when source
files change.

Run `npm run typecheck` and `npm run build` before handing changes to the native
host. The build fails if `dist` contains anything except `index.html`, or if the
HTML references external scripts, styles, localhost, or HTTP resources.

## Archicad Integration

Close Archicad and run `tools\build\Build-AddOn29.ps1` from the repository root in native Windows PowerShell.
CMake runs `npm ci` and the production build before compiling add-on resources.
The generated HTML is a build input for the later resource-embedding task; no
Vite server is required by the add-on.
