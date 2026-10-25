#include "TextIO.h"

#include <QFile>
#include <QSaveFile>
#include <QStringDecoder>

namespace textio {

Encoding detectEncoding(const QByteArray &bytes)
{
    if (bytes.startsWith("\xff\xfe"))
        return Encoding::Utf16LE;
    if (bytes.startsWith("\xef\xbb\xbf"))
        return Encoding::Utf8;
    // UTF-16LE without BOM: ASCII text with every odd byte zero.
    const int probe = std::min<int>(bytes.size(), 512) & ~1;
    if (probe >= 4) {
        int zeroOdd = 0, zeroEven = 0;
        for (int i = 0; i < probe; i += 2) {
            if (bytes[i + 1] == 0)
                ++zeroOdd;
            if (bytes[i] == 0)
                ++zeroEven;
        }
        if (zeroOdd > probe / 4 && zeroEven < probe / 32)
            return Encoding::Utf16LE;
    }
    return Encoding::Latin1;
}

QString decode(const QByteArray &bytes, Encoding *detected)
{
    const Encoding enc = detectEncoding(bytes);
    if (detected)
        *detected = enc;
    switch (enc) {
    case Encoding::Utf16LE: {
        QByteArray b = bytes.startsWith("\xff\xfe") ? bytes.mid(2) : bytes;
        if (b.size() % 2)
            b.chop(1);
        return QString::fromUtf16(reinterpret_cast<const char16_t *>(b.constData()), b.size() / 2);
    }
    case Encoding::Utf8:
        return QString::fromUtf8(bytes.mid(3));
    case Encoding::Latin1:
    default:
        // Windows-1252 differs from Latin-1 only in 0x80-0x9F; decode via Qt if available.
        {
            QStringDecoder dec("windows-1252");
            if (dec.isValid())
                return dec.decode(bytes);
        }
        return QString::fromLatin1(bytes);
    }
}

QByteArray encode(const QString &text, Encoding encoding)
{
    switch (encoding) {
    case Encoding::Utf16LE: {
        QByteArray out;
        out.resize(text.size() * 2);
        for (int i = 0; i < text.size(); ++i) {
            const char16_t c = text[i].unicode();
            out[2 * i] = char(c & 0xff);
            out[2 * i + 1] = char(c >> 8);
        }
        return out;
    }
    case Encoding::Utf8:
        return text.toUtf8();
    case Encoding::Latin1:
    default: {
        QByteArray out;
        out.reserve(text.size());
        for (QChar c : text) {
            const char16_t u = c.unicode();
            if (u < 0x100)
                out.append(char(u));
            else if (u == 0x03BC) // Greek mu -> micro sign
                out.append(char(0xB5));
            else
                out.append('?');
        }
        return out;
    }
    }
}

bool readFile(const QString &path, TextFile &out, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = f.errorString();
        return false;
    }
    const QByteArray bytes = f.readAll();
    out.text = decode(bytes, &out.encoding);
    out.crlf = out.text.contains(QLatin1String("\r\n")) || !out.text.contains('\n');
    return true;
}

bool writeFile(const QString &path, const TextFile &file, QString *error)
{
    QString text = file.text;
    text.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    if (file.crlf)
        text.replace('\n', QLatin1String("\r\n"));
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error)
            *error = f.errorString();
        return false;
    }
    f.write(encode(text, file.encoding));
    if (!f.commit()) {
        if (error)
            *error = f.errorString();
        return false;
    }
    return true;
}

QString readText(const QString &path, bool *ok)
{
    TextFile tf;
    const bool r = readFile(path, tf);
    if (ok)
        *ok = r;
    return tf.text;
}

QStringList splitLines(const QString &text)
{
    QString t = text;
    t.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    t.replace('\r', '\n');
    return t.split('\n');
}

QStringList tokenize(const QString &line, int keepRestFrom)
{
    QStringList out;
    const int n = line.size();
    int i = 0;
    while (i < n) {
        while (i < n && (line[i] == ' ' || line[i] == '\t'))
            ++i;
        if (i >= n)
            break;
        if (keepRestFrom >= 0 && out.size() == keepRestFrom) {
            out << line.mid(i);
            return out;
        }
        int j = i;
        while (j < n && line[j] != ' ' && line[j] != '\t')
            ++j;
        out << line.mid(i, j - i);
        i = j;
    }
    return out;
}

} // namespace textio
