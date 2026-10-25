#pragma once

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QTabWidget;

// "Edit Simulation Command" dialog with one tab per analysis type. Produces
// the SPICE directive text (e.g. ".tran 0 10m 0 1u startup").
class AnalysisDialog : public QDialog
{
    Q_OBJECT
public:
    explicit AnalysisDialog(const QString &existing, QWidget *parent = nullptr);
    QString command() const;

private:
    void load(const QString &cmd);
    void updateSyntax();

    QTabWidget *m_tabs = nullptr;
    QLineEdit *m_syntax = nullptr;
    bool m_updating = false;
    // Transient
    QLineEdit *m_tStop = nullptr, *m_tStart = nullptr, *m_tMax = nullptr, *m_tStep = nullptr;
    QCheckBox *m_tStartup = nullptr, *m_tUic = nullptr, *m_tSteady = nullptr, *m_tNoDiscard = nullptr;
    // AC
    QComboBox *m_acType = nullptr;
    QLineEdit *m_acPoints = nullptr, *m_acStart = nullptr, *m_acStop = nullptr;
    // DC sweep (up to three sources)
    struct DcSweep {
        QLineEdit *source = nullptr;
        QComboBox *type = nullptr;
        QLineEdit *start = nullptr, *stop = nullptr, *incr = nullptr;
    };
    DcSweep m_dc[3];
    // Noise
    QLineEdit *m_nOut = nullptr, *m_nRef = nullptr, *m_nSrc = nullptr, *m_nPoints = nullptr, *m_nStart = nullptr, *m_nStop = nullptr;
    QComboBox *m_nType = nullptr;
    // TF
    QLineEdit *m_tfOut = nullptr, *m_tfSrc = nullptr;
};
