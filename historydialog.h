#ifndef HISTORYDIALOG_H
#define HISTORYDIALOG_H

#include "qcustomplot.h"
#include <QDialog>
#include <QVariantList>
class HistoryQuery;
class QPushButton;
class QLabel;

class QComboBox;
class QDateTimeEdit;
class QTableWidget;

class HistoryDialog final : public QDialog
{
    Q_OBJECT
public:
    HistoryDialog(const QString &databasePath, QWidget *parent = nullptr);

private:
    void queryData();

    HistoryQuery *m_query;
    QPushButton *m_queryButton, *m_previous, *m_next;
    QLabel *m_pageLabel;
    int m_offset = 0, m_requestedSensor = -1;
    void showRows(const QVariantList &rows);
    QDateTimeEdit *m_startEdit;
    QDateTimeEdit *m_endEdit;
    QComboBox *m_sensorCombo;
    QTableWidget *m_table;
    QCustomPlot *m_plot;
};

#endif
