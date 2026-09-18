#include "SsdvCodec.h"

#include <QBuffer>
#include <QByteArrayView>
#include <QImageWriter>

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

extern "C" {
#include "ssdv.h"
}

namespace AetherSDR {

namespace {

QString sanitizeCallsign(QString text)
{
    text = text.trimmed().toUpper();
    QString filtered;
    filtered.reserve(6);
    for (const QChar ch : text) {
        if ((ch >= QLatin1Char('A') && ch <= QLatin1Char('Z'))
            || (ch >= QLatin1Char('0') && ch <= QLatin1Char('9'))) {
            filtered.append(ch);
            if (filtered.size() >= 6)
                break;
        }
    }
    if (filtered.isEmpty())
        filtered = QStringLiteral("NOCALL");
    return filtered.left(6);
}

QImage makeSsdvReadyImage(const QImage& image)
{
    if (image.isNull())
        return {};

    QImage rgb = image.convertToFormat(QImage::Format_RGB32);
    const int width = std::max(16, ((rgb.width() + 15) / 16) * 16);
    const int height = std::max(16, ((rgb.height() + 15) / 16) * 16);
    if (width == rgb.width() && height == rgb.height())
        return rgb;

    QImage padded(width, height, QImage::Format_RGB32);
    padded.fill(Qt::black);
    for (int y = 0; y < rgb.height(); ++y) {
        std::memcpy(padded.scanLine(y), rgb.constScanLine(y),
                    static_cast<size_t>(rgb.width()) * sizeof(QRgb));
    }
    return padded;
}

bool toJpegBytes(const QImage& image, QByteArray* out, QString* err)
{
    QBuffer buffer(out);
    if (!buffer.open(QIODevice::WriteOnly)) {
        if (err)
            *err = QStringLiteral("Failed to open JPEG output buffer.");
        return false;
    }

    QImageWriter writer(&buffer, "jpg");
    writer.setQuality(88);
    if (!writer.write(makeSsdvReadyImage(image))) {
        if (err)
            *err = writer.errorString();
        return false;
    }
    return true;
}

} // namespace

bool SsdvCodec::encodeImage(const QImage& image,
                            const QString& callsign,
                            int imageId,
                            QByteArray* packets,
                            QString* errorMessage)
{
    if (!packets) {
        if (errorMessage)
            *errorMessage = QStringLiteral("No SSDV output buffer provided.");
        return false;
    }

    packets->clear();
    QByteArray jpeg;
    if (!toJpegBytes(image, &jpeg, errorMessage))
        return false;

    ssdv_t ssdv{};
    std::array<uint8_t, SSDV_PKT_SIZE> packet{};
    QByteArray call = sanitizeCallsign(callsign).toLatin1();

    if (ssdv_enc_init(&ssdv, SSDV_TYPE_NORMAL, call.data(),
                      static_cast<uint8_t>(std::clamp(imageId, 0, 255)),
                      4, SSDV_PKT_SIZE) != SSDV_OK) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Failed to initialize SSDV encoder.");
        return false;
    }

    if (ssdv_enc_set_buffer(&ssdv, packet.data()) != SSDV_OK) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Failed to initialize SSDV packet buffer.");
        return false;
    }

    int offset = 0;
    while (true) {
        char rc = ssdv_enc_get_packet(&ssdv);
        while (rc == SSDV_FEED_ME && offset < jpeg.size()) {
            const int chunk = std::min<qsizetype>(256, jpeg.size() - offset);
            rc = ssdv_enc_feed(&ssdv,
                               reinterpret_cast<uint8_t*>(jpeg.data() + offset),
                               static_cast<size_t>(chunk));
            offset += chunk;
            if (rc == SSDV_FEED_ME)
                rc = ssdv_enc_get_packet(&ssdv);
        }

        if (rc == SSDV_EOI)
            break;

        if (rc == SSDV_FEED_ME && offset >= jpeg.size()) {
            if (errorMessage)
                *errorMessage = QStringLiteral("SSDV encoder reached end of JPEG input unexpectedly.");
            return false;
        }

        if (rc != SSDV_OK) {
            if (errorMessage)
                *errorMessage = QStringLiteral("SSDV encoder failed with code %1.").arg(static_cast<int>(rc));
            return false;
        }

        packets->append(reinterpret_cast<const char*>(packet.data()), SSDV_PKT_SIZE);
    }

    return !packets->isEmpty();
}

bool SsdvCodec::decodePackets(const QByteArray& packets,
                              QImage* image,
                              QString* callsign,
                              int* imageId,
                              QString* errorMessage)
{
    if (image)
        *image = QImage();
    if (callsign)
        callsign->clear();
    if (imageId)
        *imageId = 0;

    if (packets.isEmpty()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("The SSDV packet stream is empty.");
        return false;
    }

    ssdv_t ssdv{};
    if (ssdv_dec_init(&ssdv, SSDV_PKT_SIZE) != SSDV_OK) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Failed to initialize SSDV decoder.");
        return false;
    }

    std::vector<uint8_t> jpeg(8 * 1024 * 1024);
    if (ssdv_dec_set_buffer(&ssdv, jpeg.data(), jpeg.size()) != SSDV_OK) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Failed to allocate SSDV JPEG buffer.");
        return false;
    }

    bool sawPacket = false;
    for (int offset = 0; offset + SSDV_PKT_SIZE <= packets.size(); offset += SSDV_PKT_SIZE) {
        auto* pkt = reinterpret_cast<uint8_t*>(const_cast<char*>(packets.constData() + offset));
        int errors = 0;
        if (ssdv_dec_is_packet(pkt, SSDV_PKT_SIZE, &errors) != 0)
            continue;

        if (!sawPacket) {
            ssdv_packet_info_t info{};
            ssdv_dec_header(&info, pkt);
            if (callsign)
                *callsign = QString::fromLatin1(info.callsign_s);
            if (imageId)
                *imageId = info.image_id;
        }

        sawPacket = true;
        if (ssdv_dec_feed(&ssdv, pkt) != SSDV_OK) {
            if (errorMessage)
                *errorMessage = QStringLiteral("SSDV packet decode failed.");
            return false;
        }
    }

    if (!sawPacket) {
        if (errorMessage)
            *errorMessage = QStringLiteral("No valid SSDV packets were found in the file.");
        return false;
    }

    uint8_t* jpegPtr = jpeg.data();
    size_t jpegLength = jpeg.size();
    if (ssdv_dec_get_jpeg(&ssdv, &jpegPtr, &jpegLength) != SSDV_OK || jpegLength == 0) {
        if (errorMessage)
            *errorMessage = QStringLiteral("The SSDV packet stream did not produce a JPEG image.");
        return false;
    }

    QImage decoded;
    if (!decoded.loadFromData(jpeg.data(), static_cast<int>(jpegLength), "JPG")) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Failed to decode JPEG data reconstructed from SSDV packets.");
        return false;
    }

    if (image)
        *image = decoded;
    return true;
}

bool SsdvCodec::inspectPacket(const QByteArray& packet,
                              SsdvPacketInfo* info,
                              QString* errorMessage)
{
    if (info)
        *info = {};

    if (packet.size() != SSDV_PKT_SIZE) {
        if (errorMessage)
            *errorMessage = QStringLiteral("The SSDV packet must be exactly %1 bytes.")
                                .arg(SSDV_PKT_SIZE);
        return false;
    }

    int errors = 0;
    auto* pkt = reinterpret_cast<uint8_t*>(const_cast<char*>(packet.constData()));
    if (ssdv_dec_is_packet(pkt, SSDV_PKT_SIZE, &errors) != 0) {
        if (errorMessage)
            *errorMessage = QStringLiteral("The buffer does not contain a valid SSDV packet.");
        return false;
    }

    if (info) {
        ssdv_packet_info_t header{};
        ssdv_dec_header(&header, pkt);
        info->callsign = QString::fromLatin1(header.callsign_s);
        info->imageId = header.image_id;
        info->packetId = header.packet_id;
        info->width = header.width;
        info->height = header.height;
        info->endOfImage = header.eoi != 0;
        info->errors = errors;
    }

    return true;
}

} // namespace AetherSDR
