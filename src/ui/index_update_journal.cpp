#include "ui/index_update_journal.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStandardPaths>

#include <utility>

#ifdef Q_OS_UNIX
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace everything_lite {
namespace {
constexpr char kPrefix[] = "index-update-";
constexpr char kSuffix[] = ".jsonl";

bool isJournalName(const QString& name) {
    static const QRegularExpression pattern(QStringLiteral("^index-update-(\\d{4}-\\d{2}-\\d{2})\\.jsonl$"));
    const auto match = pattern.match(name);
    return match.hasMatch() && QDate::fromString(match.captured(1), Qt::ISODate).isValid();
}

void setError(QString* target, const QString& message) {
    if (target) *target = message;
}
} // namespace

IndexUpdateJournal::IndexUpdateJournal(QString directory) : directory_(QDir::cleanPath(std::move(directory))) {}

QString IndexUpdateJournal::defaultDirectory() {
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
        .filePath(QStringLiteral("index-update-logs"));
}

QString IndexUpdateJournal::filePath(const QDate& date) const {
    return QDir(directory_).filePath(QString::fromLatin1(kPrefix) + date.toString(Qt::ISODate)
                                      + QString::fromLatin1(kSuffix));
}

bool IndexUpdateJournal::containsPath(const QString& path) const {
    const auto clean = QDir::cleanPath(path);
    return clean == directory_ || clean.startsWith(directory_ + QDir::separator());
}

bool IndexUpdateJournal::append(const IndexUpdateEvent& event, QString* error) const {
    if (!event.at.isValid() || directory_.isEmpty() || !QDir::isAbsolutePath(directory_)) {
        setError(error, QStringLiteral("日志时间或目录无效"));
        return false;
    }
    if (!QDir().mkpath(directory_) || QFileInfo(directory_).isSymLink()) {
        setError(error, QStringLiteral("无法建立安全的日志目录"));
        return false;
    }
    if (!QFile::setPermissions(directory_, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)) {
        setError(error, QStringLiteral("无法限制日志目录权限"));
        return false;
    }

    const auto path = filePath(event.at.date());
    QJsonObject object{
        {QStringLiteral("at"), event.at.toString(Qt::ISODateWithMs)},
        {QStringLiteral("kind"), event.kind},
        {QStringLiteral("root"), event.root},
        {QStringLiteral("path"), event.path},
        {QStringLiteral("reason"), event.reason},
        {QStringLiteral("attempt"), event.attempt},
    };
    auto line = QJsonDocument(object).toJson(QJsonDocument::Compact);
    line.append('\n');
#ifdef Q_OS_UNIX
    const auto native_path = QFile::encodeName(path);
    const int fd = ::open(native_path.constData(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        setError(error, QString::fromLocal8Bit(std::strerror(errno)));
        return false;
    }
    struct stat info{};
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || ::fchmod(fd, 0600) != 0) {
        setError(error, QStringLiteral("日志文件不是安全的普通文件"));
        ::close(fd);
        return false;
    }
    const char* cursor = line.constData();
    auto remaining = static_cast<std::size_t>(line.size());
    while (remaining > 0) {
        const auto count = ::write(fd, cursor, remaining);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            setError(error, QString::fromLocal8Bit(std::strerror(errno)));
            ::close(fd);
            return false;
        }
        cursor += count;
        remaining -= static_cast<std::size_t>(count);
    }
    if (::close(fd) != 0) {
        setError(error, QString::fromLocal8Bit(std::strerror(errno)));
        return false;
    }
#else
    if (QFileInfo(path).isSymLink()) {
        setError(error, QStringLiteral("日志文件不能是符号链接"));
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append) || file.write(line) != line.size()) {
        setError(error, file.errorString());
        return false;
    }
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
#endif
    if (error) error->clear();
    return true;
}

QStringList IndexUpdateJournal::dates() const {
    QStringList result;
    if (QFileInfo(directory_).isSymLink()) return result;
    const QDir dir(directory_);
    for (const auto& info : dir.entryInfoList(QDir::Files | QDir::NoSymLinks, QDir::Name | QDir::Reversed)) {
        if (!isJournalName(info.fileName())) continue;
        result << info.fileName().mid(int(sizeof(kPrefix) - 1), 10);
    }
    return result;
}

QVector<IndexUpdateEvent> IndexUpdateJournal::read(const QDate& date) const {
    QVector<IndexUpdateEvent> result;
    if (!date.isValid() || QFileInfo(directory_).isSymLink()) return result;
    const auto path = filePath(date);
    const QFileInfo info(path);
    if (info.isSymLink() || !info.isFile()) return result;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return result;
    while (!file.atEnd()) {
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(file.readLine(), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) continue;
        const auto object = document.object();
        result.push_back({object.value(QStringLiteral("kind")).toString(),
                          object.value(QStringLiteral("root")).toString(),
                          object.value(QStringLiteral("path")).toString(),
                          object.value(QStringLiteral("reason")).toString(),
                          object.value(QStringLiteral("attempt")).toInt(),
                          QDateTime::fromString(object.value(QStringLiteral("at")).toString(), Qt::ISODateWithMs)});
    }
    return result;
}

bool IndexUpdateJournal::removeDate(const QDate& date, QString* error) const {
    if (!date.isValid() || QFileInfo(directory_).isSymLink()) {
        setError(error, QStringLiteral("日志日期或目录无效"));
        return false;
    }
    const auto path = filePath(date);
    const QFileInfo info(path);
    if (!isJournalName(info.fileName()) || info.isSymLink() || !info.isFile()) {
        setError(error, QStringLiteral("只允许删除应用的普通日志文件"));
        return false;
    }
    if (!QFile::remove(path)) {
        setError(error, QStringLiteral("无法删除日志文件"));
        return false;
    }
    if (error) error->clear();
    return true;
}

int IndexUpdateJournal::prune(int retention_days, const QDate& today, QString* error) const {
    if (error) error->clear();
    if (retention_days <= 0) return 0;
    if (!today.isValid() || QFileInfo(directory_).isSymLink()) {
        setError(error, QStringLiteral("当前日期或日志目录无效"));
        return -1;
    }
    const auto cutoff = today.addDays(1 - retention_days);
    int removed = 0;
    for (const auto& day : dates()) {
        const auto date = QDate::fromString(day, Qt::ISODate);
        if (!date.isValid() || date >= cutoff || date == today) continue;
        if (!removeDate(date, error)) return -1;
        ++removed;
    }
    return removed;
}

} // namespace everything_lite
