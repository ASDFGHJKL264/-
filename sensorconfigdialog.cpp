#include "sensorconfigdialog.h"

#include "mainwindow.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSet>
#include <QSpinBox>
#include <QVBoxLayout>

SensorConfigDialog::SensorConfigDialog(const AppConfig &config, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle("传感器配置");
    resize(820, 300);

    auto *layout = new QVBoxLayout(this);
    auto *grid = new QGridLayout;
    const QStringList headers = {
        "模块", "启用", "名称", "站地址", "寄存器类型", "温度地址", "湿度地址"
    };
    for (int column = 0; column < headers.size(); ++column) {
        auto *label = new QLabel(headers.at(column));
        label->setAlignment(Qt::AlignCenter);
        grid->addWidget(label, 0, column);
    }

    for (int i = 0; i < SensorLimit; ++i) {
        const auto &sensor = config.sensors[i];
        grid->addWidget(new QLabel(QString("模块%1").arg(i + 1)), i + 1, 0);

        m_enabledEdits[i] = new QCheckBox;
        m_enabledEdits[i]->setChecked(sensor.enabled);
        grid->addWidget(m_enabledEdits[i], i + 1, 1, Qt::AlignCenter);

        m_nameEdits[i] = new QLineEdit(sensor.name);
        grid->addWidget(m_nameEdits[i], i + 1, 2);

        m_slaveEdits[i] = new QSpinBox;
        m_slaveEdits[i]->setRange(1, 247);
        m_slaveEdits[i]->setValue(sensor.slaveId);
        grid->addWidget(m_slaveEdits[i], i + 1, 3);

        m_typeEdits[i] = new QComboBox;
        m_typeEdits[i]->addItem("保持寄存器（功能码03）", false);
        m_typeEdits[i]->addItem("输入寄存器（功能码04）", true);
        m_typeEdits[i]->setCurrentIndex(sensor.useInputRegisters ? 1 : 0);
        grid->addWidget(m_typeEdits[i], i + 1, 4);

        m_temperatureEdits[i] = new QSpinBox;
        m_temperatureEdits[i]->setRange(0, 65535);
        m_temperatureEdits[i]->setValue(sensor.temperatureRegister);
        grid->addWidget(m_temperatureEdits[i], i + 1, 5);

        m_humidityEdits[i] = new QSpinBox;
        m_humidityEdits[i]->setRange(0, 65535);
        m_humidityEdits[i]->setValue(sensor.humidityRegister);
        grid->addWidget(m_humidityEdits[i], i + 1, 6);
    }

    grid->setColumnStretch(2, 1);
    grid->setColumnStretch(4, 1);
    layout->addLayout(grid);

    auto *hint = new QLabel(
        "说明：每个启用模块必须使用不同站地址。保存后立即生效，下次启动自动加载。"
        "未连接的模块建议不要启用。");
    hint->setWordWrap(true);
    layout->addWidget(hint);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Save)->setText("保存");
    buttons->button(QDialogButtonBox::Cancel)->setText("取消");
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &SensorConfigDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &SensorConfigDialog::reject);
}

void SensorConfigDialog::applyTo(AppConfig &config) const
{
    for (int i = 0; i < SensorLimit; ++i) {
        auto &sensor = config.sensors[i];
        sensor.enabled = m_enabledEdits[i]->isChecked();
        sensor.name = m_nameEdits[i]->text().trimmed();
        if (sensor.name.isEmpty())
            sensor.name = QString("温湿度模块%1").arg(i + 1);
        sensor.slaveId = m_slaveEdits[i]->value();
        sensor.useInputRegisters = m_typeEdits[i]->currentData().toBool();
        sensor.temperatureRegister = m_temperatureEdits[i]->value();
        sensor.humidityRegister = m_humidityEdits[i]->value();
    }
}

bool SensorConfigDialog::validateInput(QString *errorMessage) const
{
    int enabledCount = 0;
    QSet<int> addresses;
    for (int i = 0; i < SensorLimit; ++i) {
        if (!m_enabledEdits[i]->isChecked())
            continue;
        ++enabledCount;
        const int slaveId = m_slaveEdits[i]->value();
        if (addresses.contains(slaveId)) {
            *errorMessage = "启用的传感器不能使用重复站地址。";
            return false;
        }
        addresses.insert(slaveId);
        if (qAbs(m_temperatureEdits[i]->value() - m_humidityEdits[i]->value()) >= 125) {
            *errorMessage = QString("模块%1的温湿度寄存器跨度超过Modbus单次读取上限。")
                                .arg(i + 1);
            return false;
        }
    }
    if (enabledCount == 0) {
        *errorMessage = "至少要启用一个传感器。";
        return false;
    }
    return true;
}

void SensorConfigDialog::accept()
{
    QString error;
    if (!validateInput(&error)) {
        QMessageBox::warning(this, "配置无效", error);
        return;
    }
    QDialog::accept();
}
