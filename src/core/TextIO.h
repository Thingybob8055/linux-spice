#pragma once

#include <QString>
#include <QStringList>

// LTspice text files come in two encodings: Windows-1252 (classic) and
// UTF-16LE without BOM (newer releases). We detect on read and preserve the
// encoding on write so files round-trip with LTspice itself.
namespace textio {

enum class Encoding { Latin1, Utf16LE, Utf8 };

struct TextFile {
    QString text;
    Encoding encoding = Encoding::Latin1;
    bool crlf = true;
};

Encoding detectEncoding(const QByteArray &bytes);
QString decode(const QByteArray &bytes, Encoding *detected = nullptr);
QByteArray encode(const QString &text, Encoding encoding);

bool readFile(const QString &path, TextFile &out, QString *error = nullptr);
bool writeFile(const QString &path, const TextFile &file, QString *error = nullptr);

// Convenience: read a file as text with auto-detection, empty string on failure.
QString readText(const QString &path, bool *ok = nullptr);

// Splits on any of CRLF, LF, CR.
QStringList splitLines(const QString &text);

// Tokenizer for the line-oriented .asc/.asy formats: splits on single spaces
// but keeps the rest of the line intact from the `keepRestFrom`-th token on.
QStringList tokenize(const QString &line, int keepRestFrom = -1);

} // namespace textio
