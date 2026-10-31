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

The collection supports up to 30 processed snapshots. Changing editor settings
does not change existing gallery items. The preview colors are approximate, not
calibrated representations of the physical panel.

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
