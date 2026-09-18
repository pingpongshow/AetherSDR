#include "ProfileImportExportDialog.h"
#include "core/AppSettings.h"
#include "core/LogManager.h"
#include "models/RadioModel.h"
#include "models/TransmitModel.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSet>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace AetherSDR {

namespace {

// Matches ProfileManagerDialog so the Profiles menu dialogs look alike.
const QString kDialogStyle =
    "QDialog { background: #0f0f1a; color: #c8d8e8; }"
    "QTabWidget::pane { border: 1px solid #203040; background: #0f0f1a; }"
    "QTabBar::tab { background: #1a2a3a; color: #8898a8; padding: 6px 14px;"
    "  border: 1px solid #203040; border-bottom: none; border-top-left-radius: 4px;"
    "  border-top-right-radius: 4px; margin-right: 2px; }"
    "QTabBar::tab:selected { background: #0f0f1a; color: #c8d8e8; }"
    "QLineEdit { background: #0a0a18; border: 1px solid #1e2e3e; border-radius: 3px;"
    "  padding: 4px 6px; color: #c8d8e8; }"
    "QTreeWidget { background: #0a0a18; border: 1px solid #1e2e3e; border-radius: 3px;"
    "  color: #c8d8e8; }"
    "QTreeWidget::item:selected { background: #0070c0; }"
    "QPushButton { background: #1a2a3a; border: 1px solid #203040;"
    "  border-radius: 3px; padding: 4px 12px; color: #c8d8e8; }"
    "QPushButton:hover { background: #2a3a4a; }"
    "QPushButton:disabled { color: #506070; }"
    "QLabel { color: #c8d8e8; }"
    "QProgressBar { background: #0a0a18; border: 1px solid #1e2e3e; border-radius: 3px;"
    "  color: #c8d8e8; text-align: center; }"
    "QProgressBar::chunk { background: #00b4d8; }";

enum class Kind { Global, Tx, Mic, Memory, Preferences, Tnfs, UsbCables, Xvtrs, Unknown };

constexpr int kKindRole  = Qt::UserRole + 1;
constexpr int kTokenRole = Qt::UserRole + 2;
constexpr int kGroupRole = Qt::UserRole + 3;

QTreeWidgetItem* addCategory(QTreeWidget* tree, const QString& label, Kind kind)
{
    auto* item = new QTreeWidgetItem(tree);
    item->setText(0, label);
    item->setData(0, kKindRole, static_cast<int>(kind));
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate);
    item->setCheckState(0, Qt::Checked);
    return item;
}

void addListCategory(QTreeWidget* tree, const QString& label, Kind kind, const QStringList& tokens)
{
    QTreeWidgetItem* parent = addCategory(tree, label, kind);
    if (tokens.isEmpty()) {
        parent->setText(0, label + QStringLiteral("  (none)"));
        parent->setCheckState(0, Qt::Unchecked);
        parent->setFlags(Qt::NoItemFlags);
        return;
    }
    parent->setText(0, QStringLiteral("%1  (%2)").arg(label).arg(tokens.size()));
    for (const QString& token : tokens) {
        auto* child = new QTreeWidgetItem(parent);
        child->setText(0, ProfileDb::displayToken(token));
        child->setData(0, kKindRole, static_cast<int>(kind));
        child->setData(0, kTokenRole, token);
        child->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        child->setCheckState(0, Qt::Checked);
    }
}

void addFlagCategory(QTreeWidget* tree, const QString& label, Kind kind, bool available,
                     const QString& token = {})
{
    if (!available) {
        return;
    }
    QTreeWidgetItem* item = addCategory(tree, label, kind);
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
    item->setData(0, kTokenRole, token);
}

} // namespace

ProfileImportExportDialog::ProfileImportExportDialog(RadioModel* model, QWidget* parent)
    : QDialog(parent), m_model(model)
{
    setWindowTitle("Import/Export Profiles");
    setMinimumSize(520, 560);
    setStyleSheet(kDialogStyle);

    m_transfer = new ProfileTransfer(
        [model = QPointer<RadioModel>(model)](const QString& cmd, ProfileTransfer::ReplyFn reply) {
            if (!model || !model->isConnected()) {
                if (reply) {
                    reply(-1, QStringLiteral("not connected"));
                }
                return;
            }
            model->sendCmdPublic(cmd, std::move(reply));
        },
        [model = QPointer<RadioModel>(model)]() {
            return model ? model->radioAddress() : QHostAddress();
        },
        this);
    connect(m_transfer, &ProfileTransfer::progressChanged, this, &ProfileImportExportDialog::onProgress);
    connect(m_transfer, &ProfileTransfer::finished, this, &ProfileImportExportDialog::onFinished);
    connect(m_transfer, &ProfileTransfer::metaDataReceived, this, [this](const DbMetaData& meta) {
        populateTree(m_exportTree, meta);
    });

    auto* root = new QVBoxLayout(this);
    m_tabs = new QTabWidget;
    m_tabs->addTab(buildExportTab(), "Export");
    m_tabs->addTab(buildImportTab(), "Import");
    root->addWidget(m_tabs, 1);

    m_status = new QLabel;
    m_status->setWordWrap(true);
    root->addWidget(m_status);

    m_progress = new QProgressBar;
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    root->addWidget(m_progress);

    auto* bottom = new QHBoxLayout;
    bottom->addStretch();
    m_cancelBtn = new QPushButton("Cancel Transfer");
    connect(m_cancelBtn, &QPushButton::clicked, this, [this] { m_transfer->cancel(); });
    bottom->addWidget(m_cancelBtn);
    m_closeBtn = new QPushButton("Close");
    connect(m_closeBtn, &QPushButton::clicked, this, &QDialog::reject);
    bottom->addWidget(m_closeBtn);
    root->addLayout(bottom);

    populateTree(m_exportTree, metaFromModel(model));

    if (!model || !model->isConnected()) {
        m_status->setText("Not connected to a radio.");
    } else if (model->isWan()) {
        // The radio opens a TCP connection back to the client for downloads,
        // which cannot traverse SmartLink.
        m_status->setText("Profile import/export requires a direct LAN connection "
                          "(not available over SmartLink).");
    } else {
        refreshFromRadio();
    }
    updateButtons();
}

ProfileImportExportDialog::~ProfileImportExportDialog() = default;

void ProfileImportExportDialog::reject()
{
    if (m_transfer->isBusy()) {
        const auto answer = QMessageBox::question(
            this, "Transfer in Progress",
            m_pending == Pending::Import
                ? "An import is in progress. Closing now may leave the radio database "
                  "partially updated. Cancel the transfer and close?"
                : "A transfer is in progress. Cancel it and close?");
        if (answer != QMessageBox::Yes) {
            return;
        }
        m_transfer->cancel();
    }
    QDialog::reject();
}

// ── Tabs ───────────────────────────────────────────────────────────────────

QWidget* ProfileImportExportDialog::buildExportTab()
{
    auto* page = new QWidget;
    auto* v = new QVBoxLayout(page);

    v->addWidget(new QLabel("Select the items to export from the radio:"));

    m_exportTree = new QTreeWidget;
    m_exportTree->setHeaderHidden(true);
    v->addWidget(m_exportTree, 1);

    auto* selRow = new QHBoxLayout;
    auto* allBtn = new QPushButton("Select All");
    auto* noneBtn = new QPushButton("Select None");
    m_refreshBtn = new QPushButton("Refresh from Radio");
    connect(allBtn, &QPushButton::clicked, this, [this] { setAllChecked(m_exportTree, true); });
    connect(noneBtn, &QPushButton::clicked, this, [this] { setAllChecked(m_exportTree, false); });
    connect(m_refreshBtn, &QPushButton::clicked, this, &ProfileImportExportDialog::refreshFromRadio);
    selRow->addWidget(allBtn);
    selRow->addWidget(noneBtn);
    selRow->addStretch();
    selRow->addWidget(m_refreshBtn);
    v->addLayout(selRow);

    auto* pathRow = new QHBoxLayout;
    m_exportPath = new QLineEdit;
    const QString dir = AppSettings::instance()
        .value("ProfileExportLastDir",
               QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
        .toString();
    // FlexLib file name format: SSDR_Config_08-04-14_3.16_PM.ssdr_cfg
    const QString name = QStringLiteral("SSDR_Config_%1.ssdr_cfg")
        .arg(QDateTime::currentDateTime().toString("MM-dd-yy_h.mm_AP"));
    m_exportPath->setText(QDir(dir).filePath(name));
    auto* browse = new QPushButton("Browse...");
    connect(browse, &QPushButton::clicked, this, &ProfileImportExportDialog::browseExport);
    pathRow->addWidget(new QLabel("Save to:"));
    pathRow->addWidget(m_exportPath, 1);
    pathRow->addWidget(browse);
    v->addLayout(pathRow);

    auto* goRow = new QHBoxLayout;
    goRow->addStretch();
    m_exportBtn = new QPushButton("Export");
    connect(m_exportBtn, &QPushButton::clicked, this, &ProfileImportExportDialog::startExport);
    goRow->addWidget(m_exportBtn);
    v->addLayout(goRow);

    return page;
}

QWidget* ProfileImportExportDialog::buildImportTab()
{
    auto* page = new QWidget;
    auto* v = new QVBoxLayout(page);

    auto* pathRow = new QHBoxLayout;
    m_importPath = new QLineEdit;
    m_importPath->setPlaceholderText("Choose a SmartSDR .ssdr_cfg file");
    connect(m_importPath, &QLineEdit::editingFinished, this, [this] {
        loadImportFile(m_importPath->text());
    });
    auto* browse = new QPushButton("Browse...");
    connect(browse, &QPushButton::clicked, this, &ProfileImportExportDialog::browseImport);
    pathRow->addWidget(new QLabel("File:"));
    pathRow->addWidget(m_importPath, 1);
    pathRow->addWidget(browse);
    v->addLayout(pathRow);

    m_importInfo = new QLabel;
    m_importInfo->setWordWrap(true);
    v->addWidget(m_importInfo);

    v->addWidget(new QLabel("Select the items to import into the radio:"));
    m_importTree = new QTreeWidget;
    m_importTree->setHeaderHidden(true);
    v->addWidget(m_importTree, 1);

    auto* selRow = new QHBoxLayout;
    auto* allBtn = new QPushButton("Select All");
    auto* noneBtn = new QPushButton("Select None");
    connect(allBtn, &QPushButton::clicked, this, [this] { setAllChecked(m_importTree, true); });
    connect(noneBtn, &QPushButton::clicked, this, [this] { setAllChecked(m_importTree, false); });
    selRow->addWidget(allBtn);
    selRow->addWidget(noneBtn);
    selRow->addStretch();
    m_importBtn = new QPushButton("Import");
    connect(m_importBtn, &QPushButton::clicked, this, &ProfileImportExportDialog::startImport);
    selRow->addWidget(m_importBtn);
    v->addLayout(selRow);

    return page;
}

// ── Tree helpers ───────────────────────────────────────────────────────────

void ProfileImportExportDialog::populateTree(QTreeWidget* tree, const DbMetaData& meta)
{
    tree->clear();
    addListCategory(tree, "Global Profiles", Kind::Global, meta.globalProfiles);
    addListCategory(tree, "TX Profiles", Kind::Tx, meta.txProfiles);
    addListCategory(tree, "MIC Profiles", Kind::Mic, meta.micProfiles);

    QTreeWidgetItem* mem = addCategory(tree, "Memories", Kind::Memory);
    if (meta.memoryGroups.isEmpty()) {
        mem->setText(0, "Memories  (none)");
        mem->setCheckState(0, Qt::Unchecked);
        mem->setFlags(Qt::NoItemFlags);
    } else {
        mem->setText(0, QStringLiteral("Memories  (%1 groups)").arg(meta.memoryGroups.size()));
        for (const DbMemoryGroup& g : meta.memoryGroups) {
            auto* child = new QTreeWidgetItem(mem);
            child->setText(0, ProfileDb::memoryGroupLabel(g));
            child->setData(0, kKindRole, static_cast<int>(Kind::Memory));
            child->setData(0, kTokenRole, g.owner);
            child->setData(0, kGroupRole, g.group);
            child->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
            child->setCheckState(0, Qt::Checked);
        }
    }

    addFlagCategory(tree, "Preferences (band, mode and global persistence)",
                    Kind::Preferences, meta.preferences());
    addFlagCategory(tree, "TNFs", Kind::Tnfs, meta.tnfs);
    addFlagCategory(tree, "USB Cables", Kind::UsbCables, meta.usbCables);
    addFlagCategory(tree, "Transverters (XVTRs)", Kind::Xvtrs, meta.xvtrs);
    for (const QString& line : meta.unknownLines) {
        addFlagCategory(tree, QStringLiteral("Other: %1").arg(line.section('^', 0, 0)),
                        Kind::Unknown, true, line);
    }
    tree->expandToDepth(0);
}

DbMetaData ProfileImportExportDialog::selectionFromTree(QTreeWidget* tree)
{
    DbMetaData sel;
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* top = tree->topLevelItem(i);
        const auto kind = static_cast<Kind>(top->data(0, kKindRole).toInt());
        const bool topChecked = top->checkState(0) == Qt::Checked
                                && top->flags().testFlag(Qt::ItemIsEnabled);

        for (int c = 0; c < top->childCount(); ++c) {
            QTreeWidgetItem* child = top->child(c);
            if (child->checkState(0) != Qt::Checked) {
                continue;
            }
            const QString token = child->data(0, kTokenRole).toString();
            switch (kind) {
            case Kind::Global: sel.globalProfiles.append(token); break;
            case Kind::Tx:     sel.txProfiles.append(token); break;
            case Kind::Mic:    sel.micProfiles.append(token); break;
            case Kind::Memory:
                sel.memoryGroups.append({token, child->data(0, kGroupRole).toString()});
                break;
            default: break;
            }
        }

        if (top->childCount() > 0 || !topChecked) {
            continue;
        }
        switch (kind) {
        case Kind::Preferences: sel.setPreferences(true); break;
        case Kind::Tnfs:        sel.tnfs = true; break;
        case Kind::UsbCables:   sel.usbCables = true; break;
        case Kind::Xvtrs:       sel.xvtrs = true; break;
        case Kind::Unknown:     sel.unknownLines.append(top->data(0, kTokenRole).toString()); break;
        default: break;
        }
    }
    return sel;
}

void ProfileImportExportDialog::setAllChecked(QTreeWidget* tree, bool checked)
{
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* top = tree->topLevelItem(i);
        if (!top->flags().testFlag(Qt::ItemIsEnabled)) {
            continue;
        }
        top->setCheckState(0, checked ? Qt::Checked : Qt::Unchecked);
        for (int c = 0; c < top->childCount(); ++c) {
            top->child(c)->setCheckState(0, checked ? Qt::Checked : Qt::Unchecked);
        }
    }
}

DbMetaData ProfileImportExportDialog::metaFromModel(RadioModel* model)
{
    // Provisional view from the already-known lists; replaced by the radio's
    // own db_meta_data listing (exact tokens) as soon as it arrives.
    DbMetaData meta;
    if (!model) {
        return meta;
    }
    meta.globalProfiles = model->globalProfiles();
    meta.txProfiles = model->transmitModel().profileList();
    meta.micProfiles = model->transmitModel().micProfileList();
    QSet<QString> seen;
    for (const MemoryEntry& m : model->memories()) {
        const QString key = m.owner + QChar(0x1f) + m.group;
        if (seen.contains(key)) {
            continue;
        }
        seen.insert(key);
        meta.memoryGroups.append({m.owner, m.group});
    }
    meta.setPreferences(true);
    meta.tnfs = true;
    meta.usbCables = true;
    meta.xvtrs = true;
    return meta;
}

// ── Actions ────────────────────────────────────────────────────────────────

void ProfileImportExportDialog::refreshFromRadio()
{
    if (m_transfer->isBusy() || !m_model || !m_model->isConnected() || m_model->isWan()) {
        return;
    }
    m_pending = Pending::FetchMeta;
    m_transfer->fetchMetaData();
    updateButtons();
}

void ProfileImportExportDialog::browseExport()
{
    const QString path = QFileDialog::getSaveFileName(
        this, "Export Profiles", m_exportPath->text(),
        "SmartSDR Configuration (*.ssdr_cfg)");
    if (!path.isEmpty()) {
        m_exportPath->setText(path);
    }
}

void ProfileImportExportDialog::startExport()
{
    QString path = m_exportPath->text().trimmed();
    if (path.isEmpty()) {
        browseExport();
        path = m_exportPath->text().trimmed();
        if (path.isEmpty()) {
            return;
        }
    }
    if (!path.endsWith(".ssdr_cfg", Qt::CaseInsensitive)) {
        path += ".ssdr_cfg";
        m_exportPath->setText(path);
    }
    const DbMetaData sel = selectionFromTree(m_exportTree);
    if (sel.isEmpty()) {
        QMessageBox::information(this, "Export Profiles", "Select at least one item to export.");
        return;
    }
    if (QFileInfo::exists(path)) {
        const auto answer = QMessageBox::question(
            this, "Export Profiles",
            QStringLiteral("%1 already exists. Replace it?").arg(QFileInfo(path).fileName()));
        if (answer != QMessageBox::Yes) {
            return;
        }
    }
    auto& s = AppSettings::instance();
    s.setValue("ProfileExportLastDir", QFileInfo(path).absolutePath());
    s.save();

    m_pending = Pending::Export;
    m_transfer->exportDatabase(sel, path);
    updateButtons();
}

void ProfileImportExportDialog::browseImport()
{
    const QString start = m_importPath->text().isEmpty()
        ? AppSettings::instance()
              .value("ProfileExportLastDir",
                     QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
              .toString()
        : m_importPath->text();
    const QString path = QFileDialog::getOpenFileName(
        this, "Import Profiles", start, "SmartSDR Configuration (*.ssdr_cfg);;All Files (*)");
    if (!path.isEmpty()) {
        m_importPath->setText(path);
        loadImportFile(path);
    }
}

void ProfileImportExportDialog::loadImportFile(const QString& path)
{
    m_importValid = false;
    m_importTree->clear();
    m_importInfo->clear();
    if (path.trimmed().isEmpty()) {
        updateButtons();
        return;
    }

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        m_importInfo->setText(QStringLiteral("Cannot open file: %1").arg(f.errorString()));
        updateButtons();
        return;
    }
    const QByteArray pkg = f.read(64LL * 1024 * 1024 + 1);
    QString error;
    const std::optional<ProfileDb::PackageInfo> info = ProfileDb::inspectPackage(pkg, &error);
    if (!info) {
        m_importInfo->setText(QStringLiteral("Not a valid SmartSDR export: %1").arg(error));
        updateButtons();
        return;
    }

    populateTree(m_importTree, info->meta);
    QString text = QStringLiteral("%1 KB, contains: %2")
        .arg(pkg.size() / 1024)
        .arg(info->entryNames.join(", "));
    if (info->memoryCsvRows > 0) {
        text += QStringLiteral(" (%1 memory channels)").arg(info->memoryCsvRows);
    }
    m_importInfo->setText(text);
    m_importValid = true;
    updateButtons();
}

void ProfileImportExportDialog::startImport()
{
    if (!m_importValid || !m_model || !m_model->isConnected()) {
        return;
    }
    const DbMetaData sel = selectionFromTree(m_importTree);
    if (sel.isEmpty()) {
        QMessageBox::information(this, "Import Profiles", "Select at least one item to import.");
        return;
    }
    if (m_model->transmitModel().isTransmitting()) {
        QMessageBox::warning(this, "Import Profiles", "Stop transmitting before importing.");
        return;
    }

    QStringList parts;
    const auto count = [&parts](qsizetype n, const char* what) {
        if (n > 0) {
            parts << QStringLiteral("%1 %2").arg(n).arg(QLatin1String(what));
        }
    };
    count(sel.globalProfiles.size(), "global profile(s)");
    count(sel.txProfiles.size(), "TX profile(s)");
    count(sel.micProfiles.size(), "MIC profile(s)");
    count(sel.memoryGroups.size(), "memory group(s)");
    if (sel.preferences()) { parts << "preferences"; }
    if (sel.tnfs) { parts << "TNFs"; }
    if (sel.usbCables) { parts << "USB cables"; }
    if (sel.xvtrs) { parts << "XVTRs"; }

    QMessageBox box(QMessageBox::Warning, "Import Profiles",
                    "Importing will OVERWRITE data stored in the radio.",
                    QMessageBox::Cancel, this);
    box.setInformativeText(
        QStringLiteral("The following will be written to the radio, replacing any existing "
                       "items with the same name:\n\n%1\n\nThis cannot be undone. Consider "
                       "exporting a backup first. Continue?").arg(parts.join(", ")));
    QPushButton* go = box.addButton("Import and Overwrite", QMessageBox::DestructiveRole);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() != go) {
        return;
    }

    qCInfo(lcGui) << "ProfileImportExportDialog: user confirmed import of" << m_importPath->text();
    m_pending = Pending::Import;
    m_transfer->importDatabase(m_importPath->text(), sel);
    updateButtons();
}

// ── Transfer feedback ──────────────────────────────────────────────────────

void ProfileImportExportDialog::onProgress(int percent, const QString& status)
{
    if (percent < 0) {
        m_progress->setRange(0, 0);
    } else {
        m_progress->setRange(0, 100);
        m_progress->setValue(percent);
    }
    m_status->setText(status);
}

void ProfileImportExportDialog::onFinished(bool ok, const QString& message)
{
    const Pending was = m_pending;
    m_pending = Pending::None;
    m_progress->setRange(0, 100);
    m_progress->setValue(ok ? 100 : 0);

    if (was == Pending::FetchMeta) {
        m_progress->setValue(0);
        m_status->setText(ok ? QString() : QStringLiteral("Could not read the radio's database "
                                                          "listing (%1); showing known profiles.")
                                               .arg(message));
    } else {
        m_status->setText(message);
        if (was == Pending::Export || was == Pending::Import) {
            // Non-blocking: this runs from a transfer callback, and a nested
            // exec() loop here lets the dialog be destroyed underneath the
            // stack-allocated box QMessageBox::warning() would create.
            auto* box = new QMessageBox(ok ? QMessageBox::Information : QMessageBox::Warning,
                                        windowTitle(), message, QMessageBox::Ok, this);
            box->setAttribute(Qt::WA_DeleteOnClose);
            box->open();
        }
    }
    updateButtons();
}

void ProfileImportExportDialog::updateButtons()
{
    const bool busy = m_transfer->isBusy();
    const bool usable = m_model && m_model->isConnected() && !m_model->isWan();
    m_exportBtn->setEnabled(!busy && usable);
    m_refreshBtn->setEnabled(!busy && usable);
    m_importBtn->setEnabled(!busy && usable && m_importValid);
    m_cancelBtn->setEnabled(busy);
}

} // namespace AetherSDR
