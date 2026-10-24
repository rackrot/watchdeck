//! The sleep screen's picture: a PNG, JPEG, WebP or SVG in `screens/`, decoded with the icon
//! pipeline's checks, fitted to the 1024 x 512 image area and converted to RGB565, little-endian,
//! the panel's own format. The first 16 bytes of the pixels' SHA-256 name it on the wire. This
//! module also turns `[sleep]` into the `sleep` frame, with the defaults for absent keys.

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};

use image::{imageops, RgbaImage};
use sha2::{Digest, Sha256};

use super::config::{color, SleepConfig};
use super::icons::{
    self, decode, decodes, file_stamp, import_into, list_in, name_matches_contents, read_image,
    resize, FileStamp, Kind, Target,
};
use super::protocol::{
    Sleep, SleepAnchor, SleepShow, SleepSpinner, ASSET_CHUNK_BYTES, ASSET_ID_BYTES,
};

/// Where screen images live, inside the config dir.
pub const DIR: &str = "screens";
/// The image area, which every image fills exactly.
pub const IMAGE_W: u32 = 1024;
pub const IMAGE_H: u32 = 512;
/// Bytes in a rendered image: 2 for each RGB565 pixel.
pub const IMAGE_BYTES: usize = (IMAGE_W * IMAGE_H * 2) as usize;
/// The largest image file. A phone photo is several MB, so this is more than an icon's 2 MB.
const MAX_FILE_BYTES: u64 = 16 * 1024 * 1024;

// What an absent `[sleep]` key means. The board's built-in screen uses the same values
// (`sleepcfg_defaults` in `firmware/main/sleepcfg.c`), and `site/src/config.md` lists them.
const DEFAULT_BG: u32 = 0x00_0000;
const DEFAULT_FG: u32 = 0xd0_d0d0;
const DEFAULT_SPINNER_COLOR: u32 = 0x38_bdf8;
const DEFAULT_OFFLINE_AFTER_S: u32 = 10;
const DEFAULT_BACKLIGHT: u32 = 100;

/// How a picture of another shape meets the image area.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Fit {
    /// Fills the area and cuts off what overflows, equally on both sides.
    Cover,
    /// Shows the whole picture, centered, with `bg` around it.
    Contain,
}

/// A rendered image: RGB565 little-endian pixels, and the id derived from them.
pub struct ScreenImage {
    pub id: [u8; ASSET_ID_BYTES],
    pixels: Vec<u8>,
}

/// The id is the pixels' hash, so equal ids mean equal pixels. Comparing ids keeps the live
/// config's equality check from comparing a megabyte.
impl PartialEq for ScreenImage {
    fn eq(&self, other: &Self) -> bool {
        self.id == other.id
    }
}

impl Eq for ScreenImage {}

impl std::fmt::Debug for ScreenImage {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "ScreenImage({})", super::protocol::encode_hex(&self.id))
    }
}

impl ScreenImage {
    /// The asset chunk that starts at `offset`: [`ASSET_CHUNK_BYTES`] bytes, or what is left of
    /// the image. `None` for an offset that is not a whole chunk or is at or past the end.
    pub fn chunk(&self, offset: u64) -> Option<&[u8]> {
        let start = usize::try_from(offset).ok()?;
        if start % ASSET_CHUNK_BYTES != 0 {
            return None;
        }
        self.pixels.get(start..)?.chunks(ASSET_CHUNK_BYTES).next()
    }

    /// All of the image: [`IMAGE_BYTES`] bytes of RGB565 little-endian.
    pub fn pixels(&self) -> &[u8] {
        &self.pixels
    }
}

/// The key a render is kept under: the file, the version of it, and what shapes the pixels.
type Key = (String, FileStamp, Fit, u32);

struct Cached {
    key: Key,
    image: Arc<ScreenImage>,
}

/// The `screens/` directory and the one image `[sleep]` names, rendered. The render is kept until
/// its file, `fit` or `bg` changes, so a reload decodes only what is new.
pub struct Screens {
    dir: PathBuf,
    cache: Mutex<Option<Cached>>,
    /// Held while decoding, so two callers that want the same new picture (a prewarm and a save)
    /// decode it once, not twice at the same time.
    rendering: Mutex<()>,
    /// Whether each listed file decoded, by name, for the version of the file it was checked at.
    listed: Mutex<HashMap<String, (FileStamp, bool)>>,
}

impl Screens {
    pub fn new(dir: PathBuf) -> Self {
        Self {
            dir,
            cache: Mutex::default(),
            rendering: Mutex::default(),
            listed: Mutex::default(),
        }
    }

    /// The image `sleep` names, rendered, or `None` when it names none. `sleep` must already pass
    /// [`Config::check`](super::config::Config::check). The cache lock is held only to look up and
    /// to store, never while decoding.
    pub fn load(
        &self,
        sleep: Option<&SleepConfig>,
    ) -> Result<Option<Arc<ScreenImage>>, Vec<String>> {
        let Some((name, fit, bg)) = sleep.and_then(|s| Some((s.image.as_deref()?, fit(s), bg(s))))
        else {
            *self.cache() = None;
            return Ok(None);
        };
        self.render_file(name, fit, bg)
            .map(Some)
            .map_err(|e| vec![format!("sleep.image {name:?}: {e}")])
    }

    fn cache(&self) -> std::sync::MutexGuard<'_, Option<Cached>> {
        self.cache.lock().expect("screen cache poisoned")
    }

    fn render_file(&self, name: &str, fit: Fit, bg: u32) -> Result<Arc<ScreenImage>, String> {
        if !icons::plain_name(name) {
            return Err(format!("must be {}", icons::name_rule(DIR)));
        }
        let path = self.dir.join(name);
        if !path.try_exists().map_err(|e| e.to_string())? {
            return Err(format!("no such file in {DIR}/"));
        }
        let stamp = file_stamp(&path)?;
        let key = (name.to_string(), stamp, fit, bg);
        let hit = |this: &Self| {
            (this.cache().as_ref())
                .filter(|hit| hit.key == key)
                .map(|hit| hit.image.clone())
        };
        if let Some(image) = hit(self) {
            return Ok(image);
        }
        let _one_at_a_time = self.rendering.lock().expect("screen render poisoned");
        // Whoever held the lock may have rendered this very picture.
        if let Some(image) = hit(self) {
            return Ok(image);
        }
        let bytes = read_image(&path, MAX_FILE_BYTES)?;
        name_matches_contents(name, &bytes)?;
        let image = Arc::new(render(&bytes, fit, bg)?);
        *self.cache() = Some(Cached {
            key,
            image: image.clone(),
        });
        Ok(image)
    }

    /// Copies the image at `src` into the directory under a name made from its content and
    /// returns that name for `image` in `[sleep]`. The same file gives the same name. Only a PNG,
    /// JPEG, WebP or SVG within the limits is copied, and an SVG must draw under both fits.
    pub fn import(&self, src: &Path) -> Result<String, String> {
        import_into(
            &self.dir,
            src,
            MAX_FILE_BYTES,
            &[
                Target::Screen { cover: true },
                Target::Screen { cover: false },
            ],
        )
    }

    /// The files in the directory that `image` could name, sorted. Whether a file decodes is
    /// remembered by the version of the file, so listing again decodes only what is new.
    pub fn list(&self) -> Result<Vec<String>, String> {
        list_in(&self.dir, |name, path| {
            let Ok(stamp) = file_stamp(path) else {
                return false;
            };
            if let Some((seen, ok)) = self.listed.lock().expect("listing poisoned").get(name) {
                if *seen == stamp {
                    return *ok;
                }
            }
            let ok = decodes(name, path, MAX_FILE_BYTES, Target::Screen { cover: true });
            (self.listed.lock().expect("listing poisoned")).insert(name.to_string(), (stamp, ok));
            ok
        })
    }
}

/// Decodes `bytes`, fits the picture to the image area by `fit`, puts it on `bg` (a picture with
/// transparency shows `bg` through it) and converts it to RGB565.
pub fn render(bytes: &[u8], fit: Fit, bg: u32) -> Result<ScreenImage, String> {
    let (picture, kind) = decode(
        bytes,
        Target::Screen {
            cover: fit == Fit::Cover,
        },
    )?;
    // An SVG was drawn at the size that fits already.
    let picture = if kind == Kind::Svg {
        picture
    } else {
        fitted(picture, fit)
    };
    let pixels = compose(&picture, bg);
    let id = Sha256::digest(&pixels)[..ASSET_ID_BYTES]
        .try_into()
        .expect("a SHA-256 is longer than an id");
    Ok(ScreenImage { id, pixels })
}

/// A raster scaled, keeping its shape, until it fills the area exactly (`Cover`, which first cuts
/// the picture to the area's shape, equally at both ends, so the scaled copy is never bigger than
/// the area) or fits inside it (`Contain`), up as well as down.
fn fitted(picture: RgbaImage, fit: Fit) -> RgbaImage {
    let (w, h) = picture.dimensions();
    let (wide, high) = (u64::from(w), u64::from(h));
    let (area_w, area_h) = (u64::from(IMAGE_W), u64::from(IMAGE_H));
    // Whether the picture is at least as wide as the area, which decides the side that touches it.
    let wide_as_area = wide * area_h >= high * area_w;
    let (picture, nw, nh) = match fit {
        Fit::Cover => {
            let (cw, ch) = if wide_as_area {
                (((high * area_w + area_h / 2) / area_h).min(wide), high)
            } else {
                (wide, ((wide * area_h + area_w / 2) / area_w).min(high))
            };
            let (cw, ch) = (cw as u32, ch as u32);
            let picture = if (cw, ch) == (w, h) {
                picture
            } else {
                imageops::crop_imm(&picture, (w - cw) / 2, (h - ch) / 2, cw, ch).to_image()
            };
            (picture, IMAGE_W, IMAGE_H)
        }
        Fit::Contain if wide_as_area => {
            let nh = (high * area_w + wide / 2) / wide;
            (picture, IMAGE_W, nh as u32)
        }
        Fit::Contain => {
            let nw = (wide * area_h + high / 2) / high;
            (picture, nw as u32, IMAGE_H)
        }
    };
    if picture.dimensions() == (nw, nh.max(1)) {
        picture
    } else {
        resize(picture, nw.max(1), nh.max(1))
    }
}

/// `picture` centered on the image area over `bg`, as RGB565 little-endian. A picture bigger than
/// the area is cut off equally at both ends.
fn compose(picture: &RgbaImage, bg: u32) -> Vec<u8> {
    let (w, h) = picture.dimensions();
    let left = (i64::from(IMAGE_W) - i64::from(w)) / 2;
    let top = (i64::from(IMAGE_H) - i64::from(h)) / 2;
    let back = [(bg >> 16) as u8, (bg >> 8) as u8, bg as u8];
    let mut pixels = Vec::with_capacity(IMAGE_BYTES);
    for y in 0..i64::from(IMAGE_H) {
        for x in 0..i64::from(IMAGE_W) {
            let source = u32::try_from(x - left)
                .ok()
                .zip(u32::try_from(y - top).ok())
                .and_then(|(sx, sy)| picture.get_pixel_checked(sx, sy));
            let [r, g, b] = match source {
                // Straight alpha over the background.
                Some(p) => std::array::from_fn(|i| {
                    let a = u32::from(p[3]);
                    ((u32::from(p[i]) * a + u32::from(back[i]) * (255 - a) + 127) / 255) as u8
                }),
                None => back,
            };
            let rgb565 = u16::from(r >> 3) << 11 | u16::from(g >> 2) << 5 | u16::from(b >> 3);
            pixels.extend_from_slice(&rgb565.to_le_bytes());
        }
    }
    pixels
}

fn fit(sleep: &SleepConfig) -> Fit {
    match sleep.fit.as_deref() {
        Some("contain") => Fit::Contain,
        _ => Fit::Cover,
    }
}

fn bg(sleep: &SleepConfig) -> u32 {
    sleep.bg.as_deref().and_then(color).unwrap_or(DEFAULT_BG)
}

/// The `sleep` frame for `sleep` (`None` is an absent table, which means every default) and its
/// rendered `image`. An absent key takes its default here, so the board needs no rules of its own.
/// `sleep` must already pass [`Config::check`](super::config::Config::check), so a value that
/// doesn't parse can't happen and falls back to the default.
pub fn sleep_frame(sleep: Option<&SleepConfig>, image: Option<&ScreenImage>) -> Sleep {
    let none = SleepConfig::default();
    let sleep = sleep.unwrap_or(&none);
    let rgb = |text: &Option<String>, default| text.as_deref().and_then(color).unwrap_or(default);
    let show: Vec<SleepShow> = match &sleep.show {
        Some(names) => (names.iter())
            .filter_map(|name| match name.as_str() {
                "clock" => Some(SleepShow::Clock),
                "date" => Some(SleepShow::Date),
                "host" => Some(SleepShow::Host),
                "text" => Some(SleepShow::Text),
                _ => None,
            })
            .collect(),
        None => vec![SleepShow::Clock, SleepShow::Date],
    };
    let seconds =
        |n: Option<i64>, default| n.and_then(|n| u32::try_from(n).ok()).unwrap_or(default);
    Sleep {
        image: image.map(|image| super::protocol::encode_hex(&image.id)),
        bg: bg(sleep),
        fg: rgb(&sleep.fg, DEFAULT_FG),
        // The text only goes out when it will be shown.
        text: sleep
            .text
            .clone()
            .filter(|_| show.contains(&SleepShow::Text)),
        show,
        anchor: match sleep.anchor.as_deref() {
            Some("center") => SleepAnchor::Center,
            Some("top_left") => SleepAnchor::TopLeft,
            Some("top_right") => SleepAnchor::TopRight,
            Some("bottom_right") => SleepAnchor::BottomRight,
            _ => SleepAnchor::BottomLeft,
        },
        spinner: match sleep.spinner.as_deref() {
            Some("dots") => SleepSpinner::Dots,
            Some("bar") => SleepSpinner::Bar,
            Some("pulse") => SleepSpinner::Pulse,
            Some("none") => SleepSpinner::Off,
            _ => SleepSpinner::Arc,
        },
        spinner_color: rgb(&sleep.spinner_color, DEFAULT_SPINNER_COLOR),
        offline_after_s: seconds(sleep.offline_after_s, DEFAULT_OFFLINE_AFTER_S),
        backlight: seconds(sleep.backlight, DEFAULT_BACKLIGHT),
    }
}

#[cfg(test)]
mod tests {
    use std::fs;
    use std::io::Cursor;

    use image::{ImageFormat, Rgba};

    use super::*;
    use crate::daemon::protocol::encode_hex;

    const RED: Rgba<u8> = Rgba([255, 0, 0, 255]);
    const GREEN: Rgba<u8> = Rgba([0, 255, 0, 255]);
    const BLUE: Rgba<u8> = Rgba([0, 0, 255, 255]);

    fn png(picture: &RgbaImage) -> Vec<u8> {
        let mut out = Cursor::new(Vec::new());
        picture.write_to(&mut out, ImageFormat::Png).unwrap();
        out.into_inner()
    }

    /// The RGB565 value at (`x`, `y`) of a rendered image.
    fn at(image: &ScreenImage, x: u32, y: u32) -> [u8; 2] {
        let i = ((y * IMAGE_W + x) * 2) as usize;
        [image.pixels()[i], image.pixels()[i + 1]]
    }

    const RED565: [u8; 2] = [0x00, 0xf8];
    const GREEN565: [u8; 2] = [0xe0, 0x07];
    const BLUE565: [u8; 2] = [0x1f, 0x00];

    /// 400 x 300 (4:3): red down to row 100, green to row 200, then blue.
    fn bands() -> RgbaImage {
        RgbaImage::from_fn(400, 300, |_, y| match y {
            0..100 => RED,
            100..200 => GREEN,
            _ => BLUE,
        })
    }

    #[test]
    fn pure_red_is_f800_little_endian_and_the_id_hashes_the_pixels() {
        let red = RgbaImage::from_pixel(IMAGE_W, IMAGE_H, RED);
        let image = render(&png(&red), Fit::Cover, 0).unwrap();
        assert_eq!(image.pixels().len(), IMAGE_BYTES);
        assert_eq!(IMAGE_BYTES, 1_048_576);
        assert!(image.pixels().chunks(2).all(|p| p == RED565));
        let hash = Sha256::digest(image.pixels());
        assert_eq!(image.id, hash[..16]);
        // The same picture gives the same id, and another gives another.
        let again = render(&png(&red), Fit::Cover, 0).unwrap();
        assert_eq!(again, image);
        let other = render(
            &png(&RgbaImage::from_pixel(IMAGE_W, IMAGE_H, BLUE)),
            Fit::Cover,
            0,
        )
        .unwrap();
        assert_ne!(other, image);
    }

    #[test]
    fn a_four_to_three_photo_under_cover_fills_the_area_and_crops_top_and_bottom_equally() {
        let image = render(&png(&bands()), Fit::Cover, 0x00_00ff).unwrap();
        // Scaled to 1024 x 768, then 128 rows come off the top and the bottom: the red band
        // (rows 0..256 of 768) ends at row 128 of the area, and the green one at 384.
        for x in [0, 511, 1023] {
            assert_eq!(at(&image, x, 0), RED565);
            assert_eq!(at(&image, x, 120), RED565);
            assert_eq!(at(&image, x, 140), GREEN565);
            assert_eq!(at(&image, x, 370), GREEN565);
            assert_eq!(at(&image, x, 400), BLUE565);
            assert_eq!(at(&image, x, 511), BLUE565);
        }
        // Nothing is left to show the background through.
        let other_bg = render(&png(&bands()), Fit::Cover, 0xff_0000).unwrap();
        assert_eq!(other_bg, image, "the background does not show under cover");
    }

    #[test]
    fn a_wide_picture_under_cover_is_cut_at_its_sides() {
        // 4:1, with red at the left quarter, green in the middle half, blue at the right quarter.
        let wide = RgbaImage::from_fn(800, 200, |x, _| match x {
            0..200 => RED,
            200..600 => GREEN,
            _ => BLUE,
        });
        let image = render(&png(&wide), Fit::Cover, 0).unwrap();
        // 2048 x 512 with 512 off each side: the red quarter is gone, and so is the blue one.
        assert_eq!(at(&image, 0, 256), GREEN565);
        assert_eq!(at(&image, 1023, 256), GREEN565);
    }

    #[test]
    fn an_extreme_shape_is_cut_before_it_is_scaled_so_it_never_allocates_more_than_the_area() {
        // Scaled whole to cover, a 4096 x 1 picture would be 2,097,152 x 512 pixels (4 GiB).
        for (w, h) in [(4096, 1), (1, 4096), (4096, 2), (3, 4096)] {
            let thin = RgbaImage::from_pixel(w, h, RED);
            for fit in [Fit::Cover, Fit::Contain] {
                let image = render(&png(&thin), fit, 0x00_00ff)
                    .unwrap_or_else(|e| panic!("{w}x{h} {fit:?}: {e}"));
                assert_eq!(image.pixels().len(), IMAGE_BYTES, "{w}x{h} {fit:?}");
                // Contain leaves a one pixel strip of it at the very least.
                assert!(
                    image.pixels().chunks(2).any(|p| p == RED565),
                    "{w}x{h} {fit:?}"
                );
                if fit == Fit::Cover {
                    assert_eq!(at(&image, 512, 256), RED565, "{w}x{h}");
                }
            }
        }
    }

    #[test]
    fn a_picture_under_contain_is_letterboxed_on_the_background() {
        let image = render(&png(&bands()), Fit::Contain, 0x00_00ff).unwrap();
        // 683 x 512, centered: 170 px of background on the left and 171 on the right.
        assert_eq!(at(&image, 0, 256), BLUE565);
        assert_eq!(at(&image, 169, 256), BLUE565);
        assert_eq!(at(&image, 1023, 256), BLUE565);
        assert_eq!(at(&image, 512, 10), RED565);
        assert_eq!(at(&image, 512, 256), GREEN565);
        assert_eq!(at(&image, 512, 500), BLUE565);
        // A wide one is letterboxed above and below.
        let wide = RgbaImage::from_pixel(800, 200, RED);
        let image = render(&png(&wide), Fit::Contain, 0x00_ff00).unwrap();
        assert_eq!(at(&image, 512, 0), GREEN565);
        assert_eq!(at(&image, 512, 256), RED565);
        assert_eq!(at(&image, 0, 256), RED565);
        assert_eq!(at(&image, 512, 511), GREEN565);
    }

    #[test]
    fn a_small_picture_is_scaled_up_to_fit() {
        let small = RgbaImage::from_pixel(8, 4, RED);
        for fit in [Fit::Cover, Fit::Contain] {
            let image = render(&png(&small), fit, 0).unwrap();
            assert_eq!(at(&image, 0, 0), RED565, "{fit:?}");
            assert_eq!(at(&image, 1023, 511), RED565, "{fit:?}");
        }
    }

    #[test]
    fn transparency_shows_the_background() {
        let half = RgbaImage::from_fn(IMAGE_W, IMAGE_H, |x, _| {
            if x < 512 {
                RED
            } else {
                Rgba([255, 0, 0, 0])
            }
        });
        let image = render(&png(&half), Fit::Cover, 0x00_00ff).unwrap();
        assert_eq!(at(&image, 100, 100), RED565);
        assert_eq!(at(&image, 900, 100), BLUE565);
    }

    fn svg(width: u32, height: u32, body: &str) -> Vec<u8> {
        format!(
            "<svg xmlns='http://www.w3.org/2000/svg' width='{width}' height='{height}' \
             viewBox='0 0 {width} {height}'>{body}</svg>"
        )
        .into_bytes()
    }

    #[test]
    fn an_svg_is_drawn_at_the_size_of_the_fit() {
        let square = svg(100, 100, "<rect width='100' height='100' fill='#f00'/>");
        // Cover draws it at 1024 x 1024 and cuts the top and bottom off, so red everywhere.
        let image = render(&square, Fit::Cover, 0x00_00ff).unwrap();
        assert_eq!(at(&image, 0, 0), RED565);
        assert_eq!(at(&image, 1023, 511), RED565);
        // Contain draws it at 512 x 512 in the middle.
        let image = render(&square, Fit::Contain, 0x00_00ff).unwrap();
        assert_eq!(at(&image, 100, 256), BLUE565);
        assert_eq!(at(&image, 512, 256), RED565);
        assert_eq!(at(&image, 900, 256), BLUE565);
        // A very thin drawing under cover still fills the area and does not allocate a huge pixmap.
        let thin = svg(4000, 2, "<rect width='4000' height='2' fill='#f00'/>");
        let image = render(&thin, Fit::Cover, 0).unwrap();
        assert_eq!(at(&image, 512, 256), RED565);
    }

    #[test]
    fn an_svg_over_the_cost_limit_is_refused_with_the_icon_rules_message() {
        let layers = "<rect width='1024' height='512' fill='url(#g)'/>".repeat(100);
        let body = format!(
            "<defs><linearGradient id='g'><stop offset='0' stop-color='#f00'/>\
             <stop offset='1' stop-color='#00f'/></linearGradient></defs>{layers}"
        );
        let heavy = svg(1024, 512, &body);
        for fit in [Fit::Cover, Fit::Contain] {
            let e = render(&heavy, fit, 0).unwrap_err();
            assert!(e.starts_with("the SVG would take too long to draw"), "{e}");
        }
        // The same drawing is fine as an icon: the screen is eight times the pixels.
        icons::render(&heavy, 256).unwrap();
    }

    #[test]
    fn a_picture_that_is_not_an_image_or_is_too_big_is_refused() {
        assert!(render(b"not an image", Fit::Cover, 0)
            .unwrap_err()
            .contains("PNG, JPEG, WebP or SVG"));
        let big = RgbaImage::new(icons::MAX_SOURCE_SIDE + 1, 1);
        assert!(render(&png(&big), Fit::Cover, 0).is_err());
    }

    #[test]
    fn chunks_cover_the_image_in_four_pieces_and_refuse_a_bad_offset() {
        let image = render(&png(&bands()), Fit::Cover, 0).unwrap();
        let chunk = ASSET_CHUNK_BYTES as u64;
        let pieces: Vec<&[u8]> = (0..4).map(|i| image.chunk(i * chunk).unwrap()).collect();
        assert!(pieces.iter().all(|p| p.len() == ASSET_CHUNK_BYTES));
        assert_eq!(pieces.concat(), image.pixels());
        for bad in [1, chunk + 1, 4 * chunk, 5 * chunk, u64::MAX] {
            assert_eq!(image.chunk(bad), None, "{bad}");
        }
    }

    fn sleep_table(text: &str) -> SleepConfig {
        toml::from_str(text).unwrap()
    }

    #[test]
    fn an_absent_table_or_key_takes_its_default() {
        let frame = sleep_frame(None, None);
        assert_eq!(
            frame,
            Sleep {
                image: None,
                bg: 0,
                fg: 0xd0d0d0,
                show: vec![SleepShow::Clock, SleepShow::Date],
                text: None,
                anchor: SleepAnchor::BottomLeft,
                spinner: SleepSpinner::Arc,
                spinner_color: 0x38bdf8,
                offline_after_s: 10,
                backlight: 100,
            }
        );
        assert_eq!(sleep_frame(Some(&SleepConfig::default()), None), frame);
        // Zero is a value, not an absent key.
        let zero = sleep_table("offline_after_s = 0\nbacklight = 0\nshow = []");
        let frame = sleep_frame(Some(&zero), None);
        assert_eq!((frame.offline_after_s, frame.backlight), (0, 0));
        assert!(frame.show.is_empty());
    }

    #[test]
    fn every_key_reaches_the_frame_and_every_frame_is_acceptable() {
        let sleep = sleep_table(
            r##"
            bg = "#102030"
            fg = "#ffffff"
            show = ["host", "text", "clock"]
            text = "Back soon"
            anchor = "top_right"
            spinner = "pulse"
            spinner_color = "#010203"
            offline_after_s = 3600
            backlight = 30
            "##,
        );
        let red = RgbaImage::from_pixel(IMAGE_W, IMAGE_H, RED);
        let image = render(&png(&red), Fit::Cover, 0).unwrap();
        let frame = sleep_frame(Some(&sleep), Some(&image));
        assert_eq!(frame.image, Some(encode_hex(&image.id)));
        assert_eq!((frame.bg, frame.fg), (0x102030, 0xffffff));
        assert_eq!(
            frame.show,
            [SleepShow::Host, SleepShow::Text, SleepShow::Clock]
        );
        assert_eq!(frame.text.as_deref(), Some("Back soon"));
        assert_eq!(frame.anchor, SleepAnchor::TopRight);
        assert_eq!(frame.spinner, SleepSpinner::Pulse);
        assert_eq!(frame.spinner_color, 0x010203);
        assert_eq!((frame.offline_after_s, frame.backlight), (3600, 30));
        assert_eq!(frame.check(), Ok(()));
        assert_eq!(sleep_frame(None, None).check(), Ok(()));
        // Text that is not shown does not go out.
        let hidden = sleep_table("show = [\"clock\"]\ntext = \"x\"");
        assert_eq!(sleep_frame(Some(&hidden), None).text, None);
        // Every anchor and spinner the config accepts maps to one.
        for (anchor, spinner) in [
            ("center", "arc"),
            ("top_left", "dots"),
            ("top_right", "bar"),
            ("bottom_left", "pulse"),
            ("bottom_right", "none"),
        ] {
            let table = sleep_table(&format!("anchor = {anchor:?}\nspinner = {spinner:?}"));
            let frame = sleep_frame(Some(&table), None);
            let json = serde_json::to_value(&frame).unwrap();
            assert_eq!(json["anchor"], anchor);
            assert_eq!(json["spinner"], spinner);
        }
    }

    fn encoded(picture: &RgbaImage, format: ImageFormat) -> Vec<u8> {
        let mut out = Cursor::new(Vec::new());
        match format {
            ImageFormat::Jpeg => image::DynamicImage::ImageRgba8(picture.clone())
                .to_rgb8()
                .write_to(&mut out, format),
            _ => picture.write_to(&mut out, format),
        }
        .unwrap();
        out.into_inner()
    }

    #[test]
    fn jpeg_and_webp_render_like_png() {
        let solid = RgbaImage::from_pixel(64, 32, GREEN);
        for format in [ImageFormat::Jpeg, ImageFormat::WebP] {
            let image = render(&encoded(&solid, format), Fit::Cover, 0)
                .unwrap_or_else(|e| panic!("{format:?}: {e}"));
            assert_eq!(image.pixels().len(), IMAGE_BYTES);
            // JPEG is lossy, so the shade may be off by a step in the low bits.
            let [lo, hi] = at(&image, 512, 256);
            let value = u16::from_le_bytes([lo, hi]);
            assert!(
                value >> 11 <= 1 && (value >> 5) & 63 >= 62 && value & 31 <= 1,
                "{format:?}: {value:#06x}"
            );
        }
    }

    #[test]
    fn a_jpeg_is_turned_upright_by_its_exif_orientation_before_it_is_fitted() {
        // 8x4, red on the left and blue on the right.
        let wide = RgbaImage::from_fn(8, 4, |x, _| if x < 4 { RED } else { BLUE });
        let jpeg = encoded(&wide, ImageFormat::Jpeg);
        // An APP1 segment with one IFD entry, Orientation (0x0112) = 6: rotate 90 degrees.
        let mut exif = b"Exif\0\0II*\0\x08\0\0\0\x01\0".to_vec();
        exif.extend_from_slice(&[0x12, 0x01, 0x03, 0x00, 1, 0, 0, 0, 6, 0, 0, 0, 0, 0, 0, 0]);
        let mut turned = jpeg[..2].to_vec();
        turned.extend_from_slice(&[0xff, 0xe1]);
        turned.extend_from_slice(&(exif.len() as u16 + 2).to_be_bytes());
        turned.extend(exif);
        turned.extend_from_slice(&jpeg[2..]);

        // Wide, it fills the area and is red on the left. Turned, it is a portrait in the middle
        // of the area, red above and blue below, with the background at its sides.
        let plain = render(&jpeg, Fit::Contain, 0x00_ff00).unwrap();
        assert_eq!(at(&plain, 100, 256), RED565);
        let upright = render(&turned, Fit::Contain, 0x00_ff00).unwrap();
        assert_eq!(at(&upright, 100, 256), GREEN565, "pillarbox");
        assert_eq!(at(&upright, 512, 20), RED565);
        assert_eq!(at(&upright, 512, 490), BLUE565);
    }

    #[test]
    fn an_svg_loads_through_screens_and_is_drawn_for_the_fit() {
        let dir = dir_with(
            "mark.svg",
            &svg(100, 50, "<rect width='100' height='50' fill='#f00'/>"),
        );
        let screens = Screens::new(dir.path().to_path_buf());
        let table = sleep_table("image = \"mark.svg\"\nfit = \"contain\"\nbg = \"#0000ff\"");
        let image = screens.load(Some(&table)).unwrap().unwrap();
        // 2:1, so it fills the area.
        assert_eq!(at(&image, 0, 0), RED565);
        assert_eq!(at(&image, 1023, 511), RED565);
        assert_eq!(screens.list(), Ok(vec!["mark.svg".to_string()]));
    }

    #[test]
    fn a_file_over_16_mib_and_a_picture_over_4096_wide_say_so() {
        let dir = tempfile::tempdir().unwrap();
        fs::write(
            dir.path().join("big.png"),
            vec![0; MAX_FILE_BYTES as usize + 1],
        )
        .unwrap();
        fs::write(dir.path().join("wide.png"), png(&RgbaImage::new(4097, 1))).unwrap();
        let screens = Screens::new(dir.path().to_path_buf());
        let e = screens
            .load(Some(&sleep_table("image = \"big.png\"")))
            .unwrap_err();
        assert_eq!(e, ["sleep.image \"big.png\": over 16384 KB"]);
        let e = screens
            .load(Some(&sleep_table("image = \"wide.png\"")))
            .unwrap_err();
        assert_eq!(
            e,
            [
                "sleep.image \"wide.png\": can't decode the image (at most 4096x4096): \
              Image size exceeds limit"
            ]
        );
        // Neither is listed, and the exact size is fine.
        assert_eq!(screens.list(), Ok(vec![]));
    }

    #[test]
    fn listing_remembers_a_file_until_it_changes() {
        let dir = dir_with("a.png", &png(&bands()));
        let screens = Screens::new(dir.path().to_path_buf());
        assert_eq!(screens.list(), Ok(vec!["a.png".to_string()]));
        // Same version of the file: the answer comes from memory, not from reading it.
        let stamp = file_stamp(&dir.path().join("a.png")).unwrap();
        assert_eq!(screens.listed.lock().unwrap()["a.png"], (stamp, true));
        fs::write(dir.path().join("a.png"), b"junk now").unwrap();
        assert_eq!(screens.list(), Ok(vec![]));
        assert!(!screens.listed.lock().unwrap()["a.png"].1);
    }

    #[test]
    fn two_callers_that_want_the_same_new_picture_decode_it_once() {
        let dir = dir_with("a.png", &png(&bands()));
        let screens = Arc::new(Screens::new(dir.path().to_path_buf()));
        let table = sleep_table("image = \"a.png\"");
        let loads: Vec<_> = (0..4)
            .map(|_| {
                let (screens, table) = (screens.clone(), table.clone());
                std::thread::spawn(move || screens.load(Some(&table)).unwrap().unwrap())
            })
            .collect();
        let images: Vec<_> = loads.into_iter().map(|t| t.join().unwrap()).collect();
        assert!(images
            .windows(2)
            .all(|pair| Arc::ptr_eq(&pair[0], &pair[1])));
    }

    /// `cargo test --lib twelve_megapixel -- --ignored --nocapture` for the figure.
    #[test]
    #[ignore = "a timing, not a rule"]
    fn a_twelve_megapixel_jpeg_renders() {
        let photo = RgbaImage::from_fn(4000, 3000, |x, y| {
            Rgba([(x % 256) as u8, (y % 256) as u8, ((x ^ y) % 256) as u8, 255])
        });
        let jpeg = encoded(&photo, ImageFormat::Jpeg);
        let start = std::time::Instant::now();
        let image = render(&jpeg, Fit::Cover, 0).unwrap();
        eprintln!(
            "12 MP JPEG ({} KB): {:?}",
            jpeg.len() / 1024,
            start.elapsed()
        );
        assert_eq!(image.pixels().len(), IMAGE_BYTES);
    }

    fn dir_with(name: &str, bytes: &[u8]) -> tempfile::TempDir {
        let dir = tempfile::tempdir().unwrap();
        fs::write(dir.path().join(name), bytes).unwrap();
        dir
    }

    #[test]
    fn loading_renders_the_named_file_and_keeps_the_render_until_something_changes() {
        let dir = dir_with("a.png", &png(&bands()));
        let screens = Screens::new(dir.path().to_path_buf());
        assert_eq!(screens.load(None), Ok(None));
        assert_eq!(screens.load(Some(&SleepConfig::default())), Ok(None));

        let table = sleep_table("image = \"a.png\"");
        let first = screens.load(Some(&table)).unwrap().unwrap();
        let again = screens.load(Some(&table)).unwrap().unwrap();
        assert!(
            Arc::ptr_eq(&first, &again),
            "an unchanged file is not rendered again"
        );

        // The fit and the background reshape the pixels.
        let contain = sleep_table("image = \"a.png\"\nfit = \"contain\"");
        let contained = screens.load(Some(&contain)).unwrap().unwrap();
        assert_ne!(contained, first);
        let blue = sleep_table("image = \"a.png\"\nfit = \"contain\"\nbg = \"#0000ff\"");
        assert_ne!(screens.load(Some(&blue)).unwrap().unwrap(), contained);

        // Another picture under the same name is another image.
        fs::write(
            dir.path().join("a.png"),
            png(&RgbaImage::from_pixel(8, 8, GREEN)),
        )
        .unwrap();
        let replaced = screens.load(Some(&table)).unwrap().unwrap();
        assert_ne!(replaced, first);
        assert_eq!(at(&replaced, 5, 5), GREEN565);
    }

    #[test]
    fn a_missing_or_bad_file_says_what_is_wrong() {
        let dir = dir_with("bad.png", b"nope");
        let screens = Screens::new(dir.path().to_path_buf());
        let missing = screens
            .load(Some(&sleep_table("image = \"x\"")))
            .unwrap_err();
        assert_eq!(missing, ["sleep.image \"x\": no such file in screens/"]);
        let bad = screens
            .load(Some(&sleep_table("image = \"bad.png\"")))
            .unwrap_err();
        assert!(bad[0].starts_with("sleep.image \"bad.png\": "), "{bad:?}");
        // An SVG keeps to a name that ends in .svg, as an icon does.
        fs::write(
            dir.path().join("s.png"),
            svg(8, 8, "<rect width='8' height='8'/>"),
        )
        .unwrap();
        let e = screens
            .load(Some(&sleep_table("image = \"s.png\"")))
            .unwrap_err();
        assert!(
            e[0].contains("must be in a file whose name ends in .svg"),
            "{e:?}"
        );
        // A directory is not an image either, and a name with a slash never leaves the folder.
        fs::create_dir(dir.path().join("d")).unwrap();
        let e = screens
            .load(Some(&sleep_table("image = \"d\"")))
            .unwrap_err();
        assert!(e[0].contains("not a regular file"), "{e:?}");
        let e = screens
            .load(Some(&sleep_table("image = \"../x.png\"")))
            .unwrap_err();
        assert!(e[0].contains("a file name in screens/"), "{e:?}");
    }

    #[test]
    fn import_copies_a_good_image_under_its_hash_and_list_finds_it() {
        let src = dir_with("photo.png", &png(&bands()));
        let dir = tempfile::tempdir().unwrap();
        let screens = Screens::new(dir.path().join("screens"));
        assert_eq!(screens.list(), Ok(vec![]));
        let name = screens.import(&src.path().join("photo.png")).unwrap();
        assert!(icons::plain_name(&name) && name.ends_with(".png"), "{name}");
        assert_eq!(
            screens.import(&src.path().join("photo.png")),
            Ok(name.clone())
        );
        assert_eq!(screens.list(), Ok(vec![name]));
        fs::write(src.path().join("junk.png"), b"junk").unwrap();
        assert!(screens.import(&src.path().join("junk.png")).is_err());
    }
}
