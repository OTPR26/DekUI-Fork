// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <catch2/catch_test_macros.hpp>
#include "citra_qt/launcher_icon.h"

TEST_CASE("Selected launcher artwork becomes a multi-resolution Windows icon", "[desktop][icon]") {
    QTemporaryDir folder;
    REQUIRE(folder.isValid());
    QImage image(512, 512, QImage::Format_ARGB32);
    image.fill(qRgb(23, 45, 67));
    const auto source = folder.filePath(QStringLiteral("chosen.png"));
    REQUIRE(image.save(source));
    const auto destination = folder.filePath(QStringLiteral("folder with spaces/chosen.ico"));
    REQUIRE(MM3D::LauncherIcon::SaveIco(source, destination));
    QFile file(destination);
    REQUIRE(file.open(QIODevice::ReadOnly));
    const auto data = file.readAll();
    QDataStream stream(data);
    stream.setByteOrder(QDataStream::LittleEndian);
    quint16 reserved, type, count;
    stream >> reserved >> type >> count;
    REQUIRE(reserved == 0);
    REQUIRE(type == 1);
    REQUIRE(count == 7);
    constexpr std::array sizes{16, 24, 32, 48, 64, 128, 256};
    for (int size : sizes) {
        quint8 width, height, colors, unused;
        quint16 planes, bits;
        quint32 bytes, offset;
        stream >> width >> height >> colors >> unused >> planes >> bits >> bytes >> offset;
        REQUIRE(width == size % 256);
        REQUIRE(height == size % 256);
        REQUIRE(planes == 1);
        REQUIRE(bits == 32);
        const auto decoded = QImage::fromData(data.mid(offset, bytes), "PNG");
        REQUIRE(decoded.size() == QSize(size, size));
        REQUIRE(decoded.pixelColor(size / 2, size / 2) == QColor(23, 45, 67));
    }
    file.close();
    REQUIRE_FALSE(MM3D::LauncherIcon::SaveIco(folder.filePath("missing.png"), destination));
    REQUIRE(file.open(QIODevice::ReadOnly));
    REQUIRE(file.readAll() == data);
}

TEST_CASE("Shortcut icon updates match the exact application installation", "[desktop][icon]") {
    QTemporaryDir folder;
    REQUIRE(folder.isValid());
    const auto app = folder.filePath("Majora-ReMasked.exe");
    QFile executable(app);
    REQUIRE(executable.open(QIODevice::WriteOnly));
    executable.close();
    REQUIRE(QDir(folder.path()).mkdir("other"));
    const auto other = folder.filePath("other/Majora-ReMasked.exe");
    REQUIRE(QFile::copy(app, other));
    REQUIRE(MM3D::LauncherIcon::SameApplication(app, app));
    REQUIRE(MM3D::LauncherIcon::SameApplication(folder.filePath("other/../Majora-ReMasked.exe"), app));
    REQUIRE_FALSE(MM3D::LauncherIcon::SameApplication(other, app));
    REQUIRE_FALSE(MM3D::LauncherIcon::SameApplication(folder.filePath("missing.exe"), app));
}

#ifdef Q_OS_WIN
#include <windows.h>
#include <shobjidl.h>
#include <QFileInfo>

TEST_CASE("Windows shell shortcuts accept selected icons without changing their target",
          "[desktop][icon][windows]") {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    REQUIRE(SUCCEEDED(initialized));
    struct Uninitialize { ~Uninitialize() { CoUninitialize(); } } uninitialize;
    QTemporaryDir folder;
    REQUIRE(folder.isValid());
    const auto application = folder.filePath("Majora-ReMasked.exe");
    QFile executable(application);
    REQUIRE(executable.open(QIODevice::WriteOnly));
    executable.close();
    const auto shortcut = folder.filePath("Majora ReMasked.lnk");
    const auto icon = folder.filePath("chosen.ico");
    QImage image(32, 32, QImage::Format_ARGB32);
    image.fill(Qt::cyan);
    const auto png = folder.filePath("chosen.png");
    REQUIRE(image.save(png));
    REQUIRE(MM3D::LauncherIcon::SaveIco(png, icon));
    IShellLinkW* link = nullptr;
    REQUIRE(SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&link))));
    struct LinkRelease { IShellLinkW* link; ~LinkRelease() { link->Release(); } } release{link};
    REQUIRE(SUCCEEDED(link->SetPath(QDir::toNativeSeparators(application).toStdWString().c_str())));
    IPersistFile* file = nullptr;
    REQUIRE(SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file))));
    struct FileRelease { IPersistFile* file; ~FileRelease() { file->Release(); } } file_release{file};
    const auto filename = QDir::toNativeSeparators(shortcut).toStdWString();
    REQUIRE(SUCCEEDED(file->Save(filename.c_str(), TRUE)));
    REQUIRE(QFileInfo(shortcut).isShortcut());
    REQUIRE(MM3D::LauncherIcon::UpdateShortcut(shortcut, application, icon));
    REQUIRE(SUCCEEDED(file->Load(filename.c_str(), STGM_READ)));
    std::array<wchar_t, 32768> selected{}, target{};
    int index = -1;
    REQUIRE(SUCCEEDED(link->GetIconLocation(selected.data(), int(selected.size()), &index)));
    REQUIRE(MM3D::LauncherIcon::SameApplication(QString::fromWCharArray(selected.data()), icon));
    REQUIRE(index == 0);
    REQUIRE(SUCCEEDED(link->GetPath(target.data(), int(target.size()), nullptr, 0)));
    REQUIRE(MM3D::LauncherIcon::SameApplication(QString::fromWCharArray(target.data()), application));
    const auto before = QFile(shortcut).size();
    REQUIRE_FALSE(MM3D::LauncherIcon::UpdateShortcut(shortcut, png, application));
    REQUIRE(QFile(shortcut).size() == before);
}
#endif
