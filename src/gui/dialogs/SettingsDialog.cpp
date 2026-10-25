#include "SettingsDialog.h"

#include "MiscDialogs.h"
#include "core/AppSettings.h"
#include "core/ModelLibrary.h"
#include "core/SymbolLibrary.h"
#include "core/DirCache.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFontComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QThread>
#include <QVBoxLayout>

SettingsDialog::SettingsDialog(QWidget *parent, int initialTab)
    : QDialog(parent)
{
    setWindowTitle(tr("Settings"));
    resize(640, 520);
    auto *lay = new QVBoxLayout(this);
    m_tabs = new QTabWidget;
    m_tabs->addTab(simulatorTab(), tr("SPICE"));
    m_tabs->addTab(schematicTab(), tr("Schematic"));
    m_tabs->addTab(waveformTab(), tr("Waveforms"));
    m_tabs->addTab(pathsTab(), tr("Search Paths"));
    m_tabs->addTab(colorsTab(), tr("Colors"));
    m_tabs->setCurrentIndex(initialTab);
    lay->addWidget(m_tabs);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Apply);
    connect(bb, &QDialogButtonBox::accepted, this, [this]() {
        apply();
        accept();
    });
    connect(bb->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, &SettingsDialog::apply);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(bb);
}

QWidget *SettingsDialog::simulatorTab()
{
    const AppSettings &s = AppSettings::instance();
    auto *w = new QWidget;
    auto *f = new QFormLayout(w);
    m_threads = new QSpinBox;
    m_threads->setRange(1, std::max(1, QThread::idealThreadCount() * 2));
    m_threads->setValue(s.maxThreads());
    f->addRow(tr("Parallel simulations (for .step):"), m_threads);
    auto *ngRow = new QHBoxLayout;
    m_ngspice = new QLineEdit(s.ngspiceLibrary());
    m_ngspice->setPlaceholderText(tr("(automatic: system libngspice.so)"));
    auto *browse = new QPushButton(tr("Browse..."));
    ngRow->addWidget(m_ngspice, 1);
    ngRow->addWidget(browse);
    connect(browse, &QPushButton::clicked, this, [this]() {
        const QString f = QFileDialog::getOpenFileName(this, tr("Locate libngspice"), QStringLiteral("/usr/lib"), tr("Shared libraries (*.so*)"));
        if (!f.isEmpty())
            m_ngspice->setText(f);
    });
    f->addRow(tr("ngspice shared library:"), ngRow);
    m_options = new QLineEdit(s.defaultSolverOptions());
    m_options->setPlaceholderText(tr("e.g. reltol=1e-4 gmin=1e-12 method=gear"));
    f->addRow(tr("Default .options:"), m_options);
    m_saveCurrents = new QCheckBox(tr("Save device currents (needed to probe component currents)"));
    m_saveCurrents->setChecked(s.saveDeviceCurrents());
    f->addRow(m_saveCurrents);
    m_indRser = new QCheckBox(tr("Give inductors a default series resistance of 1mΩ (LTspice behavior)"));
    m_indRser->setChecked(s.defaultInductorRser());
    f->addRow(m_indRser);
    m_keepRaw = new QCheckBox(tr("Write .raw/.log/.net files next to the schematic"));
    m_keepRaw->setChecked(s.keepRawFiles());
    f->addRow(m_keepRaw);
    auto *note = new QLabel(tr("Simulations run in separate processes using the ngspice engine with LTspice compatibility enabled."));
    note->setWordWrap(true);
    note->setEnabled(false);
    f->addRow(note);
    return w;
}

QWidget *SettingsDialog::schematicTab()
{
    const AppSettings &s = AppSettings::instance();
    auto *w = new QWidget;
    auto *f = new QFormLayout(w);
    m_grid = new QCheckBox(tr("Show schematic grid points"));
    m_grid->setChecked(s.showGrid());
    m_ortho = new QCheckBox(tr("Orthogonal snap wires and ortho stretch"));
    m_ortho->setChecked(s.orthoWires());
    m_unconn = new QCheckBox(tr("Mark unconnected pins"));
    m_unconn->setChecked(s.markUnconnectedPins());
    m_shorts = new QCheckBox(tr("Allow direct component pin shorts (do not cut wires through components)"));
    m_shorts->setChecked(s.allowPinShorts());
    m_thick = new QCheckBox(tr("Draft with thick lines"));
    m_thick->setChecked(s.thickLines());
    for (QCheckBox *c : {m_grid, m_ortho, m_unconn, m_shorts, m_thick})
        f->addRow(c);
    m_font = new QFontComboBox;
    m_font->setCurrentFont(QFont(s.fontFamily()));
    f->addRow(tr("Schematic font:"), m_font);
    return w;
}

QWidget *SettingsDialog::waveformTab()
{
    const AppSettings &s = AppSettings::instance();
    auto *w = new QWidget;
    auto *f = new QFormLayout(w);
    m_wThick = new QCheckBox(tr("Plot data with thick lines"));
    m_wThick->setChecked(s.waveformThickLines());
    m_wGrid = new QCheckBox(tr("Show grid"));
    m_wGrid->setChecked(s.waveformGrid());
    m_wPoints = new QCheckBox(tr("Mark data points"));
    m_wPoints->setChecked(s.markDataPoints());
    for (QCheckBox *c : {m_wThick, m_wGrid, m_wPoints})
        f->addRow(c);
    return w;
}

QWidget *SettingsDialog::pathsTab()
{
    const AppSettings &s = AppSettings::instance();
    auto *w = new QWidget;
    auto *f = new QFormLayout(w);
    auto pathRow = [this](QLineEdit *edit) {
        auto *row = new QHBoxLayout;
        auto *b = new QPushButton(tr("Browse..."));
        row->addWidget(edit, 1);
        row->addWidget(b);
        connect(b, &QPushButton::clicked, this, [this, edit]() {
            const QString d = QFileDialog::getExistingDirectory(this, tr("Select directory"), edit->text());
            if (!d.isEmpty())
                edit->setText(d);
        });
        return row;
    };
    m_libRoot = new QLineEdit(s.libraryRoot());
    f->addRow(tr("Component library (sym/, sub/, cmp/):"), pathRow(m_libRoot));
    m_examples = new QLineEdit(s.examplesRoot());
    f->addRow(tr("Examples directory:"), pathRow(m_examples));
    m_userFiles = new QLineEdit(s.userFilesDir());
    f->addRow(tr("User files:"), pathRow(m_userFiles));
    auto *import = new QPushButton(tr("Import LTspice library / examples..."));
    connect(import, &QPushButton::clicked, this, [this]() {
        LibraryImportDialog dlg(this);
        if (dlg.exec() == QDialog::Accepted) {
            m_libRoot->setText(AppSettings::instance().libraryRoot());
            m_examples->setText(AppSettings::instance().examplesRoot());
        }
    });
    f->addRow(QString(), import);
    m_symPaths = new QPlainTextEdit(s.symbolSearchPaths().join('\n'));
    m_symPaths->setPlaceholderText(tr("One directory per line"));
    m_symPaths->setMaximumHeight(90);
    f->addRow(tr("Additional symbol search paths:"), m_symPaths);
    m_libPaths = new QPlainTextEdit(s.librarySearchPaths().join('\n'));
    m_libPaths->setPlaceholderText(tr("One directory per line"));
    m_libPaths->setMaximumHeight(90);
    f->addRow(tr("Additional library (.lib/.include) search paths:"), m_libPaths);
    return w;
}

QWidget *SettingsDialog::colorsTab()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    m_colorList = new QListWidget;
    for (const QString &key : AppSettings::colorKeys()) {
        const QColor c = AppSettings::instance().color(key);
        m_colors.insert(key, c);
        auto *it = new QListWidgetItem(AppSettings::colorLabel(key), m_colorList);
        it->setData(Qt::UserRole, key);
        QPixmap pm(24, 16);
        pm.fill(c);
        it->setIcon(QIcon(pm));
    }
    v->addWidget(m_colorList);
    auto *row = new QHBoxLayout;
    auto *change = new QPushButton(tr("Change..."));
    auto *reset = new QPushButton(tr("Reset to defaults"));
    row->addWidget(change);
    row->addWidget(reset);
    row->addStretch();
    v->addLayout(row);
    auto edit = [this]() {
        QListWidgetItem *it = m_colorList->currentItem();
        if (!it)
            return;
        const QString key = it->data(Qt::UserRole).toString();
        const QColor c = QColorDialog::getColor(m_colors.value(key), this, it->text());
        if (!c.isValid())
            return;
        m_colors[key] = c;
        QPixmap pm(24, 16);
        pm.fill(c);
        it->setIcon(QIcon(pm));
    };
    connect(change, &QPushButton::clicked, this, edit);
    connect(m_colorList, &QListWidget::itemDoubleClicked, this, edit);
    connect(reset, &QPushButton::clicked, this, [this]() {
        for (int i = 0; i < m_colorList->count(); ++i) {
            QListWidgetItem *it = m_colorList->item(i);
            const QString key = it->data(Qt::UserRole).toString();
            m_colors[key] = AppSettings::defaultColor(key);
            QPixmap pm(24, 16);
            pm.fill(m_colors[key]);
            it->setIcon(QIcon(pm));
        }
    });
    return w;
}

void SettingsDialog::apply()
{
    AppSettings &s = AppSettings::instance();
    s.setMaxThreads(m_threads->value());
    s.setNgspiceLibrary(m_ngspice->text().trimmed());
    s.setDefaultSolverOptions(m_options->text().trimmed());
    s.setSaveDeviceCurrents(m_saveCurrents->isChecked());
    s.setDefaultInductorRser(m_indRser->isChecked());
    s.setKeepRawFiles(m_keepRaw->isChecked());
    s.setShowGrid(m_grid->isChecked());
    s.setOrthoWires(m_ortho->isChecked());
    s.setMarkUnconnectedPins(m_unconn->isChecked());
    s.setAllowPinShorts(m_shorts->isChecked());
    s.setThickLines(m_thick->isChecked());
    s.setFontFamily(m_font->currentFont().family());
    s.setWaveformThickLines(m_wThick->isChecked());
    s.setWaveformGrid(m_wGrid->isChecked());
    s.setMarkDataPoints(m_wPoints->isChecked());
    const bool pathsChanged = s.libraryRoot() != m_libRoot->text().trimmed()
        || s.symbolSearchPaths() != m_symPaths->toPlainText().split('\n', Qt::SkipEmptyParts)
        || s.librarySearchPaths() != m_libPaths->toPlainText().split('\n', Qt::SkipEmptyParts);
    s.setLibraryRoot(m_libRoot->text().trimmed());
    s.setExamplesRoot(m_examples->text().trimmed());
    s.setUserFilesDir(m_userFiles->text().trimmed());
    s.setSymbolSearchPaths(m_symPaths->toPlainText().split('\n', Qt::SkipEmptyParts));
    s.setLibrarySearchPaths(m_libPaths->toPlainText().split('\n', Qt::SkipEmptyParts));
    for (auto it = m_colors.constBegin(); it != m_colors.constEnd(); ++it)
        s.setColor(it.key(), it.value());
    if (pathsChanged) {
        DirCache::clear();
        SymbolLibrary::instance().rescan();
        ModelLibrary::instance().clearCache();
    }
}
