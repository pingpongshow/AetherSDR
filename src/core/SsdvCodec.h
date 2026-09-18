#pragma once

#include <QByteArray>
#include <QImage>
#include <QString>

namespace AetherSDR {

struct SsdvPacketInfo {
    QString callsign;
    int imageId{0};
    int packetId{0};
    int width{0};
    int height{0};
    bool endOfImage{false};
    int errors{0};
};

class SsdvCodec {
public:
    static bool encodeImage(const QImage& image,
                            const QString& callsign,
                            int imageId,
                            QByteArray* packets,
                            QString* errorMessage);

    static bool decodePackets(const QByteArray& packets,
                              QImage* image,
                              QString* callsign,
                              int* imageId,
                              QString* errorMessage);

    static bool inspectPacket(const QByteArray& packet,
                              SsdvPacketInfo* info,
                              QString* errorMessage);
};

} // namespace AetherSDR
