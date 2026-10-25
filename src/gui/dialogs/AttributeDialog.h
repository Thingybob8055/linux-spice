#pragma once

#include "core/Schematic.h"

#include <QDialog>

class QTableWidget;

// "Super expert" attribute editor: every attribute with a visibility toggle.
class AttributeDialog : public QDialog
{
    Q_OBJECT
public:
    AttributeDialog(const SymbolInstance &s, QWidget *parent = nullptr);
    SymbolInstance result() const;

private:
    SymbolInstance m_sym;
    QTableWidget *m_table = nullptr;
};
