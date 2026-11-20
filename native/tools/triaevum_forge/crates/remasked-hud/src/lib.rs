// SPDX-License-Identifier: GPL-3.0-or-later

use std::collections::BTreeMap;
use std::ffi::{c_char, CStr};
use std::io::{Cursor, Read};
use std::path::Path;

use forge_base::hashing::sha256_hex;
use sevenz_rust::{Password, SevenZReader};

const ARCHIVE_SHA256: &str = "2c55e262b65edcbfafbb896d2434d5eb3ccb966183e86229602a72a92d525b35";
const FILES: &[&str] = &["layout/Joker.Main.New.gar", "layout/Joker.Main.Play.gar", "layout/Joker.mfpj"];
const MAX_ARCHIVE_BYTES: usize = 128 * 1024;
const MAX_ATLAS_BYTES: usize = 2 * 1024 * 1024;
const ATLAS_SHA256: &str = "592eb6087fecb394d7e6d807ebab330b9a90036fbf6c6e84dee171e71ca284f6";

fn neutral_atlas(atlas: &[u8]) -> Result<Vec<u8>, String> {
    if atlas.len() > MAX_ATLAS_BYTES || sha256_hex(atlas) != ATLAS_SHA256 {
        return Err("The Project Restoration HUD artwork failed verification.".into());
    }
    let mut decoder = png::Decoder::new(Cursor::new(atlas));
    decoder.set_transformations(png::Transformations::EXPAND | png::Transformations::STRIP_16);
    let mut reader = decoder.read_info().map_err(|error| error.to_string())?;
    let mut pixels = vec![0; reader.output_buffer_size()];
    let info = reader.next_frame(&mut pixels).map_err(|error| error.to_string())?;
    if (info.width, info.height, info.color_type) != (2048, 1024, png::ColorType::Rgba) {
        return Err("Unexpected HUD artwork format.".into());
    }
    // The top two action tiles share the gameplay HUD atlas with the text,
    // status icons and clock. Replace just these tiles with neutral rings.
    for y in 0..150 {
        for x in 24..344 {
            let center = if x < 188 { 108.0 } else { 268.0 };
            let distance = (x as f64 - center).hypot(y as f64 - 74.0);
            let coverage = (2.5 - (distance - 68.0).abs()).clamp(0.0, 1.0);
            let at = (y * 2048 + x) * 4;
            pixels[at..at + 4].copy_from_slice(&[255, 255, 255, (coverage * 255.0).round() as u8]);
        }
    }
    // The frame UVs in HudItemIconSet point to this otherwise empty tile.
    // Trace a neutral bumper outline; the two mounts mirror the same tile.
    let shoulder = [
        (1094.0, 688.0),
        (1094.0, 602.0),
        (1097.0, 594.0),
        (1106.0, 591.0),
        (1132.0, 592.0),
        (1238.0, 601.0),
        (1272.0, 609.0),
        (1291.0, 620.0),
        (1306.0, 639.0),
        (1320.0, 666.0),
        (1330.0, 685.0),
        (1330.0, 691.0),
        (1099.0, 691.0),
        (1094.0, 688.0),
    ];
    for y in 576..704 {
        for x in 1088..1342 {
            let distance = shoulder
                .windows(2)
                .map(|line| {
                    let [(ax, ay), (bx, by)] = [line[0], line[1]];
                    let (dx, dy): (f64, f64) = (bx - ax, by - ay);
                    let (px, py) = (x as f64 - ax, y as f64 - ay);
                    let t = ((px * dx + py * dy) / (dx * dx + dy * dy)).clamp(0.0, 1.0);
                    (px - t * dx).hypot(py - t * dy)
                })
                .fold(f64::INFINITY, f64::min);
            let coverage = (2.5 - distance).clamp(0.0, 1.0);
            let at = (y * 2048 + x) * 4;
            pixels[at..at + 4].copy_from_slice(&[255, 255, 255, (coverage * 255.0).round() as u8]);
        }
    }
    let mut out = Vec::new();
    {
        let mut encoder = png::Encoder::new(&mut out, info.width, info.height);
        encoder.set_color(png::ColorType::Rgba);
        encoder.set_depth(png::BitDepth::Eight);
        let mut writer = encoder.write_header().map_err(|error| error.to_string())?;
        writer.write_image_data(&pixels).map_err(|error| error.to_string())?;
    }
    Ok(out)
}

pub fn prepare(archive: &[u8], atlas: &[u8], staging: &Path) -> Result<(), String> {
    if archive.len() > MAX_ARCHIVE_BYTES || sha256_hex(archive) != ARCHIVE_SHA256 {
        return Err("The Project Restoration layout download failed verification.".into());
    }
    let mut files = BTreeMap::new();
    let mut reader = SevenZReader::new(Cursor::new(archive), archive.len() as u64, Password::empty())
        .map_err(|error| error.to_string())?;
    reader
        .for_each_entries(|entry, stream| {
            let Some(name) = entry.name().strip_prefix("romfs/") else {
                return Ok(true);
            };
            if !FILES.contains(&name) {
                return Ok(true);
            }
            let mut bytes = Vec::new();
            stream.take(1024 * 1024).read_to_end(&mut bytes).map_err(sevenz_rust::Error::io)?;
            files.insert(name.to_string(), bytes);
            Ok(true)
        })
        .map_err(|error| error.to_string())?;
    let outputs =
        forge_assets::remasked_hud::prepare(&staging.join("romfs.bin"), &files).map_err(|error| error.to_string())?;
    let texture = neutral_atlas(atlas)?;
    let layout = staging.join("RestorationHud/layout");
    std::fs::create_dir_all(&layout).map_err(|error| error.to_string())?;
    for (name, bytes) in outputs {
        std::fs::write(layout.join(name), bytes).map_err(|error| error.to_string())?;
    }
    let artwork = staging.join("HudArtwork");
    let texture_name = "tex1_512x256_A4890349AF9FFFB4_13_mip0.png";
    let overlay = artwork.join("HudOverlay");
    std::fs::create_dir_all(overlay.join("CUSTOM_HUD")).map_err(|error| error.to_string())?;
    std::fs::write(overlay.join("CUSTOM_HUD").join(texture_name), &texture).map_err(|error| error.to_string())?;
    std::fs::write(
        overlay.join("pack.json"),
        r#"{"options":{"skip_mipmap":true,"flip_png_files":true,"use_new_hash":true}}"#,
    )
    .map_err(|error| error.to_string())?;
    std::fs::write(
        artwork.join("owner-content.json"),
        format!(r#"{{"files":{{"HudOverlay/CUSTOM_HUD/{texture_name}":"{}"}}}}"#, sha256_hex(&texture)),
    )
    .map_err(|error| error.to_string())?;
    Ok(())
}

/// # Safety
/// The caller supplies a terminated UTF-8 path and buffers readable for their specified lengths.
#[no_mangle]
pub unsafe extern "C" fn remasked_prepare_hud(
    staging: *const c_char,
    archive: *const u8,
    size: usize,
    atlas: *const u8,
    atlas_size: usize,
) -> bool {
    if staging.is_null()
        || archive.is_null()
        || atlas.is_null()
        || size > MAX_ARCHIVE_BYTES
        || atlas_size > MAX_ATLAS_BYTES
    {
        return false;
    }
    std::panic::catch_unwind(|| {
        let Ok(staging) = CStr::from_ptr(staging).to_str() else {
            return false;
        };
        prepare(
            std::slice::from_raw_parts(archive, size),
            std::slice::from_raw_parts(atlas, atlas_size),
            Path::new(staging),
        )
        .is_ok()
    })
    .unwrap_or(false)
}
