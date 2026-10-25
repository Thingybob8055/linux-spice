#pragma once

#include <QString>

class QWidget;

namespace NetLabelDialog {
// Edits a net name (and port type for hierarchical sheets). Returns false if cancelled.
bool edit(QWidget *parent, QString &name, QString &ioDirection);
}
