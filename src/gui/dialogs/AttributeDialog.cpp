#include "AttributeDialog.h"

#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {
const QStringList &standardAttrs()
{
    static const QStringList a = {QStringLiteral("Prefix"),     QStringLiteral("InstName"),   QStringLiteral("SpiceModel"),
                                  QStringLiteral("Value"),      QStringLiteral("Value2"),     QStringLiteral("SpiceLine"),
                                  QStringLiteral("SpiceLine2"), QStringLiteral("ModelFile"),  QStringLiteral("Type"),
                                  QStringLiteral("Description")};
    return a;
}
} // namespace

AttributeDialog::AttributeDialog(const SymbolInstance &s, QWidget *parent)
    : QDialog(parent)
    , m_sym(s)
{
    setWindowTitle(tr("Component Attribute Editor - %1").arg(s.instName()));
    resize(640, 380);
    auto *lay = new QVBoxLayout(this);
    lay->addWidget(new QLabel(tr("Symbol: %1%2").arg(s.symbolName, s.def ? QStringLiteral("  —  ") + s.def->description() : QString())));
    m_table = new QTableWidget(0, 3);
    m_table->setHorizontalHeaderLabels({tr("Visible"), tr("Attribute"), tr("Value")});
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->verticalHeader()->hide();

    QStringList names = standardAttrs();
    auto addName = [&names](const QString &n) {
        for (const QString &x : names)
            if (x.compare(n, Qt::CaseInsensitive) == 0)
                return;
        names << n;
    };
    if (s.def)
        for (const auto &kv : s.def->attrs.items())
            addName(kv.first);
    for (const auto &kv : s.attrs.items())
        addName(kv.first);
    for (const QString &n : names) {
        const int r = m_table->rowCount();
        m_table->insertRow(r);
        auto *vis = new QTableWidgetItem;
        const int id = n.compare(QLatin1String("InstName"), Qt::CaseInsensitive) == 0 ? AttrId::InstName : attrIdForName(n);
        const AttrWindow *w = id >= 0 ? s.window(id) : nullptr;
        vis->setCheckState(w && w->fontSize != 0 && !w->just.invisible ? Qt::Checked : Qt::Unchecked);
        if (id < 0 || id == AttrId::Prefix || id == AttrId::Description)
            vis->setFlags(Qt::NoItemFlags);
        m_table->setItem(r, 0, vis);
        auto *name = new QTableWidgetItem(n);
        name->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        m_table->setItem(r, 1, name);
        m_table->setItem(r, 2, new QTableWidgetItem(s.attr(n)));
    }
    lay->addWidget(m_table);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    auto *addBtn = bb->addButton(tr("Add attribute"), QDialogButtonBox::ActionRole);
    connect(addBtn, &QPushButton::clicked, this, [this]() {
        const int r = m_table->rowCount();
        m_table->insertRow(r);
        auto *vis = new QTableWidgetItem;
        vis->setFlags(Qt::NoItemFlags);
        m_table->setItem(r, 0, vis);
        m_table->setItem(r, 1, new QTableWidgetItem(QStringLiteral("User%1").arg(r)));
        m_table->setItem(r, 2, new QTableWidgetItem);
        m_table->editItem(m_table->item(r, 1));
    });
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(bb);
}

SymbolInstance AttributeDialog::result() const
{
    SymbolInstance s = m_sym;
    for (int r = 0; r < m_table->rowCount(); ++r) {
        const QString name = m_table->item(r, 1)->text().trimmed();
        const QString value = m_table->item(r, 2) ? m_table->item(r, 2)->text() : QString();
        if (name.isEmpty())
            continue;
        const QString defVal = s.def ? s.def->attrs.value(name) : QString();
        if (value == defVal && !s.attrs.contains(name)) {
            // unchanged inherited value
        } else if (value.isEmpty() && defVal.isEmpty()) {
            s.attrs.remove(name);
        } else {
            s.attrs.set(name, value);
        }
        // Visibility
        const int id = name.compare(QLatin1String("InstName"), Qt::CaseInsensitive) == 0 ? AttrId::InstName : attrIdForName(name);
        if (id < 0 || !(m_table->item(r, 0)->flags() & Qt::ItemIsUserCheckable))
            continue;
        const bool want = m_table->item(r, 0)->checkState() == Qt::Checked;
        const AttrWindow *cur = s.window(id);
        const bool visible = cur && cur->fontSize != 0 && !cur->just.invisible;
        if (want == visible)
            continue;
        AttrWindow w;
        if (cur) {
            w = *cur;
        } else {
            w.id = id;
            // Place new windows below the existing text.
            int y = 0;
            if (s.def)
                y = s.def->bounds().bottom() + 8;
            w.pos = QPoint(s.def ? s.def->bounds().right() + 8 : 24, y);
            w.just = Justify::fromString(QStringLiteral("Left"));
        }
        w.fontSize = want ? 2 : 0;
        w.just.invisible = !want;
        if (!want)
            w.just = Justify::fromString(QStringLiteral("Left"));
        bool replaced = false;
        for (AttrWindow &existing : s.windows) {
            if (existing.id == id) {
                existing = w;
                replaced = true;
            }
        }
        if (!replaced)
            s.windows << w;
    }
    return s;
}
