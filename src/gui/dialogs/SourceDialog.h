#pragma once

#include "core/Schematic.h"

#include <QDialog>
#include <QList>

class QButtonGroup;
class QCheckBox;
class QLabel;
class QLineEdit;
class QStackedWidget;
class QTableWidget;

// Assisted editor for independent voltage/current sources.
class SourceDialog : public QDialog
{
    Q_OBJECT
public:
    SourceDialog(const SymbolInstance &s, QWidget *parent = nullptr);
    SymbolInstance result() const;

    // Splits a source value into its parts (public for testing).
    struct Parsed {
        QString dc;
        QString function; // PULSE, SINE, EXP, SFFM, PWL, PWLFILE or ""
        QStringList args;
        QString pwlFile;
        QString acMag, acPhase;
        QString rser, cpar;
        QString extra; // unrecognised remainder
        bool load = false;
    };
    static Parsed parse(const QString &value);

private:
    QString composeValue() const;
    void updatePreview();

    SymbolInstance m_sym;
    bool m_isCurrent = false;
    QLineEdit *m_name = nullptr;
    QButtonGroup *m_funcGroup = nullptr;
    QStackedWidget *m_pages = nullptr;
    QLineEdit *m_dc = nullptr;
    QList<QList<QLineEdit *>> m_funcArgs; // per function page
    QTableWidget *m_pwlTable = nullptr;
    QLineEdit *m_pwlFile = nullptr;
    QLineEdit *m_acMag = nullptr, *m_acPhase = nullptr;
    QLineEdit *m_rser = nullptr, *m_cpar = nullptr;
    QCheckBox *m_load = nullptr;
    QLineEdit *m_extra = nullptr;
    QLabel *m_preview = nullptr;
};
