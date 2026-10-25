#pragma once

#include <QColor>
#include <QDialog>
#include <QHash>

class QCheckBox;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QSpinBox;
class QTabWidget;
class QFontComboBox;

class SettingsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget *parent = nullptr, int initialTab = 0);
    void apply();

private:
    QWidget *simulatorTab();
    QWidget *schematicTab();
    QWidget *waveformTab();
    QWidget *pathsTab();
    QWidget *colorsTab();

    QTabWidget *m_tabs = nullptr;
    // Simulator
    QSpinBox *m_threads = nullptr;
    QLineEdit *m_ngspice = nullptr;
    QLineEdit *m_options = nullptr;
    QCheckBox *m_saveCurrents = nullptr, *m_indRser = nullptr, *m_keepRaw = nullptr;
    // Schematic
    QCheckBox *m_grid = nullptr, *m_ortho = nullptr, *m_unconn = nullptr, *m_shorts = nullptr, *m_thick = nullptr;
    QFontComboBox *m_font = nullptr;
    // Waveforms
    QCheckBox *m_wThick = nullptr, *m_wGrid = nullptr, *m_wPoints = nullptr;
    // Paths
    QLineEdit *m_libRoot = nullptr, *m_examples = nullptr, *m_userFiles = nullptr;
    QPlainTextEdit *m_symPaths = nullptr, *m_libPaths = nullptr;
    // Colors
    QListWidget *m_colorList = nullptr;
    QHash<QString, QColor> m_colors;
};
