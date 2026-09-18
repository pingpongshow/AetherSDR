#pragma once

#include <QString>

namespace AetherSDR::DigitalModeStyles {

inline QString rxPanelStyle()
{
    return QStringLiteral(
        "QWidget { background: #cfe8fb; border-radius: 6px; color: #17324a; }"
        "QLabel { color: #17324a; background: transparent; }"
        "QLabel:disabled { color: #5f768b; }"
        "QCheckBox { color: #17324a; spacing: 6px; background: transparent; }"
        "QCheckBox:disabled { color: #5f768b; }"
        "QLineEdit, QComboBox, QDoubleSpinBox, QSpinBox { "
        "background: #f8fbff; color: #11263a; border: 1px solid #5d7891; "
        "border-radius: 4px; padding: 4px 8px; selection-background-color: #0d3b84; "
        "selection-color: white; }"
        "QLineEdit:disabled, QComboBox:disabled, QDoubleSpinBox:disabled, QSpinBox:disabled { "
        "background: #dfeaf3; color: #4d6479; border-color: #90a5b8; }"
        "QLineEdit { placeholder-text-color: #71869a; }"
        "QComboBox QAbstractItemView { background: #f8fbff; color: #11263a; "
        "selection-background-color: #3e89ef; selection-color: white; }"
        "QPushButton { color: #17324a; border: 1px solid #4d647a; "
        "background: rgba(255,255,255,0.22); border-radius: 4px; padding: 6px 12px; }"
        "QPushButton:disabled { color: #667c8f; border-color: #8ea4b6; "
        "background: rgba(255,255,255,0.4); }"
        "QSlider::groove:horizontal { background: #a8c2d9; height: 6px; border-radius: 3px; }"
        "QSlider::handle:horizontal { background: #ffffff; width: 18px; margin: -6px 0; "
        "border: 1px solid #6f88a1; border-radius: 9px; }"
        "QProgressBar { background: #e4eef7; border: 1px solid #7b92a9; border-radius: 4px; }"
        "QProgressBar::chunk { background: #0d3b84; border-radius: 3px; }");
}

inline QString txPanelStyle()
{
    return QStringLiteral(
        "QWidget { background: #f8c5c8; border-radius: 6px; color: #4a2630; }"
        "QLabel { color: #4a2630; background: transparent; }"
        "QLabel:disabled { color: #8a6770; }"
        "QCheckBox { color: #4a2630; spacing: 6px; background: transparent; }"
        "QCheckBox:disabled { color: #8a6770; }"
        "QLineEdit, QComboBox, QDoubleSpinBox, QSpinBox { "
        "background: #fff8f8; color: #341720; border: 1px solid #a97f88; "
        "border-radius: 4px; padding: 4px 8px; selection-background-color: #0d3b84; "
        "selection-color: white; }"
        "QLineEdit:disabled, QComboBox:disabled, QDoubleSpinBox:disabled, QSpinBox:disabled { "
        "background: #f5e1e3; color: #7b5e67; border-color: #bf9aa2; }"
        "QLineEdit { placeholder-text-color: #9a7d84; }"
        "QComboBox QAbstractItemView { background: #fff8f8; color: #341720; "
        "selection-background-color: #3e89ef; selection-color: white; }"
        "QPushButton { color: #4a2630; border: 1px solid #916872; "
        "background: rgba(255,255,255,0.24); border-radius: 4px; padding: 6px 12px; }"
        "QPushButton:disabled { color: #8b6d76; border-color: #b4939b; "
        "background: rgba(255,255,255,0.45); }"
        "QSlider::groove:horizontal { background: #e1b3b7; height: 6px; border-radius: 3px; }"
        "QSlider::handle:horizontal { background: #ffffff; width: 18px; margin: -6px 0; "
        "border: 1px solid #b0868f; border-radius: 9px; }"
        "QProgressBar { background: #f8e6e8; border: 1px solid #c29aa2; border-radius: 4px; }"
        "QProgressBar::chunk { background: #0d3b84; border-radius: 3px; }");
}

} // namespace AetherSDR::DigitalModeStyles
