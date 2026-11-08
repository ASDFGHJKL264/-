#pragma once
#include <QDialog>
#include <QVariantMap>
class MonitorController;
class QTableWidget;
class QLineEdit;
class QLabel;
class QCheckBox;
class QComboBox;
class QPushButton;
class MonitorDialog final : public QDialog
{
    Q_OBJECT
  public:
    MonitorDialog(MonitorController *controller, const QList<int> &enabledZones, int intervalMs,
                  QWidget *parent = nullptr);

  private:
    void updateView(const QVariantMap &view);
    void refresh();
    MonitorController *m_controller;
    QTableWidget *m_rules, *m_alarms, *m_tasks;
    QLineEdit *m_name;
    QLabel *m_hint;
    QCheckBox *m_participants[4];
    QPushButton *m_save, *m_start, *m_finish;
    QPushButton *m_alarmPrevious, *m_alarmNext, *m_taskPrevious, *m_taskNext;
    QLabel *m_alarmPageLabel, *m_taskPageLabel;
    QComboBox *m_alarmFilter;
    QList<int> m_enabled;
    int m_interval;
    bool m_loaded = false;
};
