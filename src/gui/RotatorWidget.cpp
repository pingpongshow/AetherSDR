#include "RotatorWidget.h"
#include "GuardedSlider.h"
#include "core/GeoUtils.h"
#include "core/RotatorClient.h"
#include "core/RotatorSettings.h"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <cmath>
#include <functional>
#include <memory>
#include <numbers>
#include <optional>

namespace AetherSDR {

// ── Styling constants (same palette as AntennaGeniusApplet) ─────────────────

static constexpr const char* kButtonBase =
    "QPushButton { background: #1a2a3a; border: 1px solid #203040; "
    "border-radius: 3px; padding: 2px 2px; font-size: 10px; color: #c8d8e8; }"
    "QPushButton:hover { background: #243848; }"
    "QPushButton:disabled { color: #506070; }";

static constexpr const char* kGreenActive =
    "QPushButton:checked { background-color: #006040; color: #00ff88; "
    "border: 1px solid #00a060; }";

static constexpr const char* kLabelStyle =
    "color: #8090a0; font-size: 10px; font-weight: bold;";

static constexpr const char* kValueStyle =
    "color: #00b4d8; font-size: 11px; font-weight: bold;";

static constexpr const char* kEditStyle =
    "QLineEdit { background: #1a2a3a; border: 1px solid #203040; "
    "border-radius: 3px; padding: 2px 4px; color: #c8d8e8; font-size: 10px; }";

static constexpr const char* kComboStyle =
    "QComboBox { background: #1a2a3a; border: 1px solid #203040; "
    "border-radius: 3px; padding: 2px 4px; color: #c8d8e8; font-size: 10px; }"
    "QComboBox::drop-down { border: none; }"
    "QComboBox QAbstractItemView { background: #1a2a3a; color: #c8d8e8; "
    "selection-background-color: #0070c0; }";

static const QColor kRoseBg(0x0a, 0x0a, 0x18);       // HGauge bar background
static const QColor kRoseRing(0x20, 0x30, 0x40);
static const QColor kRoseTick(0x80, 0x90, 0xa0);
static const QColor kRoseText(0xc8, 0xd8, 0xe8);
static const QColor kHeadingColor(0x00, 0xb4, 0xd8);
static const QColor kTargetColor(0xff, 0xe0, 0x80);  // kAmberActive text

// ── CompassRose ─────────────────────────────────────────────────────────────

class CompassRose : public QWidget {
public:
    explicit CompassRose(QWidget* parent = nullptr) : QWidget(parent)
    {
        setMinimumSize(120, 120);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setFixedHeight(140);
        setCursor(Qt::CrossCursor);
        setToolTip(QStringLiteral("Click to turn the rotator to that bearing"));
    }

    void setHeading(std::optional<double> deg) { m_heading = deg; update(); }
    void setTarget(std::optional<double> deg)  { m_target = deg; update(); }
    void setOnline(bool on)                    { m_online = on; update(); }

    std::function<void(double)> onBearingClicked;

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        const double side = std::min(width(), height()) - 4.0;
        const QPointF c(width() / 2.0, height() / 2.0);
        const double r = side / 2.0;

        p.setPen(QPen(kRoseRing, 1.5));
        p.setBrush(kRoseBg);
        p.drawEllipse(c, r, r);

        // Ticks every 10°, long every 30°
        for (int deg = 0; deg < 360; deg += 10) {
            const bool major = (deg % 30) == 0;
            const double a = (deg - 90.0) * std::numbers::pi / 180.0;
            const double r0 = r - (major ? 9.0 : 5.0);
            p.setPen(QPen(kRoseTick, major ? 1.2 : 0.8));
            p.drawLine(QPointF(c.x() + r0 * std::cos(a), c.y() + r0 * std::sin(a)),
                       QPointF(c.x() + (r - 1.5) * std::cos(a), c.y() + (r - 1.5) * std::sin(a)));
        }

        QFont f = font();
        f.setPixelSize(10);
        f.setBold(true);
        p.setFont(f);
        p.setPen(kRoseText);
        const char* cardinals[] = {"N", "E", "S", "W"};
        for (int i = 0; i < 4; ++i) {
            const double a = (i * 90.0 - 90.0) * std::numbers::pi / 180.0;
            const double rl = r - 18.0;
            const QPointF pt(c.x() + rl * std::cos(a), c.y() + rl * std::sin(a));
            p.drawText(QRectF(pt.x() - 8, pt.y() - 7, 16, 14), Qt::AlignCenter,
                       QString::fromLatin1(cardinals[i]));
        }

        auto drawPointer = [&](double deg, const QColor& col, double len, double width, bool dashed) {
            const double a = (deg - 90.0) * std::numbers::pi / 180.0;
            QPen pen(col, width);
            pen.setCapStyle(Qt::RoundCap);
            if (dashed) {
                pen.setStyle(Qt::DashLine);
            }
            p.setPen(pen);
            p.drawLine(c, QPointF(c.x() + len * std::cos(a), c.y() + len * std::sin(a)));
        };

        if (m_target) {
            drawPointer(*m_target, kTargetColor, r - 4.0, 1.5, true);
        }
        if (m_heading) {
            drawPointer(*m_heading, m_online ? kHeadingColor : kRoseTick, r - 12.0, 3.0, false);
        }
        p.setPen(Qt::NoPen);
        p.setBrush(m_online ? kHeadingColor : kRoseTick);
        p.drawEllipse(c, 3.0, 3.0);

        // Numeric heading in the lower half
        f.setPixelSize(12);
        p.setFont(f);
        p.setPen(m_online ? kHeadingColor : kRoseTick);
        const QString txt = m_heading ? QStringLiteral("%1°").arg(std::lround(*m_heading) % 360, 3, 10, QLatin1Char('0'))
                                      : QStringLiteral("---°");
        p.drawText(QRectF(c.x() - 30, c.y() + r * 0.30, 60, 16), Qt::AlignCenter, txt);
    }

    void mousePressEvent(QMouseEvent* ev) override
    {
        if (ev->button() != Qt::LeftButton || !onBearingClicked) {
            QWidget::mousePressEvent(ev);
            return;
        }
        const QPointF c(width() / 2.0, height() / 2.0);
        const QPointF d = ev->position() - c;
        if (std::hypot(d.x(), d.y()) < 8.0) {
            return;   // too close to the centre to mean anything
        }
        const double deg = std::atan2(d.x(), -d.y()) * 180.0 / std::numbers::pi;
        onBearingClicked(GeoUtils::normalizeBearing(std::round(deg)));
        ev->accept();
    }

private:
    std::optional<double> m_heading;
    std::optional<double> m_target;
    bool m_online{false};
};

// ── RotatorWidget ───────────────────────────────────────────────────────────

namespace {

QString presetLabel(double deg)
{
    static const char* names[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    const long r = std::lround(deg);
    if (r % 45 == 0 && std::abs(deg - r) < 0.01) {
        return QString::fromLatin1(names[(r / 45) % 8]);
    }
    return QString::number(r);
}

QPushButton* makeButton(const QString& text, const QString& extraStyle = {})
{
    auto* b = new QPushButton(text);
    b->setStyleSheet(QString(kButtonBase) + extraStyle);
    b->setFixedHeight(22);
    b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    b->setMinimumWidth(0);
    return b;
}

QLineEdit* makeEdit(const QString& placeholder)
{
    auto* e = new QLineEdit;
    e->setStyleSheet(kEditStyle);
    e->setPlaceholderText(placeholder);
    e->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    e->setMinimumWidth(0);
    return e;
}

QLabel* makeLabel(const QString& text)
{
    auto* l = new QLabel(text);
    l->setStyleSheet(kLabelStyle);
    return l;
}

} // namespace

RotatorWidget::RotatorWidget(QWidget* parent)
    : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    setMaximumWidth(260);
    buildUI();
}

void RotatorWidget::buildUI()
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto* body = new QWidget;
    auto* vbox = new QVBoxLayout(body);
    vbox->setContentsMargins(4, 4, 4, 4);
    vbox->setSpacing(4);

    // ── Status row: [status text ............] [ON] ─────────────────────────
    {
        auto* row = new QHBoxLayout;
        row->setSpacing(4);
        m_statusLabel = new QLabel(QStringLiteral("Rotator disabled"));
        m_statusLabel->setStyleSheet("color: #606878; font-size: 10px;");
        m_statusLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        row->addWidget(m_statusLabel, 1);

        m_enableBtn = makeButton(QStringLiteral("ON"), kGreenActive);
        m_enableBtn->setCheckable(true);
        m_enableBtn->setFixedWidth(40);
        m_enableBtn->setToolTip(QStringLiteral("Enable rotator control"));
        row->addWidget(m_enableBtn);
        vbox->addLayout(row);

        connect(m_enableBtn, &QPushButton::toggled, this, &RotatorWidget::setEnabledFromUi);
    }

    // ── Compass rose ───────────────────────────────────────────────────────
    m_rose = new CompassRose;
    m_rose->onBearingClicked = [this](double deg) {
        if (m_client) {
            m_client->turnTo(deg);
        }
    };
    vbox->addWidget(m_rose);

    // Heading / target readout
    m_headingLabel = new QLabel(QStringLiteral("Heading ---°"));
    m_headingLabel->setStyleSheet(kValueStyle);
    m_headingLabel->setAlignment(Qt::AlignCenter);
    vbox->addWidget(m_headingLabel);

    // ── Turn to: [call / grid / deg] [SP] [LP] ─────────────────────────────
    {
        auto* row = new QHBoxLayout;
        row->setSpacing(4);
        m_turnEdit = makeEdit(QStringLiteral("Call, grid or °"));
        m_turnEdit->setToolTip(QStringLiteral("Callsign (cty.dat), Maidenhead grid (FN31, IO91wm) "
                                              "or bearing in degrees. Enter = short path."));
        row->addWidget(m_turnEdit, 1);
        m_spBtn = makeButton(QStringLiteral("SP"));
        m_spBtn->setFixedWidth(32);
        m_spBtn->setToolTip(QStringLiteral("Turn to short-path bearing"));
        m_lpBtn = makeButton(QStringLiteral("LP"));
        m_lpBtn->setFixedWidth(32);
        m_lpBtn->setToolTip(QStringLiteral("Turn to long-path bearing (short path + 180°)"));
        row->addWidget(m_spBtn);
        row->addWidget(m_lpBtn);
        vbox->addLayout(row);

        m_pathLabel = new QLabel;
        m_pathLabel->setStyleSheet("color: #8090a0; font-size: 10px;");
        m_pathLabel->setWordWrap(true);
        m_pathLabel->setAlignment(Qt::AlignCenter);
        m_pathLabel->hide();
        vbox->addWidget(m_pathLabel);

        connect(m_turnEdit, &QLineEdit::returnPressed, this, [this]() { turnFromEntry(false); });
        connect(m_turnEdit, &QLineEdit::textChanged, this, &RotatorWidget::refreshTargetInfo);
        connect(m_spBtn, &QPushButton::clicked, this, [this]() { turnFromEntry(false); });
        connect(m_lpBtn, &QPushButton::clicked, this, [this]() { turnFromEntry(true); });
    }

    // ── Presets ────────────────────────────────────────────────────────────
    m_presetRow = new QWidget;
    m_presetLayout = new QHBoxLayout(m_presetRow);
    m_presetLayout->setContentsMargins(0, 0, 0, 0);
    m_presetLayout->setSpacing(2);
    vbox->addWidget(m_presetRow);
    rebuildPresetButtons();

    // ── STOP / PARK ────────────────────────────────────────────────────────
    {
        auto* row = new QHBoxLayout;
        row->setSpacing(4);
        m_stopBtn = makeButton(QStringLiteral("STOP"),
                               QStringLiteral("QPushButton { font-weight: bold; }"));
        m_parkBtn = makeButton(QStringLiteral("PARK"),
                               QStringLiteral("QPushButton { font-weight: bold; }"));
        row->addWidget(m_stopBtn);
        row->addWidget(m_parkBtn);
        vbox->addLayout(row);

        connect(m_stopBtn, &QPushButton::clicked, this, [this]() {
            if (m_client) {
                m_client->stopRotation();
            }
        });
        connect(m_parkBtn, &QPushButton::clicked, this, [this]() {
            if (m_client) {
                m_client->park();
            }
        });
    }

    // ── Settings (collapsible) ─────────────────────────────────────────────
    m_settingsToggle = makeButton(QStringLiteral("▸ Settings"));
    m_settingsToggle->setCheckable(true);
    m_settingsToggle->setStyleSheet(QString(kButtonBase)
        + "QPushButton { text-align: left; padding-left: 4px; }"
          "QPushButton:checked { background: #1a2a3a; }");
    vbox->addWidget(m_settingsToggle);

    m_settingsBody = buildSettingsSection();
    m_settingsBody->hide();
    vbox->addWidget(m_settingsBody);
    connect(m_settingsToggle, &QPushButton::toggled, this, [this](bool open) {
        m_settingsToggle->setText(open ? QStringLiteral("▾ Settings")
                                       : QStringLiteral("▸ Settings"));
        m_settingsBody->setVisible(open);
        if (open) {
            loadSettingsIntoForm();
        }
    });

    outer->addWidget(body);
    refreshStatus();
}

QWidget* RotatorWidget::buildSettingsSection()
{
    auto* w = new QWidget;
    auto* grid = new QGridLayout(w);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(4);
    grid->setVerticalSpacing(3);

    int row = 0;
    grid->addWidget(makeLabel(QStringLiteral("Backend:")), row, 0);
    m_backendCombo = new GuardedComboBox;
    m_backendCombo->setStyleSheet(kComboStyle);
    m_backendCombo->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    m_backendCombo->addItem(QStringLiteral("Hamlib rotctld (TCP)"),
                            RotatorConfig::backendToString(RotatorConfig::Backend::Rotctld));
    m_backendCombo->addItem(QStringLiteral("PstRotator / ARSWIN (UDP)"),
                            RotatorConfig::backendToString(RotatorConfig::Backend::PstRotator));
    m_backendCombo->addItem(QStringLiteral("N1MM rotor (UDP)"),
                            RotatorConfig::backendToString(RotatorConfig::Backend::N1mm));
    grid->addWidget(m_backendCombo, row++, 1, 1, 3);

    grid->addWidget(makeLabel(QStringLiteral("Host:")), row, 0);
    m_hostEdit = makeEdit(QStringLiteral("127.0.0.1"));
    grid->addWidget(m_hostEdit, row, 1);
    grid->addWidget(makeLabel(QStringLiteral("Port:")), row, 2);
    m_portEdit = makeEdit(QStringLiteral("4533"));
    m_portEdit->setValidator(new QIntValidator(1, 65535, m_portEdit));
    m_portEdit->setFixedWidth(52);
    grid->addWidget(m_portEdit, row++, 3);

    grid->addWidget(makeLabel(QStringLiteral("Poll ms:")), row, 0);
    m_pollEdit = makeEdit(QStringLiteral("1000"));
    m_pollEdit->setValidator(new QIntValidator(100, 60000, m_pollEdit));
    grid->addWidget(m_pollEdit, row, 1);
    grid->addWidget(makeLabel(QStringLiteral("N1MM:")), row, 2);
    m_n1mmNameEdit = makeEdit(QStringLiteral("Rotor"));
    m_n1mmNameEdit->setToolTip(QStringLiteral("N1MM rotor name (N1MM backend only)"));
    m_n1mmNameEdit->setFixedWidth(52);
    grid->addWidget(m_n1mmNameEdit, row++, 3);

    grid->addWidget(makeLabel(QStringLiteral("Min az:")), row, 0);
    m_minAzEdit = makeEdit(QStringLiteral("0"));
    m_minAzEdit->setValidator(new QIntValidator(-360, 720, m_minAzEdit));
    m_minAzEdit->setToolTip(QStringLiteral("Mechanical range, e.g. 0–450 for an overlap "
                                           "rotator or -180–180 for south-centred stops"));
    grid->addWidget(m_minAzEdit, row, 1);
    grid->addWidget(makeLabel(QStringLiteral("Max az:")), row, 2);
    m_maxAzEdit = makeEdit(QStringLiteral("360"));
    m_maxAzEdit->setValidator(new QIntValidator(-360, 720, m_maxAzEdit));
    m_maxAzEdit->setFixedWidth(52);
    grid->addWidget(m_maxAzEdit, row++, 3);

    grid->addWidget(makeLabel(QStringLiteral("Presets:")), row, 0);
    m_presetsEdit = makeEdit(QStringLiteral("0,45,90,135,180,225,270,315"));
    grid->addWidget(m_presetsEdit, row++, 1, 1, 3);

    auto* applyBtn = makeButton(QStringLiteral("Apply"),
                                QStringLiteral("QPushButton { font-weight: bold; }"));
    grid->addWidget(applyBtn, row++, 0, 1, 4);

    grid->setColumnStretch(1, 1);

    connect(m_backendCombo, &QComboBox::currentIndexChanged, this, [this](int) {
        // Swap the port to the new backend's default if the user hadn't
        // customised it away from the old default.
        const RotatorConfig::Backend b = RotatorConfig::backendFromString(
            m_backendCombo->currentData().toString());
        const int cur = m_portEdit->text().toInt();
        const bool wasDefault = m_portEdit->text().isEmpty()
            || cur == RotatorConfig::defaultPort(RotatorConfig::Backend::Rotctld)
            || cur == RotatorConfig::defaultPort(RotatorConfig::Backend::PstRotator)
            || cur == RotatorConfig::defaultPort(RotatorConfig::Backend::N1mm);
        if (wasDefault) {
            m_portEdit->setText(QString::number(RotatorConfig::defaultPort(b)));
        }
        m_n1mmNameEdit->setEnabled(b == RotatorConfig::Backend::N1mm);
    });
    connect(applyBtn, &QPushButton::clicked, this, &RotatorWidget::applySettingsFromForm);
    return w;
}

void RotatorWidget::loadSettingsIntoForm()
{
    const RotatorConfig c = RotatorSettings::load();
    {
        const QSignalBlocker b(m_backendCombo);
        const int idx = m_backendCombo->findData(RotatorConfig::backendToString(c.backend));
        m_backendCombo->setCurrentIndex(std::max(0, idx));
    }
    m_hostEdit->setText(c.host);
    m_portEdit->setText(QString::number(c.port));
    m_pollEdit->setText(QString::number(c.pollIntervalMs));
    m_minAzEdit->setText(QString::number(c.minAz, 'f', 0));
    m_maxAzEdit->setText(QString::number(c.maxAz, 'f', 0));
    m_n1mmNameEdit->setText(c.n1mmRotorName);
    m_n1mmNameEdit->setEnabled(c.backend == RotatorConfig::Backend::N1mm);

    QStringList parts;
    for (double p : RotatorSettings::presets()) {
        parts.append(QString::number(p, 'f', 0));
    }
    m_presetsEdit->setText(parts.join(QLatin1Char(',')));
}

void RotatorWidget::applySettingsFromForm()
{
    RotatorConfig c = RotatorSettings::load();
    c.backend = RotatorConfig::backendFromString(m_backendCombo->currentData().toString());
    c.host = m_hostEdit->text().trimmed().isEmpty() ? QStringLiteral("127.0.0.1")
                                                   : m_hostEdit->text().trimmed();
    const int port = m_portEdit->text().toInt();
    c.port = static_cast<quint16>(port > 0 ? port : RotatorConfig::defaultPort(c.backend));
    c.pollIntervalMs = std::clamp(m_pollEdit->text().toInt(), 100, 60000);
    c.minAz = m_minAzEdit->text().toDouble();
    c.maxAz = m_maxAzEdit->text().toDouble();
    if (c.maxAz <= c.minAz) {
        c.minAz = 0.0;
        c.maxAz = 360.0;
    }
    c.n1mmRotorName = m_n1mmNameEdit->text().trimmed().isEmpty()
                          ? QStringLiteral("Rotor") : m_n1mmNameEdit->text().trimmed();
    RotatorSettings::save(c);

    QList<double> presets;
    for (const QString& tok : m_presetsEdit->text().split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        bool ok = false;
        const double v = tok.trimmed().toDouble(&ok);
        if (ok) {
            presets.append(v);
        }
    }
    RotatorSettings::setPresets(presets);
    rebuildPresetButtons();

    if (m_client) {
        m_client->setConfig(c);   // restarts the backend if running
    }
    loadSettingsIntoForm();
    refreshStatus();
}

void RotatorWidget::rebuildPresetButtons()
{
    // takeAt() hands ownership of the QLayoutItem back to us.
    while (std::unique_ptr<QLayoutItem> item{m_presetLayout->takeAt(0)}) {
        if (QWidget* w = item->widget()) {
            w->deleteLater();
        }
    }
    const QList<double> presets = RotatorSettings::presets();
    for (double deg : presets) {
        auto* b = makeButton(presetLabel(deg));
        b->setToolTip(QStringLiteral("Turn to %1°").arg(deg, 0, 'f', 0));
        connect(b, &QPushButton::clicked, this, [this, deg]() {
            if (m_client) {
                m_client->turnTo(deg);
            }
        });
        m_presetLayout->addWidget(b);
    }
    m_presetRow->setVisible(!presets.isEmpty());
}

void RotatorWidget::setRotatorClient(RotatorClient* client)
{
    if (m_client == client) {
        return;
    }
    m_client = client;
    if (!m_client) {
        return;
    }

    connect(m_client, &RotatorClient::stateChanged, this, [this]() {
        m_lastError.clear();
        refreshStatus();
    });
    connect(m_client, &RotatorClient::positionChanged, this, [this](double az, double) {
        m_rose->setHeading(az);
        refreshStatus();
    });
    connect(m_client, &RotatorClient::targetChanged, this, [this](double t) {
        m_lastError.clear();
        m_rose->setTarget(t);
        refreshStatus();
    });
    connect(m_client, &RotatorClient::targetReached, this, [this]() {
        m_rose->setTarget(std::nullopt);
        refreshStatus();
    });
    connect(m_client, &RotatorClient::targetCleared, this, [this]() {
        m_rose->setTarget(std::nullopt);
        refreshStatus();
    });
    connect(m_client, &RotatorClient::errorOccurred, this, [this](const QString& msg) {
        m_lastError = msg;
        refreshStatus();
    });
    connect(m_client, &RotatorClient::commandFailed, this,
            [this](const QString& cmd, int rprt, const QString& msg) {
        m_lastError = QStringLiteral("%1: %2 (RPRT %3)").arg(cmd.section(QLatin1Char(' '), 0, 0), msg)
                          .arg(rprt);
        refreshStatus();
    });
    refreshStatus();
}

void RotatorWidget::applySavedSettings()
{
    if (!m_client) {
        return;
    }
    m_client->setConfig(RotatorSettings::load());
    m_client->setStationGrid(RotatorSettings::stationGrid());
    const bool on = RotatorSettings::isEnabled();
    {
        const QSignalBlocker b(m_enableBtn);
        m_enableBtn->setChecked(on);
    }
    if (on) {
        m_client->start();
    }
    refreshStatus();
}

void RotatorWidget::setEnabledFromUi(bool on)
{
    RotatorSettings::setEnabled(on);
    if (!m_client) {
        return;
    }
    if (on) {
        m_client->setConfig(RotatorSettings::load());
        m_client->setStationGrid(RotatorSettings::stationGrid());
        m_client->start();
    } else {
        m_client->stop();
        m_rose->setHeading(std::nullopt);
    }
    refreshStatus();
}

void RotatorWidget::refreshStatus()
{
    const bool running = m_client && m_client->isRunning();
    const bool online = m_client && m_client->isConnected();

    m_rose->setOnline(online);
    for (QPushButton* b : {m_spBtn, m_lpBtn, m_stopBtn, m_parkBtn}) {
        b->setEnabled(online);
    }
    m_presetRow->setEnabled(online);

    QString text;
    QString color = QStringLiteral("#606878");
    if (!m_client || !running) {
        text = QStringLiteral("Rotator disabled");
    } else {
        const RotatorConfig& c = m_client->config();
        const QString where = QStringLiteral("%1 %2:%3")
            .arg(RotatorConfig::backendToString(c.backend), c.host).arg(c.port);
        switch (m_client->state()) {
        case RotatorClient::State::Connected:
            text = where;
            color = QStringLiteral("#00b4d8");
            break;
        case RotatorClient::State::Connecting:
            text = QStringLiteral("Connecting %1…").arg(where);
            break;
        case RotatorClient::State::Reconnecting:
            text = QStringLiteral("Reconnecting %1…").arg(where);
            color = QStringLiteral("#ff4444");
            break;
        case RotatorClient::State::Disabled:
            text = QStringLiteral("Rotator disabled");
            break;
        }
    }
    if (!m_lastError.isEmpty() && running) {
        text = m_lastError;
        color = QStringLiteral("#ff4444");
    }
    m_statusLabel->setText(text);
    m_statusLabel->setToolTip(text);
    m_statusLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 10px;").arg(color));

    QString heading = QStringLiteral("Heading ---°");
    if (m_client && m_client->hasPosition() && running) {
        heading = QStringLiteral("Heading %1°").arg(m_client->azimuth(), 0, 'f', 0);
        if (std::abs(m_client->rawAzimuth() - m_client->azimuth()) > 0.5) {
            heading += QStringLiteral(" (%1°)").arg(m_client->rawAzimuth(), 0, 'f', 0);   // in overlap
        }
    }
    if (m_client && m_client->hasTarget() && running) {
        heading += QStringLiteral("  → %1°").arg(m_client->targetAzimuth(), 0, 'f', 0);
    }
    m_headingLabel->setText(heading);
}

void RotatorWidget::refreshTargetInfo()
{
    const QString text = m_turnEdit->text().trimmed();
    if (text.isEmpty()) {
        m_pathLabel->hide();
        return;
    }
    const CtyDatParser* cty = m_client ? m_client->ctyParser() : nullptr;
    const GeoUtils::TurnTarget t = GeoUtils::parseTurnTarget(text, cty);
    QString info;
    switch (t.kind) {
    case GeoUtils::TurnTarget::Kind::Invalid:
        info = QStringLiteral("Unknown call / grid");
        break;
    case GeoUtils::TurnTarget::Kind::Bearing:
        info = QStringLiteral("SP %1° · LP %2°")
                   .arg(t.bearingDeg, 0, 'f', 0)
                   .arg(GeoUtils::normalizeBearing(t.bearingDeg + 180.0), 0, 'f', 0);
        break;
    case GeoUtils::TurnTarget::Kind::Grid:
    case GeoUtils::TurnTarget::Kind::Callsign: {
        const std::optional<LatLon> home = GeoUtils::gridToLatLon(RotatorSettings::stationGrid());
        if (!home) {
            info = QStringLiteral("%1 — set station grid in Radio Setup").arg(t.label);
            break;
        }
        const GreatCirclePath p = GeoUtils::pathBetween(*home, t.position);
        info = QStringLiteral("%1\nSP %2° %3 km · LP %4° %5 km")
                   .arg(t.label)
                   .arg(p.shortPathDeg, 0, 'f', 0)
                   .arg(p.shortPathKm, 0, 'f', 0)
                   .arg(p.longPathDeg, 0, 'f', 0)
                   .arg(p.longPathKm, 0, 'f', 0);
        break;
    }
    }
    m_pathLabel->setText(info);
    m_pathLabel->show();
}

void RotatorWidget::turnFromEntry(bool longPath)
{
    if (!m_client) {
        return;
    }
    m_client->setStationGrid(RotatorSettings::stationGrid());   // may have changed in Radio Setup
    const GeoUtils::TurnTarget t = GeoUtils::parseTurnTarget(m_turnEdit->text(), m_client->ctyParser());
    switch (t.kind) {
    case GeoUtils::TurnTarget::Kind::Invalid:
        m_lastError = QStringLiteral("Unknown call / grid: %1").arg(m_turnEdit->text().trimmed());
        refreshStatus();
        return;
    case GeoUtils::TurnTarget::Kind::Bearing:
        m_client->turnTo(longPath ? t.bearingDeg + 180.0 : t.bearingDeg);
        return;
    case GeoUtils::TurnTarget::Kind::Grid:
        m_client->turnToGrid(t.label, longPath);
        return;
    case GeoUtils::TurnTarget::Kind::Callsign:
        m_client->turnToCallsign(m_turnEdit->text().trimmed(), longPath);
        return;
    }
}

} // namespace AetherSDR
