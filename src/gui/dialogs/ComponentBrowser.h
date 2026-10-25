#pragma once

#include "core/Symbol.h"

#include <QDialog>
#include <QWidget>

class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QTreeWidget;
class QPushButton;

// Draws a symbol definition scaled to fit (used for previews).
class SymbolPreview : public QWidget
{
    Q_OBJECT
public:
    using QWidget::QWidget;
    void setSymbol(SymbolPtr sym) { m_sym = sym; update(); }
    QSize sizeHint() const override { return {220, 220}; }

protected:
    void paintEvent(QPaintEvent *) override;

private:
    SymbolPtr m_sym;
};

// "Select Component Symbol" dialog: category tree, filtered list, preview.
class ComponentBrowser : public QDialog
{
    Q_OBJECT
public:
    explicit ComponentBrowser(QWidget *parent = nullptr);
    QString selectedSymbol() const { return m_selected; }
    QString exampleToOpen() const { return m_example; }

private:
    void populate();
    void applyFilter();
    void select(QListWidgetItem *item);
    QString findExample(const QString &name) const;

    QTreeWidget *m_tree = nullptr;
    QListWidget *m_list = nullptr;
    QLineEdit *m_search = nullptr;
    SymbolPreview *m_preview = nullptr;
    QLabel *m_desc = nullptr;
    QLabel *m_path = nullptr;
    QPushButton *m_exampleBtn = nullptr;
    QString m_selected;
    QString m_example;
    QString m_category;
};
