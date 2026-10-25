#include "SymbolLibrary.h"

#include "AppSettings.h"
#include "DirCache.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QMutexLocker>

QString normalizeSymbolName(const QString &name)
{
    QString n = name.trimmed();
    n.replace(QLatin1String("\\\\"), QLatin1String("/"));
    n.replace('\\', '/');
    while (n.contains(QLatin1String("//")))
        n.replace(QLatin1String("//"), QLatin1String("/"));
    if (n.endsWith(QLatin1String(".asy"), Qt::CaseInsensitive))
        n.chop(4);
    return n.toLower();
}

SymbolLibrary &SymbolLibrary::instance()
{
    static SymbolLibrary lib;
    return lib;
}

SymbolLibrary::SymbolLibrary() = default;

QString SymbolLibrary::libraryRoot() const
{
    return AppSettings::instance().libraryRoot();
}

QStringList SymbolLibrary::roots() const
{
    QStringList r;
    for (const QString &p : AppSettings::instance().symbolSearchPaths())
        r << QDir::cleanPath(p);
    const QString lib = libraryRoot() + QStringLiteral("/sym");
    if (QFileInfo(lib).isDir())
        r << lib;
    r << QStringLiteral(":/symbols");
    return r;
}

void SymbolLibrary::rescan()
{
    QMutexLocker lock(&m_mutex);
    m_indexed = false;
    m_entries.clear();
    m_byRelName.clear();
    m_byBaseName.clear();
    m_cache.clear();
}

void SymbolLibrary::indexRoot(const QString &root, bool builtin)
{
    QDirIterator it(root, {QStringLiteral("*.asy"), QStringLiteral("*.ASY")}, QDir::Files, QDirIterator::Subdirectories);
    const QString rootPrefix = QDir(root).absolutePath();
    while (it.hasNext()) {
        const QString path = it.next();
        QString rel = builtin ? path.mid(root.size() + 1) : QDir(rootPrefix).relativeFilePath(path);
        rel.chop(4);
        const QString key = rel.toLower();
        if (m_byRelName.contains(key))
            continue; // earlier roots win
        m_byRelName.insert(key, path);
        const QString base = key.section('/', -1);
        if (!m_byBaseName.contains(base))
            m_byBaseName.insert(base, path);
        Entry e;
        e.name = QString(rel).replace('/', '\\');
        e.path = path;
        e.category = rel.contains('/') ? rel.section('/', 0, -2) : QString();
        e.root = root;
        m_entries.append(e);
    }
}

void SymbolLibrary::ensureIndexed()
{
    if (m_indexed)
        return;
    m_indexed = true;
    const QStringList rs = roots();
    for (const QString &r : rs)
        indexRoot(r, r.startsWith(':'));
}

QList<SymbolLibrary::Entry> SymbolLibrary::entries()
{
    QMutexLocker lock(&m_mutex);
    ensureIndexed();
    return m_entries;
}

SymbolPtr SymbolLibrary::loadPath(const QString &path)
{
    auto it = m_cache.constFind(path);
    if (it != m_cache.constEnd())
        return it.value();
    SymbolPtr sym = SymbolDef::load(path);
    if (sym) {
        sym->builtin = path.startsWith(':');
        sym->name = QFileInfo(path).completeBaseName();
    }
    m_cache.insert(path, sym);
    return sym;
}

QString SymbolLibrary::findInDir(const QString &dir, const QString &relName) const
{
    if (dir.isEmpty())
        return QString();
    // Case-insensitive walk of each path component.
    const QStringList parts = relName.split('/', Qt::SkipEmptyParts);
    QString cur = dir;
    for (int i = 0; i < parts.size(); ++i) {
        const bool last = (i == parts.size() - 1);
        const QString match = last ? DirCache::match(cur, parts[i] + QStringLiteral(".asy"))
                                   : DirCache::match(cur, parts[i], true);
        if (match.isEmpty())
            return QString();
        cur = cur + '/' + match;
    }
    return cur;
}

SymbolPtr SymbolLibrary::find(const QString &name, const QString &schematicDir)
{
    QMutexLocker lock(&m_mutex);
    const QString key = normalizeSymbolName(name);
    if (key.isEmpty())
        return {};

    // 1. Schematic directory (exact relative path, then bare name).
    if (!schematicDir.isEmpty()) {
        QString p = findInDir(schematicDir, key);
        if (p.isEmpty() && key.contains('/'))
            p = findInDir(schematicDir, key.section('/', -1));
        if (!p.isEmpty())
            return loadPath(p);
    }

    ensureIndexed();
    auto it = m_byRelName.constFind(key);
    if (it != m_byRelName.constEnd())
        return loadPath(it.value());
    const QString base = key.section('/', -1);
    it = m_byBaseName.constFind(base);
    if (it != m_byBaseName.constEnd())
        return loadPath(it.value());
    return {};
}
