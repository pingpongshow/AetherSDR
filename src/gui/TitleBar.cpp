#include "TitleBar.h"
#include "GuardedSlider.h"
#include "core/AppSettings.h"

#include <QHBoxLayout>
#include <QPushButton>
#include <QSlider>
#include <QLabel>
#include <QMenuBar>
#include <QTimer>
#include <QMenu>

namespace AetherSDR {

TitleBar::TitleBar(QWidget* parent)
    : QWidget(parent)
{
    setFixedHeight(32);
    setStyleSheet("TitleBar { background: #0a0a14; border-bottom: 1px solid #203040; }");

    m_hbox = new QHBoxLayout(this);
    m_hbox->setContentsMargins(4, 2, 8, 2);
    m_hbox->setSpacing(6);

    // Keep the identity/status cluster anchored on the left, immediately after
    // the menu bar once it is inserted via setMenuBar().

    // ── Heartbeat indicator ─────────────────────────────────────────────────
    m_heartbeat = new QLabel;
    m_heartbeat->setFixedSize(10, 10);
    m_heartbeat->setStyleSheet(
        "QLabel { background: #404858; border-radius: 5px; }");
    m_heartbeat->setToolTip("Radio discovery heartbeat");
    m_heartbeat->setAccessibleName("Radio heartbeat");
    m_heartbeat->setAccessibleDescription("Flashes green when radio discovery packets are received");

    // 100ms timer to return green flash back to grey
    m_heartbeatOffTimer = new QTimer(this);
    m_heartbeatOffTimer->setSingleShot(true);
    m_heartbeatOffTimer->setInterval(100);
    connect(m_heartbeatOffTimer, &QTimer::timeout, this, [this]() {
        m_heartbeat->setStyleSheet(
            "QLabel { background: #404858; border-radius: 5px; }");
    });

    // 500ms alarm blink timer (red/grey alternating)
    m_heartbeatAlarmTimer = new QTimer(this);
    m_heartbeatAlarmTimer->setInterval(500);
    connect(m_heartbeatAlarmTimer, &QTimer::timeout, this, [this]() {
        m_alarmRed = !m_alarmRed;
        m_heartbeat->setStyleSheet(m_alarmRed
            ? "QLabel { background: #cc2020; border-radius: 5px; }"
            : "QLabel { background: #404858; border-radius: 5px; }");
    });

    // Load persisted blink preference (default: enabled)
    m_blinkEnabled = AppSettings::instance()
        .value("HeartbeatBlinkEnabled", "True").toString() == "True";

    // Right-click on the indicator to toggle blink on/off without opening a menu
    m_heartbeat->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_heartbeat, &QWidget::customContextMenuRequested,
            this, [this](const QPoint& pos) {
        // Heap-allocate with WA_DeleteOnClose so the menu outlives this lambda.
        // Use popup() not exec() — exec() creates a nested event loop which can
        // allow network/connection events to be processed out of order while the
        // menu is open. popup() is non-blocking and safe during connection setup.
        QMenu* menu = new QMenu(this);
        menu->setAttribute(Qt::WA_DeleteOnClose);
        QAction* blinkAction = menu->addAction("Blink status indicator");
        blinkAction->setCheckable(true);
        blinkAction->setChecked(m_blinkEnabled);
        connect(blinkAction, &QAction::triggered, this, [this](bool checked) {
            setBlinkEnabled(checked);
        });
        menu->popup(m_heartbeat->mapToGlobal(pos));
    });

    // On Linux/Windows the menu bar occupies the left side, so add a stretch
    // to center the app name. On macOS the menu is at the OS level, so the
    // app name stays flush left.
#ifndef Q_OS_MAC
    // Linux/Windows: center the identity cluster (menu bar is on the left)
    m_hbox->addStretch(1);
    m_hbox->addWidget(m_heartbeat);
    m_hbox->addSpacing(4);
#endif

    auto* appName = new QLabel("AetherSDR");
    appName->setStyleSheet("QLabel { color: #00b4d8; font-size: 14px; font-weight: bold; }");
    appName->setAlignment(Qt::AlignCenter);
    m_hbox->addWidget(appName);

    m_mfBtn = new QPushButton("multiFLEX");
    m_mfBtn->setFlat(true);
    m_mfBtn->setStyleSheet(
        "QPushButton { color: #20c060; font-size: 11px; font-weight: bold; "
        "border: 1px solid #20c060; border-radius: 4px; "
        "background: transparent; padding: 0px 3px; }"
        "QPushButton:hover { background: rgba(32, 192, 96, 30); }");
    m_mfBtn->setVisible(false);
    m_mfBtn->setCursor(Qt::PointingHandCursor);
    m_mfBtn->setAccessibleName("multiFLEX status");
    connect(m_mfBtn, &QPushButton::clicked, this, &TitleBar::multiFlexClicked);
    m_hbox->addWidget(m_mfBtn);

#ifdef Q_OS_MAC
    // macOS: heartbeat after multiFLEX (left-aligned, no menu bar in title)
    m_hbox->addSpacing(4);
    m_hbox->addWidget(m_heartbeat);
#endif

    m_hbox->addStretch(1);

    // ── Right: Other client TX indicator + PC Audio + Master Vol + HP Vol ──
    m_otherTxLabel = new QLabel();
    m_otherTxLabel->setStyleSheet(
        "QLabel { background: white; color: #cc0000; font-size: 12px; "
        "font-weight: bold; border-radius: 3px; padding: 2px 8px; }");
    m_otherTxLabel->setVisible(false);
    m_hbox->addWidget(m_otherTxLabel);

    // ── PC Audio + Master Vol + HP Vol ──────────────────────────────────────
    auto& s = AppSettings::instance();

    // PC Audio toggle
    m_pcBtn = new QPushButton("PC Audio");
    m_pcBtn->setCheckable(true);
    m_pcBtn->setFixedHeight(22);
    m_pcBtn->setFixedWidth(70);

    bool pcOn = s.value("PcAudioEnabled", "True").toString() == "True";
    m_pcBtn->setChecked(pcOn);
    m_pcBtn->setAccessibleName("PC Audio");
    m_pcBtn->setAccessibleDescription("Toggle PC audio input for microphone");

    auto updatePcStyle = [this]() {
        m_pcBtn->setStyleSheet(m_pcBtn->isChecked()
            ? "QPushButton { background: #1a6030; color: #40ff80; border: 1px solid #20a040; "
              "border-radius: 3px; font-size: 10px; font-weight: bold; }"
              "QPushButton:hover { background: #207040; }"
            : "QPushButton { background: #1a2a3a; color: #607080; border: 1px solid #304050; "
              "border-radius: 3px; font-size: 10px; font-weight: bold; }"
              "QPushButton:hover { background: #243848; }");
    };
    updatePcStyle();

    connect(m_pcBtn, &QPushButton::toggled, this, [this, updatePcStyle](bool on) {
        updatePcStyle();
        auto& ss = AppSettings::instance();
        ss.setValue("PcAudioEnabled", on ? "True" : "False");
        ss.save();
        emit pcAudioToggled(on);
    });
    m_hbox->addWidget(m_pcBtn);

    m_hbox->addSpacing(8);

    // Master volume (click icon to mute/unmute)
    m_speakerBtn = new QPushButton("\xF0\x9F\x94\x8A");  // 🔊
    m_speakerBtn->setFixedSize(20, 20);
    m_speakerBtn->setCheckable(true);
    m_speakerBtn->setStyleSheet(
        "QPushButton { background: transparent; border: none; font-size: 14px; padding: 0; }"
        "QPushButton:checked { opacity: 0.4; }");
    m_speakerBtn->setToolTip("Click to mute/unmute line out");
    m_speakerBtn->setAccessibleName("Line out mute");
    m_speakerBtn->setAccessibleDescription("Mute or unmute line out speaker audio");
    connect(m_speakerBtn, &QPushButton::toggled, this, [this](bool muted) {
        m_speakerBtn->setText(muted ? "\xF0\x9F\x94\x87" : "\xF0\x9F\x94\x8A");  // 🔇 / 🔊
        emit lineoutMuteChanged(muted);
    });
    m_hbox->addWidget(m_speakerBtn);

    m_masterSlider = new GuardedSlider(Qt::Horizontal);
    m_masterSlider->setRange(0, 100);
    int savedVol = s.value("MasterVolume", "100").toInt();
    m_masterSlider->setValue(savedVol);
    m_masterSlider->setFixedWidth(80);
    m_masterSlider->setFixedHeight(16);
    m_masterSlider->setAccessibleName("Master volume");
    m_masterSlider->setAccessibleDescription("Line out volume level, 0 to 100 percent");
    m_masterSlider->setStyleSheet(
        "QSlider::groove:horizontal { background: #1a2a3a; height: 4px; border-radius: 2px; }"
        "QSlider::handle:horizontal { background: #00b4d8; width: 10px; margin: -3px 0; border-radius: 5px; }"
        "QSlider::sub-page:horizontal { background: #00b4d8; border-radius: 2px; }");
    m_hbox->addWidget(m_masterSlider);

    m_masterLabel = new QLabel(QString::number(savedVol));
    m_masterLabel->setFixedWidth(22);
    m_masterLabel->setStyleSheet("QLabel { color: #8aa8c0; font-size: 10px; }");
    m_masterLabel->setAlignment(Qt::AlignCenter);
    m_hbox->addWidget(m_masterLabel);

    connect(m_masterSlider, &QSlider::valueChanged, this, [this](int v) {
        m_masterLabel->setText(QString::number(v));
        emit masterVolumeChanged(v);
    });

    m_hbox->addSpacing(8);

    // Headphone volume (click icon to mute/unmute)
    m_headphoneBtn = new QPushButton("\xF0\x9F\x8E\xA7");  // 🎧
    m_headphoneBtn->setFixedSize(20, 20);
    m_headphoneBtn->setCheckable(true);
    m_headphoneBtn->setStyleSheet(
        "QPushButton { background: transparent; border: none; font-size: 14px; padding: 0; }"
        "QPushButton:checked { opacity: 0.4; }");
    m_headphoneBtn->setToolTip("Click to mute/unmute headphones");
    m_headphoneBtn->setAccessibleName("Headphone mute");
    m_headphoneBtn->setAccessibleDescription("Mute or unmute headphone audio");
    connect(m_headphoneBtn, &QPushButton::toggled, this, [this](bool muted) {
        m_headphoneBtn->setText(muted ? "\xF0\x9F\x94\x87" : "\xF0\x9F\x8E\xA7");  // 🔇 / 🎧
        emit headphoneMuteChanged(muted);
    });
    m_hbox->addWidget(m_headphoneBtn);

    m_hpSlider = new GuardedSlider(Qt::Horizontal);
    m_hpSlider->setRange(0, 100);
    m_hpSlider->setValue(50);
    m_hpSlider->setFixedWidth(80);
    m_hpSlider->setFixedHeight(16);
    m_hpSlider->setAccessibleName("Headphone volume");
    m_hpSlider->setAccessibleDescription("Headphone volume level, 0 to 100 percent");
    m_hpSlider->setStyleSheet(
        "QSlider::groove:horizontal { background: #1a2a3a; height: 4px; border-radius: 2px; }"
        "QSlider::handle:horizontal { background: #00b4d8; width: 10px; margin: -3px 0; border-radius: 5px; }"
        "QSlider::sub-page:horizontal { background: #00b4d8; border-radius: 2px; }");
    m_hbox->addWidget(m_hpSlider);

    m_hpLabel = new QLabel("50");
    m_hpLabel->setFixedWidth(22);
    m_hpLabel->setStyleSheet("QLabel { color: #8aa8c0; font-size: 10px; }");
    m_hpLabel->setAlignment(Qt::AlignCenter);
    m_hbox->addWidget(m_hpLabel);

    connect(m_hpSlider, &QSlider::valueChanged, this, [this](int v) {
        m_hpLabel->setText(QString::number(v));
        emit headphoneVolumeChanged(v);
    });

    m_hbox->addSpacing(12);

    // ── Minimal Mode button ─────────────────────────────────────────────────
    m_minimalBtn = new QPushButton("\xe2\x86\x99");  // ↙ U+2199
    m_minimalBtn->setFixedSize(28, 28);
    m_minimalBtn->setToolTip("Minimal Mode (Ctrl+M)");
    m_minimalBtn->setAccessibleName("Minimal mode");
    m_minimalBtn->setStyleSheet(
        "QPushButton { background: #1a2a3a; color: #c8d8e8; border: 1px solid #304050; "
        "border-radius: 4px; font-size: 18px; padding: 0; }"
        "QPushButton:hover { background: #203040; color: #00b4d8; border-color: #00b4d8; }");
    connect(m_minimalBtn, &QPushButton::clicked, this, &TitleBar::minimalModeRequested);
    m_hbox->addWidget(m_minimalBtn);
}

void TitleBar::setMenuBar(QMenuBar* mb)
{
    if (!mb) return;
    mb->setStyleSheet(
        "QMenuBar { background: transparent; color: #8aa8c0; font-size: 12px; }"
        "QMenuBar::item { padding: 4px 8px; }"
        "QMenuBar::item:selected { background: #203040; color: #ffffff; }"
        "QMenu { background: #0f0f1a; color: #c8d8e8; border: 1px solid #304050; }"
        "QMenu::item:selected { background: #0070c0; }");
    mb->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
    m_menuBar = mb;
    // Insert at position 0 (before the first stretch)
    m_hbox->insertWidget(0, mb);
}

void TitleBar::setPcAudioEnabled(bool on)
{
    QSignalBlocker b(m_pcBtn);
    m_pcBtn->setChecked(on);
    m_pcBtn->setStyleSheet(on
        ? "QPushButton { background: #1a6030; color: #40ff80; border: 1px solid #20a040; "
          "border-radius: 3px; font-size: 10px; font-weight: bold; }"
          "QPushButton:hover { background: #207040; }"
        : "QPushButton { background: #1a2a3a; color: #607080; border: 1px solid #304050; "
          "border-radius: 3px; font-size: 10px; font-weight: bold; }"
          "QPushButton:hover { background: #243848; }");
}

void TitleBar::setLineoutMuted(bool muted)
{
    QSignalBlocker b(m_speakerBtn);
    m_speakerBtn->setChecked(muted);
    m_speakerBtn->setText(muted ? "\xF0\x9F\x94\x87" : "\xF0\x9F\x94\x8A");  // 🔇 / 🔊
}

void TitleBar::setMasterVolume(int pct)
{
    // Don't fight the user: ignore external sync (e.g. delayed lineout_gain
    // status echoes) while the slider is being dragged.
    if (m_masterSlider->isSliderDown()) {
        return;
    }
    QSignalBlocker b(m_masterSlider);
    m_masterSlider->setValue(pct);
    m_masterLabel->setText(QString::number(pct));
}

void TitleBar::setHeadphoneVolume(int pct)
{
    QSignalBlocker b(m_hpSlider);
    m_hpSlider->setValue(pct);
    m_hpLabel->setText(QString::number(pct));
}

void TitleBar::setMultiFlexStatus(int count, const QStringList& names)
{
    if (count > 0) {
        m_mfBtn->setVisible(true);
        QString tip = QString("multiFLEX — %1 other client%2:\n")
            .arg(count).arg(count > 1 ? "s" : "");
        for (const QString& n : names)
            tip += "  " + n + "\n";
        m_mfBtn->setToolTip(tip.trimmed());
    } else {
        m_mfBtn->setVisible(false);
    }
}

void TitleBar::setOtherClientTx(bool transmitting, const QString& station)
{
    if (transmitting && !station.isEmpty()) {
        m_otherTxLabel->setText(QString("TX %1").arg(station));
        m_otherTxLabel->setVisible(true);
    } else {
        m_otherTxLabel->setVisible(false);
    }
}

void TitleBar::setDiscovering(bool active)
{
    m_discovering = active;
    if (active) {
        // Solid amber — discovery in progress, no connection yet
        m_heartbeatOffTimer->stop();
        m_heartbeatAlarmTimer->stop();
        m_heartbeat->setStyleSheet(
            "QLabel { background: #e0a020; border-radius: 5px; }");
        m_heartbeat->setToolTip("Searching for radio…");
    } else {
        m_heartbeat->setToolTip("Radio discovery heartbeat");
        // Return to idle gray — onHeartbeat() will take over once pings arrive
        m_heartbeat->setStyleSheet(
            "QLabel { background: #404858; border-radius: 5px; }");
    }
}

void TitleBar::onHeartbeat()
{
    m_discovering = false;
    m_missedBeats = 0;
    m_heartbeatAlarmTimer->stop();
    m_alarmRed = false;
    m_heartbeat->setToolTip("Radio discovery heartbeat");
    m_heartbeat->setStyleSheet(
        "QLabel { background: #20c060; border-radius: 5px; }");
    if (m_blinkEnabled) {
        m_heartbeatOffTimer->start();  // flash green → gray after 100ms
    }
    // When blink is off: stays static green — no timer, no animation
}

void TitleBar::onHeartbeatLost()
{
    m_missedBeats++;
    if (m_missedBeats >= 3 && !m_heartbeatAlarmTimer->isActive()) {
        m_heartbeatOffTimer->stop();
        if (m_blinkEnabled) {
            m_heartbeatAlarmTimer->start();  // blinking red ↔ gray every 500ms
        } else {
            // Static red — alarm timer is NEVER started.
            // Indicator stays red until the next successful ping restores connection.
            // This is intentional and safety-critical: contest operators must see
            // connection loss clearly, even with blink disabled.
            m_alarmRed = true;
            m_heartbeat->setStyleSheet(
                "QLabel { background: #cc2020; border-radius: 5px; }");
        }
    }
}

void TitleBar::setBlinkEnabled(bool enabled)
{
    if (m_blinkEnabled == enabled) return;
    m_blinkEnabled = enabled;
    AppSettings::instance().setValue("HeartbeatBlinkEnabled", enabled ? "True" : "False");
    AppSettings::instance().save();
    emit blinkEnabledChanged(enabled);

    if (enabled) {
        // Resume alarm blink immediately if currently in alarm state (m_missedBeats >= 3).
        // Without this, re-enabling blink while connection is lost leaves the indicator
        // static red until the next onHeartbeatLost() call increments the counter again.
        if (m_missedBeats >= 3 && !m_heartbeatAlarmTimer->isActive()) {
            m_heartbeatAlarmTimer->start();
        }
        return;
    }

    // Immediately reconcile mid-session: stop any active animation and freeze state
    if (m_heartbeatAlarmTimer->isActive()) {
        // Was blinking red — freeze to solid red (connection lost)
        m_heartbeatAlarmTimer->stop();
        m_alarmRed = true;
        m_heartbeat->setStyleSheet(
            "QLabel { background: #cc2020; border-radius: 5px; }");
    } else if (m_heartbeatOffTimer->isActive()) {
        // Was mid green-flash — freeze to solid green (connected)
        m_heartbeatOffTimer->stop();
        m_heartbeat->setStyleSheet(
            "QLabel { background: #20c060; border-radius: 5px; }");
    }
}

void TitleBar::setMinimalMode(bool on)
{
    // Hide everything except heartbeat, logo, ↗/↙ button, and 💡
    if (m_menuBar) m_menuBar->setVisible(!on);
    m_pcBtn->setVisible(!on);
    m_speakerBtn->setVisible(!on);
    m_headphoneBtn->setVisible(!on);
    m_masterSlider->setVisible(!on);
    m_hpSlider->setVisible(!on);
    m_masterLabel->setVisible(!on);
    m_hpLabel->setVisible(!on);
    // Don't touch m_otherTxLabel or m_mfBtn — their visibility is
    // managed by setOtherClientTx() and setMultiFlexStatus() respectively

    // Swap icon and tooltip
    if (m_minimalBtn) {
        m_minimalBtn->setText(on ? "\xe2\x86\x97" : "\xe2\x86\x99");  // ↗ or ↙
        m_minimalBtn->setToolTip(on ? "Restore Full Mode (Ctrl+M)" : "Minimal Mode (Ctrl+M)");
    }
}

} // namespace AetherSDR
