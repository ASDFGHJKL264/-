#pragma once
#include <QDialog>
#include <QVariantMap>
class MonitorService;
class QTableWidget;
class QLineEdit;
class QLabel;
class QCheckBox;
class QPushButton;
class MonitorDialog final : public QDialog
{
    Q_OBJECT
  public:
    MonitorDialog(MonitorService *service, const QList<int> &enabledZones, int intervalMs,
                  QWidget *parent = nullptr);

  private:
    void updateView(const QVariantMap &view);
    void refresh();
    MonitorService *m_service;
    QTableWidget *m_rules, *m_alarms, *m_tasks;
    QLineEdit *m_name;
    QLabel *m_hint;
    QCheckBox *m_participants[4];
    QPushButton *m_save, *m_start, *m_finish;
    QList<int> m_enabled;
    int m_interval;
    bool m_loaded = false;
};
