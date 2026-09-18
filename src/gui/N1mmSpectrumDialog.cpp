#include "N1mmSpectrumDialog.h"
#include "models/PanadapterModel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHostAddress>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace AetherSDR {

N1mmSpectrumDialog::N1mmSpectrumDialog(PanadapterModel* pan, QWidget* parent)
    : QDialog(parent)
    , m_pan(pan)
{
    setWindowTitle(pan ? QStringLiteral("N1MM+ Spectrum — Pan %1").arg(pan->panId())
                       : QStringLiteral("N1MM+ Spectrum"));
    setAttribute(Qt::WA_DeleteOnClose);

    auto* root = new QVBoxLayout(this);

    auto* info = new QLabel(
        "Forward this panadapter's spectrum to N1MM+ Logger over UDP.\n"
        "Settings are stored in the radio.");
    info->setWordWrap(true);
    root->addWidget(info);

    auto* form = new QFormLayout;
    m_enableChk = new QCheckBox("Send spectrum to N1MM+");
    form->addRow(QString(), m_enableChk);

    m_addressEdit = new QLineEdit;
    m_addressEdit->setPlaceholderText("127.0.0.1");
    m_addressEdit->setToolTip("IPv4 address of the computer running N1MM+.");
    form->addRow("Address:", m_addressEdit);

    m_portSpin = new QSpinBox;
    m_portSpin->setRange(1, 65535);
    m_portSpin->setToolTip(QStringLiteral("UDP port N1MM+ listens on for spectrum data (default %1).")
                               .arg(PanadapterModel::kN1mmDefaultPort));
    form->addRow("Port:", m_portSpin);

    m_radioCmb = new QComboBox;
    m_radioCmb->addItem("1", 1);
    m_radioCmb->addItem("2", 2);
    m_radioCmb->setToolTip("N1MM+ radio number (SO2R: 1 = left, 2 = right).");
    form->addRow("Radio:", m_radioCmb);
    root->addLayout(form);

    m_errorLbl = new QLabel;
    m_errorLbl->setStyleSheet("QLabel { color: #ff6060; }");
    m_errorLbl->hide();
    root->addWidget(m_errorLbl);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Apply | QDialogButtonBox::Cancel);
    auto* defaultsBtn = buttons->addButton("Defaults", QDialogButtonBox::ResetRole);
    root->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        apply();
        if (m_errorLbl->isHidden()) {
            accept();
        }
    });
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked,
            this, &N1mmSpectrumDialog::apply);
    connect(defaultsBtn, &QPushButton::clicked, this, [this] {
        m_addressEdit->setText(QStringLiteral("127.0.0.1"));
        m_portSpin->setValue(PanadapterModel::kN1mmDefaultPort);
        m_radioCmb->setCurrentIndex(0);
    });

    if (m_pan) {
        // Pan removed while dialog is open → nothing left to configure.
        connect(m_pan, &QObject::destroyed, this, &QDialog::reject);
    }

    loadFromModel();
}

void N1mmSpectrumDialog::loadFromModel()
{
    if (!m_pan) {
        m_enableChk->setChecked(false);
        m_addressEdit->setText(QStringLiteral("127.0.0.1"));
        m_portSpin->setValue(PanadapterModel::kN1mmDefaultPort);
        m_radioCmb->setCurrentIndex(0);
        return;
    }
    m_enableChk->setChecked(m_pan->n1mmSpectrumEnabled());
    m_addressEdit->setText(m_pan->n1mmAddress());
    m_portSpin->setValue(m_pan->n1mmPort());
    m_radioCmb->setCurrentIndex(m_pan->n1mmRadio() == 2 ? 1 : 0);
}

void N1mmSpectrumDialog::apply()
{
    if (!m_pan) {
        return;
    }
    QHostAddress host;
    const QString addr = m_addressEdit->text().trimmed();
    if (!host.setAddress(addr) || host.protocol() != QAbstractSocket::IPv4Protocol) {
        m_errorLbl->setText("Enter a valid IPv4 address.");
        m_errorLbl->show();
        return;
    }
    m_errorLbl->hide();

    // Destination first, enable last, so the radio never starts streaming to
    // a stale address/port.
    m_pan->setN1mmAddress(host.toString());
    m_pan->setN1mmPort(static_cast<quint16>(m_portSpin->value()));
    m_pan->setN1mmRadio(m_radioCmb->currentData().toInt());
    m_pan->setN1mmSpectrumEnabled(m_enableChk->isChecked());
}

} // namespace AetherSDR
