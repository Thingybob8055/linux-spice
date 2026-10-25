#include "TextDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QRadioButton>
#include <QVBoxLayout>

bool TextDialog::edit(QWidget *parent, SchText &text, bool isNew)
{
    QDialog dlg(parent);
    dlg.setWindowTitle(isNew ? (text.directive ? QObject::tr("Edit Text on the Schematic: SPICE directive")
                                               : QObject::tr("Edit Text on the Schematic: Comment"))
                             : QObject::tr("Edit Text on the Schematic"));
    dlg.resize(620, 320);
    auto *lay = new QVBoxLayout(&dlg);
    auto *hint = new QLabel(QObject::tr("How to netlist this text:"));
    lay->addWidget(hint);
    auto *radios = new QHBoxLayout;
    auto *comment = new QRadioButton(QObject::tr("Comment"));
    auto *directive = new QRadioButton(QObject::tr("SPICE directive"));
    (text.directive ? directive : comment)->setChecked(true);
    radios->addWidget(comment);
    radios->addWidget(directive);
    radios->addStretch();
    lay->addLayout(radios);
    auto *edit = new QPlainTextEdit(text.text);
    edit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    lay->addWidget(edit, 1);
    auto *note = new QLabel(QObject::tr("Ctrl+Enter accepts. Lines starting with '+' continue the previous line."));
    note->setEnabled(false);
    lay->addWidget(note);
    auto *form = new QFormLayout;
    auto *just = new QComboBox;
    const QStringList justs = {QStringLiteral("Left"), QStringLiteral("Center"), QStringLiteral("Right"), QStringLiteral("Top"),
                               QStringLiteral("Bottom")};
    just->addItems(justs);
    Justify j = text.just;
    const bool vertical = j.vertical;
    j.vertical = false;
    just->setCurrentText(j.toString());
    auto *vert = new QCheckBox(QObject::tr("Vertical text"));
    vert->setChecked(vertical);
    auto *size = new QComboBox;
    const QStringList sizes = {QStringLiteral("0.625x"), QStringLiteral("1.0x"), QStringLiteral("1.5x (default)"), QStringLiteral("2.0x"),
                               QStringLiteral("2.5x"), QStringLiteral("3.5x"), QStringLiteral("5.0x"), QStringLiteral("7.0x")};
    size->addItems(sizes);
    size->setCurrentIndex(std::clamp(text.fontSize, 0, 7));
    auto *row = new QHBoxLayout;
    row->addWidget(just);
    row->addWidget(vert);
    form->addRow(QObject::tr("Justification:"), row);
    form->addRow(QObject::tr("Font size:"), size);
    lay->addLayout(form);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(bb);
    auto *okShortcut = new QAction(&dlg);
    okShortcut->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return));
    dlg.addAction(okShortcut);
    QObject::connect(okShortcut, &QAction::triggered, &dlg, &QDialog::accept);
    edit->setFocus();
    if (dlg.exec() != QDialog::Accepted)
        return false;
    QString t = edit->toPlainText();
    while (t.endsWith('\n'))
        t.chop(1);
    if (t.trimmed().isEmpty())
        return false;
    text.text = t;
    text.directive = directive->isChecked();
    text.just = Justify::fromString(just->currentText());
    text.just.vertical = vert->isChecked();
    text.fontSize = size->currentIndex();
    return true;
}
