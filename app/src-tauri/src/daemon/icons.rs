//! Button icons: a PNG, JPEG or WebP in `icons/`, decoded, shrunk to fit its button and
//! converted to the device's pixel format. The picture's hash names it on the wire.

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
const MAX_NAME_BYTES: usize = 63;

// The device's button geometry, from `firmware/main/ui.c` (GRID_TOP, GRID_GAP, ICON_PAD,
// ICON_GAP) and `display.h`. A button with an icon is a column: the icon, then a label of fixed
// height, with padding around them. LABEL_LINE is the line height of Montserrat 32.
const SCREEN_W: u32 = 1024;
const SCREEN_H: u32 = 600;
const GRID_TOP: u32 = 204;
const GRID_GAP: u32 = 8;
const ICON_PAD: u32 = 4;
const ICON_GAP: u32 = 2;
const LABEL_LINE: u32 = 35;
/// A label gets two lines unless that leaves the icon less than this.
const MIN_ICON_SIDE: u32 = 24;

/// The icons of a config by file name. A name that several buttons share appears once.
pub type IconSet = HashMap<String, Arc<IconFrame>>;

/// The side of the square an icon must fit in on a `cols` x `rows` grid, in pixels: what is left
/// of the button after its padding and a label of two lines, or of one where two would leave the
/// icon under 24 px. That is 256 with one row, 110 with two, 44 with three, 46 with four and 26
/// with five, or less where a cell is narrower than that. The grid must already be valid, so both are at least 1.
pub fn box_px(cols: u32, rows: u32) -> u32 {
    let cell_w = (SCREEN_W - (cols + 1) * GRID_GAP) / cols;
    let cell_h = (SCREEN_H - GRID_TOP - rows * GRID_GAP) / rows;
    let room = cell_h - 2 * ICON_PAD;
    let lines = if room >= 2 * LABEL_LINE + ICON_GAP + MIN_ICON_SIDE {
        2
    } else {
        1
    };
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
        let side = box_px(config.layout.cols, config.layout.rows);
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
        let icon = Arc::new(render(&read_image(&path)?, side)?);
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
    /// the same name. Only a PNG, JPEG or WebP within the limits is copied.
    pub fn import(&self, src: &Path) -> Result<String, String> {
        let bytes = read_image(src).map_err(|e| format!("{}: {e}", src.display()))?;
        let (_, format) = decode(&bytes).map_err(|e| format!("{}: {e}", src.display()))?;
        let ext = match format {
            ImageFormat::Png => "png",
            ImageFormat::Jpeg => "jpg",
            _ => "webp",
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
}

fn file_stamp(path: &Path) -> Result<FileStamp, String> {
    let meta = fs::metadata(path).map_err(|e| match e.kind() {
        std::io::ErrorKind::NotFound => "not found".to_string(),
        _ => e.to_string(),
    })?;
    let modified = meta.modified().map_err(|e| e.to_string())?;
    Ok((modified, meta.len(), meta.ino()))
}

/// Decodes `bytes` and shrinks the picture to fit a `side` x `side` square, keeping its shape.
/// A picture that already fits is left as it is.
pub fn render(bytes: &[u8], side: u32) -> Result<IconFrame, String> {
    let (picture, _) = decode(bytes)?;
    let (w, h) = picture.dimensions();
    let picture = if w <= side && h <= side {
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

fn decode(bytes: &[u8]) -> Result<(RgbaImage, ImageFormat), String> {
    let mut reader = ImageReader::new(Cursor::new(bytes))
        .with_guessed_format()
        .map_err(|e| e.to_string())?;
    let format = reader
        .format()
        .filter(|f| matches!(f, ImageFormat::Png | ImageFormat::Jpeg | ImageFormat::WebP))
        .ok_or("not a PNG, JPEG or WebP image")?;
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
    Ok((picture.into_rgba8(), format))
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
            (
                "an SVG",
                b"<svg xmlns='http://www.w3.org/2000/svg'/>".to_vec(),
            ),
            ("text", b"hello".to_vec()),
            ("nothing", Vec::new()),
        ] {
            let e = render(&bytes, 8).unwrap_err();
            assert!(e.contains("PNG, JPEG or WebP"), "{what}: {e}");
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
                assert_eq!(box_px(cols, rows), high.min(width), "{cols}x{rows}");
            }
        }
        // 5x3 cells are 195x124 px: 116 inside the padding, 116 - 70 - 2 = 44.
        assert_eq!(box_px(5, 3), 44);
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
            "button \"junk\": icon \"junk.png\": not a PNG, JPEG or WebP image",
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
            .contains("PNG, JPEG or WebP"));
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
}
