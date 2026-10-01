#ifndef ALARMDIALOG_H
#define ALARMDIALOG_H

#include <QDialog>
#include <QVariantList>
class HistoryQuery;
class QPushButton;
class QLabel;

class QComboBox;
class QDateTimeEdit;
class QTableWidget;

class AlarmDialog final : public QDialog
{
    Q_OBJECT
public:
    AlarmDialog(const QString &databasePath, QWidget *parent = nullptr);

private:
    void queryData();
    void clearData();

    HistoryQuery *m_query;
    QPushButton *m_queryButton, *m_previous, *m_next;
    QLabel *m_pageLabel;
    int m_offset = 0, m_requestedSensor = -1;
    void showRows(const QVariantList &rows);
    QDateTimeEdit *m_startEdit;
    QDateTimeEdit *m_endEdit;
    QComboBox *m_sensorCombo;
    QTableWidget *m_table;
};

#endif
