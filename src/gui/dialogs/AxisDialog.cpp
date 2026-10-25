#include "AxisDialog.h"

#include "core/Units.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

AxisDialog::AxisDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Axis Limits"));
    auto *lay = new QVBoxLayout(this);
    m_title = new QLabel;
    QFont f = m_title->font();
    f.setBold(true);
    m_title->setFont(f);
    lay->addWidget(m_title);
    auto *form = new QFormLayout;
    m_min = new QLineEdit;
    m_max = new QLineEdit;
    m_log = new QCheckBox(tr("Logarithmic"));
    m_expr = new QLineEdit;
    m_expr->setPlaceholderText(tr("(default sweep variable)"));
    form->addRow(tr("Left/Bottom:"), m_min);
    form->addRow(tr("Right/Top:"), m_max);
    form->addRow(QString(), m_log);
    form->addRow(tr("Quantity plotted:"), m_expr);
    lay->addLayout(form);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    auto *autoBtn = bb->addButton(tr("Auto range"), QDialogButtonBox::ActionRole);
    connect(autoBtn, &QPushButton::clicked, this, [this]() {
        m_auto = true;
        accept();
    });
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(bb);
    m_exprLabel = form->labelForField(m_expr);
    m_expr->hide();
    m_exprLabel->hide();
}

void AxisDialog::setRange(double min, double max, bool log, const QString &unit, const QString &title)
{
    Q_UNUSED(unit);
    m_title->setText(title);
    m_min->setText(units::formatSpice(min, 7));
    m_max->setText(units::formatSpice(max, 7));
    m_log->setChecked(log);
}

void AxisDialog::setExpression(const QString &expr)
{
    m_expr->setText(expr);
    m_expr->show();
    m_exprLabel->show();
}

double AxisDialog::minimum() const
{
    return units::parseSpiceNumber(m_min->text()).value_or(0.0);
}

double AxisDialog::maximum() const
{
    return units::parseSpiceNumber(m_max->text()).value_or(1.0);
}

bool AxisDialog::isLog() const
{
    return m_log->isChecked();
}

QString AxisDialog::expression() const
{
    return m_expr->text().trimmed();
}
