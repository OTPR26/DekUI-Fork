// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.otpr26.remasked.nativegame;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.net.URI;
import java.nio.file.Path;
import javax.net.ssl.HttpsURLConnection;

final class HudSetup {
    private static final String BASE = "https://zora.re/dl/";

    static void prepare(Path staging) throws IOException {
        byte[] archive = download("mm3d_big_screen_layout_779512b.7z", 128 * 1024);
        byte[] atlas = download("tex1_512x256_E2CEBCB19DA2608F_13.png", 2 * 1024 * 1024);
        System.loadLibrary("SDL2");
        System.loadLibrary("TriAevum");
        if (!prepareNative(staging.toString(), archive, atlas)) {
            throw new IOException("The Project Restoration HUD could not be verified or rebuilt for this ROM.");
        }
    }

    private static byte[] download(String name, int limit) throws IOException {
        var connection = (HttpsURLConnection) URI.create(BASE + name).toURL().openConnection();
        connection.setConnectTimeout(12000);
        connection.setReadTimeout(15000);
        connection.setInstanceFollowRedirects(false);
        try {
            if (connection.getResponseCode() != 200 || connection.getContentLengthLong() > limit) {
                throw new IOException("Unexpected HUD download response.");
            }
            try (var input = connection.getInputStream()) {
                var bytes = new ByteArrayOutputStream();
                byte[] buffer = new byte[8192];
                int count;
                while ((count = input.read(buffer)) != -1) {
                    if (count > limit - bytes.size()) throw new IOException("HUD download exceeds the size limit.");
                    bytes.write(buffer, 0, count);
                }
                if (bytes.size() == 0) throw new IOException("HUD download was empty.");
                return bytes.toByteArray();
            }
        } catch (IOException error) {
            throw new IOException("Could not download the Project Restoration HUD. Check your connection and retry.", error);
        } finally {
            connection.disconnect();
        }
    }

    private static native boolean prepareNative(String staging, byte[] archive, byte[] atlas);
}
