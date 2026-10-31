# Inkhibit
a Raspberry Pi-powered digital art frame that transforms uploaded images into colorful e-ink artwork. Featuring a self-hosted web interface and image optimization, Inkhibit brings a miniature, customizable art gallery to your wall.

## Frontend prototype

See [web/README.md](web/README.md) for the browser-only login, gallery, image
editor, and slideshow demo. It needs no frontend build or npm dependencies.

```sh
python -m http.server 8080 --bind 127.0.0.1 --directory web
```

Open <http://127.0.0.1:8080>. The login is a mock, images are temporary, and no
physical display is updated. The C++ backend will be added separately.
