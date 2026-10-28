#include "ModelLibrary.h"

#include "AppSettings.h"
#include "DirCache.h"
#include "TextIO.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QMutexLocker>
#include <QRegularExpression>

ModelLibrary &ModelLibrary::instance()
{
    static ModelLibrary lib;
    return lib;
}

void ModelLibrary::clearCache()
{
    QMutexLocker lock(&m_mutex);
    m_cache.clear();
    m_subIndex.clear();
    m_subIndexed = false;
    m_standard.clear();
    m_standardLoaded = false;
}

bool ModelLibrary::isEncryptedFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QByteArray head = f.read(256);
    // Two formats: a "<Binary File>" marker, and newer files that announce themselves as
    // "* LTspice Encrypted File" in a comment header.
    return head.contains("<Binary File>") || head.contains("LTspice Encrypted File");
}

bool ModelLibrary::isBinaryLine(const QString &line)
{
    int bad = 0;
    for (QChar c : line) {
        const char16_t u = c.unicode();
        if ((u < 0x20 && u != '\t') || (u >= 0x7F && u < 0xA0) || u == 0xFFFD)
            ++bad;
    }
    return bad > 2 || (line.size() > 0 && bad * 20 > line.size());
}

QStringList ModelLibrary::logicalLines(const QString &text)
{
    QStringList out;
    for (const QString &raw : textio::splitLines(text)) {
        if (isBinaryLine(raw)) {
            out << QStringLiteral("*<binary>");
            continue;
        }
        QString l = raw;
        // The micro sign is a valid SPICE multiplier in LTspice files.
        l.replace(QChar(0xB5), 'u');
        l.replace(QChar(0x03BC), 'u');
        // Inline ';' comments (LTspice) — but keep ';' inside quotes/braces untouched.
        int depth = 0;
        bool quote = false;
        for (int i = 0; i < l.size(); ++i) {
            const QChar c = l[i];
            if (c == '"')
                quote = !quote;
            else if (!quote && (c == '{' || c == '('))
                ++depth;
            else if (!quote && (c == '}' || c == ')'))
                --depth;
            else if (!quote && depth <= 0 && c == ';') {
                l.truncate(i);
                break;
            }
        }
        const QString t = l.trimmed();
        if (t.isEmpty() || (t.startsWith('*') && t != QLatin1String("*<binary>")))
            continue;
        if (t.startsWith('+') && !out.isEmpty()) {
            out.last() += ' ' + t.mid(1).trimmed();
            continue;
        }
        out << t;
    }
    return out;
}

LibraryFilePtr ModelLibrary::parseText(const QString &text, const QString &path)
{
    auto lib = LibraryFilePtr::create();
    lib->path = path;
    const QStringList lines = logicalLines(text);
    static const QRegularExpression modelRe(QStringLiteral("^\\.model\\s+(\\S+)\\s+(?:ako:\\s*\\S+\\s+)?([A-Za-z]+)"),
                                            QRegularExpression::CaseInsensitiveOption);
    ModelDef *openSub = nullptr;
    int subDepth = 0;
    QString section;
    QStringList sectionLines;
    for (const QString &l : lines) {
        const QString low = l.toLower();
        if (!section.isEmpty()) {
            if (low.startsWith(QLatin1String(".endl"))) {
                lib->sections.insert(section.toLower(), sectionLines);
                section.clear();
                sectionLines.clear();
            } else {
                sectionLines << l;
            }
        }
        if (l == QLatin1String("*<binary>")) {
            if (openSub)
                openSub->encrypted = true;
            continue;
        }
        if (openSub) {
            openSub->lines << l;
            if (low.startsWith(QLatin1String(".subckt")))
                ++subDepth;
            else if (low.startsWith(QLatin1String(".ends"))) {
                if (--subDepth == 0)
                    openSub = nullptr;
            }
            continue;
        }
        if (low.startsWith(QLatin1String(".subckt"))) {
            const QStringList t = l.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
            if (t.size() >= 2) {
                ModelDef d;
                d.kind = ModelDef::Subckt;
                d.name = t[1];
                d.file = path;
                d.lines << l;
                const QString key = d.name.toLower();
                if (!lib->defs.contains(key)) {
                    lib->defs.insert(key, d);
                    openSub = &lib->defs[key];
                    subDepth = 1;
                } else {
                    // Duplicate definition: skip its body.
                    static ModelDef sink;
                    sink = ModelDef();
                    openSub = &sink;
                    subDepth = 1;
                }
            }
            continue;
        }
        if (low.startsWith(QLatin1String(".model"))) {
            const auto m = modelRe.match(l);
            if (m.hasMatch()) {
                ModelDef d;
                d.kind = ModelDef::Model;
                d.name = m.captured(1);
                d.type = m.captured(2).toUpper();
                d.lines << l;
                d.file = path;
                if (!lib->defs.contains(d.name.toLower()))
                    lib->defs.insert(d.name.toLower(), d);
            }
            continue;
        }
        if (low.startsWith(QLatin1String(".lib ")) && section.isEmpty()) {
            // HSPICE style section header: ".lib name" (single word, no file extension).
            const QStringList t = l.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
            if (t.size() == 2 && !t[1].contains('.') && !t[1].contains('/') && !t[1].contains('\\')) {
                section = t[1];
                continue;
            }
        }
        if (low.startsWith(QLatin1String(".param")) || low.startsWith(QLatin1String(".func")))
            lib->globalLines << l;
    }
    return lib;
}

LibraryFilePtr ModelLibrary::load(const QString &path)
{
    QMutexLocker lock(&m_mutex);
    const QFileInfo fi(path);
    auto it = m_cache.constFind(path);
    if (it != m_cache.constEnd() && it->mtime == fi.lastModified())
        return it->lib;
    LibraryFilePtr lib;
    if (isEncryptedFile(path)) {
        lib = LibraryFilePtr::create();
        lib->path = path;
        lib->encrypted = true;
    } else {
        bool ok = false;
        const QString text = textio::readText(path, &ok);
        lib = ok ? parseText(text, path) : LibraryFilePtr();
    }
    if (lib)
        m_cache.insert(path, {fi.lastModified(), lib});
    return lib;
}

void ModelLibrary::indexSubDir()
{
    if (m_subIndexed)
        return;
    m_subIndexed = true;
    const QString root = AppSettings::instance().libraryRoot();
    for (const QString &sub : {root + QStringLiteral("/sub"), root + QStringLiteral("/cmp"), QStringLiteral(":/sub")}) {
        QDirIterator it(sub, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString p = it.next();
            const QString key = QFileInfo(p).fileName().toLower();
            if (!m_subIndex.contains(key))
                m_subIndex.insert(key, p);
        }
    }
}

static QString findCaseInsensitive(const QString &dir, const QString &rel)
{
    if (dir.isEmpty())
        return QString();
    QString cur = dir;
    const QStringList parts = QString(rel).replace('\\', '/').split('/', Qt::SkipEmptyParts);
    for (int i = 0; i < parts.size(); ++i) {
        if (parts[i] == QLatin1String(".")) // "./lib/x.sub"
            continue;
        const bool last = i == parts.size() - 1;
        QString match = DirCache::match(cur, parts[i], !last);
        if (match.isEmpty() && parts[i] == QLatin1String(".."))
            match = parts[i];
        if (match.isEmpty())
            return QString();
        cur = cur + '/' + match;
    }
    return QDir::cleanPath(cur);
}

QString ModelLibrary::resolveFile(const QString &nameIn, const QString &baseDir)
{
    QString name = nameIn.trimmed();
    if (name.size() >= 2 && (name.startsWith('"') || name.startsWith('\'')))
        name = name.mid(1, name.size() - 2);
    name.replace(QLatin1String("\\\\"), QLatin1String("/"));
    name.replace('\\', '/');
    if (name.isEmpty())
        return QString();
    // Windows absolute paths cannot exist here; fall back to the file name.
    static const QRegularExpression winAbs(QStringLiteral("^[A-Za-z]:/"));
    if (winAbs.match(name).hasMatch())
        name = QFileInfo(name).fileName();
    if (QDir::isAbsolutePath(name) && QFileInfo::exists(name))
        return name;

    QStringList dirs;
    if (!baseDir.isEmpty())
        dirs << baseDir;
    dirs << AppSettings::instance().librarySearchPaths();
    const QString root = AppSettings::instance().libraryRoot();
    dirs << root + QStringLiteral("/sub") << root + QStringLiteral("/cmp") << root;
    dirs << AppSettings::instance().userFilesDir();
    dirs << QStringLiteral(":/sub"); // built-in models shipped with LinSpice
    for (const QString &d : dirs) {
        const QString p = findCaseInsensitive(d, name);
        if (!p.isEmpty() && QFileInfo(p).isFile())
            return p;
    }
    QMutexLocker lock(&m_mutex);
    indexSubDir();
    return m_subIndex.value(QFileInfo(name).fileName().toLower());
}

QString ModelLibrary::findSubcktFile(const QString &name)
{
    QMutexLocker lock(&m_mutex);
    indexSubDir();
    for (const char *ext : {".sub", ".lib", ".mod", ".cir"}) {
        const QString p = m_subIndex.value(name.toLower() + QLatin1String(ext));
        if (!p.isEmpty())
            return p;
    }
    return QString();
}

QList<ModelDef> ModelLibrary::standardModelsOfType(const QStringList &types)
{
    standardModel(QStringLiteral("__load__"));
    QMutexLocker lock(&m_mutex);
    QList<ModelDef> out;
    for (const ModelDef &d : std::as_const(m_standard))
        if (types.contains(d.type))
            out << d;
    std::sort(out.begin(), out.end(), [](const ModelDef &a, const ModelDef &b) { return a.name.compare(b.name, Qt::CaseInsensitive) < 0; });
    return out;
}

std::optional<ModelDef> ModelLibrary::standardModel(const QString &name)
{
    QMutexLocker lock(&m_mutex);
    if (!m_standardLoaded) {
        m_standardLoaded = true;
        const QString cmp = AppSettings::instance().libraryRoot() + QStringLiteral("/cmp");
        for (const char *f : {"standard.bjt", "standard.dio", "standard.mos", "standard.jft"}) {
            const QString path = findCaseInsensitive(cmp, QLatin1String(f));
            if (path.isEmpty())
                continue;
            bool ok = false;
            const QString text = textio::readText(path, &ok);
            if (!ok)
                continue;
            const LibraryFilePtr lib = parseText(text, path);
            for (auto it = lib->defs.constBegin(); it != lib->defs.constEnd(); ++it)
                if (!m_standard.contains(it.key()))
                    m_standard.insert(it.key(), it.value());
        }
    }
    auto it = m_standard.constFind(name.toLower());
    if (it == m_standard.constEnd())
        return std::nullopt;
    return it.value();
}
