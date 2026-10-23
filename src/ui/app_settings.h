#pragma once

#include <QSettings>

namespace everything_lite {

inline QSettings makeAppSettings() {
    const auto test_file = qgetenv("EVERYTHING_LITE_TEST_SETTINGS_FILE");
    if (!test_file.isEmpty()) return QSettings(QString::fromUtf8(test_file), QSettings::IniFormat);
    return QSettings(QStringLiteral("CICHI"), QStringLiteral("EverythingLite"));
}

} // namespace everything_lite
