#pragma once

#include "core/Schematic.h"

#include <QDialog>
#include <QHash>

class QLabel;
class QLineEdit;
class QProgressBar;
class QCheckBox;
class QComboBox;

// Imports an LTspice component library and example circuits the user already
// owns: from an LTspice installation folder (lib.zip / examples.zip), an
// extracted library directory, or a Wine prefix.
class LibraryImportDialog : public QDialog
{
    Q_OBJECT
public:
    explicit LibraryImportDialog(QWidget *parent = nullptr);
    static QStringList detectCandidates();

private:
    void runImport();
    QLineEdit *m_source = nullptr;
    QCheckBox *m_examples = nullptr;
    QProgressBar *m_progress = nullptr;
    QLabel *m_status = nullptr;
};

namespace dialogs {
void showOperatingPoint(QWidget *parent, const QHash<QString, double> &op, const QString &title);
void showBillOfMaterials(QWidget *parent, const Schematic &sch);
void exportPcbNetlist(QWidget *parent, const Schematic &sch);
void showShortcuts(QWidget *parent);
void showAbout(QWidget *parent);
}
