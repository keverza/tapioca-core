# Tapioca documentation site

This directory is an Astro + Starlight documentation site. It is published as
the GitHub Pages site for `keverza/tapioca-core` by
`.github/workflows/pages.yml`.

The page structure uses the core and miscellaneous content types from [The
Good Docs Project templates](https://gitlab.com/tgdp/templates) as a starting
point. The current mapping is:

| Documentation type | Site page |
| --- | --- |
| Project README / overview | `src/content/docs/index.md` |
| Quickstart and installation guide | `src/content/docs/getting-started.md` |
| Concept and how-to | `src/content/docs/commands.md` |
| API getting started and reference overview | `src/content/docs/api.md` |
| Troubleshooting | `src/content/docs/troubleshooting.md` |
| Development and contributing | `src/content/docs/development.md` |

The template repository is licensed under MIT No Attribution. The site keeps
its own content and styling, and links back to the template project as a
structural reference.

## Publish with GitHub Pages

1. Push the `core` repository changes, including `package-lock.json`, to the
   `main` branch on GitHub.
2. In the repository, open **Settings > Pages**.
3. Under **Build and deployment > Source**, choose **GitHub Actions**.
4. Open the **Actions** tab and wait for **Deploy Tapioca documentation** to
   finish. The workflow prints the published URL in its deployment summary.

The workflow installs the locked npm dependencies, runs `npm run build`, and
uploads only `docs/pages/dist`. The generated Starlight site is the Pages root.
It runs on changes to the site or its workflow file and can also be started
from the **Actions** tab with **Run workflow**.

## Preview locally

From `core/docs/pages`, install dependencies once and start Starlight:

```powershell
npm install
npm run dev
```

Open the URL printed by Astro. Because the deployed site uses the repository
base path, the local URL usually includes `/tapioca-core/`.

To test the production build locally:

```powershell
npm run build
npm run preview
```

## Adding documentation

Add Markdown or MDX pages under `src/content/docs/`. Configure the navigation
and site metadata in `astro.config.mjs`, and put shared visual rules in
`src/styles/custom.css`. Deeper technical source documents remain in `docs/`
and should be linked to their GitHub source when they are not yet represented
as website pages.

The `site` and `base` values in `astro.config.mjs` target
`https://keverza.github.io/tapioca-core`. Change both if the repository is
published under another GitHub Pages project URL.
