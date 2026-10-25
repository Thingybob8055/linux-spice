#pragma once

#include <QDialog>

class QCheckBox;
class QLineEdit;
class QLabel;

class AxisDialog : public QDialog
{
    Q_OBJECT
public:
    explicit AxisDialog(QWidget *parent = nullptr);
    void setRange(double min, double max, bool log, const QString &unit, const QString &title);
    void setExpression(const QString &expr);
    double minimum() const;
    double maximum() const;
    bool isLog() const;
    QString expression() const;
    bool autoRequested() const { return m_auto; }

private:
    QLabel *m_title = nullptr;
    QLineEdit *m_min = nullptr, *m_max = nullptr, *m_expr = nullptr;
    QCheckBox *m_log = nullptr;
    QWidget *m_exprLabel = nullptr;
    bool m_auto = false;
};
