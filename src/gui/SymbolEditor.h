#pragma once

#include "Document.h"
#include "SchematicPainter.h"
#include "core/Symbol.h"

#include <QSet>
#include <QStringList>

class SymbolEditor : public DocumentWidget
{
    Q_OBJECT
public:
    enum class Mode { Normal, DrawLine, DrawRect, DrawCircle, DrawArc, AddPin, PlaceText, PlaceWindow, Delete };

    explicit SymbolEditor(QWidget *parent = nullptr);

    Kind kind() const override { return Kind::Symbol; }
    QString filePath() const override { return m_path; }
    bool isModified() const override { return m_modified; }
    bool save() override;
    bool saveAs(const QString &path) override;
    QString fileFilter() const override { return tr("Symbols (*.asy)"); }
    void zoomIn() override;
    void zoomOut() override;
    void zoomFit() override;
    void undo() override;
    void redo() override;
    bool canUndo() const override { return !m_undo.isEmpty(); }
    bool canRedo() const override { return !m_redo.isEmpty(); }
    bool exportImage(const QString &path) override;

    bool load(const QString &path, QString *error = nullptr);
    void setSymbol(const SymbolDef &def);
    const SymbolDef &symbol() const { return m_sym; }
    void setMode(Mode m);
    Mode mode() const { return m_mode; }
    void editAttributes();
    void startPlaceWindow();
    void deleteSelection();
    void rotateSelection();
    void mirrorSelection();

    // Auto-generated rectangular symbols.
    static SymbolDef generateFromSubckt(const QString &name, const QStringList &ports, const QString &modelFile);
    static SymbolDef generateBlock(const QStringList &inputs, const QStringList &outputs, const QStringList &bidirs);

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void contextMenuEvent(QContextMenuEvent *) override;

private:
    struct Item {
        enum Type { None, Shape, Pin, Text, Window } type = None;
        int index = -1;
        bool operator==(const Item &o) const { return type == o.type && index == o.index; }
    };
    friend size_t qHash(const SymbolEditor::Item &i, size_t seed);

    QPointF toWorld(QPointF w) const;
    QPoint snap(QPointF w, bool fine = false) const;
    QTransform transform() const;
    Item itemAt(QPointF world) const;
    QString windowLabel(const AttrWindow &w) const;
    void pushUndo();
    void changed();
    void editPin(int index);
    void editText(int index);
    void editWindow(int index);
    void moveItems(QPoint delta);

    SymbolDef m_sym;
    QString m_path;
    bool m_modified = false;
    QStringList m_undo, m_redo;
    double m_scale = 3.0;
    QPointF m_origin{-120, -120};
    Mode m_mode = Mode::Normal;
    QList<Item> m_selection;
    bool m_shapeActive = false;
    int m_shapeStage = 0;
    Shape m_shape;
    int m_windowId = 0;
    QString m_pendingText;
    bool m_dragging = false;
    bool m_panning = false;
    QPoint m_dragLast;
    QPointF m_lastWidget;
    QPointF m_mouseWorld;
    QString m_dragBackup;
};

inline size_t qHash(const SymbolEditor::Item &i, size_t seed = 0)
{
    return ::qHash((int(i.type) << 20) ^ i.index, seed);
}
