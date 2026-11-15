// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <QApplication>
#include <QBuffer>
#include <QDataStream>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QImage>
#include <QSaveFile>
#include <QWidget>
#include <QTimer>
#include "citra_qt/launcher_icon.h"

#ifdef Q_OS_WIN
#include <memory>
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <initguid.h>
#include <propkey.h>
#include <propsys.h>
#include <propvarutil.h>
#endif

namespace MM3D::LauncherIcon {
bool SaveIco(const QString& resource, const QString& destination) {
    const QImage source(resource);
    if (source.isNull())
        return false;
    constexpr std::array sizes{16, 24, 32, 48, 64, 128, 256};
    std::array<QByteArray, sizes.size()> images;
    for (size_t i = 0; i < sizes.size(); ++i) {
        QBuffer buffer(&images[i]);
        if (!buffer.open(QIODevice::WriteOnly) ||
            !source.scaled(sizes[i], sizes[i], Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                 .save(&buffer, "PNG"))
            return false;
    }
    QByteArray encoded;
    QDataStream stream(&encoded, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream << quint16(0) << quint16(1) << quint16(sizes.size());
    quint32 offset = 6 + 16 * sizes.size();
    for (size_t i = 0; i < sizes.size(); ++i) {
        stream << quint8(sizes[i] % 256) << quint8(sizes[i] % 256) << quint8(0) << quint8(0)
               << quint16(1) << quint16(32) << quint32(images[i].size()) << offset;
        offset += images[i].size();
    }
    for (const auto& image : images)
        stream.writeRawData(image.constData(), image.size());
    QFile existing(destination);
    if (existing.open(QIODevice::ReadOnly) && existing.readAll() == encoded)
        return true;
    if (!QDir().mkpath(QFileInfo(destination).absolutePath()))
        return false;
    QSaveFile file(destination);
    return file.open(QIODevice::WriteOnly) && file.write(encoded) == encoded.size() && file.commit();
}

bool SameApplication(const QString& target, const QString& application) {
    const auto actual = QFileInfo(target).canonicalFilePath();
    const auto expected = QFileInfo(application).canonicalFilePath();
    return !actual.isEmpty() && !expected.isEmpty() &&
           actual.compare(expected, Qt::CaseInsensitive) == 0;
}

#ifdef Q_OS_WIN
namespace {
constexpr auto AppId = L"OTPR26.MajoraReMasked";
struct Release {
    template <typename T> void operator()(T* object) const { object->Release(); }
};
template <typename T> using ComPtr = std::unique_ptr<T, Release>;

bool SetString(IPropertyStore* store, REFPROPERTYKEY key, const QString& value) {
    PROPVARIANT property;
    const auto text = value.toStdWString();
    if (FAILED(InitPropVariantFromString(text.c_str(), &property)))
        return false;
    const auto result = store->SetValue(key, property);
    PropVariantClear(&property);
    return SUCCEEDED(result);
}

QString KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &path)))
        return {};
    const auto result = QString::fromWCharArray(path);
    CoTaskMemFree(path);
    return result;
}

} // namespace

bool UpdateShortcut(const QString& shortcut, const QString& application, const QString& icon_path) {
    // Qt's isSymLink() also reports ordinary .lnk files; reject only filesystem links.
    if (QFileInfo(shortcut).isSymbolicLink() || QFileInfo(shortcut).isJunction())
        return false;
    const auto icon = QDir::toNativeSeparators(icon_path).toStdWString();
    IShellLinkW* raw_link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&raw_link))))
        return false;
    ComPtr<IShellLinkW> link(raw_link);
    IPersistFile* raw_file = nullptr;
    if (FAILED(link->QueryInterface(IID_PPV_ARGS(&raw_file))))
        return false;
    ComPtr<IPersistFile> file(raw_file);
    const auto filename = QDir::toNativeSeparators(shortcut).toStdWString();
    std::array<wchar_t, 32768> target{};
    if (FAILED(file->Load(filename.c_str(), STGM_READWRITE)) ||
        FAILED(link->GetPath(target.data(), int(target.size()), nullptr, 0)) ||
        !SameApplication(QString::fromWCharArray(target.data()), application))
        return false; // Never alter a shortcut to another installation or application.
    IPropertyStore* raw_store = nullptr;
    if (FAILED(link->QueryInterface(IID_PPV_ARGS(&raw_store))))
        return false;
    ComPtr<IPropertyStore> store(raw_store);
    if (FAILED(link->SetIconLocation(icon.c_str(), 0)) ||
        !SetString(store.get(), PKEY_AppUserModel_ID, QString::fromWCharArray(AppId)) ||
        FAILED(store->Commit()) || FAILED(file->Save(filename.c_str(), TRUE)))
        return false;
    SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW, filename.c_str(), nullptr);
    return true;
}

namespace {
ComPtr<ITaskbarList> Taskbar() {
    ITaskbarList* raw = nullptr;
    if (FAILED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&raw))))
        return {};
    ComPtr<ITaskbarList> bar(raw);
    return SUCCEEDED(bar->HrInit()) ? std::move(bar) : ComPtr<ITaskbarList>{};
}

void RefreshTaskbar(QWidget* widget) {
    // Refresh the existing application button, without adding buttons for owned dialogs.
    const auto window = reinterpret_cast<HWND>(widget->winId());
    // Qt's parent relationships can differ from native taskbar ownership.
    if (!IsWindowVisible(window) || GetWindow(window, GW_OWNER) != nullptr)
        return;
    auto bar = Taskbar();
    if (!bar || FAILED(bar->DeleteTab(window)) || FAILED(bar->AddTab(window)))
        return;
    if (!widget->property("launcherTaskbarRegistered").toBool()) {
        widget->setProperty("launcherTaskbarRegistered", true);
        // AddTab registrations must be removed when their window is destroyed.
        QObject::connect(widget, &QObject::destroyed, [window] {
            const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            if (SUCCEEDED(initialized) || initialized == RPC_E_CHANGED_MODE) {
                if (auto cleanup = Taskbar())
                    cleanup->DeleteTab(window);
            }
            if (SUCCEEDED(initialized))
                CoUninitialize();
        });
    }
}

void UpdateShortcuts(const QString& icon_path) {
    const auto application = QCoreApplication::applicationFilePath();
    const std::array<QString, 3> roots{
        KnownFolder(FOLDERID_Desktop), KnownFolder(FOLDERID_Programs),
        KnownFolder(FOLDERID_RoamingAppData) +
            QStringLiteral("/Microsoft/Internet Explorer/Quick Launch/User Pinned/TaskBar")};
    for (const auto& root : roots) {
        if (root.isEmpty() || !QDir(root).exists())
            continue;
        QDirIterator files(root, {QStringLiteral("*.lnk")}, QDir::Files,
                           QDirIterator::Subdirectories);
        while (files.hasNext()) {
            const auto path = files.next();
            UpdateShortcut(path, application, icon_path);
        }
    }
}
} // namespace
#endif

void ApplyWindows(const QString& icon_path) {
#ifdef Q_OS_WIN
    const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE)
        return;
    static QString previous_icon;
    const bool changed = !previous_icon.isEmpty() && previous_icon != icon_path;
    previous_icon = icon_path;
    SetCurrentProcessExplicitAppUserModelID(AppId);
    const auto application = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    const QString icon = QDir::toNativeSeparators(icon_path) + QStringLiteral(",0");
    for (auto* widget : QApplication::topLevelWidgets()) {
        if (widget->windowType() != Qt::Window && widget->windowType() != Qt::Dialog)
            continue;
        IPropertyStore* raw_store = nullptr;
        if (FAILED(SHGetPropertyStoreForWindow(reinterpret_cast<HWND>(widget->winId()),
                                              IID_PPV_ARGS(&raw_store))))
            continue;
        ComPtr<IPropertyStore> store(raw_store);
        // Set relaunch details before the ID, which tells the taskbar to refresh.
        SetString(store.get(), PKEY_AppUserModel_RelaunchCommand,
                  QStringLiteral("\"%1\"").arg(application));
        SetString(store.get(), PKEY_AppUserModel_RelaunchDisplayNameResource,
                  QStringLiteral("Majora ReMasked"));
        SetString(store.get(), PKEY_AppUserModel_RelaunchIconResource, icon);
        SetString(store.get(), PKEY_AppUserModel_ID, QString::fromWCharArray(AppId));
    }
    UpdateShortcuts(icon_path);
    if (changed) {
        for (auto* widget : QApplication::topLevelWidgets()) {
            if (widget->windowType() != Qt::Window && widget->windowType() != Qt::Dialog)
                continue;
            // Let the shell process the window/shortcut icon notifications before
            // rebuilding its cached application button. The context cancels this
            // callback automatically if the window closes in the meantime.
            QTimer::singleShot(100, widget, [widget] {
                const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                if (SUCCEEDED(initialized) || initialized == RPC_E_CHANGED_MODE)
                    RefreshTaskbar(widget);
                if (SUCCEEDED(initialized))
                    CoUninitialize();
            });
        }
    }
    if (SUCCEEDED(initialized))
        CoUninitialize();
#else
    Q_UNUSED(icon_path);
#endif
}
} // namespace MM3D::LauncherIcon
