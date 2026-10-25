#include "ComponentBrowser.h"

#include "core/AppSettings.h"
#include "core/SymbolLibrary.h"
#include "gui/SchematicPainter.h"

#include <QDialogButtonBox>
#include <QDirIterator>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QTreeWidget>
#include <QVBoxLayout>

void SymbolPreview::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    const SchematicStyle st = SchematicStyle::fromSettings();
    p.fillRect(rect(), st.background);
    p.setPen(palette().color(QPalette::Mid));
    p.drawRect(rect().adjusted(0, 0, -1, -1));
    if (!m_sym)
        return;
    QRectF b = m_sym->bounds();
    for (const AttrWindow &w : m_sym->windows) {
        // Include visible attribute text placement in the frame.
        b = b.united(QRectF(w.pos, QSizeF(1, 1)));
    }
    b.adjust(-16, -16, 16, 16);
    const double s = std::min(width() / b.width(), height() / b.height());
    p.setRenderHint(QPainter::Antialiasing);
    p.translate(width() / 2.0, height() / 2.0);
    p.scale(s, s);
    p.translate(-b.center());
    p.setPen(QPen(st.component, 1.5 / s));
    SchematicPainter::drawSymbolBody(p, *m_sym, QPoint(0, 0), Orient::R0, st.component, st.pin, true, st.fontFamily);
    // Pin markers
    p.setPen(QPen(st.pin, 1.0 / s));
    for (const SymPin &pin : m_sym->pins)
        p.drawRect(QRectF(pin.pos.x() - 2, pin.pos.y() - 2, 4, 4));
}

ComponentBrowser::ComponentBrowser(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Select Component Symbol"));
    resize(860, 560);
    auto *lay = new QVBoxLayout(this);
    m_search = new QLineEdit;
    m_search->setPlaceholderText(tr("Search symbols (type part of a name or description)"));
    m_search->setClearButtonEnabled(true);
    lay->addWidget(m_search);

    auto *split = new QSplitter;
    m_tree = new QTreeWidget;
    m_tree->setHeaderHidden(true);
    split->addWidget(m_tree);
    m_list = new QListWidget;
    split->addWidget(m_list);
    auto *right = new QWidget;
    auto *rl = new QVBoxLayout(right);
    m_preview = new SymbolPreview;
    rl->addWidget(m_preview, 1);
    m_desc = new QLabel;
    m_desc->setWordWrap(true);
    m_desc->setTextInteractionFlags(Qt::TextSelectableByMouse);
    rl->addWidget(m_desc);
    m_path = new QLabel;
    m_path->setWordWrap(true);
    m_path->setEnabled(false);
    rl->addWidget(m_path);
    m_exampleBtn = new QPushButton(tr("Open this macromodel's test fixture"));
    m_exampleBtn->setEnabled(false);
    rl->addWidget(m_exampleBtn);
    split->addWidget(right);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 2);
    split->setStretchFactor(2, 2);
    lay->addWidget(split, 1);

    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    lay->addWidget(bb);
    connect(bb, &QDialogButtonBox::accepted, this, [this]() {
        if (!m_selected.isEmpty())
            accept();
    });
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_search, &QLineEdit::textChanged, this, &ComponentBrowser::applyFilter);
    connect(m_tree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *cur) {
        m_category = cur ? cur->data(0, Qt::UserRole).toString() : QString();
        applyFilter();
    });
    connect(m_list, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *cur) { select(cur); });
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *it) {
        select(it);
        accept();
    });
    connect(m_exampleBtn, &QPushButton::clicked, this, [this]() {
        m_example = findExample(QFileInfo(m_selected).fileName());
        if (!m_example.isEmpty()) {
            m_selected.clear();
            accept();
        }
    });
    populate();
    const QString last = AppSettings::instance().raw().value(QStringLiteral("browser/lastCategory")).toString();
    for (QTreeWidgetItemIterator it(m_tree); *it; ++it) {
        if ((*it)->data(0, Qt::UserRole).toString() == last) {
            m_tree->setCurrentItem(*it);
            break;
        }
    }
    if (!m_tree->currentItem() && m_tree->topLevelItemCount())
        m_tree->setCurrentItem(m_tree->topLevelItem(0));
    m_search->setFocus();
}

void ComponentBrowser::populate()
{
    m_tree->clear();
    auto *top = new QTreeWidgetItem(m_tree, {tr("[Top level]")});
    top->setData(0, Qt::UserRole, QString());
    QHash<QString, QTreeWidgetItem *> nodes;
    const auto entries = SymbolLibrary::instance().entries();
    QStringList cats;
    for (const auto &e : entries)
        if (!e.category.isEmpty() && !cats.contains(e.category, Qt::CaseInsensitive))
            cats << e.category;
    std::sort(cats.begin(), cats.end(), [](const QString &a, const QString &b) { return a.compare(b, Qt::CaseInsensitive) < 0; });
    for (const QString &c : cats) {
        const QStringList parts = c.split('/');
        QString path;
        QTreeWidgetItem *parent = nullptr;
        for (const QString &part : parts) {
            path = path.isEmpty() ? part : path + '/' + part;
            const QString key = path.toLower();
            if (!nodes.contains(key)) {
                auto *item = parent ? new QTreeWidgetItem(parent, {part}) : new QTreeWidgetItem(m_tree, {part});
                item->setData(0, Qt::UserRole, path);
                nodes.insert(key, item);
            }
            parent = nodes.value(key);
        }
    }
}

void ComponentBrowser::applyFilter()
{
    m_list->clear();
    const QString q = m_search->text().trimmed();
    const auto entries = SymbolLibrary::instance().entries();
    QList<QPair<QString, const SymbolLibrary::Entry *>> rows;
    for (const auto &e : entries) {
        const QString base = e.name.section('\\', -1);
        if (q.isEmpty()) {
            if (e.category.compare(m_category, Qt::CaseInsensitive) != 0)
                continue;
        } else if (!base.contains(q, Qt::CaseInsensitive)) {
            continue;
        }
        rows.append({q.isEmpty() ? base : e.name, &e});
        if (rows.size() > 5000)
            break;
    }
    std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) { return a.first.compare(b.first, Qt::CaseInsensitive) < 0; });
    for (const auto &r : rows) {
        auto *item = new QListWidgetItem(r.first, m_list);
        item->setData(Qt::UserRole, r.second->name);
        item->setToolTip(r.second->path);
    }
    if (m_list->count())
        m_list->setCurrentRow(0);
}

void ComponentBrowser::select(QListWidgetItem *item)
{
    if (!item) {
        m_selected.clear();
        m_preview->setSymbol({});
        return;
    }
    m_selected = item->data(Qt::UserRole).toString();
    SymbolPtr sym = SymbolLibrary::instance().find(m_selected);
    m_preview->setSymbol(sym);
    m_desc->setText(sym ? sym->description() : QString());
    m_path->setText(item->toolTip());
    m_exampleBtn->setEnabled(!findExample(QFileInfo(m_selected).fileName()).isEmpty());
    if (!m_category.isEmpty() || m_search->text().isEmpty())
        AppSettings::instance().raw().setValue(QStringLiteral("browser/lastCategory"), m_category);
}

QString ComponentBrowser::findExample(const QString &nameIn) const
{
    const QString name = QString(nameIn).section('\\', -1);
    if (name.isEmpty())
        return QString();
    static QHash<QString, QString> index;
    static bool built = false;
    if (!built) {
        built = true;
        QDirIterator it(AppSettings::instance().examplesRoot(), {QStringLiteral("*.asc")}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString p = it.next();
            index.insert(QFileInfo(p).completeBaseName().toLower(), p);
        }
    }
    return index.value(name.toLower());
}
