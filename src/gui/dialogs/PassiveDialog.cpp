#include "PassiveDialog.h"

#include "core/ModelLibrary.h"
#include "core/Units.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QVBoxLayout>

QHash<QString, QString> parseParamList(const QString &text, QStringList *order, QString *rest)
{
    QHash<QString, QString> out;
    static const QRegularExpression re(QStringLiteral("([A-Za-z_][\\w]*)\\s*=\\s*(\"[^\"]*\"|\\{[^}]*\\}|\\S+)"));
    QString remaining = text;
    auto it = re.globalMatch(text);
    while (it.hasNext()) {
        const auto m = it.next();
        out.insert(m.captured(1).toLower(), m.captured(2));
        if (order)
            order->append(m.captured(1));
        remaining.replace(m.captured(0), QString());
    }
    if (rest)
        *rest = remaining.simplified();
    return out;
}

namespace {

struct ParamSpec {
    const char *key;
    const char *label;
};

QList<ParamSpec> specsFor(QChar kind)
{
    switch (kind.unicode()) {
    case 'R':
        return {{"tol", "Tolerance [%]"}, {"pwr", "Power rating [W]"}, {"tc1", "Temp. coefficient 1"}, {"tc2", "Temp. coefficient 2"}};
    case 'C':
        return {{"V", "Voltage rating [V]"}, {"Irms", "RMS current rating [A]"}, {"Rser", "Equiv. series resistance [Ω]"},
                {"Lser", "Equiv. series inductance [H]"}, {"Rpar", "Equiv. parallel resistance [Ω]"},
                {"Cpar", "Equiv. parallel capacitance [F]"}, {"RLshunt", "RLshunt [Ω]"}, {"m", "Number in parallel"},
                {"ic", "Initial voltage [V]"}};
    case 'L':
        return {{"Ipk", "Peak current [A]"}, {"Rser", "Series resistance [Ω] (default 1mΩ)"}, {"Rpar", "Parallel resistance [Ω]"},
                {"Cpar", "Parallel capacitance [F]"}, {"m", "Number in parallel"}, {"ic", "Initial current [A]"}};
    default:
        return {};
    }
}

} // namespace

PassiveDialog::PassiveDialog(const SymbolInstance &s, QWidget *parent)
    : QDialog(parent)
    , m_sym(s)
{
    const QString pfx = s.attr(QStringLiteral("Prefix")).toUpper();
    m_kind = pfx.isEmpty() ? QChar('R') : pfx[0];
    const QString what = m_kind == 'R' ? tr("Resistor") : (m_kind == 'C' ? tr("Capacitor") : tr("Inductor"));
    setWindowTitle(tr("%1 - %2").arg(what, s.instName()));
    auto *lay = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    m_name = new QLineEdit(s.instName());
    form->addRow(tr("Instance name:"), m_name);
    m_value = new QLineEdit(s.attr(QStringLiteral("Value")));
    const QString unitLabel = m_kind == 'R' ? tr("Resistance [Ω]:") : (m_kind == 'C' ? tr("Capacitance [F]:") : tr("Inductance [H]:"));
    form->addRow(unitLabel, m_value);
    lay->addLayout(form);

    auto *box = new QGroupBox(tr("Ratings and parasitics"));
    auto *pform = new QFormLayout(box);
    const QString spiceLine = s.attr(QStringLiteral("SpiceLine")) + ' ' + s.attr(QStringLiteral("Value2"));
    const QHash<QString, QString> existing = parseParamList(spiceLine, &m_order, &m_otherParams);
    for (const ParamSpec &ps : specsFor(m_kind)) {
        auto *e = new QLineEdit(existing.value(QString::fromLatin1(ps.key).toLower()));
        pform->addRow(QString::fromUtf8(ps.label) + ':', e);
        m_params.insert(QString::fromLatin1(ps.key), e);
    }
    lay->addWidget(box);
    auto *hint = new QLabel(tr("Values use SPICE suffixes: f p n u m k Meg G T (note: M = milli)."));
    hint->setWordWrap(true);
    hint->setEnabled(false);
    lay->addWidget(hint);

    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(bb, &QDialogButtonBox::accepted, this, [this]() {
        const QString v = m_value->text().trimmed();
        if (m_kind == 'R' && (v.endsWith('M') && !v.endsWith(QLatin1String("Meg"), Qt::CaseInsensitive))) {
            m_value->setStyleSheet(QStringLiteral("background: #fff3c4"));
            m_value->setToolTip(tr("'M' means milli in SPICE; use 'Meg' for mega."));
        }
        accept();
    });
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(bb);
    m_value->setFocus();
    m_value->selectAll();
}

SymbolInstance PassiveDialog::result() const
{
    SymbolInstance s = m_sym;
    s.attrs.set(QStringLiteral("InstName"), m_name->text().trimmed());
    s.attrs.set(QStringLiteral("Value"), m_value->text().trimmed());
    QStringList parts;
    for (auto it = m_params.constBegin(); it != m_params.constEnd(); ++it) {
        const QString v = it.value()->text().trimmed();
        if (!v.isEmpty())
            parts << QStringLiteral("%1=%2").arg(it.key(), v);
    }
    std::sort(parts.begin(), parts.end(), [this](const QString &a, const QString &b) {
        const auto ka = a.section('=', 0, 0), kb = b.section('=', 0, 0);
        int ia = 999, ib = 999;
        const QList<ParamSpec> specs = specsFor(m_kind);
        for (int i = 0; i < specs.size(); ++i) {
            if (ka == QLatin1String(specs[i].key)) ia = i;
            if (kb == QLatin1String(specs[i].key)) ib = i;
        }
        return ia < ib;
    });
    if (!m_otherParams.isEmpty())
        parts << m_otherParams;
    if (parts.isEmpty())
        s.attrs.remove(QStringLiteral("SpiceLine"));
    else
        s.attrs.set(QStringLiteral("SpiceLine"), parts.join(' '));
    // Parameters were merged from Value2 into SpiceLine.
    s.attrs.remove(QStringLiteral("Value2"));
    return s;
}

// ---------------------------------------------------------------- ModelSelectDialog

ModelSelectDialog::ModelSelectDialog(const SymbolInstance &s, QWidget *parent)
    : QDialog(parent)
    , m_sym(s)
{
    setWindowTitle(tr("Select model - %1").arg(s.instName()));
    resize(560, 460);
    auto *lay = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    m_name = new QLineEdit(s.instName());
    m_value = new QLineEdit(s.attr(QStringLiteral("Value")));
    form->addRow(tr("Instance name:"), m_name);
    form->addRow(tr("Model:"), m_value);
    m_search = new QLineEdit;
    m_search->setPlaceholderText(tr("Filter models..."));
    form->addRow(tr("Search:"), m_search);
    lay->addLayout(form);
    m_list = new QListWidget;
    lay->addWidget(m_list, 2);
    m_details = new QPlainTextEdit;
    m_details->setReadOnly(true);
    m_details->setMaximumHeight(110);
    lay->addWidget(m_details, 1);

    const QString prefix = s.attr(QStringLiteral("Prefix")).toUpper();
    QStringList types;
    if (prefix.startsWith('D')) types = {QStringLiteral("D")};
    else if (prefix == QLatin1String("QN")) types = {QStringLiteral("NPN")};
    else if (prefix == QLatin1String("QP")) types = {QStringLiteral("PNP")};
    else if (prefix.startsWith('Q')) types = {QStringLiteral("NPN"), QStringLiteral("PNP")};
    else if (prefix == QLatin1String("MN")) types = {QStringLiteral("NMOS"), QStringLiteral("VDMOS")};
    else if (prefix == QLatin1String("MP")) types = {QStringLiteral("PMOS"), QStringLiteral("VDMOS")};
    else if (prefix.startsWith('M')) types = {QStringLiteral("NMOS"), QStringLiteral("PMOS"), QStringLiteral("VDMOS")};
    else if (prefix == QLatin1String("JN")) types = {QStringLiteral("NJF")};
    else if (prefix == QLatin1String("JP")) types = {QStringLiteral("PJF")};
    else if (prefix.startsWith('J')) types = {QStringLiteral("NJF"), QStringLiteral("PJF")};
    const QList<ModelDef> models = ModelLibrary::instance().standardModelsOfType(types);
    for (const ModelDef &m : models) {
        // VDMOS polarity check: pchan models for P symbols.
        const bool pchan = m.lines.join(' ').contains(QLatin1String("pchan"), Qt::CaseInsensitive);
        if (m.type == QLatin1String("VDMOS") && prefix == QLatin1String("MN") && pchan)
            continue;
        if (m.type == QLatin1String("VDMOS") && prefix == QLatin1String("MP") && !pchan)
            continue;
        auto *item = new QListWidgetItem(QStringLiteral("%1   (%2)").arg(m.name, m.type), m_list);
        item->setData(Qt::UserRole, m.name);
        item->setData(Qt::UserRole + 1, m.lines.join('\n'));
        // Show manufacturer/ratings if present.
        static const QRegularExpression mfg(QStringLiteral("mfg=\"?([^\"\\s)]+)"), QRegularExpression::CaseInsensitiveOption);
        const auto mm = mfg.match(m.lines.join(' '));
        if (mm.hasMatch())
            item->setText(item->text() + QStringLiteral("  — ") + mm.captured(1));
    }
    if (models.isEmpty())
        m_details->setPlainText(tr("No standard component library found. Import your LTspice library under Tools > Settings > Search Paths, "
                                   "or type a model name defined with .model / .lib."));
    connect(m_search, &QLineEdit::textChanged, this, &ModelSelectDialog::filter);
    connect(m_list, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *cur) {
        if (!cur)
            return;
        m_value->setText(cur->data(Qt::UserRole).toString());
        m_details->setPlainText(cur->data(Qt::UserRole + 1).toString());
    });
    connect(m_list, &QListWidget::itemDoubleClicked, this, &QDialog::accept);
    for (int i = 0; i < m_list->count(); ++i) {
        if (m_list->item(i)->data(Qt::UserRole).toString().compare(m_value->text(), Qt::CaseInsensitive) == 0) {
            m_list->setCurrentRow(i);
            m_list->scrollToItem(m_list->item(i));
        }
    }
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(bb);
}

void ModelSelectDialog::filter(const QString &text)
{
    for (int i = 0; i < m_list->count(); ++i)
        m_list->item(i)->setHidden(!text.isEmpty() && !m_list->item(i)->text().contains(text, Qt::CaseInsensitive));
}

SymbolInstance ModelSelectDialog::result() const
{
    SymbolInstance s = m_sym;
    s.attrs.set(QStringLiteral("InstName"), m_name->text().trimmed());
    s.attrs.set(QStringLiteral("Value"), m_value->text().trimmed());
    return s;
}
