#pragma once

#include <QIcon>
#include <QString>

// Toolbar and menu icons, drawn with QPainter so they scale and follow the
// palette (dark/light themes).
namespace icons {
QIcon get(const QString &name);
QIcon appIcon();
}
