#ifndef ALARMDIALOG_H
#define ALARMDIALOG_H

#include <QDialog>

class QComboBox;
class QDateTimeEdit;
class QTableWidget;

class AlarmDialog final : public QDialog
{
    Q_OBJECT
public:
    AlarmDialog(const QString &connectionName, QWidget *parent = nullptr);

private:
    void queryData();
    void clearData();

    QString m_connectionName;
    QDateTimeEdit *m_startEdit;
    QDateTimeEdit *m_endEdit;
    QComboBox *m_sensorCombo;
    QTableWidget *m_table;
};

#endif
