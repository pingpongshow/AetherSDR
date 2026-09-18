#pragma once

#include <QWidget>
#include <QHash>
#include <QList>
#include <QPointer>

#include <functional>

class QPushButton;
class QTextEdit;
class QSpinBox;
class QLabel;
class QLineEdit;
class QStackedWidget;
class QScrollArea;
class QVBoxLayout;

namespace AetherSDR {

class CwxModel;
class CwxBubble;

class CwxPanel : public QWidget {
    Q_OBJECT
public:
    explicit CwxPanel(CwxModel* model, QWidget* parent = nullptr);

    void setModel(CwxModel* model);

    // Optional providers used to guard the global F1-F12 / ESC shortcuts
    // so they don't fire in modes/states where they'd be surprising (#1552).
    //  - modeProvider returns the active slice's mode ("CW", "CWL", ...)
    //  - transmittingProvider returns true when the radio is actively TXing
    // When unset, the shortcuts fire unconditionally (legacy behavior).
    void setActiveModeProvider(std::function<QString()> provider) {
        m_activeModeProvider = std::move(provider);
    }
    void setTransmittingProvider(std::function<bool()> provider) {
        m_transmittingProvider = std::move(provider);
    }

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private slots:
    void onCharSent(int index);
    void onMessageQueued(int block, int radioIndex);
    void onErased(int start, int stop);
    void onSpeedChanged(int wpm);

private:
    void buildSendView();
    void buildSetupView();
    void showSendView();
    void showSetupView();
    void sendBuffer();
    void onKeyPress(const QString& text);
    void updateLiveHighlight(int sentIndex);
    void resetLiveTracking();

    // Live-mode character tracking: document position ↔ radio buffer index
    struct LiveChar {
        int     id{-1};
        QString ch;
        int     docPos{-1};
        int     radioIndex{-1};   // -1 until the "cwx send" reply arrives
        bool    sent{false};
    };
    static constexpr int kMaxTrackedBubbles = 50;
    static constexpr int kMaxLiveChars = 512;

    CwxModel*       m_model{nullptr};

    QStackedWidget* m_stack{nullptr};

    // Send/Live view
    QWidget*        m_sendPage{nullptr};
    QScrollArea*    m_historyScroll{nullptr};
    QWidget*        m_historyContainer{nullptr};
    QVBoxLayout*    m_historyLayout{nullptr};
    QTextEdit*      m_textEdit{nullptr};     // input area at bottom
    QList<QPointer<CwxBubble>> m_bubbles;    // recent bubbles awaiting highlight
    QList<LiveChar> m_liveChars;
    QHash<int, int> m_livePendingBlocks;     // block → LiveChar::id
    int             m_nextLiveCharId{0};

    // Setup view
    QWidget*        m_setupPage{nullptr};
    QTextEdit*      m_macroEdits[12]{};
    QSpinBox*       m_delaySpin{nullptr};
    QPushButton*    m_qskBtn{nullptr};

    // Bottom bar
    QPushButton*    m_sendBtn{nullptr};
    QPushButton*    m_liveBtn{nullptr};
    QPushButton*    m_setupBtn{nullptr};
    QSpinBox*       m_speedSpin{nullptr};

    std::function<QString()> m_activeModeProvider;
    std::function<bool()>    m_transmittingProvider;
};

} // namespace AetherSDR
