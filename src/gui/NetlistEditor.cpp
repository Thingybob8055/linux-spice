#include "NetlistEditor.h"

#include "core/AppSettings.h"

#include <QFileInfo>
#include <QFontDatabase>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPrinter>
#include <QRegularExpression>
#include <QSyntaxHighlighter>
#include <QTextBlock>
#include <QVBoxLayout>

QString DocumentWidget::displayName() const
{
    const QString p = filePath();
    return p.isEmpty() ? tr("Untitled") : QFileInfo(p).fileName();
}

namespace {

class SpiceHighlighter : public QSyntaxHighlighter
{
public:
    explicit SpiceHighlighter(QTextDocument *doc)
        : QSyntaxHighlighter(doc)
    {
        const AppSettings &s = AppSettings::instance();
        m_comment.setForeground(s.color(QStringLiteral("net.comment")));
        m_directive.setForeground(s.color(QStringLiteral("net.directive")));
        m_directive.setFontWeight(QFont::Bold);
        m_element.setForeground(QColor(128, 0, 128));
        m_number.setForeground(QColor(0, 120, 120));
        m_func.setForeground(QColor(160, 80, 0));
    }

protected:
    void highlightBlock(const QString &text) override
    {
        const QString t = text.trimmed();
        if (t.startsWith('*')) {
            setFormat(0, text.size(), m_comment);
            return;
        }
        static const QRegularExpression num(QStringLiteral("(?<![\\w.])[-+]?(\\d+\\.?\\d*|\\.\\d+)([eE][-+]?\\d+)?(meg|[tgkmunpfa])?\\w*"),
                                            QRegularExpression::CaseInsensitiveOption);
        auto it = num.globalMatch(text);
        while (it.hasNext()) {
            const auto m = it.next();
            setFormat(int(m.capturedStart()), int(m.capturedLength()), m_number);
        }
        static const QRegularExpression fn(QStringLiteral("\\b(PULSE|SINE?|EXP|SFFM|PWL|AC|DC|V|I|params:|table|if|limit)\\b"),
                                           QRegularExpression::CaseInsensitiveOption);
        it = fn.globalMatch(text);
        while (it.hasNext()) {
            const auto m = it.next();
            setFormat(int(m.capturedStart()), int(m.capturedLength()), m_func);
        }
        const int lead = int(text.size() - text.trimmed().size() > 0 ? text.indexOf(t) : 0);
        if (t.startsWith('.')) {
            const int end = t.indexOf(QRegularExpression(QStringLiteral("\\s")));
            setFormat(lead, end < 0 ? int(t.size()) : end, m_directive);
        } else if (!t.isEmpty() && t[0].isLetter()) {
            const int end = t.indexOf(QRegularExpression(QStringLiteral("\\s")));
            setFormat(lead, end < 0 ? int(t.size()) : end, m_element);
        }
        const int sc = text.indexOf(';');
        if (sc >= 0)
            setFormat(sc, text.size() - sc, m_comment);
    }

private:
    QTextCharFormat m_comment, m_directive, m_element, m_number, m_func;
};

} // namespace

NetlistEditor::NetlistEditor(QWidget *parent)
    : DocumentWidget(parent)
{
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    m_edit = new QPlainTextEdit;
    m_edit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_edit->setLineWrapMode(QPlainTextEdit::NoWrap);
    QPalette pal = m_edit->palette();
    pal.setColor(QPalette::Base, AppSettings::instance().color(QStringLiteral("net.background")));
    pal.setColor(QPalette::Text, AppSettings::instance().color(QStringLiteral("net.text")));
    m_edit->setPalette(pal);
    new SpiceHighlighter(m_edit->document());
    lay->addWidget(m_edit);
    m_edit->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_edit, &QPlainTextEdit::customContextMenuRequested, this, &NetlistEditor::contextMenu);
    connect(m_edit->document(), &QTextDocument::modificationChanged, this, &DocumentWidget::modifiedChanged);
    setFocusProxy(m_edit);
}

bool NetlistEditor::load(const QString &path, QString *error)
{
    if (!textio::readFile(path, m_file, error))
        return false;
    m_path = QFileInfo(path).absoluteFilePath();
    m_edit->setPlainText(QString(m_file.text).replace(QLatin1String("\r\n"), QLatin1String("\n")));
    m_edit->document()->setModified(false);
    emit titleChanged();
    return true;
}

void NetlistEditor::setText(const QString &text, const QString &title)
{
    m_edit->setPlainText(text);
    m_title = title;
    m_edit->document()->setModified(false);
    emit titleChanged();
}

QString NetlistEditor::text() const
{
    return m_edit->toPlainText();
}

QStringList NetlistEditor::lines() const
{
    return text().split('\n');
}

void NetlistEditor::setReadOnly(bool ro)
{
    m_edit->setReadOnly(ro);
}

bool NetlistEditor::isModified() const
{
    return !m_edit->isReadOnly() && m_edit->document()->isModified();
}

bool NetlistEditor::save()
{
    if (m_path.isEmpty())
        return false;
    return saveAs(m_path);
}

bool NetlistEditor::saveAs(const QString &path)
{
    m_file.text = m_edit->toPlainText();
    QString err;
    if (!textio::writeFile(path, m_file, &err)) {
        QMessageBox::warning(this, tr("Save failed"), err);
        return false;
    }
    m_path = QFileInfo(path).absoluteFilePath();
    m_edit->document()->setModified(false);
    emit titleChanged();
    return true;
}

void NetlistEditor::undo() { m_edit->undo(); }
void NetlistEditor::redo() { m_edit->redo(); }
void NetlistEditor::zoomIn() { m_edit->zoomIn(1); }
void NetlistEditor::zoomOut() { m_edit->zoomOut(1); }
void NetlistEditor::print(QPrinter *printer) { m_edit->print(printer); }

void NetlistEditor::contextMenu(const QPoint &pos)
{
    QMenu *menu = m_edit->createStandardContextMenu();
    const QTextBlock block = m_edit->cursorForPosition(pos).block();
    const QString line = block.text().trimmed();
    if (line.startsWith(QLatin1String(".subckt"), Qt::CaseInsensitive)) {
        // Collect continuation lines too.
        QString full = line;
        QTextBlock b = block.next();
        while (b.isValid() && b.text().trimmed().startsWith('+')) {
            full += ' ' + b.text().trimmed().mid(1);
            b = b.next();
        }
        QStringList t = full.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        menu->addSeparator();
        QAction *a = menu->addAction(tr("Create Symbol for %1").arg(t.value(1)));
        connect(a, &QAction::triggered, this, [this, t]() {
            QStringList ports;
            for (int i = 2; i < t.size(); ++i) {
                if (t[i].contains('=') || t[i].compare(QLatin1String("params:"), Qt::CaseInsensitive) == 0)
                    break;
                ports << t[i];
            }
            emit createSymbolRequested(t.value(1), ports, m_path);
        });
    }
    menu->exec(m_edit->mapToGlobal(pos));
    delete menu;
}

// ---------------------------------------------------------------- LogWindow

LogWindow::LogWindow(QWidget *parent)
    : DocumentWidget(parent)
{
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    m_edit = new QPlainTextEdit;
    m_edit->setReadOnly(true);
    m_edit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_edit->setLineWrapMode(QPlainTextEdit::NoWrap);
    lay->addWidget(m_edit);
    m_title = tr("SPICE Output Log");
}

void LogWindow::setTitle(const QString &t)
{
    m_title = t;
    emit titleChanged();
}

void LogWindow::setLog(const QString &text)
{
    m_edit->setPlainText(text);
    m_edit->moveCursor(QTextCursor::End);
}

void LogWindow::appendLog(const QString &text)
{
    m_edit->moveCursor(QTextCursor::End);
    m_edit->insertPlainText(text);
    m_edit->moveCursor(QTextCursor::End);
}

QString LogWindow::text() const
{
    return m_edit->toPlainText();
}

void LogWindow::print(QPrinter *printer)
{
    m_edit->print(printer);
}
