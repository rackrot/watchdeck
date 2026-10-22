//! Button icons: a PNG, JPEG, WebP or SVG in `icons/`, decoded (an SVG is drawn), fitted to its
//! button and converted to the device's pixel format. The picture's hash names it on the wire.

use std::cell::Cell;
use std::collections::HashMap;
use std::fs::{self, DirBuilder, File};
use std::io::{Cursor, Read};
use std::os::unix::fs::{DirBuilderExt, MetadataExt};
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use std::time::SystemTime;

use image::imageops::{self, FilterType};
use image::metadata::Orientation;
use image::{DynamicImage, ImageDecoder, ImageFormat, ImageReader, Limits, RgbaImage};
use resvg::{tiny_skia, usvg};
use sha2::{Digest, Sha256};

use super::config::Config;
use super::protocol::{encode_hex, IconFrame, ICON_MAX_SIDE};
use super::store::write_private;

/// Where icons live, inside the config dir.
pub const DIR: &str = "icons";
/// The largest image file the app reads.
pub const MAX_FILE_BYTES: u64 = 2 * 1024 * 1024;
/// The largest image the app decodes, per side. Bigger pictures are refused, not scaled.
pub const MAX_SOURCE_SIDE: u32 = 4096;
/// Caps what the decoder may allocate, so a small file can't expand into gigabytes.
const MAX_DECODE_BYTES: u64 = 128 * 1024 * 1024;
// What [`check_svg`] allows in the XML, checked before usvg runs. They bound what usvg builds.
/// The largest SVG file. Rasters have [`MAX_FILE_BYTES`].
const MAX_SVG_BYTES: usize = 256 * 1024;
/// Elements nested below the root.
const MAX_SVG_DEPTH: usize = 64;
/// Attributes, namespace declarations included, on one element. roxmltree checks for duplicates
/// in time that grows with the square of this, so it is counted before roxmltree parses.
const MAX_SVG_ATTRIBUTES: usize = 64;
/// `xmlns` declarations in the whole file. The subset needs two at most.
const MAX_SVG_NAMESPACES: usize = 16;
/// Elements, text runs and comments together, for roxmltree.
const MAX_SVG_XML_NODES: u32 = 10_000;
/// Elements once each `<use>` is counted as a copy of what it names.
const MAX_SVG_NODES: usize = 2_000;
const MAX_SVG_USES: usize = 64;
/// Bytes of `d` and `points`, with each `<use>` again counting what it copies.
const MAX_SVG_PATH_DATA: usize = 256 * 1024;
const MAX_SVG_GRADIENT_STOPS: usize = 32;
const MAX_SVG_STOPS: usize = 256;
/// The longest run of gradients that inherit from each other with `href`.
const MAX_SVG_HREF_CHAIN: usize = 8;
// What [`render_cost`] allows of the tree usvg built, checked before resvg draws it. Measured on
// tiny-skia 0.12 built with optimizations (the dev profile builds it so too), the units are about
// a nanosecond: a solid fill costs about 0.3 per pixel, a 32-stop gradient 11, a hairline stroke
// 90 µs per segment that crosses the box, and filling a path whose segments all cross each other
// about 3 per segment squared.
/// About half a second.
const MAX_SVG_RENDER_COST: f64 = 5e8;
/// Pixels of the group layers resvg allocates, 4 bytes each: 32 MB.
const MAX_SVG_LAYER_PIXELS: f64 = 8.0 * 1024.0 * 1024.0;
/// usvg and resvg recurse once per nesting level, `<use>` copies included, and debug frames are
/// large, so an SVG is drawn on a thread of its own with this much stack.
const SVG_STACK_BYTES: usize = 16 * 1024 * 1024;
const SVG_NS: &str = "http://www.w3.org/2000/svg";
const XLINK_NS: &str = "http://www.w3.org/1999/xlink";
const XML_NS: &str = "http://www.w3.org/XML/1998/namespace";
/// The only elements an SVG icon may use, all in the SVG namespace: plain shapes, groups,
/// gradients and `<use>`. Anything that makes the renderer recurse or allocate by its own say-so
/// is left out: `pattern`, `mask`, `clipPath`, `marker`, `filter` and its primitives, `image`,
/// `text`, `switch`, `foreignObject`, `script`, `animate`, `style`, and a nested `svg`.
const SVG_ELEMENTS: &[&str] = &[
    "svg",
    "g",
    "defs",
    "path",
    "rect",
    "circle",
    "ellipse",
    "line",
    "polyline",
    "polygon",
    "linearGradient",
    "radialGradient",
    "stop",
    "use",
    "symbol",
    "title",
    "desc",
    "metadata",
];
/// Presentation properties allowed both as attributes and in a `style` attribute. Left out, among
/// others: `filter`, `mask`, `clip-path`, `marker*`, `stroke-dasharray`, `mix-blend-mode`,
/// `isolation`, and the font and text properties.
const SVG_PROPERTIES: &[&str] = &[
    "fill",
    "fill-opacity",
    "fill-rule",
    "stroke",
    "stroke-width",
    "stroke-linecap",
    "stroke-linejoin",
    "stroke-miterlimit",
    "stroke-opacity",
    "stroke-dashoffset",
    "opacity",
    "color",
    "display",
    "visibility",
    "stop-color",
    "stop-opacity",
    "overflow",
    "paint-order",
    "shape-rendering",
    "clip-rule",
];
/// Other attributes allowed with no namespace: geometry, links, and a few that usvg ignores but
/// icon sets carry (`class` does nothing, since no stylesheet reaches usvg).
const SVG_ATTRIBUTES: &[&str] = &[
    "id",
    "style",
    "class",
    "version",
    "baseProfile",
    "role",
    "focusable",
    "enable-background",
    "viewBox",
    "preserveAspectRatio",
    "x",
    "y",
    "width",
    "height",
    "d",
    "points",
    "rx",
    "ry",
    "cx",
    "cy",
    "r",
    "x1",
    "y1",
    "x2",
    "y2",
    "fx",
    "fy",
    "fr",
    "transform",
    "href",
    "offset",
    "gradientUnits",
    "gradientTransform",
    "spreadMethod",
];
const MAX_NAME_BYTES: usize = 63;

// The device's button geometry, from `firmware/main/grid.c` (`grid_geometry`; GRID_TOP, GRID_GAP,
// ICON_PAD, ICON_GAP, PAGER_H) and `display.h`. A button with an icon is a column: the icon, then a
// label of fixed height, with padding around them. LABEL_LINE is the line box of text slot 32
// (Montserrat 32's line height), the same for every font: the firmware lays out by the slot, never
// by the font.
const SCREEN_W: u32 = 1024;
const SCREEN_H: u32 = 600;
const GRID_TOP: u32 = 204;
const GRID_GAP: u32 = 8;
/// The strip under the grid for "Page x of y", which a layout of 2 or more pages takes from the grid.
pub const PAGER_H: u32 = 31;
const ICON_PAD: u32 = 4;
const ICON_GAP: u32 = 2;
const LABEL_LINE: u32 = 35;
/// A label gets two lines unless that leaves the icon less than this.
const MIN_ICON_SIDE: u32 = 24;

/// The icons of a config by file name. A name that several buttons share appears once.
pub type IconSet = HashMap<String, Arc<IconFrame>>;

/// A cell's width and height, and the lines of label an icon button has room for.
fn cell_geometry(cols: u32, rows: u32, paged: bool) -> (u32, u32, u32) {
    let grid_h = SCREEN_H - GRID_TOP - if paged { PAGER_H } else { 0 };
    let cell_w = (SCREEN_W - (cols + 1) * GRID_GAP) / cols;
    let cell_h = (grid_h - rows * GRID_GAP) / rows;
    let room = cell_h - 2 * ICON_PAD;
    let lines = if room >= 2 * LABEL_LINE + ICON_GAP + MIN_ICON_SIDE {
        2
    } else {
        1
    };
    (cell_w, cell_h, lines)
}

/// The side of the square an icon must fit in on a `cols` x `rows` grid, in pixels: what is left
/// of the button after its padding and a label of two lines, or of one where two would leave the
/// icon under 24 px. That is 256 with one row, 110 with two, 44 with three, 46 with four and 26
/// with five, or less where a cell is narrower than that. `paged` is a layout of 2 or more pages,
/// whose grid is `PAGER_H` shorter: 256, 94, 33, 38 and 20. The grid must already be valid, so both
/// are at least 1.
pub fn box_px(cols: u32, rows: u32, paged: bool) -> u32 {
    let (cell_w, cell_h, lines) = cell_geometry(cols, rows, paged);
    let room = cell_h - 2 * ICON_PAD;
    (cell_w - 2 * ICON_PAD)
        .min(room - lines * LABEL_LINE - ICON_GAP)
        .min(ICON_MAX_SIDE as u32)
}

/// Why `name` can't name a file in `icons/`, or `None` if it can. It is a plain file name, so
/// it can't reach outside the directory.
pub fn name_error(name: &str) -> Option<String> {
    let plain = !name.is_empty()
        && name.len() <= MAX_NAME_BYTES
        && !name.starts_with('.')
        && name
            .bytes()
            .all(|c| c.is_ascii_alphanumeric() || matches!(c, b'_' | b'-' | b'.'));
    (!plain).then(|| {
        format!(
            "icon must be a file name in {DIR}/: 1 to {MAX_NAME_BYTES} letters, digits, '_', '-' \
             or '.', not starting with '.'"
        )
    })
}

/// Which version of a file a rendered icon came from.
type FileStamp = (SystemTime, u64, u64);

struct Cached {
    stamp: FileStamp,
    side: u32,
    icon: Arc<IconFrame>,
}

/// The `icons/` directory, and the icons rendered from it. A render is kept until its file or
/// its box changes, so a reload decodes only what is new.
pub struct Icons {
    dir: PathBuf,
    cache: Mutex<HashMap<String, Cached>>,
}

impl Icons {
    pub fn new(dir: PathBuf) -> Self {
        Self {
            dir,
            cache: Mutex::default(),
        }
    }

    /// Reads and renders every icon `config` names, which must already pass [`Config::check`].
    /// Every file that fails is reported. The cache lock is held only to look up and to store,
    /// never while decoding.
    pub fn load(&self, config: &Config) -> Result<IconSet, Vec<String>> {
        let side = box_px(config.layout.cols, config.layout.rows, false);
        let mut icons = IconSet::new();
        let mut errors = Vec::new();
        for button in &config.buttons {
            let Some(name) = &button.icon else { continue };
            if icons.contains_key(name) {
                continue;
            }
            match self.render_file(name, side) {
                Ok(icon) => {
                    icons.insert(name.clone(), icon);
                }
                Err(e) => errors.push(format!("button {:?}: icon {name:?}: {e}", button.id)),
            }
        }
        if !errors.is_empty() {
            return Err(errors);
        }
        self.cache().retain(|name, _| icons.contains_key(name));
        Ok(icons)
    }

    fn cache(&self) -> std::sync::MutexGuard<'_, HashMap<String, Cached>> {
        self.cache.lock().expect("icon cache poisoned")
    }

    fn render_file(&self, name: &str, side: u32) -> Result<Arc<IconFrame>, String> {
        let path = self.dir.join(name);
        let stamp = file_stamp(&path)?;
        if let Some(hit) = self.cache().get(name) {
            if hit.stamp == stamp && hit.side == side {
                return Ok(hit.icon.clone());
            }
        }
        let bytes = read_image(&path)?;
        name_matches_contents(name, &bytes)?;
        let icon = Arc::new(render(&bytes, side)?);
        self.cache().insert(
            name.to_string(),
            Cached {
                stamp,
                side,
                icon: icon.clone(),
            },
        );
        Ok(icon)
    }

    /// Copies the image at `src` into the directory under a name made from its content, such as
    /// `3f2a9c1d0e44.png`, and returns that name for `icon` in `config.toml`. The same file gives
    /// the same name. Only a PNG, JPEG, WebP or SVG within the limits is copied.
    pub fn import(&self, src: &Path) -> Result<String, String> {
        let bytes = read_image(src).map_err(|e| format!("{}: {e}", src.display()))?;
        let (_, kind) = decode(&bytes, 1).map_err(|e| format!("{}: {e}", src.display()))?;
        let ext = match kind {
            Kind::Svg => "svg",
            Kind::Raster(ImageFormat::Png) => "png",
            Kind::Raster(ImageFormat::Jpeg) => "jpg",
            Kind::Raster(_) => "webp",
        };
        let hash = encode_hex(&Sha256::digest(&bytes));
        let name = format!("{}.{ext}", &hash[..12]);
        DirBuilder::new()
            .recursive(true)
            .mode(0o700)
            .create(&self.dir)
            .and_then(|()| write_private(&self.dir.join(&name), &bytes))
            .map_err(|e| format!("Could not save the icon in {}: {e}", self.dir.display()))?;
        log::info!("imported {src:?} as {DIR}/{name}");
        Ok(name)
    }

    /// The files in the directory that a button's `icon` could name, sorted: valid names
    /// ([`name_error`]) of regular files that decode within the limits and don't link out of the
    /// directory. An absent directory holds none.
    pub fn list(&self) -> Result<Vec<String>, String> {
        let entries = match fs::read_dir(&self.dir) {
            Ok(entries) => entries,
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(Vec::new()),
            Err(e) => return Err(format!("Could not read {}: {e}", self.dir.display())),
        };
        let root = fs::canonicalize(&self.dir).map_err(|e| e.to_string())?;
        let mut names: Vec<String> = entries
            .filter_map(Result::ok)
            .filter_map(|entry| entry.file_name().into_string().ok())
            .filter(|name| name_error(name).is_none())
            .filter(|name| {
                let path = self.dir.join(name);
                fs::canonicalize(&path).is_ok_and(|real| real.starts_with(&root))
                    && read_image(&path).is_ok_and(|bytes| {
                        name_matches_contents(name, &bytes).is_ok() && decode(&bytes, 1).is_ok()
                    })
            })
            .collect();
        names.sort();
        Ok(names)
    }
}

fn file_stamp(path: &Path) -> Result<FileStamp, String> {
    let meta = fs::metadata(path).map_err(|e| match e.kind() {
        std::io::ErrorKind::NotFound => "not found".to_string(),
        _ => e.to_string(),
    })?;
    let modified = meta.modified().map_err(|e| e.to_string())?;
    Ok((modified, meta.len(), meta.ino()))
}

/// Decodes `bytes` and fits the picture to a `side` x `side` square, keeping its shape. A raster
/// that already fits is left as it is; an SVG is drawn at the size that fills the square.
pub fn render(bytes: &[u8], side: u32) -> Result<IconFrame, String> {
    let (picture, kind) = decode(bytes, side)?;
    let (w, h) = picture.dimensions();
    let picture = if kind == Kind::Svg || (w <= side && h <= side) {
        picture
    } else {
        let (nw, nh) = if w >= h {
            (side, (h * side + w / 2) / w)
        } else {
            ((w * side + h / 2) / h, side)
        };
        shrink(picture, nw.max(1), nh.max(1))
    };
    let (w, h) = picture.dimensions();
    // LVGL's ARGB8888 is B, G, R, A in memory.
    let pixels = picture
        .pixels()
        .flat_map(|p| [p[2], p[1], p[0], p[3]])
        .collect();
    Ok(IconFrame::new(w as u16, h as u16, pixels))
}

#[derive(PartialEq)]
enum Kind {
    Raster(ImageFormat),
    Svg,
}

/// SVG is text, so it has no magic number. Anything that begins with `<` (after a BOM and white
/// space) is taken for one and parsed as such.
fn looks_like_svg(bytes: &[u8]) -> bool {
    let bytes = bytes.strip_prefix(b"\xef\xbb\xbf").unwrap_or(bytes);
    bytes.trim_ascii_start().first() == Some(&b'<')
}

/// Decodes `bytes` to straight RGBA. An SVG is drawn to fit `side`; a raster keeps its own size.
/// The preview loads the file through the asset protocol, which picks the content type from the
/// name, so SVG text in a `.png` would be served as something else, and a picture in a `.svg`
/// would not draw. Both are refused.
fn name_matches_contents(name: &str, bytes: &[u8]) -> Result<(), String> {
    match (looks_like_svg(bytes), name.ends_with(".svg")) {
        (true, false) => Err("an SVG must be in a file whose name ends in .svg".into()),
        (false, true) => Err("a file named .svg must hold an SVG".into()),
        _ => Ok(()),
    }
}

fn decode(bytes: &[u8], side: u32) -> Result<(RgbaImage, Kind), String> {
    if looks_like_svg(bytes) {
        return draw_svg(bytes, side).map(|picture| (picture, Kind::Svg));
    }
    let mut reader = ImageReader::new(Cursor::new(bytes))
        .with_guessed_format()
        .map_err(|e| e.to_string())?;
    let format = reader
        .format()
        .filter(|f| matches!(f, ImageFormat::Png | ImageFormat::Jpeg | ImageFormat::WebP))
        .ok_or("not a PNG, JPEG, WebP or SVG image")?;
    if format == ImageFormat::WebP {
        // image-webp ignores the limits below, so its headers get checked first.
        check_webp(bytes)?;
    }
    let mut limits = Limits::default();
    limits.max_image_width = Some(MAX_SOURCE_SIDE);
    limits.max_image_height = Some(MAX_SOURCE_SIDE);
    limits.max_alloc = Some(MAX_DECODE_BYTES);
    reader.limits(limits);
    let fail =
        |e| format!("can't decode the image (at most {MAX_SOURCE_SIDE}x{MAX_SOURCE_SIDE}): {e}");
    let mut decoder = reader.into_decoder().map_err(fail)?;
    // A missing or unreadable orientation tag means upright.
    let orientation = decoder.orientation().unwrap_or(Orientation::NoTransforms);
    let mut picture = DynamicImage::from_decoder(decoder).map_err(fail)?;
    picture.apply_orientation(orientation);
    Ok((picture.into_rgba8(), Kind::Raster(format)))
}

/// Draws an SVG, as untrusted input, into the largest rectangle of its shape that fits
/// `side` x `side`, scaling up as well as down, and returns straight (not premultiplied) RGBA.
///
/// It runs on a thread of its own with a stack of [`SVG_STACK_BYTES`], and waits for it. A panic
/// there is passed on as it is, as if it happened here; the release profile aborts on one, so
/// nothing relies on catching it.
fn draw_svg(bytes: &[u8], side: u32) -> Result<RgbaImage, String> {
    std::thread::scope(|scope| {
        std::thread::Builder::new()
            .name("svg".into())
            .stack_size(SVG_STACK_BYTES)
            .spawn_scoped(scope, || draw_svg_here(bytes, side))
            .map_err(|e| format!("can't start drawing the SVG: {e}"))?
            .join()
            .unwrap_or_else(|panic| std::panic::resume_unwind(panic))
    })
}

/// [`draw_svg`] on the calling thread.
fn draw_svg_here(bytes: &[u8], side: u32) -> Result<RgbaImage, String> {
    let text = std::str::from_utf8(bytes).map_err(|_| "can't read the SVG: not UTF-8")?;
    let doc = check_svg(text)?;
    let tree = svg_tree(&doc)?;
    let (pw, ph) = pixmap_size(&tree, side);
    let (w, h) = (tree.size().width(), tree.size().height());
    let mut pixmap = tiny_skia::Pixmap::new(pw, ph).ok_or("the SVG has no drawable size")?;
    let transform = tiny_skia::Transform::from_scale(pw as f32 / w, ph as f32 / h);
    resvg::render(&tree, transform, &mut pixmap.as_mut());
    let rgba = pixmap
        .pixels()
        .iter()
        .flat_map(|p| {
            let c = p.demultiply();
            [c.red(), c.green(), c.blue(), c.alpha()]
        })
        .collect();
    RgbaImage::from_raw(pw, ph, rgba).ok_or_else(|| "the SVG drew an unexpected size".to_string())
}

/// The pixels `tree` is drawn into to fit `side` x `side`. Neither side rounds to zero, so a very
/// thin drawing is stretched across its one row or column.
fn pixmap_size(tree: &usvg::Tree, side: u32) -> (u32, u32) {
    let (w, h) = (
        f64::from(tree.size().width()),
        f64::from(tree.size().height()),
    );
    let scale = f64::from(side) / w.max(h);
    let fit = |n: f64| ((n * scale).round() as u32).clamp(1, side);
    (fit(w), fit(h))
}

/// Builds usvg's tree from a document that passed [`check_svg`], and refuses it if its size is
/// out of bounds or drawing it would cost more than [`render_cost`] allows. The file stands
/// alone: both href resolvers refuse, `resources_dir` is unset and no fonts are loaded.
fn svg_tree(doc: &roxmltree::Document) -> Result<usvg::Tree, String> {
    let options = usvg::Options {
        resources_dir: None,
        image_href_resolver: usvg::ImageHrefResolver {
            resolve_data: Box::new(|_, _, _| None),
            resolve_string: Box::new(|_, _| None),
        },
        ..usvg::Options::default()
    };
    let tree =
        usvg::Tree::from_xmltree(doc, &options).map_err(|e| format!("can't read the SVG: {e}"))?;
    let (w, h) = (tree.size().width(), tree.size().height());
    let max = MAX_SOURCE_SIDE as f32;
    if !(w >= 1.0 / 1024.0 && h >= 1.0 / 1024.0 && w <= max && h <= max) {
        return Err(format!(
            "the SVG's size {w}x{h} is outside 0.001 to {MAX_SOURCE_SIDE} units a side"
        ));
    }
    let (work, layers) = render_cost(&tree);
    // A NaN would compare as within bounds, so it is refused by name.
    if work.is_nan() || work > MAX_SVG_RENDER_COST {
        return Err(format!(
            "the SVG would take too long to draw ({:.1} times the limit): use fewer or smaller \
             shapes, gradients, path segments or opacity groups",
            work / MAX_SVG_RENDER_COST
        ));
    }
    if layers.is_nan() || layers > MAX_SVG_LAYER_PIXELS {
        return Err(
            "the SVG needs too much memory to draw: fewer or smaller opacity groups and symbols"
                .into(),
        );
    }
    Ok(tree)
}

/// An upper estimate of what resvg spends drawing `tree` into the largest box, whatever box it
/// is drawn into, and of the pixels it allocates for group layers. It follows what resvg 0.48
/// does: a path is filled and stroked over its bounding box, cut to the canvas or to the layer of
/// the group it is in, and each of its segments is walked once per row it spans. A group with
/// opacity, or a clip (a `<symbol>` that `<use>` sizes), is drawn into a layer of its bounding
/// box, at most 5 times the canvas a side, and then composited.
fn render_cost(tree: &usvg::Tree) -> (f64, f64) {
    // Weights, from the measurements at MAX_SVG_RENDER_COST, rounded up.
    const SOLID: f64 = 1.0;
    const GRADIENT: f64 = 16.0;
    const LAYER: f64 = 8.0;
    const EDGE_ROW: f64 = 16.0;
    const STROKE_PIXEL: f64 = 256.0;
    const FILL_CROSSING: f64 = 4.0;
    const STROKE_CROSSING: f64 = 12.0;

    /// Left, top, right and bottom, in pixels.
    type Box4 = [f64; 4];
    fn cut(r: Box4, by: Box4) -> Box4 {
        [
            r[0].max(by[0]),
            r[1].max(by[1]),
            r[2].min(by[2]),
            r[3].min(by[3]),
        ]
    }
    fn size(r: Box4) -> (f64, f64) {
        ((r[2] - r[0]).max(0.0), (r[3] - r[1]).max(0.0))
    }
    struct Walk {
        scale: (f64, f64),
        reach: Box4,
        work: f64,
        layers: f64,
    }
    impl Walk {
        fn device(&self, r: usvg::Rect) -> Box4 {
            let (sx, sy) = self.scale;
            [
                f64::from(r.left()) * sx,
                f64::from(r.top()) * sy,
                f64::from(r.right()) * sx,
                f64::from(r.bottom()) * sy,
            ]
        }
        fn paint(paint: &usvg::Paint) -> f64 {
            match paint {
                usvg::Paint::Color(_) => SOLID,
                _ => GRADIENT,
            }
        }
        fn group(&mut self, group: &usvg::Group, target: Box4) {
            for node in group.children() {
                match node {
                    usvg::Node::Group(g) if g.should_isolate() => {
                        let r = self.device(g.abs_layer_bounding_box().to_rect());
                        let layer =
                            cut([r[0] - 2.0, r[1] - 2.0, r[2] + 2.0, r[3] + 2.0], self.reach);
                        let (w, h) = size(layer);
                        let passes = if g.clip_path().is_some() { 2.0 } else { 1.0 };
                        self.layers += w * h * passes;
                        self.work += w * h * passes * LAYER;
                        if let Some(clip) = g.clip_path() {
                            self.group(clip.root(), layer);
                        }
                        self.group(g, layer);
                    }
                    usvg::Node::Group(g) => self.group(g, target),
                    usvg::Node::Path(p) if p.is_visible() => {
                        let segments = p.data().len() as f64;
                        if let Some(fill) = p.fill() {
                            let (w, h) = size(cut(self.device(p.abs_bounding_box()), target));
                            self.work += w * h * Self::paint(fill.paint())
                                + segments * h * EDGE_ROW
                                + segments * segments * FILL_CROSSING;
                        }
                        if let Some(stroke) = p.stroke() {
                            let r = self.device(p.abs_stroke_bounding_box());
                            let (w, h) = size(cut(r, target));
                            self.work += w * h * Self::paint(stroke.paint())
                                + segments * (w + h) * STROKE_PIXEL
                                + segments * segments * STROKE_CROSSING;
                        }
                    }
                    usvg::Node::Path(_) => {}
                    // check_svg lets through nothing that makes these.
                    usvg::Node::Image(_) | usvg::Node::Text(_) => self.work = f64::INFINITY,
                }
            }
        }
    }

    // The pixmap draw_svg_here uses for the largest box. A smaller box has fewer pixels, or the
    // same single row or column of a very thin drawing.
    let (pw, ph) = pixmap_size(tree, ICON_MAX_SIDE as u32);
    let (pw, ph) = (f64::from(pw), f64::from(ph));
    let scale = (
        pw / f64::from(tree.size().width()),
        ph / f64::from(tree.size().height()),
    );
    let mut walk = Walk {
        scale,
        // resvg's largest layer, `max_filter_bbox` in resvg's lib.rs.
        reach: [-2.0 * pw, -2.0 * ph, 3.0 * pw, 3.0 * ph],
        work: 0.0,
        layers: 0.0,
    };
    walk.group(tree.root(), [0.0, 0.0, pw, ph]);
    (walk.work, walk.layers)
}

fn unsupported(what: &str) -> String {
    format!("the SVG uses {what}, which isn't supported")
}

/// Parses the XML of an SVG (no DTD, so no entities) and refuses anything but a strict subset,
/// before usvg sees it. Every element must be in the SVG namespace and on [`SVG_ELEMENTS`], and
/// every attribute on [`SVG_ATTRIBUTES`] or [`SVG_PROPERTIES`] with no namespace, or be
/// `xlink:href` or `xml:space`. usvg reads attributes by local name in several namespaces, so
/// after this each name it looks up means one attribute. Every element counts against the limits,
/// the ones inside `defs` too, and a `<use>` counts again what it copies.
fn check_svg(text: &str) -> Result<roxmltree::Document<'_>, String> {
    if text.len() > MAX_SVG_BYTES {
        return Err(format!("the SVG is over {} KB", MAX_SVG_BYTES / 1024));
    }
    check_tags(text)?;
    let options = roxmltree::ParsingOptions {
        allow_dtd: false,
        nodes_limit: MAX_SVG_XML_NODES,
        ..Default::default()
    };
    let doc = roxmltree::Document::parse_with_options(text, options).map_err(|e| match e {
        roxmltree::Error::DtdDetected => unsupported("a DOCTYPE"),
        roxmltree::Error::NodesLimitReached => format!(
            "the SVG has more than {MAX_SVG_XML_NODES} XML nodes (elements, text and comments)"
        ),
        e => format!("can't read the SVG: {e}"),
    })?;

    let root = doc.root_element();
    if root.tag_name().namespace() != Some(SVG_NS) || root.tag_name().name() != "svg" {
        return Err("can't read the SVG: the root is not an <svg> in the SVG namespace".into());
    }
    let mut ids = HashMap::new();
    for node in doc.descendants().filter(|n| n.is_element()) {
        check_element(node, root)?;
        if let Some(id) = node.attribute("id") {
            if ids.insert(id, node).is_some() {
                return Err(unsupported(&format!("the id {id:?} more than once")));
            }
        }
    }
    let has_size = root.has_attribute("viewBox")
        || (root.has_attribute("width") && root.has_attribute("height"));
    if !has_size {
        return Err("the SVG needs a viewBox, or a width and a height".into());
    }

    // What each element costs, and what usvg makes of it: it copies the target of a <use>.
    let weight = |node: roxmltree::Node| {
        node.descendants()
            .filter(|n| n.is_element())
            .fold((0, 0), |(elements, data), n| {
                let geometry = ["d", "points"].map(|a| n.attribute(a).map_or(0, str::len));
                (elements + 1, data + geometry.iter().sum::<usize>())
            })
    };
    let (mut elements, mut path_data) = weight(root);
    let (mut uses, mut stops) = (0, 0);
    for node in doc.descendants().filter(|n| n.is_element()) {
        match node.tag_name().name() {
            "use" => {
                uses += 1;
                if uses > MAX_SVG_USES {
                    return Err(format!(
                        "the SVG has more than {MAX_SVG_USES} <use> elements"
                    ));
                }
                if let Some(target) = href_target(node, &ids) {
                    // A <use> that reaches another <use> would multiply the copies.
                    let nested = target
                        .descendants()
                        .any(|n| n.is_element() && n.tag_name().name() == "use");
                    if nested {
                        return Err(unsupported("a <use> that reaches another <use>"));
                    }
                    let (more, data) = weight(target);
                    elements += more;
                    path_data += data;
                }
            }
            "linearGradient" | "radialGradient" => {
                let own = node
                    .children()
                    .filter(|n| n.has_tag_name((SVG_NS, "stop")))
                    .count();
                if own > MAX_SVG_GRADIENT_STOPS {
                    return Err(format!(
                        "the SVG has a gradient with more than {MAX_SVG_GRADIENT_STOPS} stops"
                    ));
                }
                let mut at = node;
                for _ in 0..MAX_SVG_HREF_CHAIN {
                    match href_target(at, &ids) {
                        Some(next) => at = next,
                        None => break,
                    }
                }
                if href_target(at, &ids).is_some() {
                    return Err(unsupported(&format!(
                        "gradients that inherit from each other more than {MAX_SVG_HREF_CHAIN} deep"
                    )));
                }
            }
            "stop" => {
                stops += 1;
                if stops > MAX_SVG_STOPS {
                    return Err(format!(
                        "the SVG has more than {MAX_SVG_STOPS} gradient stops"
                    ));
                }
            }
            _ => {}
        }
    }
    if elements > MAX_SVG_NODES {
        return Err(format!(
            "the SVG has more than {MAX_SVG_NODES} elements, counting what each <use> copies"
        ));
    }
    if path_data > MAX_SVG_PATH_DATA {
        return Err(format!(
            "the SVG has more than {} KB of path data, counting what each <use> copies",
            MAX_SVG_PATH_DATA / 1024
        ));
    }
    Ok(doc)
}

/// Refuses an element, or any of its attributes, that isn't in the strict subset [`check_svg`]
/// describes, a `style` with a property that isn't on [`SVG_PROPERTIES`], and an `href` that
/// isn't `#id` or that comes as both `href` and `xlink:href`.
fn check_element(node: roxmltree::Node, root: roxmltree::Node) -> Result<(), String> {
    let tag = node.tag_name();
    let name = tag.name();
    if tag.namespace() != Some(SVG_NS) {
        let ns = tag.namespace().unwrap_or("no namespace");
        return Err(unsupported(&format!(
            "<{name}> outside the SVG namespace ({ns})"
        )));
    }
    if !SVG_ELEMENTS.contains(&name) || (name == "svg" && node != root) {
        return Err(unsupported(&format!("<{name}>")));
    }
    let mut hrefs = 0;
    for attr in node.attributes() {
        let key = attr.name();
        let allowed = match attr.namespace() {
            None => {
                SVG_ATTRIBUTES.contains(&key)
                    || SVG_PROPERTIES.contains(&key)
                    || key.starts_with("aria-")
                    || key.starts_with("data-")
            }
            Some(XLINK_NS) => key == "href",
            Some(XML_NS) => key == "space",
            Some(_) => false,
        };
        if !allowed {
            return Err(match attr.namespace() {
                None => unsupported(&format!("the {key} attribute on <{name}>")),
                Some(ns) => unsupported(&format!("the {key} attribute in the namespace {ns}")),
            });
        }
        match key {
            "style" => check_style(attr.value())?,
            "href" => {
                hrefs += 1;
                let id = attr.value().strip_prefix('#').unwrap_or_default();
                if id.is_empty() || id.contains(|c: char| c.is_ascii_whitespace()) {
                    return Err(unsupported(
                        "an href that isn't \"#id\", or leaves the file",
                    ));
                }
            }
            _ => {}
        }
    }
    if hrefs > 1 {
        return Err(unsupported(&format!(
            "both href and xlink:href on <{name}>"
        )));
    }
    Ok(())
}

/// Refuses a `style` attribute that sets anything not on [`SVG_PROPERTIES`]. It is split into
/// declarations by the same tokenizer usvg uses, so both see the same properties.
fn check_style(css: &str) -> Result<(), String> {
    for declaration in simplecss::DeclarationTokenizer::from(css) {
        if !SVG_PROPERTIES.contains(&declaration.name) {
            return Err(unsupported(&format!(
                "the CSS property {} in a style attribute",
                declaration.name
            )));
        }
    }
    Ok(())
}

/// Refuses XML nested deeper than [`MAX_SVG_DEPTH`] below the root, an element with more than
/// [`MAX_SVG_ATTRIBUTES`] attributes, and any DOCTYPE, without parsing it. roxmltree's tokenizer
/// recurses once per open element and only stops at its node limit (tens of thousands deep),
/// which overflows a thread's stack, and it checks an element's attributes for duplicates in time
/// that grows with the square of their number. So both are counted first by walking the tags:
/// comments, CDATA and processing instructions are skipped, and quotes in a tag are honored, so
/// neither count is ever lower than the parser's. Each `=` outside quotes in a tag is one
/// attribute or namespace declaration. Namespace declarations in the whole file are capped at
/// [`MAX_SVG_NAMESPACES`] too, because roxmltree searches all those in scope for each prefix.
fn check_tags(text: &str) -> Result<(), String> {
    let b = text.as_bytes();
    let skip_to = |from: usize, end: &[u8]| {
        b.get(from..)
            .and_then(|tail| tail.windows(end.len()).position(|w| w == end))
            .map_or(b.len(), |at| from + at + end.len())
    };
    let (mut i, mut depth, mut namespaces) = (0, 0usize, 0);
    while let Some(at) = b[i..].iter().position(|&c| c == b'<') {
        i += at;
        let rest = &b[i..];
        if rest.starts_with(b"<!--") {
            i = skip_to(i + 4, b"-->");
        } else if rest.starts_with(b"<![CDATA[") {
            i = skip_to(i + 9, b"]]>");
        } else if rest.starts_with(b"<?") {
            i = skip_to(i + 2, b"?>");
        } else if rest.starts_with(b"<!") {
            return Err("the SVG uses a DOCTYPE, which isn't supported".into());
        } else {
            // A start or end tag, up to the `>` that is not inside quotes.
            let mut quote = None;
            let mut attributes = 0;
            let mut j = i + 1;
            while j < b.len() {
                match (quote, b[j]) {
                    (None, q @ (b'"' | b'\'')) => quote = Some(q),
                    (Some(q), c) if c == q => quote = None,
                    (None, b'=') => attributes += 1,
                    (None, b'>') => break,
                    (None, b'x') if b[j..].starts_with(b"xmlns") => namespaces += 1,
                    _ => {}
                }
                j += 1;
            }
            if attributes > MAX_SVG_ATTRIBUTES {
                return Err(format!(
                    "the SVG has an element with more than {MAX_SVG_ATTRIBUTES} attributes"
                ));
            }
            if namespaces > MAX_SVG_NAMESPACES {
                return Err(format!(
                    "the SVG declares more than {MAX_SVG_NAMESPACES} namespaces"
                ));
            }
            if rest.starts_with(b"</") {
                depth = depth.saturating_sub(1);
            } else {
                // The root is at depth 1, and an empty element counts as deep as one with
                // children.
                if depth + 1 > MAX_SVG_DEPTH + 1 {
                    return Err(format!(
                        "the SVG nests deeper than {MAX_SVG_DEPTH} elements"
                    ));
                }
                if !(j < b.len() && b[j - 1] == b'/') {
                    depth += 1;
                }
            }
            i = (j + 1).min(b.len());
        }
    }
    Ok(())
}

/// The element a `href="#id"` names, if it is in the document. [`check_element`] has made sure
/// there is at most one `href`, and that it is `#` and an id with no spaces, which usvg reads the
/// same way.
fn href_target<'a, 'i>(
    node: roxmltree::Node<'a, 'i>,
    ids: &HashMap<&str, roxmltree::Node<'a, 'i>>,
) -> Option<roxmltree::Node<'a, 'i>> {
    let href = node.attribute("href")?.strip_prefix('#')?;
    ids.get(href).copied()
}

/// Called with a chunk's four-byte tag and its payload.
type ChunkFn<'a> = dyn FnMut(&[u8], &[u8]) -> Result<(), String> + 'a;

/// Refuses a WebP whose headers claim a size past [`MAX_SOURCE_SIDE`], or that disagree with
/// each other, before the decoder allocates for it. A small file can name a 16384 x 16384
/// picture (about 1 GB of pixels), and image-webp 0.2 allocates for that before it cross-checks
/// the canvas. Walks the RIFF chunks (at most 64) and reads each bitstream's own header: `VP8 `
/// (lossy), `VP8L` (lossless), and the frames inside `ANMF`.
fn check_webp(bytes: &[u8]) -> Result<(), String> {
    const MAX_CHUNKS: usize = 64;
    let bad = |what: &str| format!("not a usable WebP: {what}");
    let le = |b: &[u8]| b.iter().rev().fold(0u32, |n, &x| n << 8 | u32::from(x));
    if bytes.len() < 12 || &bytes[..4] != b"RIFF" || &bytes[8..12] != b"WEBP" {
        return Err(bad("no RIFF header"));
    }
    let budget = Cell::new(MAX_CHUNKS);
    // Calls `each` with the fourcc and payload of every chunk in `data`.
    let chunks = |data: &[u8], each: &mut ChunkFn| {
        let mut at = 0;
        while at < data.len() {
            if budget.get() == 0 {
                return Err(bad("more than 64 chunks"));
            }
            budget.set(budget.get() - 1);
            let head = data.get(at..at + 8).ok_or_else(|| bad("truncated chunk"))?;
            let size = u32::from_le_bytes([head[4], head[5], head[6], head[7]]) as usize;
            let payload = data
                .get(at + 8..at + 8 + size)
                .ok_or_else(|| bad("truncated chunk"))?;
            each(&head[..4], payload)?;
            at += 8 + size + (size & 1);
        }
        Ok(())
    };
    // The size a lossy or lossless bitstream claims, or `None` for any other chunk.
    let bitstream = |fourcc: &[u8], p: &[u8]| match fourcc {
        b"VP8 " if p.len() >= 10 && p[3..6] == [0x9d, 0x01, 0x2a] => {
            let side = |i: usize| u32::from(u16::from_le_bytes([p[i], p[i + 1]]) & 0x3fff);
            Ok(Some((side(6), side(8))))
        }
        b"VP8L" if p.len() >= 5 && p[0] == 0x2f => {
            let bits = u32::from_le_bytes([p[1], p[2], p[3], p[4]]);
            Ok(Some(((bits & 0x3fff) + 1, (bits >> 14 & 0x3fff) + 1)))
        }
        b"VP8 " | b"VP8L" => Err(bad("bad bitstream header")),
        _ => Ok(None),
    };
    let over = |w: u32, h: u32| {
        if w > MAX_SOURCE_SIDE || h > MAX_SOURCE_SIDE {
            return Err(bad(&format!(
                "{w}x{h} is over {MAX_SOURCE_SIDE}x{MAX_SOURCE_SIDE}"
            )));
        }
        Ok(())
    };
    let mut canvas = None;
    let mut images = 0;
    chunks(&bytes[12..], &mut |fourcc, p| {
        match fourcc {
            b"VP8X" => {
                if p.len() < 10 {
                    return Err(bad("short VP8X"));
                }
                let (w, h) = (le(&p[4..7]) + 1, le(&p[7..10]) + 1);
                over(w, h)?;
                canvas = Some((w, h));
            }
            b"ANMF" => {
                if p.len() < 16 {
                    return Err(bad("short ANMF"));
                }
                let (x, y) = (le(&p[0..3]) * 2, le(&p[3..6]) * 2);
                let (w, h) = (le(&p[6..9]) + 1, le(&p[9..12]) + 1);
                over(w, h)?;
                if canvas.is_none_or(|(cw, ch)| x + w > cw || y + h > ch) {
                    return Err(bad("an animation frame outside its canvas"));
                }
                let mut frame_images = 0;
                chunks(&p[16..], &mut |fourcc, p| match bitstream(fourcc, p)? {
                    Some(size) if size != (w, h) => {
                        Err(bad("a frame that disagrees with its header"))
                    }
                    Some(_) => {
                        frame_images += 1;
                        Ok(())
                    }
                    None => Ok(()),
                })?;
                if frame_images == 0 {
                    return Err(bad("an animation frame with no image"));
                }
                images += 1;
            }
            _ => {
                if let Some((w, h)) = bitstream(fourcc, p)? {
                    over(w, h)?;
                    if canvas.is_some_and(|c| c != (w, h)) {
                        return Err(bad("a bitstream that disagrees with the canvas"));
                    }
                    images += 1;
                }
            }
        }
        Ok(())
    })?;
    if images == 0 {
        return Err(bad("no image data"));
    }
    Ok(())
}

/// Resizes through premultiplied alpha, which is what the filter assumes. Otherwise the color
/// of fully transparent pixels bleeds into the edges.
fn shrink(mut picture: RgbaImage, w: u32, h: u32) -> RgbaImage {
    for p in picture.pixels_mut() {
        let a = u32::from(p[3]);
        for c in &mut p.0[..3] {
            *c = ((u32::from(*c) * a + 127) / 255) as u8;
        }
    }
    let mut small = imageops::resize(&picture, w, h, FilterType::Lanczos3);
    for p in small.pixels_mut() {
        let a = u32::from(p[3]);
        for c in &mut p.0[..3] {
            *c = match a {
                0 => 0,
                _ => ((u32::from(*c) * 255 + a / 2) / a).min(255) as u8,
            };
        }
    }
    small
}

/// A regular file of at most [`MAX_FILE_BYTES`].
fn read_image(path: &Path) -> Result<Vec<u8>, String> {
    let meta = fs::metadata(path).map_err(|e| match e.kind() {
        std::io::ErrorKind::NotFound => "not found".into(),
        _ => e.to_string(),
    })?;
    if !meta.is_file() {
        return Err("not a regular file".into());
    }
    let too_big = || format!("over {} KB", MAX_FILE_BYTES / 1024);
    if meta.len() > MAX_FILE_BYTES {
        return Err(too_big());
    }
    let mut bytes = Vec::new();
    File::open(path)
        .and_then(|file| file.take(MAX_FILE_BYTES + 1).read_to_end(&mut bytes))
        .map_err(|e| e.to_string())?;
    if bytes.len() as u64 > MAX_FILE_BYTES {
        return Err(too_big());
    }
    Ok(bytes)
}

#[cfg(test)]
mod tests {
    use std::os::unix::fs::PermissionsExt;

    use image::Rgba;

    use super::*;
    use crate::daemon::protocol::{decode_hex, ICON_HEADER_BYTES, ICON_ID_BYTES};

    fn encoded(picture: &RgbaImage, format: ImageFormat) -> Vec<u8> {
        let mut out = Cursor::new(Vec::new());
        match format {
            // JPEG has no alpha channel.
            ImageFormat::Jpeg => image::DynamicImage::ImageRgba8(picture.clone())
                .to_rgb8()
                .write_to(&mut out, format),
            _ => picture.write_to(&mut out, format),
        }
        .unwrap();
        out.into_inner()
    }

    /// Red, green and blue quadrants and a fully transparent one, `side` pixels square.
    fn quadrants(side: u32) -> RgbaImage {
        RgbaImage::from_fn(side, side, |x, y| match (x < side / 2, y < side / 2) {
            (true, true) => Rgba([255, 0, 0, 255]),
            (false, true) => Rgba([0, 255, 0, 255]),
            (true, false) => Rgba([0, 0, 255, 255]),
            (false, false) => Rgba([0, 0, 0, 0]),
        })
    }

    #[test]
    fn a_picture_that_fits_renders_to_exact_bgra_bytes_and_a_hash_named_frame() {
        let png = encoded(&quadrants(2), ImageFormat::Png);
        let icon = render(&png, 4).unwrap();
        assert_eq!((icon.w, icon.h), (2, 2));
        assert_eq!(
            icon.pixels,
            [
                0, 0, 255, 255, // red
                0, 255, 0, 255, // green
                255, 0, 0, 255, // blue
                0, 0, 0, 0, // transparent
            ]
        );

        // The hash of the dimensions and pixels, computed independently with Python's hashlib.
        assert_eq!(encode_hex(&icon.id), "19d52ebc8817c79e37fe51a16333055c");
        let frame = icon.encode();
        assert_eq!(frame.len(), ICON_HEADER_BYTES + 16);
        assert_eq!(&frame[..4], b"WDI1");
        assert_eq!(frame[4..4 + ICON_ID_BYTES], icon.id);
        assert_eq!(frame[20..24], [2, 0, 2, 0]);
        assert_eq!(frame[24..], icon.pixels[..]);
        assert_eq!(IconFrame::decode(&frame).unwrap(), icon);
    }

    #[test]
    fn the_same_picture_gets_the_same_id_and_another_picture_another() {
        let a = render(&encoded(&quadrants(8), ImageFormat::Png), 8).unwrap();
        let b = render(&encoded(&quadrants(8), ImageFormat::Png), 8).unwrap();
        let mut other = quadrants(8);
        other.put_pixel(0, 0, Rgba([1, 0, 0, 255]));
        let c = render(&encoded(&other, ImageFormat::Png), 8).unwrap();
        assert_eq!(a.id, b.id);
        assert_ne!(a.id, c.id);
    }

    #[test]
    fn a_big_picture_shrinks_to_fit_and_keeps_its_shape_but_a_small_one_stays() {
        let wide = RgbaImage::from_pixel(160, 80, Rgba([9, 9, 9, 255]));
        let icon = render(&encoded(&wide, ImageFormat::Png), 40).unwrap();
        assert_eq!((icon.w, icon.h), (40, 20));
        let tall = RgbaImage::from_pixel(30, 90, Rgba([9, 9, 9, 255]));
        let icon = render(&encoded(&tall, ImageFormat::Png), 45).unwrap();
        assert_eq!((icon.w, icon.h), (15, 45));
        let thin = RgbaImage::from_pixel(1000, 3, Rgba([9, 9, 9, 255]));
        let icon = render(&encoded(&thin, ImageFormat::Png), 40).unwrap();
        assert_eq!((icon.w, icon.h), (40, 1), "a side never rounds to zero");
        let small = RgbaImage::from_pixel(10, 10, Rgba([9, 9, 9, 255]));
        let icon = render(&encoded(&small, ImageFormat::Png), 40).unwrap();
        assert_eq!((icon.w, icon.h), (10, 10), "icons are never scaled up");
    }

    #[test]
    fn shrinking_does_not_pull_the_color_of_transparent_pixels_into_the_edge() {
        // Opaque white on the left, transparent black on the right.
        let picture = RgbaImage::from_fn(30, 30, |x, _| match x < 15 {
            true => Rgba([255, 255, 255, 255]),
            false => Rgba([0, 0, 0, 0]),
        });
        let icon = render(&encoded(&picture, ImageFormat::Png), 7).unwrap();
        let mut partly = 0;
        for px in icon.pixels.as_chunks::<4>().0 {
            if px[3] > 0 {
                assert!(px[..3].iter().all(|&c| c >= 253), "{px:?}");
            }
            partly += usize::from(px[3] > 0 && px[3] < 255);
        }
        assert!(partly > 0, "the test must cross a soft edge");
    }

    #[test]
    fn png_jpeg_and_webp_decode_and_everything_else_is_refused() {
        let picture = quadrants(8);
        for format in [ImageFormat::Png, ImageFormat::Jpeg, ImageFormat::WebP] {
            let icon =
                render(&encoded(&picture, format), 8).unwrap_or_else(|e| panic!("{format:?}: {e}"));
            assert_eq!((icon.w, icon.h), (8, 8));
        }
        for (what, bytes) in [
            ("a GIF", b"GIF89a\x01\x00\x01\x00\x00\x00\x00;".to_vec()),
            ("text", b"hello".to_vec()),
            ("nothing", Vec::new()),
        ] {
            let e = render(&bytes, 8).unwrap_err();
            assert!(e.contains("PNG, JPEG, WebP or SVG"), "{what}: {e}");
        }
    }

    #[test]
    fn a_truncated_image_is_an_error_not_a_crash() {
        let png = encoded(&quadrants(16), ImageFormat::Png);
        let e = render(&png[..png.len() / 2], 8).unwrap_err();
        assert!(e.contains("can't decode"), "{e}");
    }

    #[test]
    fn a_picture_over_4096_pixels_a_side_is_refused() {
        let wide = RgbaImage::from_pixel(MAX_SOURCE_SIDE + 1, 1, Rgba([0, 0, 0, 255]));
        let e = render(&encoded(&wide, ImageFormat::Png), 8).unwrap_err();
        assert!(e.contains("at most 4096x4096"), "{e}");
        let edge = RgbaImage::from_pixel(MAX_SOURCE_SIDE, 1, Rgba([0, 0, 0, 255]));
        let icon = render(&encoded(&edge, ImageFormat::Png), 8).unwrap();
        assert_eq!((icon.w, icon.h), (8, 1));
    }

    #[test]
    fn the_box_follows_the_firmware_button() {
        // ui.c: a button with an icon is padded 4 px, then the icon, a 2 px gap, and a label of
        // lines * 35 px. Two lines while that leaves 24 px, else one. The rows decide it.
        // The rows set the height. Only a single row is tall enough for the width to bind, from
        // 4 columns on, where a cell is narrower than 256 px plus its padding.
        let widths = [1000, 492, 322, 238, 187, 153, 129, 111]; // cell width less the padding
        for (cols, width) in (1..=8).zip(widths) {
            for (rows, high) in [(1, 256), (2, 110), (3, 44), (4, 46), (5, 26)] {
                assert_eq!(box_px(cols, rows, false), high.min(width), "{cols}x{rows}");
            }
            for (rows, high) in [(1, 256), (2, 94), (3, 33), (4, 38), (5, 20)] {
                assert_eq!(
                    box_px(cols, rows, true),
                    high.min(width),
                    "{cols}x{rows} paged"
                );
            }
        }
        // 5x3 cells are 195x124 px: 116 inside the padding, 116 - 70 - 2 = 44. Paged: 195x113.
        assert_eq!(box_px(5, 3, false), 44);
        assert_eq!(box_px(5, 3, true), 33);
    }

    #[test]
    fn the_cells_follow_the_geometry_table() {
        // plans/pages.md, Geometry: (rows, cell height, label lines), full grid then paged. The
        // firmware (test_grid.c) and the preview (preview.test.ts) pin the same table.
        let full = [
            (1, 388, 2),
            (2, 190, 2),
            (3, 124, 2),
            (4, 91, 1),
            (5, 71, 1),
        ];
        let paged = [
            (1, 357, 2),
            (2, 174, 2),
            (3, 113, 2),
            (4, 83, 1),
            (5, 65, 1),
        ];
        for (is_paged, table) in [(false, full), (true, paged)] {
            for (rows, height, lines) in table {
                let (_, h, l) = cell_geometry(1, rows, is_paged);
                assert_eq!((h, l), (height, lines), "{rows} rows, paged {is_paged}");
            }
        }
        assert_eq!(PAGER_H, 27 + 4); // a slot-24 line box plus 4 px of margin
    }

    #[test]
    fn the_worst_icon_cache_fits_the_boards_cap() {
        // The board (icons.h ICONS_MAX_BYTES, 8 MiB) holds the icons of a whole layout of up to
        // PROTO_MAX_PAGES (4) pages at once, one per cell, each box_px squared in 4-byte pixels.
        const CAP: u32 = 8 * 1024 * 1024;
        const PAGES: u32 = 4;
        let mut worst = 0;
        for cols in 1..=8 {
            for rows in 1..=5 {
                let cells = cols * rows;
                for (pages, paged) in [(PAGES, true), (1, false)] {
                    let side = box_px(cols, rows, paged);
                    let bytes = pages * cells * side * side * 4;
                    assert!(bytes <= CAP, "{cols}x{rows} paged {paged}: {bytes} bytes");
                    worst = worst.max(bytes);
                }
            }
        }
        println!("worst icon cache: {worst} bytes");
        assert_eq!(worst, 3_625_216);
    }

    #[test]
    fn a_name_is_a_plain_file_name() {
        for good in ["a.png", "3f2a9c.png", "My-Icon_2.webp", "x"] {
            assert_eq!(name_error(good), None, "{good}");
        }
        for bad in [
            "",
            ".",
            "..",
            ".hidden.png",
            "../a.png",
            "a/b.png",
            "/etc/passwd",
            "a\\b.png",
            "a\0.png",
            "a b.png",
            "\u{e9}.png",
            &"a".repeat(MAX_NAME_BYTES + 1),
        ] {
            assert!(name_error(bad).is_some(), "{bad:?}");
        }
        assert_eq!(name_error(&"a".repeat(MAX_NAME_BYTES)), None);
    }

    fn config_with_icons(icons: &[(&str, &str)]) -> Config {
        let mut text = String::from("version = 1\n[layout]\ncols = 5\nrows = 3\n");
        for (i, (id, icon)) in icons.iter().enumerate() {
            text += &format!(
                "[[button]]\nid = \"{id}\"\npos = [{i}, 0]\nlabel = \"\"\nicon = \"{icon}\"\n\
                 action = {{ kind = \"shell\", argv = [\"true\"] }}\n"
            );
        }
        crate::daemon::config::parse(&text, &Ok(Vec::new())).unwrap()
    }

    #[test]
    fn load_renders_each_named_file_once_and_reports_every_failure() {
        let tmp = tempfile::tempdir().unwrap();
        let dir = tmp.path();
        fs::write(
            dir.join("a.png"),
            encoded(&quadrants(200), ImageFormat::Png),
        )
        .unwrap();
        fs::write(dir.join("junk.png"), b"not an image").unwrap();
        fs::create_dir(dir.join("dir.png")).unwrap();
        fs::write(dir.join("big.png"), vec![0; MAX_FILE_BYTES as usize + 1]).unwrap();

        let store = Icons::new(dir.to_path_buf());
        let icons = store
            .load(&config_with_icons(&[("one", "a.png"), ("two", "a.png")]))
            .unwrap();
        assert_eq!(icons.len(), 1);
        assert_eq!((icons["a.png"].w, icons["a.png"].h), (44, 44));

        let errors = store
            .load(&config_with_icons(&[
                ("ok", "a.png"),
                ("missing", "gone.png"),
                ("junk", "junk.png"),
                ("dir", "dir.png"),
                ("big", "big.png"),
            ]))
            .unwrap_err();
        assert_eq!(errors.len(), 4, "{errors:?}");
        for (error, want) in errors.iter().zip([
            "button \"missing\": icon \"gone.png\": not found",
            "button \"junk\": icon \"junk.png\": not a PNG, JPEG, WebP or SVG image",
            "button \"dir\": icon \"dir.png\": not a regular file",
            "button \"big\": icon \"big.png\": over 2048 KB",
        ]) {
            assert!(error.starts_with(want), "{error} vs {want}");
        }
    }

    #[test]
    fn import_copies_a_valid_image_under_a_hash_name_and_refuses_the_rest() {
        let tmp = tempfile::tempdir().unwrap();
        let src = tmp.path().join("my picture.PNG");
        fs::write(&src, encoded(&quadrants(16), ImageFormat::Png)).unwrap();
        let dir = tmp.path().join("config").join(DIR);

        let name = Icons::new(dir.clone()).import(&src).unwrap();
        assert!(
            name_error(&name).is_none() && name.ends_with(".png"),
            "{name}"
        );
        assert!(decode_hex::<6>(&name[..12]).is_some(), "{name}");
        assert_eq!(fs::read(dir.join(&name)).unwrap(), fs::read(&src).unwrap());
        assert_eq!(
            fs::metadata(&dir).unwrap().permissions().mode() & 0o777,
            0o700
        );
        assert_eq!(
            Icons::new(dir.clone()).import(&src).unwrap(),
            name,
            "the same file, the same name"
        );
        assert_eq!(fs::read_dir(&dir).unwrap().count(), 1);

        let jpg = tmp.path().join("p.bin");
        fs::write(&jpg, encoded(&quadrants(8), ImageFormat::Jpeg)).unwrap();
        assert!(Icons::new(dir.clone())
            .import(&jpg)
            .unwrap()
            .ends_with(".jpg"));

        let text = tmp.path().join("t.png");
        fs::write(&text, b"hello").unwrap();
        assert!(Icons::new(dir.clone())
            .import(&text)
            .unwrap_err()
            .contains("PNG, JPEG, WebP or SVG"));
        assert!(Icons::new(dir.clone())
            .import(&tmp.path().join("nope.png"))
            .unwrap_err()
            .contains("not found"));
        assert_eq!(
            fs::read_dir(&dir).unwrap().count(),
            2,
            "refusals copy nothing"
        );
    }

    #[test]
    fn list_offers_only_what_a_button_could_use() {
        let tmp = tempfile::tempdir().unwrap();
        let dir = tmp.path().join(DIR);
        let outside = tmp.path().join("outside.png");
        fs::create_dir(&dir).unwrap();
        fs::write(&outside, encoded(&quadrants(8), ImageFormat::Png)).unwrap();
        let png = encoded(&quadrants(8), ImageFormat::Png);
        fs::write(dir.join("b.png"), &png).unwrap();
        fs::write(dir.join("a.jpg"), encoded(&quadrants(8), ImageFormat::Jpeg)).unwrap();
        fs::write(
            dir.join("c.webp"),
            encoded(&quadrants(8), ImageFormat::WebP),
        )
        .unwrap();
        fs::write(dir.join("inside.png"), &png).unwrap();
        std::os::unix::fs::symlink(dir.join("inside.png"), dir.join("link-in.png")).unwrap();
        fs::write(dir.join(".hidden.png"), &png).unwrap();
        fs::write(dir.join("bad name.png"), &png).unwrap();
        fs::write(dir.join("notes.txt"), b"hello").unwrap();
        fs::write(dir.join("junk.png"), b"not an image").unwrap();
        fs::write(dir.join("big.png"), vec![0; MAX_FILE_BYTES as usize + 1]).unwrap();
        fs::create_dir(dir.join("dir.png")).unwrap();
        std::os::unix::fs::symlink(&outside, dir.join("escape.png")).unwrap();
        std::os::unix::fs::symlink("/nowhere", dir.join("dangling.png")).unwrap();

        let store = Icons::new(dir);
        assert_eq!(
            store.list().unwrap(),
            ["a.jpg", "b.png", "c.webp", "inside.png", "link-in.png"]
        );
        assert_eq!(
            Icons::new(tmp.path().join("absent")).list().unwrap(),
            Vec::<String>::new()
        );
    }

    #[test]
    fn a_reload_renders_only_what_changed() {
        let tmp = tempfile::tempdir().unwrap();
        let dir = tmp.path();
        fs::write(
            dir.join("a.png"),
            encoded(&quadrants(200), ImageFormat::Png),
        )
        .unwrap();
        fs::write(
            dir.join("b.png"),
            encoded(&quadrants(100), ImageFormat::Png),
        )
        .unwrap();
        let store = Icons::new(dir.to_path_buf());
        let config = config_with_icons(&[("one", "a.png"), ("two", "b.png")]);
        let first = store.load(&config).unwrap();

        fs::write(dir.join("b.png"), encoded(&quadrants(60), ImageFormat::Png)).unwrap();
        let second = store.load(&config).unwrap();
        assert!(
            Arc::ptr_eq(&first["a.png"], &second["a.png"]),
            "a.png is unchanged"
        );
        assert!(
            !Arc::ptr_eq(&first["b.png"], &second["b.png"]),
            "b.png was overwritten"
        );
        assert_eq!(second["b.png"].w, 44);

        // A different grid has a different box, so every icon is rendered again.
        let mut other = config.clone();
        other.layout.rows = 5;
        let third = store.load(&other).unwrap();
        assert!(!Arc::ptr_eq(&second["a.png"], &third["a.png"]));
        assert_eq!(third["a.png"].w, 26);

        // A name no config uses any more is forgotten.
        store.load(&config_with_icons(&[("one", "a.png")])).unwrap();
        assert_eq!(store.cache().len(), 1);
    }

    /// A RIFF file made of `(fourcc, payload)` chunks.
    fn riff(chunks: &[(&[u8; 4], Vec<u8>)]) -> Vec<u8> {
        let mut body = b"WEBP".to_vec();
        for (fourcc, payload) in chunks {
            body.extend_from_slice(*fourcc);
            body.extend_from_slice(&(payload.len() as u32).to_le_bytes());
            body.extend_from_slice(payload);
            body.resize(body.len() + payload.len() % 2, 0);
        }
        let mut file = b"RIFF".to_vec();
        file.extend_from_slice(&(body.len() as u32).to_le_bytes());
        file.extend(body);
        file
    }

    fn vp8x(w: u32, h: u32) -> Vec<u8> {
        let mut p = vec![0; 4];
        p.extend_from_slice(&(w - 1).to_le_bytes()[..3]);
        p.extend_from_slice(&(h - 1).to_le_bytes()[..3]);
        p
    }

    fn vp8(w: u16, h: u16) -> Vec<u8> {
        let mut p = vec![0, 0, 0, 0x9d, 0x01, 0x2a];
        p.extend_from_slice(&w.to_le_bytes());
        p.extend_from_slice(&h.to_le_bytes());
        p
    }

    fn vp8l(w: u32, h: u32) -> Vec<u8> {
        let mut p = vec![0x2f];
        p.extend_from_slice(&((w - 1) | (h - 1) << 14).to_le_bytes());
        p
    }

    fn anmf(w: u32, h: u32, frame: &[(&[u8; 4], Vec<u8>)]) -> Vec<u8> {
        let mut p = vec![0; 6];
        p.extend_from_slice(&(w - 1).to_le_bytes()[..3]);
        p.extend_from_slice(&(h - 1).to_le_bytes()[..3]);
        p.extend_from_slice(&[0; 4]);
        for (fourcc, payload) in frame {
            p.extend_from_slice(*fourcc);
            p.extend_from_slice(&(payload.len() as u32).to_le_bytes());
            p.extend_from_slice(payload);
            p.resize(p.len() + payload.len() % 2, 0);
        }
        p
    }

    #[test]
    fn a_webp_that_claims_a_huge_picture_is_refused_before_it_is_decoded() {
        let huge = 16383;
        let bombs = [
            (
                "a 1x1 canvas with a huge lossy frame",
                riff(&[(b"VP8X", vp8x(1, 1)), (b"VP8 ", vp8(huge, huge))]),
            ),
            (
                "a lossy frame with no canvas",
                riff(&[(b"VP8 ", vp8(huge, 1))]),
            ),
            (
                "a huge lossless frame",
                riff(&[(b"VP8X", vp8x(1, 1)), (b"VP8L", vp8l(16384, 16384))]),
            ),
            (
                "a huge canvas",
                riff(&[(b"VP8X", vp8x(5000, 1)), (b"VP8 ", vp8(5000, 1))]),
            ),
            (
                "a huge animation frame",
                riff(&[
                    (b"VP8X", vp8x(1, 1)),
                    (
                        b"ANMF",
                        anmf(huge as u32, huge as u32, &[(b"VP8 ", vp8(huge, huge))]),
                    ),
                ]),
            ),
        ];
        for (what, file) in bombs {
            let e = render(&file, 8).unwrap_err();
            assert!(e.contains("is over 4096x4096"), "{what}: {e}");
        }
    }

    #[test]
    fn a_webp_whose_headers_disagree_or_break_off_is_refused() {
        for (what, file, want) in [
            (
                "a frame that is not the canvas",
                riff(&[(b"VP8X", vp8x(4, 4)), (b"VP8 ", vp8(8, 8))]),
                "disagrees with the canvas",
            ),
            (
                "an animation frame that is not its header",
                riff(&[
                    (b"VP8X", vp8x(8, 8)),
                    (b"ANMF", anmf(4, 4, &[(b"VP8 ", vp8(8, 8))])),
                ]),
                "disagrees with its header",
            ),
            (
                "an animation frame off its canvas",
                riff(&[
                    (b"VP8X", vp8x(2, 2)),
                    (b"ANMF", anmf(4, 4, &[(b"VP8 ", vp8(4, 4))])),
                ]),
                "outside its canvas",
            ),
            ("no image", riff(&[(b"VP8X", vp8x(2, 2))]), "no image data"),
            (
                "an animation frame with no image",
                riff(&[
                    (b"VP8X", vp8x(4, 4)),
                    (b"ANMF", anmf(4, 4, &[(b"EXIF", vec![0; 4])])),
                ]),
                "frame with no image",
            ),
            (
                "a bad VP8 start code",
                riff(&[(b"VP8 ", vec![0; 10])]),
                "bad bitstream header",
            ),
            (
                "a short VP8L",
                riff(&[(b"VP8L", vec![0x2f])]),
                "bad bitstream header",
            ),
        ] {
            let e = render(&file, 8).unwrap_err();
            assert!(e.contains(want), "{what}: {e}");
        }
        // A chunk that runs past the end, with a header that would pass otherwise.
        let mut cut = riff(&[(b"VP8 ", vp8(2, 2))]);
        cut.truncate(cut.len() - 2);
        assert!(render(&cut, 8).unwrap_err().contains("truncated chunk"));
        // Too many chunks to walk.
        let many: Vec<(&[u8; 4], Vec<u8>)> = (0..70).map(|_| (b"JUNK", vec![0])).collect();
        assert!(render(&riff(&many), 8)
            .unwrap_err()
            .contains("more than 64"));
        assert!(render(b"RIFF\0\0\0\0WEBP", 8).is_err());
    }

    #[test]
    fn a_jpeg_is_turned_upright_by_its_exif_orientation() {
        // 8x4, wide, red on the left and blue on the right.
        let wide = RgbaImage::from_fn(8, 4, |x, _| match x < 4 {
            true => Rgba([255, 0, 0, 255]),
            false => Rgba([0, 0, 255, 255]),
        });
        let jpeg = encoded(&wide, ImageFormat::Jpeg);
        let plain = render(&jpeg, 16).unwrap();
        assert_eq!((plain.w, plain.h), (8, 4));

        // An APP1 segment with one IFD entry, Orientation (0x0112) = 6: rotate 90 degrees.
        let mut exif = b"Exif\0\0II*\0\x08\0\0\0\x01\0".to_vec();
        exif.extend_from_slice(&[0x12, 0x01, 0x03, 0x00, 1, 0, 0, 0, 6, 0, 0, 0, 0, 0, 0, 0]);
        let mut with = jpeg[..2].to_vec();
        with.extend_from_slice(&[0xff, 0xe1]);
        with.extend_from_slice(&(exif.len() as u16 + 2).to_be_bytes());
        with.extend(exif);
        with.extend_from_slice(&jpeg[2..]);
        let turned = render(&with, 16).unwrap();
        assert_eq!((turned.w, turned.h), (4, 8), "rotated to portrait");
    }

    fn svg(width: u32, height: u32, body: &str) -> Vec<u8> {
        format!(
            "<svg xmlns='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink' \
             width='{width}' height='{height}' viewBox='0 0 {width} {height}'>{body}</svg>"
        )
        .into_bytes()
    }

    #[test]
    fn an_svg_is_drawn_to_fill_the_box_in_its_shape_and_its_id_follows_the_file() {
        let red = svg(10, 5, "<rect width='10' height='5' fill='#f00'/>");
        let icon = render(&red, 40).unwrap();
        assert_eq!(
            (icon.w, icon.h),
            (40, 20),
            "scaled up to the box, shape kept"
        );
        assert!(icon.pixels.chunks(4).all(|p| p == [0, 0, 255, 255]));

        let tall = render(&svg(5, 10, "<rect width='5' height='10'/>"), 40).unwrap();
        assert_eq!((tall.w, tall.h), (20, 40));
        // A big drawing shrinks, and the sides never round to zero.
        let big = render(&svg(4000, 2, "<rect width='4000' height='2'/>"), 40).unwrap();
        assert_eq!((big.w, big.h), (40, 1));

        let again = render(&red, 40).unwrap();
        let blue = render(&svg(10, 5, "<rect width='10' height='5' fill='#00f'/>"), 40).unwrap();
        assert_eq!(icon.id, again.id);
        assert_ne!(icon.id, blue.id);

        // Transparency stays straight: half-transparent red is still red.
        let half = svg(
            2,
            2,
            "<rect width='2' height='2' fill='#f00' fill-opacity='0.5'/>",
        );
        let half = render(&half, 2).unwrap();
        assert_eq!(&half.pixels[..4], [0, 0, 255, 128]);

        // A BOM and an XML declaration are fine, and the kind comes from the contents.
        let mut bom = b"\xef\xbb\xbf<?xml version='1.0'?>\n".to_vec();
        bom.extend(&red);
        assert_eq!(render(&bom, 40).unwrap().id, icon.id);

        // Gradients, groups, a symbol and one <use> draw.
        let rich = svg(
            8,
            8,
            "<defs><linearGradient id='g'><stop offset='0' stop-color='#f00'/>\
             <stop offset='1' stop-color='#00f'/></linearGradient><g id='r'><rect width='8' \
             height='8' fill='url(#g)'/></g></defs><use href='#r'/>",
        );
        assert!(render(&rich, 8).is_ok());
    }

    /// A Lucide icon as lucide.dev serves it, with a gradient, `<defs>`, `<use>`, opacity and a
    /// style attribute added, and the attributes Heroicons and Font Awesome carry.
    const LUCIDE_HOUSE: &str = r##"<svg xmlns="http://www.w3.org/2000/svg"
        xmlns:xlink="http://www.w3.org/1999/xlink" width="24" height="24" viewBox="0 0 24 24"
        fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"
        stroke-linejoin="round" class="lucide lucide-house-icon lucide-house" aria-hidden="true"
        data-slot="icon" role="img" focusable="false" xml:space="preserve">
      <title>House</title>
      <defs>
        <linearGradient id="roof" x1="0" y1="0" x2="1" y2="1">
          <stop offset="0" stop-color="#4f46e5"/>
          <stop offset="1" stop-color="#06b6d4" stop-opacity=".8"/>
        </linearGradient>
        <path id="door" d="M15 21v-8a1 1 0 0 0-1-1h-4a1 1 0 0 0-1 1v8"/>
      </defs>
      <path d="M3 10a2 2 0 0 1 .709-1.528l7-5.999a2 2 0 0 1 2.582 0l7 5.999A2 2 0 0 1 21 10v9a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"
        fill="url(#roof)" fill-rule="evenodd" clip-rule="evenodd"/>
      <use xlink:href="#door"/>
      <circle cx="12" cy="7" r="1" fill="#fff" stroke="none" opacity=".5"/>
      <path style="fill:none;stroke:#000;stroke-opacity:.3" d="M2 22h20"/>
    </svg>"##;

    /// A Material Symbols icon as fonts.google.com serves it.
    const MATERIAL_HOME: &str = r##"<svg xmlns="http://www.w3.org/2000/svg" height="24px"
        viewBox="0 -960 960 960" width="24px" fill="#e3e3e3" enable-background="new 0 0 24 24">
        <path d="M240-200h120v-240h240v240h120v-360L480-740 240-560v360Zm-80 80v-480l320-240 320 240v480H520v-240h-80v240H160Zm320-350Z"/></svg>"##;

    #[test]
    fn real_icons_draw() {
        // Far from the drawing budget, so a busier real icon has room.
        for icon in [LUCIDE_HOUSE, MATERIAL_HOME] {
            let doc = check_svg(icon).unwrap();
            let (work, layers) = render_cost(&svg_tree(&doc).unwrap());
            eprintln!("estimated cost {work:.3e}, layers {layers} px");
            assert!(work < MAX_SVG_RENDER_COST / 20.0, "{work}");
        }
        for side in [26, 44, 110, 256] {
            let icon = render(LUCIDE_HOUSE.as_bytes(), side).unwrap();
            assert_eq!((icon.w, icon.h), (side as u16, side as u16));
            // B, G, R, A: the gradient's blue end shows, and so does the stroke.
            let px = icon.pixels.as_chunks::<4>().0;
            assert!(
                px.iter().any(|p| p[3] > 200 && p[0] > 150 && p[2] < 120),
                "{side}"
            );
            assert!(
                px.iter().any(|p| p[3] > 200 && p[..3] == [0, 0, 0]),
                "{side}"
            );
            let icon = render(MATERIAL_HOME.as_bytes(), side).unwrap();
            let px = icon.pixels.as_chunks::<4>().0;
            assert!(px.iter().any(|p| p == &[0xe3, 0xe3, 0xe3, 255]), "{side}");
        }
    }

    /// Each of these is refused by the checks before usvg runs, and quickly.
    fn refused(what: &str, bytes: &[u8], needle: &str) {
        let start = std::time::Instant::now();
        let e = render(bytes, 64)
            .err()
            .unwrap_or_else(|| format!("{what} was drawn"));
        assert!(e.contains(needle), "{what}: {e}");
        assert!(
            start.elapsed().as_millis() < 2000,
            "{what} took {:?}",
            start.elapsed()
        );
    }

    #[test]
    fn an_svg_that_loads_or_recurses_by_its_own_say_so_is_refused() {
        let tmp = tempfile::tempdir().unwrap();
        let local = tmp.path().join("secret.svg");
        fs::write(
            &local,
            svg(8, 8, "<rect width='8' height='8' fill='#f00'/>"),
        )
        .unwrap();
        let data = format!(
            "data:image/svg+xml;base64,{}",
            "PHN2ZyB4bWxucz0naHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmcnLz4="
        );
        for href in [
            local.to_str().unwrap().to_string(),
            format!("file://{}", local.display()),
            "http://127.0.0.1:9/x.png".to_string(),
            data,
        ] {
            let image = format!("<image width='8' height='8' xlink:href='{href}'/>");
            refused("image", &svg(8, 8, &image), "<image>");
            let used = format!("<use width='8' height='8' xlink:href='{href}'/>");
            refused("use", &svg(8, 8, &used), "leaves the file");
        }

        let pattern_chain =
            "<defs><pattern id='a' width='4' height='4' patternUnits='userSpaceOnUse'>\
             <rect width='4' height='4' fill='url(#b)'/></pattern>\
             <pattern id='b' width='4' height='4' patternUnits='userSpaceOnUse'>\
             <rect width='4' height='4' fill='url(#a)'/></pattern></defs>\
             <rect width='8' height='8' fill='url(#a)'/>";
        refused("pattern chain", &svg(8, 8, pattern_chain), "<pattern>");
        let huge_pattern =
            "<pattern id='p' patternUnits='userSpaceOnUse' width='1e6' height='1e6'>\
             <rect width='1' height='1'/></pattern><rect width='8' height='8' fill='url(#p)'/>";
        refused("huge pattern", &svg(8, 8, huge_pattern), "<pattern>");
        let mask_chain =
            "<mask id='a' mask='url(#b)'><rect width='8' height='8' fill='#fff'/></mask>\
             <mask id='b' mask='url(#a)'><rect width='8' height='8' fill='#fff'/></mask>\
             <rect width='8' height='8' mask='url(#a)'/>";
        refused("mask chain", &svg(8, 8, mask_chain), "<mask>");
        let masks = "<mask><rect width='1' height='1'/></mask>".repeat(2000);
        refused("masks", &svg(8, 8, &masks), "<mask>");
        let turbulence =
            "<filter id='f'><feTurbulence baseFrequency='0.1' numOctaves='4000000000'/>\
             </filter><rect width='8' height='8' filter='url(#f)'/>";
        refused("turbulence", &svg(8, 8, turbulence), "<filter>");
        let morph = "<filter id='f'><feMorphology operator='dilate' radius='100000000'/></filter>";
        refused("morphology", &svg(8, 8, morph), "<filter>");
        refused(
            "filter attribute",
            &svg(8, 8, "<rect width='8' height='8' filter='url(#nothing)'/>"),
            "filter attribute",
        );
        refused(
            "filter in css",
            &svg(
                8,
                8,
                "<rect width='8' height='8' style='fill:red;Filter:url(#f)'/>",
            ),
            "CSS property Filter",
        );
        refused(
            "filter function in css",
            &svg(
                8,
                8,
                "<rect width='8' height='8' style='filter:blur(40px)'/>",
            ),
            "CSS property filter",
        );
        refused(
            "filter in a stylesheet",
            &svg(
                8,
                8,
                "<style>rect { mask: url(#m) }</style><rect width='8' height='8'/>",
            ),
            "<style>",
        );
        refused(
            "clip path",
            &svg(
                8,
                8,
                "<clipPath id='c'><rect width='1' height='1'/></clipPath>",
            ),
            "<clipPath>",
        );
        refused(
            "marker",
            &svg(8, 8, "<marker id='m'><path d='M0 0'/></marker>"),
            "<marker>",
        );
        refused("text", &svg(40, 20, "<text y='15'>Hi</text>"), "<text>");
        refused(
            "script",
            &svg(8, 8, "<script>alert(1)</script>"),
            "<script>",
        );
        refused(
            "foreign object",
            &svg(8, 8, "<foreignObject width='8' height='8'/>"),
            "<foreignObject>",
        );
        refused(
            "dashes",
            &svg(
                8,
                8,
                "<path d='M0 0H8' stroke='red' stroke-dasharray='0.0001'/>",
            ),
            "stroke-dasharray",
        );
    }

    #[test]
    fn an_svg_over_the_limits_is_refused_cleanly() {
        let rect = "<rect width='1' height='1'/>";
        let read = "can't read the SVG";

        let nested = |n: usize| format!("{}{rect}{}", "<g>".repeat(n), "</g>".repeat(n));
        assert!(render(&svg(8, 8, &nested(MAX_SVG_DEPTH - 1)), 8).is_ok());
        refused("deep", &svg(8, 8, &nested(MAX_SVG_DEPTH)), "nests deeper");
        refused("very deep", &svg(8, 8, &nested(30_000)), "nests deeper");
        // Fake end tags in a comment, CDATA or an attribute don't lower the count.
        let fake = "<!-- </g></g></g> --><![CDATA[</g></g>]]><?pi </g> ?><g a='</g>' b=\"</g>\">";
        let evasive = format!(
            "{}{rect}{}",
            fake.repeat(MAX_SVG_DEPTH + 1),
            "</g>".repeat(MAX_SVG_DEPTH + 1)
        );
        refused("evasive", &svg(8, 8, &evasive), "nests deeper");

        let many = rect.repeat(MAX_SVG_NODES);
        refused("many", &svg(8, 8, &many), "elements");
        let hidden = format!("<defs>{}</defs>", rect.repeat(MAX_SVG_NODES));
        refused("many in defs", &svg(8, 8, &hidden), "elements");
        assert!(render(&svg(8, 8, &rect.repeat(MAX_SVG_NODES - 1)), 8).is_ok());
        // Each <use> counts the elements it copies: 64 copies of 32.
        let copied = format!(
            "<defs><g id='r'>{}</g></defs>{}",
            rect.repeat(31),
            "<use href='#r'/>".repeat(MAX_SVG_USES)
        );
        refused(
            "many copied",
            &svg(8, 8, &copied),
            "counting what each <use> copies",
        );
        let comments = "<!---->".repeat(MAX_SVG_XML_NODES as usize);
        refused("many comments", &svg(8, 8, &comments), "XML nodes");
        let big = format!("<desc>{}</desc>", "a".repeat(MAX_SVG_BYTES));
        refused("big", &svg(8, 8, &big), "over 256 KB");

        let path = format!("<path id='p' d='M0 0{}'/>", "L1 1".repeat(5_000));
        let used = format!("<defs>{path}</defs>{}", "<use href='#p'/>".repeat(13));
        refused("path data copied", &svg(8, 8, &used), "path data");

        // Layers that are cheap to draw but need over 32 MB: each is 5 times the box a side.
        let layers = "<rect x='-100' y='-100' width='200' height='200' opacity='.5'/>";
        assert!(render(&svg(8, 8, &layers.repeat(4)), 64).is_ok());
        refused("opacity layers", &svg(8, 8, &layers.repeat(16)), "memory");

        let attributes = |n: usize| {
            let list: String = (0..n).map(|i| format!(" data-a{i}=''")).collect();
            svg(8, 8, &format!("<rect width='1' height='1'{}/>", &list[..]))
        };
        assert!(render(&attributes(MAX_SVG_ATTRIBUTES - 2), 8).is_ok());
        refused(
            "attributes",
            &attributes(MAX_SVG_ATTRIBUTES - 1),
            "attributes",
        );
        // One element with as many attributes as fit, which roxmltree would check in O(n²).
        refused("attribute flood", &attributes(15_000), "attributes");
        // As many namespaces in scope as the limits allow, and elements whose every attribute
        // makes roxmltree search them all for its prefix.
        let mut flood = String::new();
        for level in 0..MAX_SVG_DEPTH - 1 {
            let declared: String = (0..MAX_SVG_ATTRIBUTES)
                .map(|i| format!(" xmlns:p{level}x{i}='urn:{i}'"))
                .collect();
            flood += &format!("<g{declared}>");
        }
        let leaf = format!(
            "<p0x0:r{}/>",
            (0..MAX_SVG_ATTRIBUTES)
                .map(|i| format!(" p0x0:a{i}=''"))
                .collect::<String>()
        );
        let room = MAX_SVG_BYTES - flood.len() - 400 * (MAX_SVG_DEPTH - 1);
        flood += &leaf.repeat(room / leaf.len());
        flood += &"</g>".repeat(MAX_SVG_DEPTH - 1);
        let flood = format!("<svg xmlns='{SVG_NS}' width='8' height='8'>{flood}</svg>");
        refused("namespace flood", flood.as_bytes(), "namespace");
        refused(
            "attributes past a quoted >",
            &svg(
                8,
                8,
                &format!("<rect a='>'{}/>", " b=''".repeat(MAX_SVG_ATTRIBUTES)),
            ),
            "attributes",
        );

        let gradient = |stops: usize| {
            format!(
                "<linearGradient>{}</linearGradient>",
                "<stop offset='0'/>".repeat(stops)
            )
        };
        assert!(render(&svg(8, 8, &gradient(MAX_SVG_GRADIENT_STOPS)), 8).is_ok());
        refused(
            "stops",
            &svg(8, 8, &gradient(MAX_SVG_GRADIENT_STOPS + 1)),
            "stops",
        );
        let all = gradient(MAX_SVG_GRADIENT_STOPS).repeat(MAX_SVG_STOPS / MAX_SVG_GRADIENT_STOPS);
        assert!(render(&svg(8, 8, &all), 8).is_ok());
        refused(
            "all stops",
            &svg(8, 8, &(all + "<radialGradient><stop/></radialGradient>")),
            "stops",
        );

        let uses = "<defs><rect id='r' width='1' height='1'/></defs>".to_string()
            + &"<use href='#r'/>".repeat(MAX_SVG_USES + 1);
        refused("uses", &svg(8, 8, &uses), "<use>");
        let chain =
            "<defs><rect id='r' width='1' height='1'/><g id='a'><use href='#r'/></g></defs>\
                     <use href='#a'/>";
        refused("use of use", &svg(8, 8, chain), "reaches another <use>");
        let itself = "<g id='a'><use href='#a'/></g>";
        refused("use of itself", &svg(8, 8, itself), "reaches another <use>");

        let mut gradients = String::from("<defs>");
        for i in 0..=MAX_SVG_HREF_CHAIN + 1 {
            gradients += &format!("<linearGradient id='g{i}' href='#g{}'/>", i + 1);
        }
        gradients += "</defs>";
        refused("gradient chain", &svg(8, 8, &gradients), "inherit");
        refused(
            "gradient loop",
            &svg(
                8,
                8,
                "<linearGradient id='a' href='#b'/><linearGradient id='b' href='#a'/>",
            ),
            "inherit",
        );

        let dtd = "<!DOCTYPE svg [<!ENTITY a 'aaaaaaaaaa'>]>\
                   <svg xmlns='http://www.w3.org/2000/svg' width='1' height='1'><title>&a;</title></svg>";
        refused("entity dtd", dtd.as_bytes(), "DOCTYPE");
        let mut flat = String::from("<!DOCTYPE svg [<!ENTITY a '");
        flat += &"a".repeat(200_000);
        flat += "'>]><svg xmlns='http://www.w3.org/2000/svg' width='1' height='1'><title>";
        flat += &"&a;".repeat(300);
        flat += "</title></svg>";
        refused("flat entities", flat.as_bytes(), "DOCTYPE");

        for (what, bytes, needle) in [
            ("truncated", svg(8, 8, rect)[..40].to_vec(), read),
            ("not xml", b"<svg".to_vec(), read),
            ("not utf-8", b"<svg \xff\xfe/>".to_vec(), read),
            (
                "not svg",
                b"<html xmlns='http://www.w3.org/2000/svg'/>".to_vec(),
                read,
            ),
            (
                "no namespace",
                b"<svg width='1' height='1'/>".to_vec(),
                read,
            ),
            (
                "no size",
                b"<svg xmlns='http://www.w3.org/2000/svg'/>".to_vec(),
                "needs a viewBox",
            ),
            ("zero size", svg(0, 8, rect), read),
            ("huge", svg(MAX_SOURCE_SIDE + 1, 8, rect), "units a side"),
            ("nested svg", svg(8, 8, "<svg/>"), "<svg>"),
            ("svgz", vec![0x1f, 0x8b, 8, 0, 0, 0, 0, 0], "not a PNG"),
        ] {
            refused(what, &bytes, needle);
        }
        // The same size at the limit is fine.
        assert!(render(&svg(MAX_SOURCE_SIDE, 8, rect), 8).is_ok());
    }

    #[test]
    fn the_name_must_match_the_contents() {
        let svg_bytes = svg(8, 8, "<rect width='8' height='8'/>");
        let png = encoded(&quadrants(8), ImageFormat::Png);
        assert!(name_matches_contents("a.svg", &svg_bytes).is_ok());
        assert!(name_matches_contents("a.png", &png).is_ok());
        for name in ["a.png", "a.jpg", "a.webp", "a"] {
            let e = name_matches_contents(name, &svg_bytes).unwrap_err();
            assert!(e.contains("ends in .svg"), "{name}: {e}");
        }
        assert!(name_matches_contents("a.svg", &png).is_err());

        let tmp = tempfile::tempdir().unwrap();
        let dir = tmp.path().join(DIR);
        fs::create_dir(&dir).unwrap();
        fs::write(dir.join("good.svg"), &svg_bytes).unwrap();
        fs::write(dir.join("html.png"), &svg_bytes).unwrap();
        fs::write(dir.join("fake.svg"), &png).unwrap();
        let store = Icons::new(dir);
        assert_eq!(store.list().unwrap(), ["good.svg"]);
        let config = config_with_icons(&[("a", "html.png")]);
        let errors = store.load(&config).unwrap_err();
        assert!(errors[0].contains("ends in .svg"), "{errors:?}");
        let config = config_with_icons(&[("a", "good.svg")]);
        assert!(store.load(&config).is_ok());
    }

    /// What an earlier check let through: it looked at elements and attributes only in the SVG
    /// namespace, while usvg reads those in no namespace too, and some attributes in others.
    #[test]
    fn everything_must_be_in_the_svg_namespace_and_on_the_lists() {
        let ns = "http://www.w3.org/2000/svg";
        let outside = "outside the SVG namespace";
        let prefixed = "in the namespace";
        let hostile = [
            (
                "a prefixed root with an unprefixed pattern",
                format!(
                    "<s:svg xmlns:s='{ns}' width='8' height='8'><pattern id='p' width='1e6' \
                     height='1e6' patternUnits='userSpaceOnUse'><rect width='1' height='1'/>\
                     </pattern><s:rect width='8' height='8' fill='url(#p)'/></s:svg>"
                )
                .into_bytes(),
                outside,
            ),
            (
                "a prefixed root with an unprefixed filter",
                format!(
                    "<s:svg xmlns:s='{ns}' width='8' height='8'><filter id='f'>\
                     <feTurbulence baseFrequency='0.1' numOctaves='4000000000'/></filter>\
                     <s:rect width='8' height='8' filter='url(#f)'/></s:svg>"
                )
                .into_bytes(),
                outside,
            ),
            (
                "children with xmlns=''",
                svg(
                    8,
                    8,
                    "<g xmlns=''><pattern id='p'/><rect fill='url(#p)'/></g>",
                ),
                outside,
            ),
            (
                "an editor's element",
                svg(8, 8, "<x:note xmlns:x='urn:x'/>"),
                outside,
            ),
            (
                "a style element in another namespace",
                svg(
                    8,
                    8,
                    "<x:style xmlns:x='urn:x'>rect { filter: blur(40px) }</x:style>\
                     <rect width='8' height='8'/>",
                ),
                outside,
            ),
            (
                "s:filter",
                svg(
                    8,
                    8,
                    &format!("<rect xmlns:s='{ns}' width='8' height='8' s:filter='blur(40px)'/>"),
                ),
                prefixed,
            ),
            (
                "s:stroke-dasharray",
                svg(
                    8,
                    8,
                    &format!(
                        "<path xmlns:s='{ns}' d='M0 0H8' stroke='red' \
                         s:stroke-dasharray='0.0001'/>"
                    ),
                ),
                prefixed,
            ),
            (
                "s:style",
                svg(
                    8,
                    8,
                    &format!(
                        "<rect xmlns:s='{ns}' width='8' height='8' s:style='filter:blur(9px)'/>"
                    ),
                ),
                prefixed,
            ),
            (
                "xlink:style",
                svg(
                    8,
                    8,
                    "<rect width='8' height='8' xlink:style='filter:blur(9px)'/>",
                ),
                prefixed,
            ),
            (
                "xml:id",
                svg(8, 8, "<rect xml:id='a' width='8' height='8'/>"),
                prefixed,
            ),
            (
                "href and xlink:href that disagree",
                svg(
                    8,
                    8,
                    "<defs><rect id='r' width='1' height='1'/><g id='a'><use href='#r'/></g>\
                     </defs><use xlink:href='#r' href='#a'/>",
                ),
                "both href and xlink:href",
            ),
            (
                "the same, the other way round",
                svg(
                    8,
                    8,
                    "<defs><rect id='r' width='1' height='1'/><g id='a'><use href='#r'/></g>\
                     </defs><use href='#a' xlink:href='#r'/>",
                ),
                "both href and xlink:href",
            ),
            (
                "an href with text after the id",
                svg(
                    8,
                    8,
                    "<defs><rect id='r' width='1' height='1'/><g id='a'><use href='#r'/></g>\
                     </defs><use href='#a x'/>",
                ),
                "isn't \"#id\"",
            ),
            (
                "an id used twice, to hide a gradient chain",
                svg(
                    8,
                    8,
                    "<linearGradient id='a' href='#b'/><linearGradient id='b'/>\
                     <linearGradient id='b' href='#a'/>",
                ),
                "more than once",
            ),
            (
                "an attribute off the list",
                svg(8, 8, "<path d='M0 0H8' stroke='red' pathLength='1'/>"),
                "the pathLength attribute on <path>",
            ),
            (
                "isolation",
                svg(8, 8, "<g isolation='isolate'/>"),
                "isolation attribute",
            ),
            (
                "a marker attribute",
                svg(8, 8, "<path d='M0 0H8' marker-start='url(#m)'/>"),
                "marker-start attribute",
            ),
            (
                "a blend mode in a style",
                svg(
                    8,
                    8,
                    "<rect width='8' height='8' style='mix-blend-mode:multiply'/>",
                ),
                "CSS property mix-blend-mode",
            ),
            (
                "a marker in a style",
                svg(8, 8, "<path d='M0 0H8' style='marker:url(#m)'/>"),
                "CSS property marker",
            ),
            (
                "a style element",
                svg(8, 8, "<style>* * * * * * * * * * * * * * * * a {}</style>"),
                "<style>",
            ),
        ];
        for (what, bytes, needle) in hostile {
            refused(what, &bytes, needle);
        }
    }

    /// Whether `bytes` pass every check before drawing.
    fn passes(bytes: &[u8]) -> Result<(), String> {
        let doc = check_svg(std::str::from_utf8(bytes).unwrap())?;
        svg_tree(&doc).map(drop)
    }

    /// The largest `n` up to `most` for which `make(n)` passes every check.
    fn largest(what: &str, make: &dyn Fn(usize) -> Vec<u8>, most: usize) -> usize {
        passes(&make(1)).unwrap_or_else(|e| panic!("{what}: {e}"));
        let (mut lo, mut hi) = (1, most);
        while lo < hi {
            let mid = (lo + hi).div_ceil(2);
            match passes(&make(mid)) {
                Ok(()) => lo = mid,
                Err(_) => hi = mid - 1,
            }
        }
        lo
    }

    /// For each way to make drawing slow, the most of it the limits allow, drawn at the largest
    /// box. The figures are in the report with `--nocapture`.
    #[test]
    fn the_costliest_svgs_the_limits_allow_draw_in_under_two_seconds() {
        let stops: String = (0..MAX_SVG_GRADIENT_STOPS)
            .map(|i| {
                format!(
                    "<stop offset='{}' stop-color='#{:02x}00ff'/>",
                    i as f32 / 31.0,
                    i * 8
                )
            })
            .collect();
        let gradients: String = (0..MAX_SVG_STOPS / MAX_SVG_GRADIENT_STOPS)
            .map(|i| format!("<linearGradient id='g{i}'>{stops}</linearGradient>"))
            .collect();
        let defs = format!("<defs>{gradients}</defs>");
        let rects = |n: usize, at: &str| -> String {
            (0..n)
                .map(|i| {
                    format!(
                        "<rect {at} fill='url(#g{})' stroke='url(#g{})'/>",
                        i % 8,
                        (i + 1) % 8
                    )
                })
                .collect()
        };
        let in_box = "x='-1' y='-1' width='10' height='10'";
        let past_box = "x='-100' y='-100' width='200' height='200'";
        // Segments that all cross each other.
        let fan = |n: usize| -> String {
            (0..n / 2)
                .map(|i| {
                    let y = i as f32 * 8.0 / (n / 2) as f32;
                    format!("L0 {y:.3}L8 {:.3}", 8.0 - y)
                })
                .collect()
        };
        type Make<'a> = Box<dyn Fn(usize) -> Vec<u8> + 'a>;
        let mut families: Vec<(&str, Make, usize)> = vec![
            (
                "every stop, every <use>, and as many elements as both allow",
                Box::new(|n| {
                    let group = format!(
                        "<g id='r'>{}</g>",
                        rects(n, "x='2' y='2' width='0.5' height='0.5'")
                    );
                    let uses = "<use href='#r'/>".repeat(MAX_SVG_USES);
                    svg(8, 8, &format!("<defs>{gradients}{group}</defs>{uses}"))
                }),
                MAX_SVG_NODES / MAX_SVG_USES,
            ),
            (
                "gradient fills and strokes over the box",
                Box::new(|n| svg(8, 8, &format!("{defs}{}", rects(n, in_box)))),
                MAX_SVG_NODES,
            ),
            (
                "opacity layers past the box",
                Box::new(|n| {
                    let shapes = rects(n, past_box).replace("<rect", "<rect opacity='.5'");
                    svg(8, 8, &format!("{defs}{shapes}"))
                }),
                MAX_SVG_NODES,
            ),
            (
                "gradient fills in one layer past the box",
                Box::new(|n| {
                    svg(
                        8,
                        8,
                        &format!("{defs}<g opacity='.5'>{}</g>", rects(n, past_box)),
                    )
                }),
                MAX_SVG_NODES,
            ),
            (
                "nested opacity layers past the box",
                Box::new(|n| {
                    let body =
                        "<g opacity='.9'>".repeat(n) + &rects(1, past_box) + &"</g>".repeat(n);
                    svg(8, 8, &format!("{defs}{body}"))
                }),
                MAX_SVG_DEPTH - 1,
            ),
            (
                "symbols drawn past the box",
                Box::new(|n| {
                    let symbol =
                        format!("<symbol id='s' viewBox='0 0 8 8'><rect {past_box}/></symbol>");
                    let uses = "<use href='#s' x='-16' y='-16' width='40' height='40'/>".repeat(n);
                    svg(8, 8, &format!("<defs>{gradients}{symbol}</defs>{uses}"))
                }),
                MAX_SVG_USES,
            ),
            (
                "a filled path of crossing segments",
                Box::new(|n| svg(8, 8, &format!("<path d='M0 0{}'/>", fan(n)))),
                MAX_SVG_PATH_DATA / 9,
            ),
            (
                "a hairline path of crossing segments",
                Box::new(|n| {
                    svg(
                        8,
                        8,
                        &format!(
                            "<path d='M0 0{}' fill='none' stroke='red' stroke-width='0.01'/>",
                            fan(n)
                        ),
                    )
                }),
                MAX_SVG_PATH_DATA / 9,
            ),
            (
                "a thick stroke of crossing segments",
                Box::new(|n| {
                    svg(
                        8,
                        8,
                        &format!(
                            "<path d='M0 0{}' fill='none' stroke='red' stroke-width='0.5' \
                             stroke-linejoin='round' stroke-linecap='round'/>",
                            fan(n)
                        ),
                    )
                }),
                MAX_SVG_PATH_DATA / 9,
            ),
            (
                "stroked paths of crossing segments, copied",
                Box::new(|n| {
                    let path = format!(
                        "<path id='p' d='M0 0{}' fill='url(#g0)' stroke='url(#g1)'/>",
                        fan(n)
                    );
                    let uses = "<use href='#p'/>".repeat(MAX_SVG_USES);
                    svg(8, 8, &format!("<defs>{gradients}{path}</defs>{uses}"))
                }),
                MAX_SVG_PATH_DATA / 9 / MAX_SVG_USES,
            ),
            (
                "arcs, filled and stroked",
                Box::new(|n| {
                    let arcs = "A4 4 0 1 1 8 8A4 4 0 1 1 0 0".repeat(n);
                    svg(
                        8,
                        8,
                        &format!("{defs}<path d='M0 0{arcs}' fill='url(#g0)' stroke='red'/>"),
                    )
                }),
                MAX_SVG_PATH_DATA / 28,
            ),
        ];
        // A drawing 4096 units wide and 0.001 high is drawn into 256 x 1 pixels, so it is
        // stretched 16,000 times as much down as across.
        let thin = |body: &str| {
            format!(
                "<svg xmlns='http://www.w3.org/2000/svg' width='4096' height='0.001' \
                 viewBox='0 0 4096 0.001'><defs>{gradients}</defs>{body}</svg>"
            )
            .into_bytes()
        };
        // Radial gradients with a focal point, the costliest kind tiny-skia draws.
        let radial = |n: usize, spread: &str| {
            let gradient = format!(
                "<radialGradient id='r' cx='.5' cy='.5' r='.2' fx='.3' fy='.3' fr='.05' \
                 spreadMethod='{spread}'>{stops}</radialGradient>"
            );
            let shapes = format!("<rect {in_box} fill='url(#r)' stroke='url(#r)'/>").repeat(n);
            svg(8, 8, &format!("<defs>{gradient}</defs>{shapes}"))
        };
        families.extend::<Vec<(&str, Make, usize)>>(vec![
            (
                "radial focal gradients, reflected",
                Box::new(|n| radial(n, "reflect")),
                MAX_SVG_NODES,
            ),
            (
                "radial focal gradients, repeated",
                Box::new(|n| radial(n, "repeat")),
                MAX_SVG_NODES,
            ),
            (
                "cubic curves, stroked",
                Box::new(|n| {
                    let curves = "C8 0 0 8 8 8C0 8 8 0 0 0".repeat(n);
                    svg(
                        8,
                        8,
                        &format!(
                            "<path d='M0 0{curves}' fill='none' stroke='red' \
                             stroke-width='0.3' stroke-linejoin='round'/>"
                        ),
                    )
                }),
                MAX_SVG_PATH_DATA / 25,
            ),
            (
                "a very wide stroke with round joins and caps",
                Box::new(|n| {
                    let zigzag = "L8 8L0 0".repeat(n);
                    svg(
                        8,
                        8,
                        &format!(
                            "<path d='M0 0{zigzag}' fill='none' stroke='url(#g0)' \
                             stroke-width='40' stroke-linejoin='round' stroke-linecap='round'/>\
                             {defs}"
                        ),
                    )
                }),
                MAX_SVG_PATH_DATA / 9,
            ),
            (
                "a thin drawing, stretched, in layers past its box",
                Box::new(|n| {
                    let shape = "<rect x='-8192' y='-0.002' width='20480' height='0.005' \
                                 opacity='.5' fill='url(#g0)' stroke='url(#g1)' \
                                 stroke-width='0.0001'/>";
                    thin(&shape.repeat(n))
                }),
                MAX_SVG_NODES,
            ),
            (
                "a thin drawing, stretched, of crossing segments",
                Box::new(|n| {
                    let fan: String = (0..n / 2)
                        .map(|i| {
                            format!(
                                "L0 {:.6}L4096 {:.6}",
                                i as f64 * 1e-3 / (n / 2) as f64,
                                1e-3 - i as f64 * 1e-3 / (n / 2) as f64
                            )
                        })
                        .collect();
                    thin(&format!(
                        "<path d='M0 0{fan}' fill='url(#g0)' stroke='red' stroke-width='1e-6'/>"
                    ))
                }),
                MAX_SVG_PATH_DATA / 24,
            ),
            (
                "nested opacity layers at the edge of the layer window",
                Box::new(|n| {
                    // resvg's window for a layer is -2 to 3 times the box, -16 to 24 units here.
                    let edge = "x='-15.97' y='-15.97' width='39.94' height='39.94'";
                    let body = "<g opacity='.9'>".repeat(n) + &rects(1, edge) + &"</g>".repeat(n);
                    svg(8, 8, &format!("{defs}{body}"))
                }),
                MAX_SVG_DEPTH - 1,
            ),
        ]);
        // What must refuse each family one step past its largest drawable size, and at its XML
        // limit, by name, in the order of `families`.
        let (elements, too_long, memory, too_big) =
            ("than 2000 elements", "too long", "memory", "over 256 KB");
        let needles = [
            (elements, elements),
            (too_long, elements),
            (memory, elements),
            (too_long, elements),
            (memory, too_long),
            (memory, too_long),
            (too_long, too_long),
            (too_long, too_long),
            (too_long, too_long),
            (too_long, too_long),
            (too_long, too_big),
            (too_long, elements),
            (too_long, elements),
            (too_long, too_long),
            (too_long, too_long),
            (too_long, too_big),
            (too_long, too_long),
            (memory, too_long),
        ];
        assert_eq!(families.len(), needles.len());
        for ((what, make, most), (next, at_most)) in families.into_iter().zip(needles) {
            let n = largest(what, &*make, most);
            let bytes = make(n);
            let doc = check_svg(std::str::from_utf8(&bytes).unwrap()).unwrap();
            let (work, layers) = render_cost(&svg_tree(&doc).unwrap());
            let start = std::time::Instant::now();
            let icon =
                render(&bytes, ICON_MAX_SIDE as u32).unwrap_or_else(|e| panic!("{what}: {e}"));
            let took = start.elapsed();
            eprintln!(
                "{what}: {n} of at most {most} drawn in {took:?}, estimated {:.0} ms ({:.2} of \
                 the estimate), {:.1}M layer px",
                work / 1e6,
                took.as_secs_f64() * 1e9 / work,
                layers / 1e6
            );
            assert_eq!(icon.w as usize, ICON_MAX_SIDE, "{what}");
            assert!(took.as_secs_f64() < 2.0, "{what}: {n} took {took:?}");
            if n < most {
                refused(what, &make(n + 1), next);
                // usvg builds the tree before it is costed, and the XML limits keep that quick.
                refused(what, &make(most), at_most);
            }
        }
    }

    /// The deepest drawing the limits allow: a group nested 63 deep in `<defs>`, copied by a
    /// `<use>` nested 63 deep, so usvg and resvg recurse through 127 levels.
    fn deepest() -> Vec<u8> {
        let open = "<g transform='translate(0.01)'>";
        let inner = MAX_SVG_DEPTH - 3; // under <defs> and <g id='t'>, above the <rect>
        let outer = MAX_SVG_DEPTH - 1; // above the <use>
        let body = format!(
            "<defs><g id='t'>{}<rect width='8' height='8' fill='red'/>{}</g></defs>{}<use \
             href='#t'/>{}",
            open.repeat(inner),
            "</g>".repeat(inner),
            open.repeat(outer),
            "</g>".repeat(outer),
        );
        svg(8, 8, &body)
    }

    #[test]
    fn the_deepest_svg_the_limits_allow_draws() {
        let bytes = deepest();
        let icon = render(&bytes, ICON_MAX_SIDE as u32).unwrap();
        assert!(icon
            .pixels
            .as_chunks::<4>()
            .0
            .iter()
            .any(|p| p == &[0, 0, 255, 255]));
        // It even fits the 2 MB a spawned thread or a tokio blocking thread gets by default, so the
        // 16 MB of the drawing thread leave a wide margin.
        let small = std::thread::Builder::new()
            .stack_size(2 * 1024 * 1024)
            .spawn(move || draw_svg_here(&bytes, ICON_MAX_SIDE as u32).map(|p| p.dimensions()))
            .unwrap()
            .join()
            .unwrap();
        assert_eq!(small.unwrap(), (256, 256));
        // One more level is refused, in <defs> or at the <use>.
        let deeper = String::from_utf8(deepest()).unwrap();
        let in_defs =
            deeper
                .replacen("<g id='t'>", "<g id='t'><g>", 1)
                .replacen("</defs>", "</g></defs>", 1);
        refused("deeper in defs", in_defs.as_bytes(), "nests deeper");
        let at_use =
            deeper
                .replacen("<use", "<g><use", 1)
                .replacen("href='#t'/>", "href='#t'/></g>", 1);
        refused("deeper at the use", at_use.as_bytes(), "nests deeper");
    }

    #[test]
    fn import_and_list_accept_svg() {
        let tmp = tempfile::tempdir().unwrap();
        let dir = tmp.path().join(DIR);
        let src = tmp.path().join("logo.bin");
        fs::write(&src, svg(8, 8, "<rect width='8' height='8'/>")).unwrap();
        let store = Icons::new(dir.clone());
        let name = store.import(&src).unwrap();
        assert!(
            name_error(&name).is_none() && name.ends_with(".svg"),
            "{name}"
        );
        fs::write(dir.join("bad.svg"), b"<svg").unwrap();
        assert_eq!(store.list().unwrap(), [name]);
        fs::write(&src, svg(8, 8, "<pattern/>")).unwrap();
        assert!(store.import(&src).unwrap_err().contains("<pattern>"));
    }
}
