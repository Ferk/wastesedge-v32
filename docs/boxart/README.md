# Box art

`boxart.svg` is the editable source. It references PNG files in this directory.
After changing it or those images, regenerate the PNG used in the main README
and release assets with librsvg:

```sh
rsvg-convert --output docs/boxart/WastesEdge-boxart.png docs/boxart/boxart.svg
```

The SVG uses system fonts, so check the exported text when regenerating it.
