#pragma once

#include "Symbol.h"

#include <QHash>
#include <QMutex>
#include <QStringList>

// Resolves symbol names (as written in SYMBOL lines, e.g. "res",
// "OpAmps\\LT1001", "PowerProducts\\LT3080") to parsed symbol definitions.
//
// Lookup order mirrors the documented LTspice behavior:
//   1. directory of the schematic being netlisted (and its subdirectories)
//   2. user configured symbol search paths
//   3. the imported component library (libraryRoot/sym)
//   4. built-in fallback symbols shipped with LinSpice
// Names are matched case-insensitively; a bare name also matches a symbol in
// any subdirectory.
class SymbolLibrary
{
public:
    static SymbolLibrary &instance();

    SymbolPtr find(const QString &name, const QString &schematicDir = QString());
    // Rescan all roots (after settings change or library import).
    void rescan();

    struct Entry {
        QString name;     // relative name using '\\' as separator (e.g. "OpAmps\\LT1001")
        QString path;     // absolute file path
        QString category; // first path component or "" for top level
        QString root;     // which root it came from
    };
    // All symbols available in the component browser (deduplicated by name).
    QList<Entry> entries();
    QStringList roots() const;

    // Directory containing the imported library's sub/ etc.
    QString libraryRoot() const;

private:
    SymbolLibrary();
    void ensureIndexed();
    void indexRoot(const QString &root, bool builtin);
    SymbolPtr loadPath(const QString &path);
    QString findInDir(const QString &dir, const QString &relName) const;

    QMutex m_mutex;
    bool m_indexed = false;
    QList<Entry> m_entries;
    QHash<QString, QString> m_byRelName;  // lower-case rel name -> path
    QHash<QString, QString> m_byBaseName; // lower-case base name -> path
    QHash<QString, SymbolPtr> m_cache;    // path -> parsed symbol
};

// Normalizes a symbol reference ("OpAmps\\\\LT1001", "opamps/LT1001.asy") to
// a lower-case relative key with '/' separators and no extension.
QString normalizeSymbolName(const QString &name);
