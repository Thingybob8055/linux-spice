#include "AnalysisDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QRegularExpression>
#include <QTabWidget>
#include <QVBoxLayout>

AnalysisDialog::AnalysisDialog(const QString &existing, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Edit Simulation Command"));
    resize(520, 400);
    auto *lay = new QVBoxLayout(this);
    m_tabs = new QTabWidget;
    lay->addWidget(m_tabs, 1);

    // Transient
    {
        auto *w = new QWidget;
        auto *f = new QFormLayout(w);
        f->addRow(new QLabel(tr("Perform a non-linear, time-domain simulation.")));
        m_tStop = new QLineEdit;
        m_tStart = new QLineEdit;
        m_tMax = new QLineEdit;
        m_tStep = new QLineEdit;
        f->addRow(tr("Stop time:"), m_tStop);
        f->addRow(tr("Time to start saving data:"), m_tStart);
        f->addRow(tr("Maximum timestep:"), m_tMax);
        f->addRow(tr("Output step (optional):"), m_tStep);
        m_tStartup = new QCheckBox(tr("Start external DC supply voltages at 0V"));
        m_tSteady = new QCheckBox(tr("Stop simulating if steady state is detected"));
        m_tNoDiscard = new QCheckBox(tr("Don't reset T=0 when steady state is detected"));
        m_tUic = new QCheckBox(tr("Skip initial operating point solution"));
        f->addRow(m_tStartup);
        f->addRow(m_tSteady);
        f->addRow(m_tNoDiscard);
        f->addRow(m_tUic);
        m_tabs->addTab(w, tr("Transient"));
        for (QLineEdit *e : {m_tStop, m_tStart, m_tMax, m_tStep})
            connect(e, &QLineEdit::textChanged, this, &AnalysisDialog::updateSyntax);
        for (QCheckBox *c : {m_tStartup, m_tSteady, m_tNoDiscard, m_tUic})
            connect(c, &QCheckBox::toggled, this, &AnalysisDialog::updateSyntax);
    }
    // AC
    {
        auto *w = new QWidget;
        auto *f = new QFormLayout(w);
        f->addRow(new QLabel(tr("Compute the small signal AC behavior of the circuit linearized about its DC operating point.")));
        m_acType = new QComboBox;
        m_acType->addItems({tr("Octave"), tr("Decade"), tr("Linear")});
        m_acType->setCurrentIndex(1);
        m_acPoints = new QLineEdit(QStringLiteral("100"));
        m_acStart = new QLineEdit;
        m_acStop = new QLineEdit;
        f->addRow(tr("Type of sweep:"), m_acType);
        f->addRow(tr("Number of points per decade/octave/total:"), m_acPoints);
        f->addRow(tr("Start frequency:"), m_acStart);
        f->addRow(tr("Stop frequency:"), m_acStop);
        m_tabs->addTab(w, tr("AC Analysis"));
        connect(m_acType, &QComboBox::currentIndexChanged, this, &AnalysisDialog::updateSyntax);
        for (QLineEdit *e : {m_acPoints, m_acStart, m_acStop})
            connect(e, &QLineEdit::textChanged, this, &AnalysisDialog::updateSyntax);
    }
    // DC sweep
    {
        auto *w = new QWidget;
        auto *v = new QVBoxLayout(w);
        v->addWidget(new QLabel(tr("Compute the DC operating point of a circuit while stepping independent sources.")));
        auto *sub = new QTabWidget;
        for (int i = 0; i < 3; ++i) {
            auto *pw = new QWidget;
            auto *f = new QFormLayout(pw);
            m_dc[i].source = new QLineEdit;
            m_dc[i].type = new QComboBox;
            m_dc[i].type->addItems({tr("Linear"), tr("Octave"), tr("Decade")});
            m_dc[i].start = new QLineEdit;
            m_dc[i].stop = new QLineEdit;
            m_dc[i].incr = new QLineEdit;
            f->addRow(tr("Name of source to sweep:"), m_dc[i].source);
            f->addRow(tr("Type of sweep:"), m_dc[i].type);
            f->addRow(tr("Start value:"), m_dc[i].start);
            f->addRow(tr("Stop value:"), m_dc[i].stop);
            f->addRow(tr("Increment / points:"), m_dc[i].incr);
            sub->addTab(pw, i == 0 ? tr("1st Source") : (i == 1 ? tr("2nd Source") : tr("3rd Source")));
            for (QLineEdit *e : {m_dc[i].source, m_dc[i].start, m_dc[i].stop, m_dc[i].incr})
                connect(e, &QLineEdit::textChanged, this, &AnalysisDialog::updateSyntax);
            connect(m_dc[i].type, &QComboBox::currentIndexChanged, this, &AnalysisDialog::updateSyntax);
        }
        v->addWidget(sub);
        m_tabs->addTab(w, tr("DC sweep"));
    }
    // Noise
    {
        auto *w = new QWidget;
        auto *f = new QFormLayout(w);
        f->addRow(new QLabel(tr("Find the noise at an output node versus frequency.")));
        m_nOut = new QLineEdit;
        m_nRef = new QLineEdit;
        m_nSrc = new QLineEdit;
        m_nType = new QComboBox;
        m_nType->addItems({tr("Octave"), tr("Decade"), tr("Linear")});
        m_nType->setCurrentIndex(1);
        m_nPoints = new QLineEdit(QStringLiteral("100"));
        m_nStart = new QLineEdit;
        m_nStop = new QLineEdit;
        f->addRow(tr("Output:"), m_nOut);
        f->addRow(tr("Reference:"), m_nRef);
        f->addRow(tr("Input source:"), m_nSrc);
        f->addRow(tr("Type of sweep:"), m_nType);
        f->addRow(tr("Number of points:"), m_nPoints);
        f->addRow(tr("Start frequency:"), m_nStart);
        f->addRow(tr("Stop frequency:"), m_nStop);
        m_tabs->addTab(w, tr("Noise"));
        for (QLineEdit *e : {m_nOut, m_nRef, m_nSrc, m_nPoints, m_nStart, m_nStop})
            connect(e, &QLineEdit::textChanged, this, &AnalysisDialog::updateSyntax);
        connect(m_nType, &QComboBox::currentIndexChanged, this, &AnalysisDialog::updateSyntax);
    }
    // DC op pnt
    {
        auto *w = new QWidget;
        auto *v = new QVBoxLayout(w);
        v->addWidget(new QLabel(tr("Find the DC operating point solution of the circuit, treating capacitors as open circuits and inductors as short circuits.")));
        v->addStretch();
        m_tabs->addTab(w, tr("DC op pnt"));
    }
    // DC transfer
    {
        auto *w = new QWidget;
        auto *f = new QFormLayout(w);
        f->addRow(new QLabel(tr("Find the DC small-signal transfer function.")));
        m_tfOut = new QLineEdit;
        m_tfSrc = new QLineEdit;
        f->addRow(tr("Output:"), m_tfOut);
        f->addRow(tr("Source:"), m_tfSrc);
        m_tabs->addTab(w, tr("DC Transfer"));
        for (QLineEdit *e : {m_tfOut, m_tfSrc})
            connect(e, &QLineEdit::textChanged, this, &AnalysisDialog::updateSyntax);
    }
    connect(m_tabs, &QTabWidget::currentChanged, this, &AnalysisDialog::updateSyntax);

    auto *syntaxForm = new QFormLayout;
    m_syntax = new QLineEdit;
    syntaxForm->addRow(tr("Syntax:"), m_syntax);
    lay->addLayout(syntaxForm);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(bb);
    load(existing);
    updateSyntax();
}

void AnalysisDialog::load(const QString &cmdIn)
{
    m_updating = true;
    const QString cmd = cmdIn.trimmed();
    const QStringList t = cmd.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    const QString kw = t.value(0).toLower();
    if (kw == QLatin1String(".tran")) {
        m_tabs->setCurrentIndex(0);
        QStringList nums;
        for (int i = 1; i < t.size(); ++i) {
            const QString l = t[i].toLower();
            if (l == QLatin1String("startup")) m_tStartup->setChecked(true);
            else if (l == QLatin1String("uic")) m_tUic->setChecked(true);
            else if (l == QLatin1String("steady")) m_tSteady->setChecked(true);
            else if (l == QLatin1String("nodiscard")) m_tNoDiscard->setChecked(true);
            else nums << t[i];
        }
        if (nums.size() == 1) {
            m_tStop->setText(nums[0]);
        } else if (nums.size() >= 2) {
            if (nums[0] != QLatin1String("0"))
                m_tStep->setText(nums[0]);
            m_tStop->setText(nums[1]);
            if (nums.size() > 2 && nums[2] != QLatin1String("0"))
                m_tStart->setText(nums[2]);
            if (nums.size() > 3)
                m_tMax->setText(nums[3]);
        }
    } else if (kw == QLatin1String(".ac")) {
        m_tabs->setCurrentIndex(1);
        const QString type = t.value(1).toLower();
        m_acType->setCurrentIndex(type == QLatin1String("oct") ? 0 : (type == QLatin1String("lin") ? 2 : 1));
        m_acPoints->setText(t.value(2));
        m_acStart->setText(t.value(3));
        m_acStop->setText(t.value(4));
    } else if (kw == QLatin1String(".dc")) {
        m_tabs->setCurrentIndex(2);
        int i = 1, src = 0;
        while (i < t.size() && src < 3) {
            QString type = QStringLiteral("lin");
            if (QStringList{QStringLiteral("lin"), QStringLiteral("oct"), QStringLiteral("dec")}.contains(t[i].toLower()))
                type = t[i++].toLower();
            if (i + 3 >= t.size() + 0 && i + 3 > t.size())
                break;
            m_dc[src].type->setCurrentIndex(type == QLatin1String("oct") ? 1 : (type == QLatin1String("dec") ? 2 : 0));
            m_dc[src].source->setText(t.value(i));
            m_dc[src].start->setText(t.value(i + 1));
            m_dc[src].stop->setText(t.value(i + 2));
            m_dc[src].incr->setText(t.value(i + 3));
            i += 4;
            ++src;
        }
    } else if (kw == QLatin1String(".noise")) {
        m_tabs->setCurrentIndex(3);
        QString out = t.value(1);
        static const QRegularExpression vre(QStringLiteral("^V\\(([^,)]+)(?:,([^)]+))?\\)$"), QRegularExpression::CaseInsensitiveOption);
        const auto m = vre.match(out);
        if (m.hasMatch()) {
            m_nOut->setText(QStringLiteral("V(%1)").arg(m.captured(1)));
            m_nRef->setText(m.captured(2));
        } else {
            m_nOut->setText(out);
        }
        m_nSrc->setText(t.value(2));
        const QString type = t.value(3).toLower();
        m_nType->setCurrentIndex(type == QLatin1String("oct") ? 0 : (type == QLatin1String("lin") ? 2 : 1));
        m_nPoints->setText(t.value(4));
        m_nStart->setText(t.value(5));
        m_nStop->setText(t.value(6));
    } else if (kw == QLatin1String(".op")) {
        m_tabs->setCurrentIndex(4);
    } else if (kw == QLatin1String(".tf")) {
        m_tabs->setCurrentIndex(5);
        m_tfOut->setText(t.value(1));
        m_tfSrc->setText(t.value(2));
    }
    m_updating = false;
}

void AnalysisDialog::updateSyntax()
{
    if (m_updating)
        return;
    QString cmd;
    switch (m_tabs->currentIndex()) {
    case 0: {
        QStringList parts{QStringLiteral(".tran")};
        const QString step = m_tStep->text().trimmed(), stop = m_tStop->text().trimmed(), start = m_tStart->text().trimmed(),
                      maxs = m_tMax->text().trimmed();
        if (step.isEmpty() && start.isEmpty() && maxs.isEmpty()) {
            parts << stop;
        } else {
            parts << (step.isEmpty() ? QStringLiteral("0") : step) << stop;
            if (!start.isEmpty() || !maxs.isEmpty())
                parts << (start.isEmpty() ? QStringLiteral("0") : start);
            if (!maxs.isEmpty())
                parts << maxs;
        }
        if (m_tStartup->isChecked()) parts << QStringLiteral("startup");
        if (m_tSteady->isChecked()) parts << QStringLiteral("steady");
        if (m_tNoDiscard->isChecked()) parts << QStringLiteral("nodiscard");
        if (m_tUic->isChecked()) parts << QStringLiteral("uic");
        cmd = parts.join(' ');
        break;
    }
    case 1: {
        static const char *types[] = {"oct", "dec", "lin"};
        cmd = QStringLiteral(".ac %1 %2 %3 %4")
                  .arg(QLatin1String(types[m_acType->currentIndex()]), m_acPoints->text().trimmed(), m_acStart->text().trimmed(),
                       m_acStop->text().trimmed());
        break;
    }
    case 2: {
        QStringList parts{QStringLiteral(".dc")};
        static const char *types[] = {"", "oct", "dec"};
        for (const DcSweep &d : m_dc) {
            if (d.source->text().trimmed().isEmpty())
                break;
            if (d.type->currentIndex() > 0)
                parts << QLatin1String(types[d.type->currentIndex()]);
            parts << d.source->text().trimmed() << d.start->text().trimmed() << d.stop->text().trimmed() << d.incr->text().trimmed();
        }
        cmd = parts.join(' ');
        break;
    }
    case 3: {
        static const char *types[] = {"oct", "dec", "lin"};
        QString out = m_nOut->text().trimmed();
        if (!m_nRef->text().trimmed().isEmpty() && out.endsWith(')'))
            out = out.left(out.size() - 1) + ',' + m_nRef->text().trimmed() + ')';
        cmd = QStringLiteral(".noise %1 %2 %3 %4 %5 %6")
                  .arg(out, m_nSrc->text().trimmed(), QLatin1String(types[m_nType->currentIndex()]), m_nPoints->text().trimmed(),
                       m_nStart->text().trimmed(), m_nStop->text().trimmed());
        break;
    }
    case 4:
        cmd = QStringLiteral(".op");
        break;
    case 5:
        cmd = QStringLiteral(".tf %1 %2").arg(m_tfOut->text().trimmed(), m_tfSrc->text().trimmed());
        break;
    }
    m_syntax->setText(cmd.simplified());
}

QString AnalysisDialog::command() const
{
    return m_syntax->text().trimmed();
}
