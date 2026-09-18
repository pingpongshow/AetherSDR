#pragma once

#include <QDialog>
#include <QPointer>

class QCheckBox;
class QLineEdit;
class QSpinBox;
class QComboBox;
class QLabel;

namespace AetherSDR {

class PanadapterModel;

// Per-panadapter N1MM+ spectrum forwarding settings (FlexLib
// Panadapter.LoggerDisplay*). The radio stores these per pan, so nothing here
// is persisted client-side. Changes are sent only when the user clicks Apply.
class N1mmSpectrumDialog : public QDialog {
    Q_OBJECT

public:
    explicit N1mmSpectrumDialog(PanadapterModel* pan, QWidget* parent = nullptr);

private:
    void loadFromModel();
    void apply();

    QPointer<PanadapterModel> m_pan;
    QCheckBox* m_enableChk{nullptr};
    QLineEdit* m_addressEdit{nullptr};
    QSpinBox*  m_portSpin{nullptr};
    QComboBox* m_radioCmb{nullptr};
    QLabel*    m_errorLbl{nullptr};
};

} // namespace AetherSDR
