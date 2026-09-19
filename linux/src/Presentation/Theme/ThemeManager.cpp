#include "Presentation/Theme/ThemeManager.h"

#include <QApplication>
#include <QFile>
#include <QGuiApplication>
#include <QSettings>
#include <QStyleHints>

// Qt::ColorScheme / QStyleHints::colorSchemeChanged landed in Qt 6.5. Ubuntu
// 24.04 ships Qt 6.4, so guard the system-theme detection and fall back to a
// dark theme on older runtimes (GUI-only; headless MCP modes never hit this).
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
#define GRIDEX_HAS_COLORSCHEME 1
#else
#define GRIDEX_HAS_COLORSCHEME 0
#endif

namespace gridex {

namespace {

bool systemPrefersDark() {
#if GRIDEX_HAS_COLORSCHEME
    const Qt::ColorScheme s = QGuiApplication::styleHints()->colorScheme();
    return (s == Qt::ColorScheme::Dark) || (s == Qt::ColorScheme::Unknown);
#else
    return true;  // Qt < 6.5: default to the dark palette
#endif
}

}  // namespace

ThemeManager& ThemeManager::instance() {
    static ThemeManager inst;
    return inst;
}

ThemeManager::ThemeManager(QObject* parent) : QObject(parent) {
    QSettings s;
    const QString saved = s.value(QStringLiteral("ui/theme"), QStringLiteral("Auto")).toString();
    if (saved == QLatin1String("Light"))       mode_ = Mode::Light;
    else if (saved == QLatin1String("Dark"))   mode_ = Mode::Dark;
    else                                        mode_ = Mode::Auto;
}

void ThemeManager::apply(QApplication* app) {
    app_ = app;

    // The legacy Phase-2 palette is still reachable via `--legacy-theme`
    // for A/B comparison during the UI refactor.
    const bool useLegacy = app && app->arguments().contains(QStringLiteral("--legacy-theme"));
    if (useLegacy) {
        if (mode_ == Mode::Auto) {
            applyLegacyForSystem(app);
#if GRIDEX_HAS_COLORSCHEME
            connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged,
                    this, [this](Qt::ColorScheme) {
                        if (mode_ == Mode::Auto && app_) {
                            applyLegacyForSystem(app_);
                            emit themeChanged();
                        }
                    }, Qt::UniqueConnection);
#endif
        } else {
            applyQss(app, mode_ == Mode::Light
                         ? QStringLiteral(":/style-light.qss")
                         : QStringLiteral(":/style-dark.qss"));
        }
        return;
    }

    // gx skin — pick dark or light based on mode (Auto follows system).
    applyGxForMode(app);
#if GRIDEX_HAS_COLORSCHEME
    if (mode_ == Mode::Auto) {
        connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged,
                this, [this](Qt::ColorScheme) {
                    if (mode_ == Mode::Auto && app_) {
                        applyGxForMode(app_);
                        emit themeChanged();
                    }
                }, Qt::UniqueConnection);
    }
#endif
}

void ThemeManager::applyGxForMode(QApplication* app) {
    bool dark = true;
    if (mode_ == Mode::Light) dark = false;
    else if (mode_ == Mode::Dark) dark = true;
    else {
        dark = systemPrefersDark();
    }
    applyQss(app, dark ? QStringLiteral(":/style-gx.qss")
                       : QStringLiteral(":/style-gx-light.qss"));
}

void ThemeManager::setMode(Mode mode, QApplication* app) {
    app_ = app;
    mode_ = mode;

    const QString key = (mode == Mode::Light) ? QStringLiteral("Light")
                      : (mode == Mode::Dark)  ? QStringLiteral("Dark")
                                              : QStringLiteral("Auto");
    QSettings s;
    s.setValue(QStringLiteral("ui/theme"), key);

    apply(app);
    emit themeChanged();
}

ThemeManager::Mode ThemeManager::mode() const {
    return mode_;
}

bool ThemeManager::isDark() const {
    if (mode_ == Mode::Light) return false;
    if (mode_ == Mode::Dark)  return true;
    return systemPrefersDark();
}

void ThemeManager::applyQss(QApplication* app, const QString& path) {
    QFile f(path);
    if (f.open(QFile::ReadOnly | QFile::Text)) {
        app->setStyleSheet(QString::fromUtf8(f.readAll()));
        f.close();
    }
}

void ThemeManager::applyLegacyForSystem(QApplication* app) {
    const bool dark = systemPrefersDark();
    applyQss(app, dark ? QStringLiteral(":/style-dark.qss")
                       : QStringLiteral(":/style-light.qss"));
}

}  // namespace gridex
