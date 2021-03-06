#include "Arduino.h"
#include "variables.h"

void qspDecodeRcDataFrame(QspConfiguration_t *qsp, RxDeviceState_t *rxDeviceSate) {

    qsp->setRcChannelCallback(0, (uint16_t) (((uint16_t) qsp->payload[0] << 2) & 0x3fc) | ((qsp->payload[1] >> 6) & 0x03), 1000);
    qsp->setRcChannelCallback(1, (uint16_t) (((uint16_t) qsp->payload[1] << 4) & 0x3f0) | ((qsp->payload[2] >> 4) & 0x0F), 1000);
    qsp->setRcChannelCallback(2, (uint16_t) (((uint16_t) qsp->payload[2] << 6) & 0x3c0) | ((qsp->payload[3] >> 2) & 0x3F), 1000);
    qsp->setRcChannelCallback(3, (uint16_t) (((uint16_t) qsp->payload[3] << 8) & 0x300) | ((qsp->payload[4]) & 0xFF), 1000);
    qsp->setRcChannelCallback(4, ((int) qsp->payload[5]) << 2, 1000);
    qsp->setRcChannelCallback(5, ((int) qsp->payload[6]) << 2, 1000);
    qsp->setRcChannelCallback(6, ((int) ((qsp->payload[7] >> 4) & 0b00001111)) << 6, 1000);
    qsp->setRcChannelCallback(7, ((int) (qsp->payload[7] & 0b00001111)) << 6, 1000);
    qsp->setRcChannelCallback(8, ((int) ((qsp->payload[8] >> 4) & 0b00001111)) << 6, 1000);
    qsp->setRcChannelCallback(9, ((int) (qsp->payload[8] & 0b00001111)) << 6, 1000);
}

uint8_t get10bitHighShift(uint8_t channel) {
    return ((channel % 4) * 2) + 2;
}

uint8_t get10bitLowShift(uint8_t channel) {
    return 8 - get10bitHighShift(channel);
}

uint8_t crc8_dvb_s2(uint8_t crc, uint8_t a)
{
    crc ^= a;
    for (int ii = 0; ii < 8; ++ii) {
        if (crc & 0x80) {
            crc = (crc << 1) ^ 0xD5;
        } else {
            crc = crc << 1;
        }
    }
    return crc;
}

void qspComputeCrc(QspConfiguration_t *qsp, uint8_t dataByte)
{
    qsp->crc = crc8_dvb_s2(qsp->crc, dataByte);
}

void encodeRxHealthPayload(QspConfiguration_t *qsp, RxDeviceState_t *rxDeviceState, uint8_t rssi, uint8_t snr, bool isFailsafe) {
    qsp->payload[0] = rssi;
    qsp->payload[1] = snr;
    qsp->payload[2] = rxDeviceState->rxVoltage;
    qsp->payload[3] = rxDeviceState->a1Voltage;
    qsp->payload[4] = rxDeviceState->a2Voltage;

    uint8_t flags = 0;

    if (isFailsafe) {
        flags |= 0x01 << 0;
    }

    qsp->payload[5] = flags;

    qsp->payloadLength = qspFrameLengths[QSP_FRAME_RX_HEALTH];
}

void decodeRxHealthPayload(QspConfiguration_t *qsp, RxDeviceState_t *rxDeviceState) {
    rxDeviceState->rssi = qsp->payload[0];
    rxDeviceState->snr = qsp->payload[1];
    rxDeviceState->rxVoltage = qsp->payload[2];
    rxDeviceState->a1Voltage = qsp->payload[3];
    rxDeviceState->a2Voltage = qsp->payload[4];
    rxDeviceState->flags = qsp->payload[5];
}

/**
 * Encode 10 RC channels 
 */
void encodeRcDataPayload(QspConfiguration_t *qsp, uint8_t noOfChannels)
{
    for (uint8_t i = 0; i < noOfChannels; i++)
    {
        int cV = constrain(qsp->rcChannelGetCallback(i), 1000, 2000) - 1000;

        uint16_t channelValue10 = cV & 0x03ff;
        uint8_t channelValue8   = (cV >> 2) & 0xff;
        uint8_t channelValue4   = (cV >> 6) & 0x0f;

        if (i < 4)
        {
            /*
             * First 4 channels encoded with 10 bits
             */
            uint8_t bitIndex = i + (i / 4);
            qsp->payload[bitIndex] |= (channelValue10 >> get10bitHighShift(i)) & (0x3ff >> get10bitHighShift(i));
            qsp->payload[bitIndex + 1] |= (channelValue10 << get10bitLowShift(i)) & 0xff << (8 - get10bitHighShift(i));
        }
        else if (i == 4 || i == 5)
        {
            /*
             * Next 2 with 8 bits
             */
            qsp->payload[i + 1] |= channelValue8;
        }
        else if (i == 6)
        {
            /*
             * And last 4 with 4 bits per channel
             */
            qsp->payload[7] |= (channelValue4 << 4) & B11110000;
        }
        else if (i == 7)
        {
            qsp->payload[7] |= channelValue4 & B00001111;
        }
        else if (i == 8)
        {
            qsp->payload[8] |= (channelValue4 << 4) & B11110000;
        }
        else if (i == 9)
        {
            qsp->payload[8] |= channelValue4 & B00001111;
        }
    }

    qsp->payloadLength = qspFrameLengths[QSP_FRAME_RC_DATA];
}

void qspClearPayload(QspConfiguration_t *qsp)
{
    for (uint8_t i = 0; i < QSP_PAYLOAD_LENGTH; i++)
    {
        qsp->payload[i] = 0;
    }
    qsp->payloadLength = 0;
}

/**
 * Init CRC with salt based on 4 byte bind key
 */
void qspInitCrc(QspConfiguration_t *qsp, uint8_t bindKey[]) {
    qsp->crc = 0;
    for (uint8_t i = 0; i < 4; i++) {
        qspComputeCrc(qsp, bindKey[i]);
    }
}

void qspDecodeIncomingFrame(
    QspConfiguration_t *qsp, 
    uint8_t incomingByte, 
    RxDeviceState_t *rxDeviceState, 
    TxDeviceState_t *txDeviceState,
    uint8_t bindKey[]
) {
    static uint8_t frameId;
    static uint8_t payloadLength;
    static uint8_t receivedPayload;
    static uint8_t receivedChannel;

    if (qsp->protocolState == QSP_STATE_IDLE)
    {
        qspInitCrc(qsp, bindKey);
        qspClearPayload(qsp);
        receivedPayload = 0;
        qsp->frameDecodingStartedAt = millis();

        //Frame ID and payload length
        qspComputeCrc(qsp, incomingByte);

        qsp->frameId = (incomingByte >> 4) & 0x0f;
        payloadLength = qspFrameLengths[qsp->frameId];
        receivedChannel = incomingByte & 0x0f;
        qsp->protocolState = QSP_STATE_FRAME_TYPE_RECEIVED;
    }
    else if (qsp->protocolState == QSP_STATE_FRAME_TYPE_RECEIVED)
    {
        if (receivedPayload >= QSP_PAYLOAD_LENGTH) {
            qsp->protocolState = QSP_STATE_IDLE;
        }

        //Now it's time for payload
        qspComputeCrc(qsp, incomingByte);
        qsp->payload[receivedPayload] = incomingByte;

        receivedPayload++;

        if (receivedPayload == payloadLength)
        {
            qsp->protocolState = QSP_STATE_PAYLOAD_RECEIVED;
            qsp->payloadLength = payloadLength;
        }
    }
    else if (qsp->protocolState == QSP_STATE_PAYLOAD_RECEIVED)
    {
        if (qsp->crc == incomingByte) {
            //CRC is correct
            qsp->onSuccessCallback(qsp, txDeviceState, rxDeviceState, receivedChannel);
        } else {
            qsp->onFailureCallback(qsp, txDeviceState, rxDeviceState);
        }

        // In both cases switch to listening for next preamble
        qsp->protocolState = QSP_STATE_IDLE;
    }
}

/**
 * Encode frame is corrent format and write to hardware
 */
void qspEncodeFrame(
    QspConfiguration_t *qsp, 
    uint8_t buffer[], 
    uint8_t *size, 
    uint8_t radioChannel,
    uint8_t bindKey[]
) {
    //Salt CRC with bind key
    qspInitCrc(qsp, bindKey);

    //Write frame type and length
    // We are no longer sending payload length, so 4 bits are now free for other usages
    // uint8_t data = qsp->payloadLength & 0x0f;
    uint8_t data = radioChannel;
    data |= (qsp->frameToSend << 4) & 0xf0;
    qspComputeCrc(qsp, data);
    buffer[0] = data;

    for (uint8_t i = 0; i < qsp->payloadLength; i++)
    {
        qspComputeCrc(qsp, qsp->payload[i]);
        buffer[i + 1] = qsp->payload[i];
    }

    buffer[qsp->payloadLength + 1] = qsp->crc;
    *size = qsp->payloadLength + 2; //Total length of QSP frame
}

void encodePingPayload(QspConfiguration_t *qsp, uint32_t currentMicros) {
    qsp->payload[0] = currentMicros & 255;
    qsp->payload[1] = (currentMicros >> 8) & 255;
    qsp->payload[2] = (currentMicros >> 16) & 255;
    qsp->payload[3] = (currentMicros >> 24) & 255;

    qsp->payloadLength = qspFrameLengths[QSP_FRAME_PING];
}

void encodeBindPayload(QspConfiguration_t *qsp, uint8_t bindKey[]) {

    for (uint8_t i = 0; i < qspFrameLengths[QSP_FRAME_PING]; i++) {
        qsp->payload[i] = bindKey[i];
    }

    qsp->payloadLength = qspFrameLengths[QSP_FRAME_PING];
}