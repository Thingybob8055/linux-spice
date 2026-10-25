#include "SourceDialog.h"

#include "core/NetlistTranslator.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QStackedWidget>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {

struct FuncSpec {
    const char *name;
    QStringList voltageLabels;
    QStringList currentLabels;
};

const QList<FuncSpec> &funcs()
{
    static const QList<FuncSpec> f = {
        {"PULSE",
         {"Vinitial [V]", "Von [V]", "Tdelay [s]", "Trise [s]", "Tfall [s]", "Ton [s]", "Tperiod [s]", "Ncycles"},
         {"Iinitial [A]", "Ion [A]", "Tdelay [s]", "Trise [s]", "Tfall [s]", "Ton [s]", "Tperiod [s]", "Ncycles"}},
        {"SINE",
         {"DC offset [V]", "Amplitude [V]", "Freq [Hz]", "Tdelay [s]", "Theta [1/s]", "Phi [deg]", "Ncycles"},
         {"DC offset [A]", "Amplitude [A]", "Freq [Hz]", "Tdelay [s]", "Theta [1/s]", "Phi [deg]", "Ncycles"}},
        {"EXP",
         {"Vinitial [V]", "Vpulsed [V]", "Rise Delay [s]", "Rise Tau [s]", "Fall Delay [s]", "Fall Tau [s]"},
         {"Iinitial [A]", "Ipulsed [A]", "Rise Delay [s]", "Rise Tau [s]", "Fall Delay [s]", "Fall Tau [s]"}},
        {"SFFM",
         {"DC offset [V]", "Amplitude [V]", "Carrier Freq [Hz]", "Modulation Index", "Signal Freq [Hz]"},
         {"DC offset [A]", "Amplitude [A]", "Carrier Freq [Hz]", "Modulation Index", "Signal Freq [Hz]"}},
    };
    return f;
}

} // namespace

SourceDialog::Parsed SourceDialog::parse(const QString &value)
{
    Parsed p;
    const QStringList t = NetlistTranslator::tokenize(value);
    QStringList extra;
    for (int i = 0; i < t.size(); ++i) {
        const QString tok = t[i];
        const QString low = tok.toLower();
        if (low == QLatin1String("dc") && i + 1 < t.size()) {
            p.dc = t[++i];
        } else if (low == QLatin1String("ac")) {
            if (i + 1 < t.size())
                p.acMag = t[++i];
            if (i + 1 < t.size() && !t[i + 1].contains('(') && !t[i + 1].contains('=') && (t[i + 1][0].isDigit() || t[i + 1][0] == '-' || t[i + 1][0] == '.' || t[i + 1][0] == '{'))
                p.acPhase = t[++i];
        } else if (low.startsWith(QLatin1String("rser="))) {
            p.rser = tok.mid(5);
        } else if (low.startsWith(QLatin1String("cpar="))) {
            p.cpar = tok.mid(5);
        } else if (low == QLatin1String("load")) {
            p.load = true;
        } else if (low.startsWith(QLatin1String("pwl")) && (i + 1 < t.size()) && t[i + 1].toLower().startsWith(QLatin1String("file="))) {
            p.function = QStringLiteral("PWLFILE");
            p.pwlFile = t[++i].mid(5);
            if (p.pwlFile.startsWith('"') && p.pwlFile.endsWith('"'))
                p.pwlFile = p.pwlFile.mid(1, p.pwlFile.size() - 2);
        } else if (tok.contains('(')) {
            const int open = tok.indexOf('(');
            QString fn = tok.left(open).toUpper();
            if (fn == QLatin1String("SIN"))
                fn = QStringLiteral("SINE");
            p.function = fn;
            p.args = NetlistTranslator::tokenize(tok.mid(open + 1, tok.size() - open - 2));
        } else if (p.dc.isEmpty() && p.function.isEmpty()) {
            p.dc = tok;
        } else {
            extra << tok;
        }
    }
    p.extra = extra.join(' ');
    return p;
}

SourceDialog::SourceDialog(const SymbolInstance &s, QWidget *parent)
    : QDialog(parent)
    , m_sym(s)
{
    m_isCurrent = s.attr(QStringLiteral("Prefix")).toUpper().startsWith('I');
    setWindowTitle(tr("Independent %1 Source - %2").arg(m_isCurrent ? tr("Current") : tr("Voltage"), s.instName()));
    const QString fullValue = (s.attr(QStringLiteral("Value")) + ' ' + s.attr(QStringLiteral("Value2")) + ' '
                               + s.attr(QStringLiteral("SpiceLine")))
                                  .simplified();
    const Parsed parsed = parse(fullValue);

    auto *lay = new QVBoxLayout(this);
    auto *top = new QFormLayout;
    m_name = new QLineEdit(s.instName());
    top->addRow(tr("Instance name:"), m_name);
    lay->addLayout(top);

    auto *body = new QHBoxLayout;
    // Function selection
    auto *funcBox = new QGroupBox(tr("Functions"));
    auto *fl = new QVBoxLayout(funcBox);
    m_funcGroup = new QButtonGroup(this);
    const QStringList names = {tr("(none)"), QStringLiteral("PULSE"), QStringLiteral("SINE"), QStringLiteral("EXP"), QStringLiteral("SFFM"),
                               QStringLiteral("PWL"), QStringLiteral("PWL FILE")};
    for (int i = 0; i < names.size(); ++i) {
        auto *rb = new QRadioButton(names[i]);
        m_funcGroup->addButton(rb, i);
        fl->addWidget(rb);
    }
    fl->addStretch();
    body->addWidget(funcBox);

    m_pages = new QStackedWidget;
    // Page 0: none
    m_pages->addWidget(new QLabel(tr("DC only (set the DC value below).")));
    m_funcArgs.append(QList<QLineEdit *>());
    for (const FuncSpec &f : funcs()) {
        auto *w = new QWidget;
        auto *form = new QFormLayout(w);
        QList<QLineEdit *> edits;
        const QStringList &labels = m_isCurrent ? f.currentLabels : f.voltageLabels;
        for (const QString &lab : labels) {
            auto *e = new QLineEdit;
            form->addRow(lab + ':', e);
            edits << e;
            connect(e, &QLineEdit::textChanged, this, &SourceDialog::updatePreview);
        }
        m_funcArgs << edits;
        m_pages->addWidget(w);
    }
    // PWL table
    {
        auto *w = new QWidget;
        auto *v = new QVBoxLayout(w);
        m_pwlTable = new QTableWidget(0, 2);
        m_pwlTable->setHorizontalHeaderLabels({tr("Time [s]"), m_isCurrent ? tr("Current [A]") : tr("Voltage [V]")});
        m_pwlTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        v->addWidget(m_pwlTable);
        auto *btns = new QHBoxLayout;
        auto *add = new QPushButton(tr("Add point"));
        auto *del = new QPushButton(tr("Remove point"));
        btns->addWidget(add);
        btns->addWidget(del);
        v->addLayout(btns);
        connect(add, &QPushButton::clicked, this, [this]() {
            m_pwlTable->insertRow(m_pwlTable->rowCount());
            updatePreview();
        });
        connect(del, &QPushButton::clicked, this, [this]() {
            if (m_pwlTable->currentRow() >= 0)
                m_pwlTable->removeRow(m_pwlTable->currentRow());
            updatePreview();
        });
        connect(m_pwlTable, &QTableWidget::cellChanged, this, &SourceDialog::updatePreview);
        m_funcArgs.append(QList<QLineEdit *>());
        m_pages->addWidget(w);
    }
    // PWL FILE
    {
        auto *w = new QWidget;
        auto *h = new QHBoxLayout(w);
        m_pwlFile = new QLineEdit(parsed.pwlFile);
        auto *browse = new QPushButton(tr("Browse..."));
        h->addWidget(new QLabel(tr("File:")));
        h->addWidget(m_pwlFile, 1);
        h->addWidget(browse);
        connect(browse, &QPushButton::clicked, this, [this]() {
            const QString f = QFileDialog::getOpenFileName(this, tr("PWL data file"), QString(), tr("Text files (*.txt *.csv *.pwl);;All files (*)"));
            if (!f.isEmpty())
                m_pwlFile->setText(f);
        });
        connect(m_pwlFile, &QLineEdit::textChanged, this, &SourceDialog::updatePreview);
        m_funcArgs.append(QList<QLineEdit *>());
        m_pages->addWidget(w);
    }
    body->addWidget(m_pages, 1);
    lay->addLayout(body);

    auto *bottom = new QHBoxLayout;
    auto *dcBox = new QGroupBox(tr("DC Value"));
    auto *dcForm = new QFormLayout(dcBox);
    m_dc = new QLineEdit(parsed.dc);
    dcForm->addRow(tr("DC value:"), m_dc);
    bottom->addWidget(dcBox);
    auto *acBox = new QGroupBox(tr("Small signal AC analysis (.AC)"));
    auto *acForm = new QFormLayout(acBox);
    m_acMag = new QLineEdit(parsed.acMag);
    m_acPhase = new QLineEdit(parsed.acPhase);
    acForm->addRow(tr("AC Amplitude:"), m_acMag);
    acForm->addRow(tr("AC Phase:"), m_acPhase);
    bottom->addWidget(acBox);
    auto *parBox = new QGroupBox(m_isCurrent ? tr("Options") : tr("Parasitic properties"));
    auto *parForm = new QFormLayout(parBox);
    if (!m_isCurrent) {
        m_rser = new QLineEdit(parsed.rser);
        m_cpar = new QLineEdit(parsed.cpar);
        parForm->addRow(tr("Series resistance [Ω]:"), m_rser);
        parForm->addRow(tr("Parallel capacitance [F]:"), m_cpar);
    } else {
        m_load = new QCheckBox(tr("This is an active load"));
        m_load->setChecked(parsed.load);
        parForm->addRow(m_load);
    }
    bottom->addWidget(parBox);
    lay->addLayout(bottom);

    auto *extraForm = new QFormLayout;
    m_extra = new QLineEdit(parsed.extra);
    extraForm->addRow(tr("Additional parameters:"), m_extra);
    lay->addLayout(extraForm);
    m_preview = new QLabel;
    m_preview->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_preview->setWordWrap(true);
    lay->addWidget(m_preview);

    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(bb);

    // Populate from parsed value.
    int page = 0;
    if (parsed.function == QLatin1String("PULSE")) page = 1;
    else if (parsed.function == QLatin1String("SINE")) page = 2;
    else if (parsed.function == QLatin1String("EXP")) page = 3;
    else if (parsed.function == QLatin1String("SFFM")) page = 4;
    else if (parsed.function == QLatin1String("PWL")) page = 5;
    else if (parsed.function == QLatin1String("PWLFILE")) page = 6;
    if (page >= 1 && page <= 4) {
        const QList<QLineEdit *> &edits = m_funcArgs[page];
        for (int i = 0; i < edits.size() && i < parsed.args.size(); ++i)
            edits[i]->setText(parsed.args[i]);
    } else if (page == 5) {
        for (int i = 0; i + 1 < parsed.args.size(); i += 2) {
            const int r = m_pwlTable->rowCount();
            m_pwlTable->insertRow(r);
            m_pwlTable->setItem(r, 0, new QTableWidgetItem(parsed.args[i]));
            m_pwlTable->setItem(r, 1, new QTableWidgetItem(parsed.args[i + 1]));
        }
    }
    m_funcGroup->button(page)->setChecked(true);
    m_pages->setCurrentIndex(page);
    connect(m_funcGroup, &QButtonGroup::idClicked, this, [this](int id) {
        m_pages->setCurrentIndex(id);
        updatePreview();
    });
    for (QLineEdit *e : {m_dc, m_acMag, m_acPhase, m_extra})
        connect(e, &QLineEdit::textChanged, this, &SourceDialog::updatePreview);
    if (m_rser)
        connect(m_rser, &QLineEdit::textChanged, this, &SourceDialog::updatePreview);
    if (m_cpar)
        connect(m_cpar, &QLineEdit::textChanged, this, &SourceDialog::updatePreview);
    updatePreview();
}

QString SourceDialog::composeValue() const
{
    QStringList parts;
    const int page = m_funcGroup->checkedId();
    const QString dc = m_dc->text().trimmed();
    if (page == 0) {
        if (!dc.isEmpty())
            parts << dc;
    } else {
        if (!dc.isEmpty())
            parts << QStringLiteral("DC %1").arg(dc);
        if (page >= 1 && page <= 4) {
            QStringList args;
            for (QLineEdit *e : m_funcArgs[page])
                args << e->text().trimmed();
            while (!args.isEmpty() && args.last().isEmpty())
                args.removeLast();
            for (QString &a : args)
                if (a.isEmpty())
                    a = QStringLiteral("0");
            parts << QStringLiteral("%1(%2)").arg(QLatin1String(funcs()[page - 1].name), args.join(' '));
        } else if (page == 5) {
            QStringList pts;
            for (int r = 0; r < m_pwlTable->rowCount(); ++r) {
                const QString t = m_pwlTable->item(r, 0) ? m_pwlTable->item(r, 0)->text().trimmed() : QString();
                const QString v = m_pwlTable->item(r, 1) ? m_pwlTable->item(r, 1)->text().trimmed() : QString();
                if (!t.isEmpty() && !v.isEmpty())
                    pts << t << v;
            }
            parts << QStringLiteral("PWL(%1)").arg(pts.join(' '));
        } else if (page == 6) {
            QString f = m_pwlFile->text().trimmed();
            if (f.contains(' '))
                f = '"' + f + '"';
            parts << QStringLiteral("PWL file=%1").arg(f);
        }
    }
    if (!m_acMag->text().trimmed().isEmpty()) {
        parts << QStringLiteral("AC %1").arg(m_acMag->text().trimmed());
        if (!m_acPhase->text().trimmed().isEmpty())
            parts << m_acPhase->text().trimmed();
    }
    if (m_load && m_load->isChecked())
        parts << QStringLiteral("load");
    if (!m_extra->text().trimmed().isEmpty())
        parts << m_extra->text().trimmed();
    if (parts.isEmpty())
        parts << QStringLiteral("0");
    return parts.join(' ');
}

QString parasiticsText(QLineEdit *rser, QLineEdit *cpar)
{
    QStringList p;
    if (rser && !rser->text().trimmed().isEmpty())
        p << QStringLiteral("Rser=%1").arg(rser->text().trimmed());
    if (cpar && !cpar->text().trimmed().isEmpty())
        p << QStringLiteral("Cpar=%1").arg(cpar->text().trimmed());
    return p.join(' ');
}

void SourceDialog::updatePreview()
{
    QString line = composeValue();
    const QString par = parasiticsText(m_rser, m_cpar);
    if (!par.isEmpty())
        line += ' ' + par;
    m_preview->setText(tr("Netlist: %1 n+ n- %2").arg(m_name->text(), line));
}

SymbolInstance SourceDialog::result() const
{
    SymbolInstance s = m_sym;
    s.attrs.set(QStringLiteral("InstName"), m_name->text().trimmed());
    s.attrs.set(QStringLiteral("Value"), composeValue());
    s.attrs.remove(QStringLiteral("Value2"));
    const QString par = parasiticsText(m_rser, m_cpar);
    if (par.isEmpty())
        s.attrs.remove(QStringLiteral("SpiceLine"));
    else
        s.attrs.set(QStringLiteral("SpiceLine"), par);
    return s;
}
