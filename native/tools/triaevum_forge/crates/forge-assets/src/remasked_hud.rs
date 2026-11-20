// SPDX-License-Identifier: GPL-3.0-or-later

use std::collections::BTreeMap;
use std::path::Path;

use forge_base::{hashing::sha256_hex, ForgeError, Result};
use serde_json::Value;

const FINGERPRINTS: &str = include_str!("../../../../../titles/mm3d/dekui/setup/setup-fingerprints.json");

pub fn prepare(romfs: &Path, restoration: &BTreeMap<String, Vec<u8>>) -> Result<BTreeMap<String, Vec<u8>>> {
    let invalid = |message: &str| ForgeError::invalid(message);
    let manifest: Value = serde_json::from_str(FINGERPRINTS).map_err(|_| invalid("Invalid HUD fingerprints"))?;
    let native = |name: &str| {
        crate::romfs::read_level3_file(romfs, name)?.ok_or_else(|| invalid("The ROM is missing a HUD input"))
    };
    let play = native("layout/Joker.Main.Play.gar")?;
    if manifest["original_action_sha256"].as_str() != Some(sha256_hex(&play).as_str()) {
        return Err(invalid("Unsupported original HUD layout"));
    }
    let mut outputs = BTreeMap::new();
    for entry in manifest["files"].as_array().ok_or_else(|| invalid("Invalid HUD inventory"))? {
        let field = |key: &str| entry[key].as_str().ok_or_else(|| invalid("Invalid HUD inventory entry"));
        let Some(output) = field("output")?.strip_prefix("romfs/layout/") else { continue };
        if output.contains(".mm3d-") {
            continue;
        }
        let input = field("input")?.strip_prefix("romfs/").ok_or_else(|| invalid("Invalid HUD input path"))?;
        let bytes = match field("input_root")? {
            "original" => native(input)?,
            "restoration" => restoration.get(input).cloned().ok_or_else(|| invalid("Missing Restoration layout"))?,
            _ => return Err(invalid("Invalid HUD input origin")),
        };
        if sha256_hex(&bytes) != field("input_sha256")? {
            return Err(invalid("The HUD input differs from the supported layout revision"));
        }
        let mut prepared = crate::mm3d_layout::transform(output, &bytes, &play)?;
        if sha256_hex(&prepared) != field("output_sha256")? || entry["size"].as_u64() != Some(prepared.len() as u64) {
            return Err(invalid("The rebuilt HUD does not match the approved layout"));
        }
        if matches!(output, "Joker.Main.New.gar" | "Joker.Main.Play.gar") {
            crate::mm3d_layout::apply_remasked_buttons(output, &mut prepared)?;
        }
        outputs.insert(output.to_owned(), prepared);
    }
    Ok(outputs)
}
