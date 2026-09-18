#include "AudioRoutingDialog.h"
#include "ComboStyle.h"
#include "core/AppSettings.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QAudioDevice>
#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QLabel>
#include <QLayoutItem>
#include <QMediaDevices>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>

namespace AetherSDR {

namespace {

// Same dark palette as DigitalVoiceDialog / ProfileManagerDialog.
const QString kDialogStyle = QStringLiteral(
    "QDialog { background: #0f0f1a; color: #c8d8e8; }"
    "QLabel { color: #c8d8e8; }"
    "QPushButton { background: #1a2a3a; border: 1px solid #203040;"
    "  border-radius: 3px; padding: 4px 12px; color: #c8d8e8; }"
    "QPushButton:hover { background: #2a3a4a; }"
    "QCheckBox { color: #c8d8e8; }"
    "QCheckBox::indicator { width: 16px; height: 16px;"
    "  border: 1px solid #406080; border-radius: 3px; background: #0a0a18; }"
    "QCheckBox::indicator:checked { background: #00b4d8; }");
const QString kNoteStyle = QStringLiteral("QLabel { color: #6888a0; font-size: 11px; }");
const QString kWarnStyle = QStringLiteral("QLabel { color: #e0a040; font-size: 11px; }");

QChar sliceLetter(int sliceId)
{
    return QChar(QLatin1Char(static_cast<char>('A' + (sliceId % 26))));
}

} // namespace

QMap<QChar, SliceAudioRoute> AudioRoutingDialog::loadRoutes()
{
    QMap<QChar, SliceAudioRoute> routes;
    auto& s = AppSettings::instance();
    for (int id = 0; id < kMaxRoutedSlices; ++id) {
        const QChar letter = sliceLetter(id);
        const QString key = SliceAudioRoute::settingsKey(letter);
        if (s.contains(key)) {
            const SliceAudioRoute r = SliceAudioRoute::fromSetting(s.value(key).toString());
            if (!r.isDefault()) {
                routes.insert(letter, r);
            }
        }
    }
    return routes;
}

void AudioRoutingDialog::saveRoute(QChar sliceLetter, const SliceAudioRoute& route)
{
    auto& s = AppSettings::instance();
    const QString key = SliceAudioRoute::settingsKey(sliceLetter);
    if (route.isDefault()) {
        s.remove(key);
    } else {
        s.setValue(key, route.toSetting());
    }
    s.save();
}

AudioRoutingDialog::AudioRoutingDialog(RadioModel* model, QWidget* parent)
    : QDialog(parent)
    , m_model(model)
{
    setWindowTitle(QStringLiteral("Slice Audio Routing"));
    setMinimumWidth(560);
    setStyleSheet(kDialogStyle);

    auto* root = new QVBoxLayout(this);
    auto* note = new QLabel(QStringLiteral(
        "By default every slice plays through the main audio output (the radio's "
        "mixed remote audio stream). A slice can also play on another output device "
        "and/or a DAX channel. Separate routing uses the slice's own DAX channel, so "
        "the slice needs a DAX channel no other slice uses. Unticking \"Main\" mutes "
        "the slice on the radio while it plays elsewhere. Volume and pan follow the "
        "slice's radio controls."));
    note->setWordWrap(true);
    note->setStyleSheet(kNoteStyle);
    root->addWidget(note);

    m_grid = new QGridLayout;
    m_grid->setHorizontalSpacing(12);
    root->addLayout(m_grid);

    m_warnLabel = new QLabel;
    m_warnLabel->setWordWrap(true);
    m_warnLabel->setStyleSheet(kWarnStyle);
    m_warnLabel->hide();
    root->addWidget(m_warnLabel);

    auto* close = new QPushButton(QStringLiteral("Close"));
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    auto* btnRow = new QHBoxLayout;
    btnRow->addStretch();
    btnRow->addWidget(close);
    root->addLayout(btnRow);

    if (m_model) {
        connect(m_model, &RadioModel::sliceAdded, this, &AudioRoutingDialog::refresh);
        connect(m_model, &RadioModel::sliceRemoved, this, &AudioRoutingDialog::refresh);
    }
    rebuildRows();
}

void AudioRoutingDialog::showEvent(QShowEvent* ev)
{
    rebuildRows();   // devices may have changed while hidden
    QDialog::showEvent(ev);
}

void AudioRoutingDialog::refresh()
{
    if (isVisible()) {
        rebuildRows();
    }
}

void AudioRoutingDialog::setPlanWarnings(const QStringList& warnings)
{
    m_warnLabel->setText(warnings.join(QLatin1Char('\n')));
    m_warnLabel->setVisible(!warnings.isEmpty());
}

void AudioRoutingDialog::rebuildRows()
{
    while (QLayoutItem* item = m_grid->takeAt(0)) {
        if (QWidget* w = item->widget()) {
            w->deleteLater();
        }
        delete item;
    }

    const QStringList headers{QStringLiteral("Slice"), QStringLiteral("Main"),
                              QStringLiteral("Also play on device"), QStringLiteral("Also to DAX"),
                              QStringLiteral("Slice DAX")};
    for (int c = 0; c < headers.size(); ++c) {
        auto* h = new QLabel(headers[c]);
        h->setStyleSheet(kNoteStyle);
        m_grid->addWidget(h, 0, c);
    }

    QStringList devices;
    for (const QAudioDevice& d : QMediaDevices::audioOutputs()) {
        devices.append(d.description());
    }

    QList<SliceModel*> slices = m_model ? m_model->slices() : QList<SliceModel*>{};
    std::sort(slices.begin(), slices.end(),
              [](const SliceModel* a, const SliceModel* b) { return a->sliceId() < b->sliceId(); });
    const QMap<QChar, SliceAudioRoute> routes = loadRoutes();

    int row = 1;
    for (SliceModel* s : slices) {
        if (s->sliceId() < 0 || s->sliceId() >= kMaxRoutedSlices) {
            continue;
        }
        const QChar letter = sliceLetter(s->sliceId());
        const SliceAudioRoute route = routes.value(letter);

        auto* name = new QLabel(QString(letter));
        auto* mainCheck = new QCheckBox;
        mainCheck->setChecked(route.toMain);

        auto* devCombo = new QComboBox;
        applyComboStyle(devCombo);
        devCombo->addItem(QStringLiteral("(none)"), QString());
        for (const QString& d : devices) {
            devCombo->addItem(d, d);
        }
        if (!route.outputDevice.isEmpty()) {
            int idx = devCombo->findData(route.outputDevice);
            if (idx < 0) {   // saved device currently absent — keep the choice
                devCombo->addItem(route.outputDevice + QStringLiteral(" (not present)"),
                                  route.outputDevice);
                idx = devCombo->count() - 1;
            }
            devCombo->setCurrentIndex(idx);
        }

        auto* daxCombo = new QComboBox;
        applyComboStyle(daxCombo);
        daxCombo->addItem(QStringLiteral("(none)"), 0);
        for (int ch = 1; ch <= 4; ++ch) {
            daxCombo->addItem(QStringLiteral("DAX %1").arg(ch), ch);
        }
        daxCombo->setCurrentIndex(std::max(0, daxCombo->findData(route.daxChannel)));

        const int radioDax = s->daxChannel();
        auto* radioDaxLabel = new QLabel(radioDax >= 1 && radioDax <= 4
                                             ? QString::number(radioDax)
                                             : QStringLiteral("—"));

        auto save = [this, letter, mainCheck, devCombo, daxCombo] {
            SliceAudioRoute r;
            r.toMain = mainCheck->isChecked();
            r.outputDevice = devCombo->currentData().toString();
            r.daxChannel = daxCombo->currentData().toInt();
            if (!r.toMain && r.outputDevice.isEmpty() && r.daxChannel == 0) {
                // Routed nowhere would silence the slice: keep it on main.
                r.toMain = true;
                mainCheck->setChecked(true);
            }
            saveRoute(letter, r);
            emit routesChanged();
        };
        connect(mainCheck, &QCheckBox::toggled, this, save);
        connect(devCombo, &QComboBox::currentIndexChanged, this, save);
        connect(daxCombo, &QComboBox::currentIndexChanged, this, save);

        m_grid->addWidget(name, row, 0);
        m_grid->addWidget(mainCheck, row, 1);
        m_grid->addWidget(devCombo, row, 2);
        m_grid->addWidget(daxCombo, row, 3);
        m_grid->addWidget(radioDaxLabel, row, 4);
        ++row;
    }
    if (row == 1) {
        auto* none = new QLabel(QStringLiteral("No slices."));
        none->setStyleSheet(kNoteStyle);
        m_grid->addWidget(none, 1, 0, 1, headers.size());
    }
}

} // namespace AetherSDR
