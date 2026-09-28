# Security policy

## Report a vulnerability

Do not report security vulnerabilities in public GitHub issues.

Use GitHub Private Vulnerability Reporting:

1. Open the **Security** tab of the repository.
2. Click **Report a vulnerability**.
3. Include the affected version or commit, the steps to reproduce, and the impact.

## Supported versions

webspine is pre-1.0. Fixes land on `main` and ship in the next tagged release.

## Threat model

webspine loads pages from the site you give it in headless Chromium and downloads their images. Treat that site as untrusted input. These issues are in scope:

- A page that makes webspine read or write files outside its workspace or output path.
- A page that makes webspine fetch URLs outside the entry scope, except the images that the page references.
- Markup that survives the EPUB cleaner as script or active content.
- Memory-safety bugs in the HTML, image, or archive handling.

Chromium runs with its own sandbox, except when webspine runs as root. As root, webspine passes `--no-sandbox`, so do not scrape untrusted sites as root.
