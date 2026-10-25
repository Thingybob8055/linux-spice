#include "ZipReader.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QtEndian>

#include <zlib.h>

namespace {

quint16 u16(const QByteArray &d, qsizetype off) { return qFromLittleEndian<quint16>(d.constData() + off); }
quint32 u32(const QByteArray &d, qsizetype off) { return qFromLittleEndian<quint32>(d.constData() + off); }

} // namespace

ZipReader::ZipReader(const QString &path)
    : m_path(path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        m_error = f.errorString();
        return;
    }
    m_data = f.readAll();
    m_valid = readDirectory();
}

bool ZipReader::readDirectory()
{
    // Locate the end of central directory record.
    const qsizetype minEocd = 22;
    if (m_data.size() < minEocd) {
        m_error = QStringLiteral("Not a ZIP file");
        return false;
    }
    qsizetype eocd = -1;
    for (qsizetype i = m_data.size() - minEocd; i >= std::max<qsizetype>(0, m_data.size() - 65557); --i) {
        if (u32(m_data, i) == 0x06054b50) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0) {
        m_error = QStringLiteral("ZIP central directory not found");
        return false;
    }
    const quint16 count = u16(m_data, eocd + 10);
    qsizetype off = u32(m_data, eocd + 16);
    for (int i = 0; i < count; ++i) {
        if (off + 46 > m_data.size() || u32(m_data, off) != 0x02014b50) {
            m_error = QStringLiteral("Corrupt ZIP directory");
            return false;
        }
        Entry e;
        const quint16 flags = u16(m_data, off + 8);
        e.method = u16(m_data, off + 10);
        e.compSize = u32(m_data, off + 20);
        e.size = u32(m_data, off + 24);
        const quint16 nameLen = u16(m_data, off + 28);
        const quint16 extraLen = u16(m_data, off + 30);
        const quint16 commentLen = u16(m_data, off + 32);
        e.localOffset = u32(m_data, off + 42);
        const QByteArray rawName = m_data.mid(off + 46, nameLen);
        e.name = (flags & 0x800) ? QString::fromUtf8(rawName) : QString::fromLatin1(rawName);
        e.name.replace('\\', '/');
        m_entries.append(e);
        off += 46 + nameLen + extraLen + commentLen;
    }
    return true;
}

QStringList ZipReader::entries() const
{
    QStringList out;
    for (const Entry &e : m_entries)
        out << e.name;
    return out;
}

bool ZipReader::readEntry(const Entry &e, QByteArray &out)
{
    const qsizetype lo = e.localOffset;
    if (lo + 30 > m_data.size() || u32(m_data, lo) != 0x04034b50)
        return false;
    const quint16 nameLen = u16(m_data, lo + 26);
    const quint16 extraLen = u16(m_data, lo + 28);
    const qsizetype dataOff = lo + 30 + nameLen + extraLen;
    if (dataOff + e.compSize > m_data.size())
        return false;
    if (e.method == 0) {
        out = m_data.mid(dataOff, e.compSize);
        return true;
    }
    if (e.method != 8)
        return false;
    out.resize(e.size);
    z_stream zs{};
    zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(m_data.constData() + dataOff));
    zs.avail_in = e.compSize;
    zs.next_out = reinterpret_cast<Bytef *>(out.data());
    zs.avail_out = e.size;
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK)
        return false;
    const int rc = inflate(&zs, Z_FINISH);
    inflateEnd(&zs);
    return rc == Z_STREAM_END || (rc == Z_OK && zs.avail_out == 0) || (rc == Z_BUF_ERROR && zs.avail_out == 0);
}

bool ZipReader::extractAll(const QString &dest, const QString &prefix, const std::function<bool(int, int)> &progress)
{
    if (!m_valid)
        return false;
    QDir root(dest);
    if (!root.mkpath(QStringLiteral(".")))
        return false;
    const QString rootAbs = root.absolutePath();
    int done = 0;
    const int total = m_entries.size();
    for (const Entry &e : m_entries) {
        ++done;
        if (progress && (done % 64 == 0 || done == total) && !progress(done, total))
            return false;
        QString name = e.name;
        if (!prefix.isEmpty()) {
            if (!name.startsWith(prefix, Qt::CaseInsensitive))
                continue;
            name = name.mid(prefix.size());
        }
        if (name.isEmpty() || name.startsWith('/') || name.contains(QLatin1String("../")) || name == QLatin1String(".."))
            continue;
        const QString target = QDir::cleanPath(rootAbs + '/' + name);
        if (!target.startsWith(rootAbs + '/'))
            continue;
        if (name.endsWith('/')) {
            QDir().mkpath(target);
            continue;
        }
        QDir().mkpath(QFileInfo(target).absolutePath());
        QByteArray content;
        if (!readEntry(e, content)) {
            m_error = QStringLiteral("Failed to extract %1").arg(e.name);
            continue;
        }
        QFile out(target);
        if (out.open(QIODevice::WriteOnly))
            out.write(content);
    }
    return true;
}
