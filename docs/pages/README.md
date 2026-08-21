# Tapioca documentation site

This directory is a dependency-free static site. It is published as the
GitHub Pages site for `keverza/tapioca-core` by

## Publish with GitHub Pages

1. Push the `core` repository changes to the `main` branch on GitHub.
2. In the repository, open **Settings > Pages**.
3. Under **Build and deployment > Source**, choose **GitHub Actions**.
4. Open the **Actions** tab and wait for **Deploy Tapioca documentation** to
   finish. The workflow prints the published URL in its deployment summary.

The workflow uploads only `docs/pages`, so `index.html` is the site root. It
runs on changes to the site or its workflow file and can also be started from
the **Actions** tab with **Run workflow**.

## Preview locally

From the `core` repository root, run:

```powershell
python -m http.server 8000 --directory docs/pages
```

Open <http://localhost:8000> in a browser. Stop the server with `Ctrl+C`.

## Adding documentation

Keep pages as plain HTML and use relative links between them. Put shared visual
rules in `styles.css`. Deeper technical source documents remain in `docs/` and
should be linked to their GitHub source when they are not yet represented as
website pages.
