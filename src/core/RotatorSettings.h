#pragma once

#include "RotatorClient.h"

#include <QList>

namespace AetherSDR::RotatorSettings {

// Client-authoritative rotator settings, persisted via AppSettings
// (the radio has no rotator state, so nothing here fights the radio).
//
//   RotatorEnabled      "True"/"False"        default "False"
//   RotatorBackend      rotctld|pstrotator|n1mm default "rotctld"
//   RotatorHost         string                default "127.0.0.1"
//   RotatorPort         int                   default per backend (4533/12000/12040)
//   RotatorListenPort   int (UDP status port) default 0 = backend default
//   RotatorPollMs       int ms                default 1000
//   RotatorMinAz        double deg            default 0
//   RotatorMaxAz        double deg            default 360 (450 for overlap rotators)
//   RotatorN1mmName     string                default "Rotor"
//   RotatorPresets      comma list of degrees default "0,45,90,135,180,225,270,315"
//
// The station location comes from the existing StationGridSquare key
// (Radio Setup → Station grid square).

bool          isEnabled();
void          setEnabled(bool on);
RotatorConfig load();
void          save(const RotatorConfig& cfg);
QList<double> presets();
void          setPresets(const QList<double>& presets);
QString       stationGrid();

} // namespace AetherSDR::RotatorSettings
