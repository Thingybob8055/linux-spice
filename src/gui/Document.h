#pragma once

#include <QString>
#include <QWidget>

class QPrinter;

// Common interface of everything that lives in an MDI sub-window
// (schematics, symbols, netlists, waveform plots).
class DocumentWidget : public QWidget
{
    Q_OBJECT
public:
    enum class Kind { Schematic, Symbol, Netlist, Waveform, Log };

    using QWidget::QWidget;
    virtual Kind kind() const = 0;
    virtual QString filePath() const { return QString(); }
    virtual QString displayName() const;
    virtual bool isModified() const { return false; }
    virtual bool save() { return true; }
    virtual bool saveAs(const QString &path) { Q_UNUSED(path); return false; }
    virtual QString fileFilter() const { return QString(); }
    virtual void zoomIn() {}
    virtual void zoomOut() {}
    virtual void zoomFit() {}
    virtual void zoomBack() {}
    virtual void undo() {}
    virtual void redo() {}
    virtual bool canUndo() const { return false; }
    virtual bool canRedo() const { return false; }
    virtual void print(QPrinter *printer) { Q_UNUSED(printer); }
    virtual void copyImageToClipboard() {}
    virtual bool exportImage(const QString &path) { Q_UNUSED(path); return false; }

signals:
    void modifiedChanged(bool modified);
    void titleChanged();
    void statusMessage(const QString &text, int timeoutMs = 0);
    void undoStateChanged(); // canUndo()/canRedo() may have changed
};
