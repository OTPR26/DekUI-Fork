// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <iostream>
#include <array>
#include <stdexcept>
#include <source_location>
#include "mm3d/texture_picker.h"

static void Check(bool ok, const std::source_location where = std::source_location::current()) {
    if (!ok) throw std::runtime_error("content matching regression at line " + std::to_string(where.line()));
}
static void Write(const QString& path, const QByteArray& bytes) {
    QFile f(path); Check(f.open(QIODevice::WriteOnly)); Check(f.write(bytes) == bytes.size());
}
int main(int argc, char** argv) try {
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    Check(dir.isValid());
    QDir(dir.path()).mkdir("title-screen");
    const auto path = dir.path() + "/title-screen/title_logo.json";
    Write(path, R"({"schema":1,"keys":["1234"],"scale":0.75,"x":0.1,"y":0})");
    const std::array<std::uint8_t, 4> pixels{1, 2, 3, 4};
    const auto hash = MM3D::Textures::ContentHash(pixels);
    const std::string context = "unit=0 shader=abc swizzle=def vertices=12 blend=1 viewport_raw=100,200";
    const auto row = QByteArray::fromStdString("key=0x1234 sha256=" + hash +
        " size=64x64 format=3 " + context + " target=999 depth=888 feedback=false\n");
    Write(dir.path() + "/logo-intro.log", row);
    Check(MM3D::Textures::LoadProfileDefinition(path.toStdString()));
    Check(MM3D::Textures::saved_scale.load() == .75f);
    // Reproduce copyright preview edits while the saved logo remains enabled.
    MM3D::Textures::ToggleSelection(0x5678);
    MM3D::Textures::group_scale.store(.25f);
    MM3D::Textures::group_x.store(.4f);
    MM3D::Textures::ScaleSelected();
    Check(MM3D::Textures::ScaleMatches(0x5678));
    Check(!MM3D::Textures::ScaleMatches(0x1234));
    MM3D::Textures::scale_current_draw = false;
    Check(MM3D::Textures::CurrentDrawTransform().scale == .75f);
    Check(MM3D::Textures::CurrentDrawTransform().x == .1f);
    MM3D::Textures::scale_current_draw = true;
    Check(MM3D::Textures::CurrentDrawTransform().scale == .25f);
    Check(MM3D::Textures::CurrentDrawTransform().x == .4f);
    MM3D::Textures::scale_until.store(0);
    MM3D::Textures::scale_current_draw = false;
    Check(!MM3D::Textures::ScaleMatches(0x5678));
    Check(MM3D::Textures::CurrentDrawTransform().scale == .75f);
    Check(MM3D::Textures::ContentCandidate(64, 64, 3));
    Check(MM3D::Textures::ContentMatches(64, 64, 3, hash, context));
    Check(!MM3D::Textures::ContentMatches(64, 64, 3, hash, context + "changed"));
    Check(!MM3D::Textures::ContentMatches(64, 64, 3, std::string(64, '0'), context));
    Check(!MM3D::Textures::ContentMatches(128, 64, 3, hash, context));
    MM3D::Textures::ObserveProfileKey(0x1234);
    Check(!MM3D::Textures::ScaleMatches(0x1234));
    MM3D::Textures::DisableProfile();
    Check(!MM3D::Textures::ContentMatches(64, 64, 3, hash, context));
    // Reload must use persisted rules even when the recording is no longer present.
    QFile::remove(dir.path() + "/logo-intro.log");
    Check(MM3D::Textures::LoadProfileDefinition(path.toStdString()));
    Check(MM3D::Textures::ContentMatches(64, 64, 3, hash, context));
    std::cout << "PASS: migration, persistence, hash/context rejection, disable, no address fallback\n";
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}
