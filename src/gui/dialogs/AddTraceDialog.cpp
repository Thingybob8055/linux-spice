#include "AddTraceDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QRegularExpression>
#include <QVBoxLayout>

AddTraceDialog::AddTraceDialog(const QStringList &vectors, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Add Traces to Plot"));
    resize(520, 560);
    auto *lay = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    m_filter = new QLineEdit;
    m_filter->setPlaceholderText(tr("Only list traces matching (wildcards * and ? allowed)"));
    m_filter->setClearButtonEnabled(true);
    form->addRow(tr("Filter:"), m_filter);
    lay->addLayout(form);
    auto *hideInternal = new QCheckBox(tr("Hide internal subcircuit nodes"));
    hideInternal->setChecked(true);
    lay->addWidget(hideInternal);
    lay->addWidget(new QLabel(tr("Available data:")));
    m_list = new QListWidget;
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    for (const QString &v : vectors) {
        if (v == QLatin1String("time") || v == QLatin1String("frequency"))
            continue;
        new QListWidgetItem(v, m_list);
    }
    lay->addWidget(m_list, 1);
    auto *ef = new QFormLayout;
    m_expr = new QLineEdit;
    m_expr->setPlaceholderText(tr("e.g. V(out)*I(R1) or db(V(out)/V(in))"));
    ef->addRow(tr("Expression(s) to add:"), m_expr);
    lay->addLayout(ef);
    auto *help = new QLabel(tr("Functions: abs sqrt exp ln log10 sin cos tan atan2 min max limit if table d() idt() db() mag() ph() re() im() "
                               "u() uramp() buf() inv() pow pwr pwrs hypot rand random white; operators + - * / ** & | ^ < > ; "
                               "@n selects a step, e.g. V(out)@2."));
    help->setWordWrap(true);
    help->setEnabled(false);
    lay->addWidget(help);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(bb);

    auto applyFilter = [this, hideInternal]() {
        const QString f = m_filter->text().trimmed();
        const QRegularExpression re(QRegularExpression::wildcardToRegularExpression(f.isEmpty() ? QStringLiteral("*") : QStringLiteral("*%1*").arg(f),
                                                                                    QRegularExpression::UnanchoredWildcardConversion),
                                    QRegularExpression::CaseInsensitiveOption);
        for (int i = 0; i < m_list->count(); ++i) {
            const QString t = m_list->item(i)->text();
            bool hide = !re.match(t).hasMatch();
            if (hideInternal->isChecked() && t.contains(':'))
                hide = true;
            m_list->item(i)->setHidden(hide);
        }
    };
    connect(m_filter, &QLineEdit::textChanged, this, applyFilter);
    connect(hideInternal, &QCheckBox::toggled, this, applyFilter);
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *it) {
        // Double-click appends the name to the expression field (LTspice behavior).
        QString e = m_expr->text();
        if (!e.isEmpty() && !e.endsWith(' ') && !e.endsWith('(') && !e.endsWith('*') && !e.endsWith('+') && !e.endsWith('-')
            && !e.endsWith('/'))
            e += ' ';
        m_expr->setText(e + it->text());
        m_expr->setFocus();
    });
    applyFilter();
    m_filter->setFocus();
}

QStringList AddTraceDialog::expressions() const
{
    QStringList out;
    for (QListWidgetItem *it : m_list->selectedItems())
        out << it->text();
    // Space-separated expressions at top level (parentheses aware).
    const QString e = m_expr->text().trimmed();
    QString cur;
    int depth = 0;
    for (QChar c : e) {
        if (c == '(')
            ++depth;
        else if (c == ')')
            --depth;
        if (c == ' ' && depth == 0) {
            // Keep spaces next to operators inside one expression.
            if (!cur.isEmpty() && !QStringLiteral("+-*/&|^<>=,").contains(cur.back())) {
                out << cur;
                cur.clear();
            }
            continue;
        }
        if (!cur.isEmpty() && cur.back() == ' ' && QStringLiteral("+-*/&|^<>=,").contains(c)) {
            cur += c;
            continue;
        }
        cur += c;
    }
    if (!cur.isEmpty())
        out << cur;
    return out;
}
