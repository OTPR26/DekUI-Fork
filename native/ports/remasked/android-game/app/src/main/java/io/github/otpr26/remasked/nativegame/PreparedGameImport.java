// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.otpr26.remasked.nativegame;

import android.content.ContentResolver;
import android.database.Cursor;
import android.net.Uri;
import android.provider.DocumentsContract;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.security.MessageDigest;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import org.json.JSONArray;
import org.json.JSONObject;

final class PreparedGameImport {
    private static final String CODE_HASH =
        "6eec27b1827eab2c87744ac38066d569ffe6fe75790b1eb3300bd91f5099038d";
    private static final Map<String, Long> LIMITS = Map.of(
        "code.bin", 32L * 1024 * 1024, "exheader.bin", 2048L,
        "romfs.bin", 4L * 1024 * 1024 * 1024, "process-manifest.json", 1024L * 1024);

    static void install(ContentResolver resolver, Uri folder, Path game, byte[] presentationDefaults) throws Exception {
        String folderId = DocumentsContract.getTreeDocumentId(folder);
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(folder, folderId);
        Map<String, Uri> sources = new HashMap<>();
        String[] columns = {DocumentsContract.Document.COLUMN_DISPLAY_NAME,
            DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_MIME_TYPE};
        try (Cursor cursor = resolver.query(children, columns, null, null, null)) {
            if (cursor == null) throw new IOException("Files could not open the selected folder.");
            while (cursor.moveToNext()) {
                String name = cursor.getString(0);
                if (!LIMITS.containsKey(name)) continue;
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(cursor.getString(2)) ||
                    sources.put(name, DocumentsContract.buildDocumentUriUsingTree(folder, cursor.getString(1))) != null) {
                    throw new IOException("The selected folder contains an invalid or duplicate " + name + ".");
                }
            }
        }
        if (sources.size() != LIMITS.size()) throw new IOException("The prepared folder is incomplete.");

        install(game, presentationDefaults, staging -> {
            for (var entry : LIMITS.entrySet()) {
                String name = entry.getKey();
                Path target = staging.resolve(name);
                try (InputStream input = resolver.openInputStream(sources.get(name));
                     OutputStream output = Files.newOutputStream(target)) {
                    if (input == null) throw new IOException("Could not open " + name + ".");
                    byte[] buffer = new byte[64 * 1024];
                    long size = 0;
                    int read;
                    while ((read = input.read(buffer)) != -1) {
                        size += read;
                        if (size > entry.getValue()) throw new IOException(name + " exceeds its size limit.");
                        output.write(buffer, 0, read);
                    }
                    if (size == 0) throw new IOException(name + " is empty.");
                }
            }
        });
    }

    static void installRom(ContentResolver resolver, Uri uri, Path game, Path privateInputs,
                           byte[] presentationDefaults, RomPreparation prepare) throws Exception {
        Files.createDirectories(privateInputs);
        Path savedRom = privateInputs.resolve("mm3d.3ds");
        Path temporary = Files.createTempFile(privateInputs, ".rom-import-", ".3ds");
        boolean[] addedRom = {false};
        try {
            try (InputStream input = resolver.openInputStream(uri);
                 OutputStream output = Files.newOutputStream(temporary)) {
                if (input == null) throw new IOException("Files could not open the selected ROM.");
                byte[] buffer = new byte[256 * 1024];
                long size = 0;
                int read;
                while ((read = input.read(buffer)) != -1) {
                    size += read;
                    if (size > LIMITS.get("romfs.bin")) throw new IOException("The ROM exceeds 4 GiB.");
                    output.write(buffer, 0, read);
                }
            }
            install(game, presentationDefaults, staging -> {
                prepare.write(temporary, staging, CODE_HASH);
                if (!Files.exists(savedRom)) {
                    Files.move(temporary, savedRom, StandardCopyOption.ATOMIC_MOVE);
                    addedRom[0] = true;
                }
            });
        } catch (Exception error) {
            if (addedRom[0]) Files.deleteIfExists(savedRom);
            throw error;
        } finally {
            Files.deleteIfExists(temporary);
        }
    }

    @FunctionalInterface
    interface RomPreparation {
        void write(Path rom, Path staging, String codeHash) throws Exception;
    }

    @FunctionalInterface
    private interface StageInput {
        void write(Path staging) throws Exception;
    }

    private static void install(Path game, byte[] presentationDefaults, StageInput input) throws Exception {
        Files.createDirectories(game);
        Path destination = game.resolve("PrivateGame");
        Path profile = game.resolve("ReMaskedNativeGame.launch.json");
        if (Files.exists(destination) || Files.exists(profile)) {
            throw new IOException("Game files are already installed locally.");
        }
        Path staging = Files.createTempDirectory(game, ".game-import-");
        boolean published = false;
        try {
            input.write(staging);
            validate(staging);
            for (String directory : List.of("resources", "saves", "logs/crashes", "savestates", "textures/load", "textures/dump")) {
                Files.createDirectories(game.resolve(directory));
            }
            Path canonical = game.resolve("presentation.json");
            if (!Files.exists(canonical))
                Files.write(canonical, presentationDefaults, java.nio.file.StandardOpenOption.CREATE_NEW);
            Path runtime = game.resolve("runtime-presentation.json");
            if (!Files.exists(runtime)) Files.copy(canonical, runtime);
            Path stagedProfile = staging.resolve("launch.json");
            Files.write(stagedProfile, profile().toString(2).getBytes(StandardCharsets.UTF_8));
            Files.move(staging, destination, StandardCopyOption.ATOMIC_MOVE);
            published = true;
            Files.move(destination.resolve("launch.json"), profile, StandardCopyOption.ATOMIC_MOVE);
        } finally {
            if (!Files.exists(profile)) {
                Path incomplete = published ? destination : staging;
                try (var files = Files.walk(incomplete)) {
                    for (Path path : files.sorted(java.util.Comparator.reverseOrder())
                            .collect(java.util.stream.Collectors.toList())) {
                        Files.deleteIfExists(path);
                    }
                }
            }
        }
    }

    private static void validate(Path staging) throws Exception {
        JSONObject manifest = new JSONObject(new String(Files.readAllBytes(staging.resolve("process-manifest.json")), StandardCharsets.UTF_8));
        JSONObject source = manifest.getJSONObject("source");
        long romfsSize = Files.size(staging.resolve("romfs.bin"));
        long offset = source.getLong("romfs_service_offset");
        long size = source.getLong("romfs_service_size");
        if (!"oot3d_native_process_manifest_v1".equals(manifest.getString("format")) ||
            !CODE_HASH.equals(source.getString("code_bin_sha256")) ||
            !"code.bin".equals(source.getString("code_bin_path")) ||
            !"exheader.bin".equals(source.getString("exheader_path")) ||
            !"romfs.bin".equals(source.getString("romfs_image_path")) ||
            Files.size(staging.resolve("exheader.bin")) != 2048 ||
            Files.size(staging.resolve("code.bin")) != source.getLong("code_bin_size") ||
            romfsSize != source.getLong("romfs_image_file_size") ||
            offset < 0 || offset > romfsSize || size <= 0 || size > romfsSize - offset) {
            throw new IOException("The prepared file layout does not match this runtime.");
        }
        byte[] digest = MessageDigest.getInstance("SHA-256").digest(Files.readAllBytes(staging.resolve("code.bin")));
        StringBuilder hash = new StringBuilder();
        for (byte value : digest) hash.append(String.format(java.util.Locale.ROOT, "%02x", value & 255));
        if (!CODE_HASH.contentEquals(hash)) throw new IOException("The prepared code image does not match this runtime.");
    }

    private static JSONObject profile() throws Exception {
        String root = "${profile_dir}";
        return new JSONObject().put("format", "oot3d_native_game_launch_profile_v1").put("arguments", new JSONArray(List.of(
            "--a32-process-manifest", root + "/PrivateGame/process-manifest.json",
            "--resource-root", root + "/resources", "--renderer", "vulkan",
            "--config", root + "/native-game.json", "--initialize-config", "product",
            "--save-data", root + "/saves", "--log-directory", root + "/logs",
            "--crash-directory", root + "/logs/crashes", "--savestate-directory", root + "/savestates",
            "--texture-load-directory", root + "/textures/load", "--texture-dump-directory", root + "/textures/dump",
            "--title-id", "0004000000125500", "--width", "1280", "--height", "720",
            "--gameplay-timing", "native30_no_interpolation", "--presentation-rate", "30", "--serial-presentation")));
    }

    private PreparedGameImport() {}
}
