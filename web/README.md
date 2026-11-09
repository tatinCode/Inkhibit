# Inkhibit frontend prototype

From the repository root, serve the frontend with Python:

```sh
python -m http.server 8080 --bind 127.0.0.1 --directory web
```

Open <http://127.0.0.1:8080>. No npm install or frontend build is needed.

## Try the gallery

1. Enter any nonempty username and password on the demo login screen.
2. Choose or drop a JPG/PNG in the artwork editor (up to 20 MB and 24 megapixels).
3. Adjust crop/fit and nearest-color/Floyd–Steinberg processing.
4. Add the processed snapshot to the gallery, or download its preview as a PNG.
5. Add a second artwork to enable the browser slideshow (3, 5, or 10 minutes).
6. Select thumbnails to change the exhibited piece. Use Remove to open a
   confirmation dialog; Escape cancels it.

The collection displays **24 processed snapshots per page**. Use First,
Previous, numbered shortcuts, Next, and Last to browse, or enter any valid page
number and press Enter or Go. The page summary shows the current page, total
pages, and total artwork count. Invalid, fractional, and out-of-range page
numbers are rejected without navigating.

New artwork opens the last page; deleting the last artwork on a page moves back
when necessary. Browsing pages does not change the exhibited artwork or stop
the slideshow, which always rotates through the entire collection.

There is no fixed artwork-count cap, but all snapshots still use browser memory.
Pagination limits rendered thumbnails, not total image storage. Changing editor
settings does not change existing gallery items. The preview colors are
approximate, not calibrated representations of the physical panel.

## Prototype boundaries

- **The login is not authentication.** No password is checked, stored, or sent.
  This static frontend must not be used as an access-control boundary.
- Images remain in this tab's memory; there is no upload endpoint or persistence.
  Reloading clears the collection. Logging out hides the dashboard, clears the
  editor, and pauses the slideshow, but keeps the tab's collection in memory.
- Slideshow changes affect the browser only. There are no hardware commands,
  and the physical display button is disabled.
- The interval selector cannot choose less than 180 seconds. Future C++ backend
  code must independently enforce hardware refresh limits across clients and
  restarts; browser controls are not a safety mechanism.
- Image decoding is delegated to the browser. The pixel limit is checked after
  decoding and is not a strict limit on decoder memory use.

Real authentication, gallery storage, upload validation, device state, and
slideshow scheduling will be implemented in the C++ backend later.

## Optional browser regression tests

The frontend itself still needs no npm packages. The regression suite uses
Node.js, Playwright, and headless Chromium as development-only tools. For
example, install them outside the repository and run from the repository root:

```sh
npm install --prefix /tmp/opencode/inkhibit-ui-tests --no-audit --no-fund playwright
node /tmp/opencode/inkhibit-ui-tests/node_modules/playwright/cli.js install chromium
NODE_PATH=/tmp/opencode/inkhibit-ui-tests/node_modules node web/tests/pagination.cjs
```

The test starts and stops its own local static server. It checks page
boundaries, shortcuts, manual jumps and invalid input, adding across page
boundaries, deletion, full-collection slideshow playback, and mobile layouts.
It uses synthetic images and never contacts physical hardware.
