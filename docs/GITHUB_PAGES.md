# GitHub Pages deployment

The operator console in `public/` is a static frontend and can be hosted on GitHub Pages. The Node.js HTTP API, SQLite database, and TCP services cannot run on GitHub Pages; host those on a separate server.

## Enable Pages

1. Merge the Pages workflow to `main`.
2. Open **Settings → Pages** in this repository.
3. Set **Build and deployment → Source** to **GitHub Actions**.
4. Open **Actions → Deploy GitHub Pages** and wait for the deployment to succeed.
5. The project site URL will normally be `https://arkansasio.github.io/server-mainframe-program/`.

The workflow publishes only `public/`, so server source, database files, configuration and secrets are not uploaded as the site artifact.

## Connect the frontend to a separately hosted API

By default the static console will try the same origin for `/api/...`. To connect it to a separately hosted Node API, open the Pages URL with `?api_base=https://YOUR-API-HOST`. Use the API's HTTPS origin only (no trailing path is required). The API host must allow the Pages origin through its CORS configuration. Do not put a secret API key in a shared bookmark or public link.

The API needs to be reachable from browsers and configured with appropriate authentication, CORS allowlisting, HTTPS, and network controls. Never expose an unauthenticated administrative API publicly. GitHub Pages provides static hosting only; it does not host the backend, persistent database, or TCP terminal/transfer ports.

## Troubleshooting

- A 404 for the site: confirm Pages source is **GitHub Actions** and inspect the latest workflow run.
- Missing CSS/JavaScript: asset paths in `public/index.html` must remain relative so they work under the repository subpath.
- API errors on the static site: deploy the backend separately, set `api_base`, and allow the Pages origin in backend CORS.
