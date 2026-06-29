# World basemaps (bundled, public domain)

The map-board feature reads up to two equirectangular world images from the
project working directory. Both are **public domain** — embed freely, no
attribution required. They are gitignored (`*.jpg`); acquire them once with the
steps below. A map board renders a flat placeholder tint (no crash) if its
basemap file is absent.

| File | mesh `basemap` value | Source | License |
|------|----------------------|--------|---------|
| `basemap_relief.jpg`    | `relief` (default) | Natural Earth raster — naturalearthdata.com | Public domain |
| `basemap_satellite.jpg` | `satellite`        | NASA Blue Marble Next Generation — visibleearth.nasa.gov | Public domain |

Target format for both: **equirectangular, 8192×4096** (2:1). That is ≈134 MB of
VRAM per basemap; they load lazily, so an unused style never loads.

## relief — DONE

Generated from the **Natural Earth II, High-Res, Land + Coast, Shaded Relief,
Water, Drainages** set (`NE2_HR_LC_SR_W_DR`, a 21600×10800 GeoTIFF). The
built-in macOS `sips` struggles with the 668 MB GeoTIFF; ImageMagick handles it:

    magick natural_earth_data_NE2_HR_LC_SR_W_DR/NE2_HR_LC_SR_W_DR.tif \
        -resize 8192x4096 -quality 88 basemap_relief.jpg

(The "Unknown field with tag …" warnings are just unread GeoTIFF geo-tags —
harmless.) Source is exactly 2:1, so `-resize 8192x4096` lands on 8192×4096.

## satellite — to add

Download a single equirectangular Blue Marble JPEG from NASA Visible Earth
(search "Blue Marble Next Generation"; the "Land Surface, Shallow Water, and
Shaded Topography" images come as single equirect JPEGs at sizes up to
21600×10800). Then downsample to the bundled size:

    magick bluemarble_source.jpg -resize 8192x4096 -quality 88 basemap_satellite.jpg
    # or, if the source is already near size, macOS sips:
    # sips -s format jpeg -z 4096 8192 bluemarble_source.jpg --out basemap_satellite.jpg

Until this file exists, maps placed with the `satellite` style show the
placeholder tint; the `relief` style works regardless.
