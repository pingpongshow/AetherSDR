#pragma once

// ---------------------------------------------------------------------------
// LogbookDialog — log window: quick-entry row, contest panel (exchange,
// serials, score, F1–F8 macros, SO2R), sortable / filterable QSO table,
// edit / delete, ADIF import / export, QSO-recording links, and online
// upload controls.
//
// Non-modal.  Owns no models: MainWindow passes Logbook / ContestEngine /
// LogUploadQueue / CredentialStore / So2rController pointers and keeps the
// active slice updated via setActiveSlice().
//
// Visual styling deliberately minimal (default palette) — maintainer review.
// ---------------------------------------------------------------------------

#include "core/ContestEngine.h"
#include "core/LogUploaders.h"
#include "models/Logbook.h"

#include <QDialog>
#include <QPointer>
#include <QTimer>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QSortFilterProxyModel;
class QTableView;
class QGroupBox;

namespace AetherSDR {

class LogTableModel;
class SliceModel;

class LogbookDialog : public QDialog {
    Q_OBJECT

public:
    LogbookDialog(Logbook* log, ContestEngine* contest, LogUploadQueue* uploads,
                  CredentialStore* creds, QWidget* parent = nullptr);
    ~LogbookDialog() override;

    void setSo2r(So2rController* so2r);
    // Slice used for freq/mode auto-fill (follows TX/active slice in MainWindow).
    void setActiveSlice(SliceModel* slice);

public slots:
    void focusQuickEntry();          // quick-log hotkey target
    void prefillCall(const QString& call, double freqMhz = 0.0, const QString& mode = {});
    void logCurrentEntry();
    void clearEntry();
    void sendMacro(int fkey);        // 1..8

signals:
    void qsoLogged(qint64 id);
    // Table context menu → point the antenna rotator at a logged station.
    void rotatorTurnRequested(const QString& call, const QString& grid, bool longPath);

protected:
    void closeEvent(QCloseEvent* e) override;

private:
    void buildUi();
    QWidget* buildQuickEntry();
    QWidget* buildContestPanel();
    QWidget* buildTablePanel();
    void updatePreview();
    void updateSliceInfo();
    void updateContestUi();
    void updateScore(const ContestScore& s);
    void updateSo2rUi();
    void reloadTable();
    void scheduleReload();
    QVector<qint64> selectedIds() const;
    void editSelected();
    void deleteSelected();
    void playRecording();
    void attachRecording();
    void importAdif();
    void exportAdif();
    void uploadSelected(UploadService s);
    void openLogSettings();
    void showTableMenu(const QPoint& pos);
    void saveContestSettings();

    QPointer<Logbook>          m_log;
    QPointer<ContestEngine>    m_contest;
    QPointer<LogUploadQueue>   m_uploads;
    QPointer<CredentialStore>  m_creds;
    QPointer<So2rController>   m_so2r;
    QPointer<SliceModel>       m_slice;
    QMetaObject::Connection    m_sliceFreqConn;
    QMetaObject::Connection    m_sliceModeConn;

    // Quick entry
    QLineEdit*  m_call{nullptr};
    QLineEdit*  m_rstS{nullptr};
    QLineEdit*  m_rstR{nullptr};
    QLineEdit*  m_name{nullptr};
    QLineEdit*  m_grid{nullptr};
    QLineEdit*  m_comment{nullptr};
    QLineEdit*  m_exchange{nullptr};
    QLabel*     m_exchangeLabel{nullptr};
    QLabel*     m_sentLabel{nullptr};
    QLineEdit*  m_freq{nullptr};
    QComboBox*  m_mode{nullptr};
    QCheckBox*  m_followSlice{nullptr};
    QLabel*     m_preview{nullptr};
    QLabel*     m_dupe{nullptr};
    QDateTime   m_timeOn;

    // Contest
    QGroupBox*  m_contestBox{nullptr};
    QComboBox*  m_profile{nullptr};
    QComboBox*  m_modeVariant{nullptr};
    QLineEdit*  m_myExchange{nullptr};
    QLineEdit*  m_customTemplate{nullptr};
    QLabel*     m_serialLabel{nullptr};
    QLabel*     m_scoreLabel{nullptr};
    QCheckBox*  m_armTx{nullptr};
    QVector<QPushButton*> m_macroButtons;
    QLabel*     m_so2rLabel{nullptr};
    QWidget*    m_so2rRow{nullptr};

    // Table
    QLineEdit*  m_filterText{nullptr};
    QComboBox*  m_filterBand{nullptr};
    QComboBox*  m_filterMode{nullptr};
    QCheckBox*  m_filterContest{nullptr};
    QTableView* m_table{nullptr};
    LogTableModel*          m_model{nullptr};
    QSortFilterProxyModel*  m_proxy{nullptr};
    QLabel*       m_status{nullptr};
    QProgressBar* m_importProgress{nullptr};
    QTimer        m_reloadTimer;
};

} // namespace AetherSDR
