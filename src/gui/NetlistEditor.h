#pragma once

#include "Document.h"
#include "core/TextIO.h"

class QPlainTextEdit;

class NetlistEditor : public DocumentWidget
{
    Q_OBJECT
public:
    explicit NetlistEditor(QWidget *parent = nullptr);
    Kind kind() const override { return Kind::Netlist; }
    QString filePath() const override { return m_path; }
    QString displayName() const override { return m_path.isEmpty() && !m_title.isEmpty() ? m_title : DocumentWidget::displayName(); }
    bool isModified() const override;
    bool save() override;
    bool saveAs(const QString &path) override;
    QString fileFilter() const override { return tr("Netlists (*.net *.cir *.sp *.lib *.sub *.inc *.mod);;All files (*)"); }
    void undo() override;
    void redo() override;
    bool canUndo() const override;
    bool canRedo() const override;
    void zoomIn() override;
    void zoomOut() override;
    void print(QPrinter *printer) override;

    bool load(const QString &path, QString *error = nullptr);
    void setText(const QString &text, const QString &title);
    QString text() const;
    QStringList lines() const;
    QPlainTextEdit *editor() const { return m_edit; }
    void setReadOnly(bool ro);

signals:
    void createSymbolRequested(const QString &subcktName, const QStringList &ports, const QString &file);

private:
    void contextMenu(const QPoint &pos);
    QPlainTextEdit *m_edit = nullptr;
    QString m_path;
    QString m_title;
    textio::TextFile m_file;
};

class LogWindow : public DocumentWidget
{
    Q_OBJECT
public:
    explicit LogWindow(QWidget *parent = nullptr);
    Kind kind() const override { return Kind::Log; }
    QString displayName() const override { return m_title; }
    void setTitle(const QString &t);
    void setLog(const QString &text);
    void appendLog(const QString &text);
    QString text() const;
    void print(QPrinter *printer) override;

private:
    QPlainTextEdit *m_edit = nullptr;
    QString m_title;
};
