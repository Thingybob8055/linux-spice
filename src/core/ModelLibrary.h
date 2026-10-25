#pragma once

#include <QDateTime>
#include <QHash>
#include <QMutex>
#include <QSharedPointer>
#include <QString>
#include <QStringList>

#include <optional>

// Index of the .model and .subckt definitions contained in SPICE library files.
struct ModelDef {
    enum Kind { Model, Subckt };
    Kind kind = Model;
    QString name;      // as written
    QString type;      // model type (NPN, D, VDMOS, ...) for models
    QStringList lines; // full text (continuations joined)
    QString file;
    bool encrypted = false; // body contains binary (encrypted) data
};

struct LibraryFile {
    QString path;
    bool encrypted = false;
    QHash<QString, ModelDef> defs; // lower-case name -> definition
    QStringList globalLines;       // .param/.func lines at file scope
    QHash<QString, QStringList> sections; // .lib section name -> lines (HSPICE style)
};
using LibraryFilePtr = QSharedPointer<LibraryFile>;

class ModelLibrary
{
public:
    static ModelLibrary &instance();

    // Locates a library file referenced by .lib/.include. Search order:
    // absolute path, schematic directory, configured library search paths,
    // libraryRoot/sub (recursively), libraryRoot/cmp.
    QString resolveFile(const QString &name, const QString &baseDir);

    // Parses (and caches) a library file.
    LibraryFilePtr load(const QString &path);

    // Looks up a model in LTspice's standard component databases
    // (cmp/standard.bjt, .dio, .mos, .jft) for implicit model references.
    std::optional<ModelDef> standardModel(const QString &name);
    // All standard models whose type is one of `types` (upper case), sorted by name.
    QList<ModelDef> standardModelsOfType(const QStringList &types);

    // Finds a subcircuit file "<name>.sub"/"<name>.lib" in the library tree.
    QString findSubcktFile(const QString &name);

    static bool isEncryptedFile(const QString &path);
    // Splits text into logical lines joining '+' continuations and dropping comments.
    static QStringList logicalLines(const QString &text);
    static bool isBinaryLine(const QString &line);
    static LibraryFilePtr parseText(const QString &text, const QString &path);

    void clearCache();

private:
    ModelLibrary() = default;
    void indexSubDir();

    QMutex m_mutex;
    struct Cached {
        QDateTime mtime;
        LibraryFilePtr lib;
    };
    QHash<QString, Cached> m_cache;
    QHash<QString, QString> m_subIndex; // lower-case file name -> path
    bool m_subIndexed = false;
    QHash<QString, ModelDef> m_standard;
    bool m_standardLoaded = false;
};
