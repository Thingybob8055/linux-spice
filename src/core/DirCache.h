#pragma once

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QString>

// Cached, case-insensitive directory listings. Library and example folders
// hold thousands of files and are probed once per symbol/model lookup, so
// listing them every time is far too slow. Entries are refreshed when the
// directory's modification time changes.
class DirCache
{
public:
    // Returns the actual file name matching `name` case-insensitively, or "".
    static QString match(const QString &dir, const QString &name, bool dirsOnly = false)
    {
        auto &self = instance();
        QMutexLocker lock(&self.m_mutex);
        const Listing &l = self.listing(dir);
        const QString key = name.toLower();
        return dirsOnly ? l.dirs.value(key) : l.files.value(key);
    }

    // All file names in dir (actual spelling) with the given lower-case suffix.
    static QStringList filesWithSuffix(const QString &dir, const QString &suffixLower)
    {
        auto &self = instance();
        QMutexLocker lock(&self.m_mutex);
        const Listing &l = self.listing(dir);
        QStringList out;
        for (auto it = l.files.constBegin(); it != l.files.constEnd(); ++it)
            if (it.key().endsWith(suffixLower))
                out << it.value();
        return out;
    }

    static void clear()
    {
        auto &self = instance();
        QMutexLocker lock(&self.m_mutex);
        self.m_cache.clear();
    }

private:
    struct Listing {
        QDateTime mtime;
        QHash<QString, QString> files; // lower -> actual
        QHash<QString, QString> dirs;
    };

    static DirCache &instance()
    {
        static DirCache c;
        return c;
    }

    const Listing &listing(const QString &dir)
    {
        const QDateTime mt = QFileInfo(dir).lastModified();
        auto it = m_cache.find(dir);
        if (it != m_cache.end() && it->mtime == mt)
            return it.value();
        Listing l;
        l.mtime = mt;
        const QDir d(dir);
        for (const QString &f : d.entryList(QDir::Files))
            l.files.insert(f.toLower(), f);
        for (const QString &f : d.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
            l.dirs.insert(f.toLower(), f);
        return m_cache.insert(dir, l).value();
    }

    QMutex m_mutex;
    QHash<QString, Listing> m_cache;
};
