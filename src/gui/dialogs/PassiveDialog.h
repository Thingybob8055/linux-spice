#pragma once

#include "core/Schematic.h"

#include <QDialog>
#include <QHash>

class QLineEdit;
class QListWidget;
class QLabel;
class QPlainTextEdit;

// Assisted editor for resistors, capacitors and inductors.
class PassiveDialog : public QDialog
{
    Q_OBJECT
public:
    PassiveDialog(const SymbolInstance &s, QWidget *parent = nullptr);
    SymbolInstance result() const;

private:
    SymbolInstance m_sym;
    QChar m_kind;
    QLineEdit *m_name = nullptr;
    QLineEdit *m_value = nullptr;
    QHash<QString, QLineEdit *> m_params; // SpiceLine parameter -> editor
    QStringList m_order;
    QString m_otherParams;
};

// Picks a semiconductor model from the standard libraries (or types one in).
class ModelSelectDialog : public QDialog
{
    Q_OBJECT
public:
    ModelSelectDialog(const SymbolInstance &s, QWidget *parent = nullptr);
    SymbolInstance result() const;

private:
    void filter(const QString &text);
    SymbolInstance m_sym;
    QLineEdit *m_name = nullptr;
    QLineEdit *m_value = nullptr;
    QLineEdit *m_search = nullptr;
    QListWidget *m_list = nullptr;
    QPlainTextEdit *m_details = nullptr;
};

// Parses "key=value key2=value2" into a map (keys lower-case) and keeps the order.
QHash<QString, QString> parseParamList(const QString &text, QStringList *order = nullptr, QString *rest = nullptr);
