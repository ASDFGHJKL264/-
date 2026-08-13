#ifndef HISTORYDIALOG_H
#define HISTORYDIALOG_H

#include "qcustomplot.h"
#include <QDialog>

class QComboBox;
class QDateTimeEdit;
class QTableWidget;

class HistoryDialog final : public QDialog
{
    Q_OBJECT
public:
    HistoryDialog(const QString &connectionName, QWidget *parent = nullptr);

private:
    void queryData();

    QString m_connectionName;
    QDateTimeEdit *m_startEdit;
    QDateTimeEdit *m_endEdit;
    QComboBox *m_sensorCombo;
    QTableWidget *m_table;
    QCustomPlot *m_plot;
};

#endif
