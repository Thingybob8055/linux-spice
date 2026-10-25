#pragma once

#include "core/Schematic.h"

class QWidget;

namespace TextDialog {
// Edits a text/directive item in place. Returns false if cancelled.
bool edit(QWidget *parent, SchText &text, bool isNew = false);
}
