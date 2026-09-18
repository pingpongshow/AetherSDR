#pragma once

#include "core/ProfileTransfer.h"

#include <QDialog>
#include <QPointer>

class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QTabWidget;
class QTreeWidget;

namespace AetherSDR {

class RadioModel;

// SmartSDR-compatible "Import/Export Profiles" dialog.
//
// Export: choose Global/TX/MIC profiles, memory groups, preferences, TNFs,
// USB cables and XVTRs; the radio packages them into a .ssdr_cfg file.
// Import: open a .ssdr_cfg, pick what to restore, confirm, upload.
class ProfileImportExportDialog : public QDialog {
    Q_OBJECT

public:
    explicit ProfileImportExportDialog(RadioModel* model, QWidget* parent = nullptr);
    ~ProfileImportExportDialog() override;

protected:
    void reject() override;

private:
    enum class Pending { None, FetchMeta, Export, Import };

    QWidget* buildExportTab();
    QWidget* buildImportTab();

    static void populateTree(QTreeWidget* tree, const DbMetaData& meta);
    static DbMetaData selectionFromTree(QTreeWidget* tree);
    static void setAllChecked(QTreeWidget* tree, bool checked);
    static DbMetaData metaFromModel(RadioModel* model);

    void refreshFromRadio();
    void browseExport();
    void startExport();
    void browseImport();
    void loadImportFile(const QString& path);
    void startImport();

    void onProgress(int percent, const QString& status);
    void onFinished(bool ok, const QString& message);
    void updateButtons();

    QPointer<RadioModel> m_model;
    ProfileTransfer*     m_transfer{nullptr};
    Pending              m_pending{Pending::None};

    QTabWidget*   m_tabs{nullptr};

    QTreeWidget*  m_exportTree{nullptr};
    QLineEdit*    m_exportPath{nullptr};
    QPushButton*  m_exportBtn{nullptr};
    QPushButton*  m_refreshBtn{nullptr};

    QTreeWidget*  m_importTree{nullptr};
    QLineEdit*    m_importPath{nullptr};
    QLabel*       m_importInfo{nullptr};
    QPushButton*  m_importBtn{nullptr};
    bool          m_importValid{false};

    QProgressBar* m_progress{nullptr};
    QLabel*       m_status{nullptr};
    QPushButton*  m_cancelBtn{nullptr};
    QPushButton*  m_closeBtn{nullptr};
};

} // namespace AetherSDR
