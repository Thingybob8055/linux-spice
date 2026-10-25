#pragma once

#include <QDialog>

class QLineEdit;
class QListWidget;

// "Add Traces to Plot": pick available data vectors or type an expression.
class AddTraceDialog : public QDialog
{
    Q_OBJECT
public:
    AddTraceDialog(const QStringList &vectors, QWidget *parent = nullptr);
    QStringList expressions() const;

private:
    QListWidget *m_list = nullptr;
    QLineEdit *m_filter = nullptr;
    QLineEdit *m_expr = nullptr;
};
