#include "NetLabelDialog.h"

#include <QButtonGroup>
#include <QDialog>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QRadioButton>
#include <QVBoxLayout>

bool NetLabelDialog::edit(QWidget *parent, QString &name, QString &ioDirection)
{
    QDialog dlg(parent);
    dlg.setWindowTitle(QObject::tr("Net Name"));
    auto *lay = new QVBoxLayout(&dlg);
    auto *nameEdit = new QLineEdit(name == QLatin1String("0") || name.compare(QLatin1String("COM"), Qt::CaseInsensitive) == 0 ? QString() : name);
    nameEdit->setPlaceholderText(QObject::tr("Net name"));
    lay->addWidget(nameEdit);
    auto *kindBox = new QGroupBox(QObject::tr("Label"));
    auto *kl = new QHBoxLayout(kindBox);
    auto *gnd = new QRadioButton(QObject::tr("GND (global node 0)"));
    auto *com = new QRadioButton(QObject::tr("COM"));
    auto *named = new QRadioButton(QObject::tr("Named net"));
    kl->addWidget(gnd);
    kl->addWidget(com);
    kl->addWidget(named);
    lay->addWidget(kindBox);
    if (name == QLatin1String("0"))
        gnd->setChecked(true);
    else if (name.compare(QLatin1String("COM"), Qt::CaseInsensitive) == 0)
        com->setChecked(true);
    else
        named->setChecked(true);
    auto *portBox = new QGroupBox(QObject::tr("Port Type (hierarchical sheets)"));
    auto *pl = new QHBoxLayout(portBox);
    auto *none = new QRadioButton(QObject::tr("None"));
    auto *in = new QRadioButton(QObject::tr("Input"));
    auto *out = new QRadioButton(QObject::tr("Output"));
    auto *bidir = new QRadioButton(QObject::tr("Bi-Direct."));
    for (auto *r : {none, in, out, bidir})
        pl->addWidget(r);
    const QString io = ioDirection.toLower();
    (io == QLatin1String("in") ? in : io == QLatin1String("out") ? out : io == QLatin1String("bidir") ? bidir : none)->setChecked(true);
    lay->addWidget(portBox);
    QObject::connect(nameEdit, &QLineEdit::textEdited, &dlg, [named]() { named->setChecked(true); });
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(bb);
    nameEdit->setFocus();
    if (dlg.exec() != QDialog::Accepted)
        return false;
    if (gnd->isChecked())
        name = QStringLiteral("0");
    else if (com->isChecked())
        name = QStringLiteral("COM");
    else {
        const QString n = nameEdit->text().trimmed().replace(' ', '_');
        if (n.isEmpty())
            return false;
        name = n;
    }
    ioDirection = in->isChecked() ? QStringLiteral("In") : out->isChecked() ? QStringLiteral("Out") : bidir->isChecked() ? QStringLiteral("BiDir") : QString();
    return true;
}
