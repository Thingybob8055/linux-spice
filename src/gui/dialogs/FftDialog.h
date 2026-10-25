#pragma once

#include "core/Fft.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QListWidget;
class QSpinBox;

class FftDialog : public QDialog
{
    Q_OBJECT
public:
    FftDialog(const QStringList &vectors, const QStringList &plotted, double dataStart, double dataEnd, double viewStart, double viewEnd,
              QWidget *parent = nullptr);
    QStringList expressions() const;
    int points() const;
    double startTime() const;
    double endTime() const;
    fft::Window window() const;
    bool removeDc() const;
    int binsAveraged() const;
    bool allSteps() const;

private:
    QListWidget *m_list = nullptr;
    QLineEdit *m_extra = nullptr;
    QComboBox *m_points = nullptr;
    QLineEdit *m_start = nullptr, *m_end = nullptr;
    QComboBox *m_window = nullptr;
    QCheckBox *m_dc = nullptr, *m_steps = nullptr;
    QSpinBox *m_bins = nullptr;
    double m_dataStart, m_dataEnd;
};
