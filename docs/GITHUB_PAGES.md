# GitHub Pages deployment

The static operator console is published from `public/` by
`.github/workflows/pages.yml`. The expected project-site URL is:

`https://arkansasio.github.io/server-mainframe-program/`

## Enable Pages

1. Open **Settings → Pages** in this repository:
   https://github.com/ArkansasIo/server-mainframe-program/settings/pages
2. Under **Build and deployment**, choose **GitHub Actions** as the source.
3. Open **Actions → Deploy GitHub Pages** and wait for both the build and deploy jobs to succeed.
4. Open the Pages URL above. If the first request returns 404, confirm Pages is enabled and the latest workflow completed successfully, then retry after GitHub's deployment has propagated.

The workflow validates that the HTML, stylesheet, favicon, and all dashboard scripts exist before publishing the contents of `public/`. The HTML uses relative asset paths so project-subpath hosting works.

## Static site versus backend

GitHub Pages serves static files only. It does not run the Node.js HTTP API, SQLite database, or TCP services. To load live data, deploy the API separately on an HTTPS-capable host, configure its CORS allowlist to permit `https://arkansasio.github.io`, and open the Pages URL with its API origin:

`https://arkansasio.github.io/server-mainframe-program/?api_base=https://YOUR-API-HOST`

Replace `https://YOUR-API-HOST` with the real API origin (origin only, no endpoint path). The dashboard remembers the origin for the current browser tab. For a protected API, supply its API key using the dashboard's supported `api_key` flow; do not place secrets in repository files or hard-code them in the static site.

## Troubleshooting

- **404 on the site URL:** Pages source must be **GitHub Actions**, and the Deploy GitHub Pages workflow must have a successful deploy job.
- **404 for JavaScript or CSS:** verify all files exist in `public/` and use relative `./` asset paths.
- **Dashboard loads but shows API/network errors:** deploy the backend separately and check the API URL, HTTPS, authentication, and CORS response headers.
- **Changes do not appear:** check the latest workflow run and hard-refresh after deployment.
