#include "FftDialog.h"

#include "core/Units.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

FftDialog::FftDialog(const QStringList &vectors, const QStringList &plotted, double dataStart, double dataEnd, double viewStart,
                     double viewEnd, QWidget *parent)
    : QDialog(parent)
    , m_dataStart(dataStart)
    , m_dataEnd(dataEnd)
{
    setWindowTitle(tr("Select Waveforms to Include in FFT"));
    resize(560, 520);
    auto *lay = new QHBoxLayout(this);
    auto *left = new QVBoxLayout;
    m_list = new QListWidget;
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    for (const QString &v : vectors) {
        if (v.compare(QLatin1String("time"), Qt::CaseInsensitive) == 0)
            continue;
        auto *it = new QListWidgetItem(v, m_list);
        it->setSelected(plotted.contains(v, Qt::CaseInsensitive));
    }
    left->addWidget(new QLabel(tr("Waveforms:")));
    left->addWidget(m_list, 1);
    m_extra = new QLineEdit;
    m_extra->setPlaceholderText(tr("Additional expression, e.g. V(a)-V(b)"));
    left->addWidget(m_extra);
    lay->addLayout(left, 1);

    auto *right = new QVBoxLayout;
    auto *form = new QFormLayout;
    m_points = new QComboBox;
    m_points->setEditable(true);
    for (int n : {1024, 2048, 4096, 8192, 16384, 32768, 65536, 131072, 262144, 524288, 1048576})
        m_points->addItem(QString::number(n));
    m_points->setCurrentText(QStringLiteral("65536"));
    form->addRow(tr("Number of data point samples in time:"), m_points);
    m_start = new QLineEdit(units::formatSpice(viewStart, 8));
    m_end = new QLineEdit(units::formatSpice(viewEnd, 8));
    form->addRow(tr("Start time:"), m_start);
    form->addRow(tr("End time:"), m_end);
    auto *useAll = new QPushButton(tr("Use extent of simulation data"));
    connect(useAll, &QPushButton::clicked, this, [this]() {
        m_start->setText(units::formatSpice(m_dataStart, 8));
        m_end->setText(units::formatSpice(m_dataEnd, 8));
    });
    form->addRow(QString(), useAll);
    m_window = new QComboBox;
    m_window->addItems(fft::windowNames());
    form->addRow(tr("Windowing function:"), m_window);
    m_bins = new QSpinBox;
    m_bins->setRange(1, 64);
    form->addRow(tr("Binomial smoothing (bins):"), m_bins);
    m_dc = new QCheckBox(tr("Remove DC component"));
    form->addRow(QString(), m_dc);
    m_steps = new QCheckBox(tr("Compute all steps"));
    form->addRow(QString(), m_steps);
    right->addLayout(form);
    right->addStretch();
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    right->addWidget(bb);
    lay->addLayout(right);
}

QStringList FftDialog::expressions() const
{
    QStringList out;
    for (QListWidgetItem *it : m_list->selectedItems())
        out << it->text();
    if (!m_extra->text().trimmed().isEmpty())
        out << m_extra->text().trimmed();
    return out;
}

int FftDialog::points() const { return std::max(16, m_points->currentText().toInt()); }
double FftDialog::startTime() const { return units::parseSpiceNumber(m_start->text()).value_or(m_dataStart); }
double FftDialog::endTime() const { return units::parseSpiceNumber(m_end->text()).value_or(m_dataEnd); }
fft::Window FftDialog::window() const { return fft::Window(m_window->currentIndex()); }
bool FftDialog::removeDc() const { return m_dc->isChecked(); }
int FftDialog::binsAveraged() const { return m_bins->value(); }
bool FftDialog::allSteps() const { return m_steps->isChecked(); }
