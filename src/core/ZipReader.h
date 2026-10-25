#pragma once

#include <QString>
#include <QStringList>

#include <functional>

// Minimal ZIP extractor (stored + deflate) used to import an LTspice
// component library (lib.zip) and example circuits (examples.zip) that the
// user already has. Rejects absolute paths and ".." components.
class ZipReader
{
public:
    explicit ZipReader(const QString &path);
    bool isValid() const { return m_valid; }
    QString error() const { return m_error; }
    QStringList entries() const;

    // Extracts everything (optionally only entries under `prefix`) into dest.
    // The progress callback receives (done, total) and may return false to cancel.
    bool extractAll(const QString &dest, const QString &prefix = QString(),
                    const std::function<bool(int, int)> &progress = {});

private:
    struct Entry {
        QString name;
        quint16 method = 0;
        quint32 compSize = 0;
        quint32 size = 0;
        quint32 localOffset = 0;
    };
    bool readDirectory();
    bool readEntry(const Entry &e, QByteArray &out);

    QString m_path;
    QByteArray m_data;
    QList<Entry> m_entries;
    bool m_valid = false;
    QString m_error;
};
