//============================================================================
// Name        : GOES LRIT Decoder
// Author      : Lucas Teske
// Version     : 1.0
// Copyright   : Copyright 2016
// Description : GOES LRIT Decoder
//============================================================================

#include <iostream>
#include <memory.h>
#include <cstdint>
#include <SatHelper/sathelper.h>
#include "Display.h"
#include "ChannelWriter.h"
#include "ChannelDispatcher.h"
#include "StatisticsDispatcher.h"

#ifdef _WIN32
#include <winsock2.h>
#include <Ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#endif

using namespace std;


// Compilation parameters

#define Q(x) #x
#define QUOTE(x) Q(x)

#ifndef MAJOR_VERSION
#define MAJOR_VERSION unk
#endif
#ifndef MINOR_VERSION
#define MINOR_VERSION unk
#endif
#ifndef MAINT_VERSION
#define MAINT_VERSION unk
#endif
#ifndef GIT_SHA1
#define GIT_SHA1 unk
#endif


//#define DUMP_CORRUPTED_PACKETS
#define USE_LAST_FRAME_DATA
//#define DEBUG_MODE
#define HRIT

#define FRAMESIZE 1024
#define FRAMEBITS (FRAMESIZE * 8)
#define CODEDFRAMESIZE (FRAMEBITS * 2)
#define MINCORRELATIONBITS 46
#define RSBLOCKS 4
#define RSPARITYSIZE 32
#define RSPARITYBLOCK (RSPARITYSIZE * RSBLOCKS)
#define SYNCWORDSIZE 32
#define LASTFRAMEDATABITS 64
#define LASTFRAMEDATA (LASTFRAMEDATABITS / 8)
#define TIMEOUT 2
#define FLYWHEELRECHECK 4

#ifdef HRIT
const uint64_t UW0 = 0xfc4ef4fd0cc2df89;
const uint64_t UW2 = 0x25010b02f33d2076;
#else
const uint64_t UW0 = 0xfca2b63db00d9794;
const uint64_t UW2 = 0x035d49c24ff2686b;
#endif

int main(int argc, char **argv) {
#ifdef USE_LAST_FRAME_DATA
    uint8_t viterbiData[CODEDFRAMESIZE + LASTFRAMEDATABITS];
    uint8_t vitdecData[FRAMESIZE + LASTFRAMEDATA];
    uint8_t decodedData[FRAMESIZE + LASTFRAMEDATA];
    uint8_t lastFrameEnd[LASTFRAMEDATABITS];
#else
    uint8_t vitdecData[FRAMESIZE];
    uint8_t decodedData[FRAMESIZE];
#endif
    uint8_t codedData[CODEDFRAMESIZE];
    uint8_t rsCorrectedData[FRAMESIZE];
    uint8_t rsWorkBuffer[255];

    uint8_t syncWord[4];
    uint64_t droppedPackets = 0;
    uint64_t averageRSCorrections = 0;
    uint64_t averageVitCorrections = 0;
    uint64_t frameCount = 0;
    uint64_t lostPackets = 0;
    int64_t lostPacketsPerFrame[256];
    int64_t lastPacketCount[256];
    int64_t receivedPacketsPerFrame[256];
    uint32_t checkTime = 0;
    bool runUi = false;
    bool dump = false;
    Statistics statistics;
    bool isCorrupted = false;
    bool lastFrameOK = false;
    int flywheelCount = 0;

    SatHelper::Correlator correlator;
    SatHelper::PacketFixer packetFixer;
#ifdef USE_LAST_FRAME_DATA
    SatHelper::Viterbi27 viterbi(FRAMEBITS + LASTFRAMEDATABITS);
#else
    SatHelper::Viterbi27 viterbi(FRAMEBITS);
#endif
    SatHelper::ReedSolomon reedSolomon;
    SatHelper::DeRandomizer deRandomizer;
    ChannelWriter channelWriter("channels");

    Display display;

    std::cout << "xRIT Decoder - v" << QUOTE(MAJOR_VERSION) << "." << QUOTE(MINOR_VERSION) << "." << QUOTE(MAINT_VERSION) << " -- " << QUOTE(GIT_SHA1) << std::endl;
    std::cout << "  Compilation Date/Time: " << __DATE__ << " - " << __TIME__ << std::endl;
    std::cout << "  SatHelper Version: " << SatHelper::Info::GetVersion() << " - " << SatHelper::Info::GetGitSHA1() << std::endl;
    std::cout << "  SatHelper Compilation Date/Time: " << SatHelper::Info::GetCompilationDate() << " - " << SatHelper::Info::GetCompilationTime() << std::endl;
    std::cout << std::endl;

    reedSolomon.SetCopyParityToOutput(true);

    if (argc > 1) {
        std::string ui(argv[1]);
        if (ui == "display") {
            runUi = true;
        }
    }

    if (argc > 2) {
        std::string dumppacket(argv[2]);
        if (dumppacket == "dump") {
            dump = true;
        }
    }

    for (int i = 0; i < 256; i++) {
        lostPacketsPerFrame[i] = 0;
        lastPacketCount[i] = -1;
        receivedPacketsPerFrame[i] = -1;
    }

#ifdef USE_LAST_FRAME_DATA
    for (int i = 0; i < LASTFRAMEDATABITS; i++) {
        lastFrameEnd[i] = 128;
    }
#endif

    correlator.addWord(UW0);
    correlator.addWord(UW2);

    // Dispatchers
    ChannelDispatcher channelDispatcher;
    StatisticsDispatcher statisticsDispatcher;

    // Socket Init
    SatHelper::TcpServer tcpServer;
    cout << "Starting Demod Receiver at port 5000\n";
    tcpServer.Listen(5000);

    while (true) {
        cout << "Waiting for a client connection" << endl;

        SatHelper::TcpSocket client = tcpServer.Accept();
        cout << "Client connected!" << endl;

        if (runUi) {
            SatHelper::ScreenManager::Clear();
        }

        while (true) {
            uint32_t chunkSize = CODEDFRAMESIZE;
            try {
                checkTime = SatHelper::Tools::getTimestamp();
                client.WaitForData(CODEDFRAMESIZE, TIMEOUT);
                client.Receive((char *) codedData, chunkSize);

                if (flywheelCount == FLYWHEELRECHECK) {
                    lastFrameOK = false;
                    flywheelCount = 0;
                }

                // This reduces CPU Usage from 70% to 50% on my laptop
                if (!lastFrameOK) {
                    correlator.correlate(codedData, chunkSize);
                } else {
                    // If we got a good lock before, let's just check if the sync is in correct pos.
                    correlator.correlate(codedData, chunkSize / 16);
                    if (correlator.getHighestCorrelationPosition() != 0) {
                        // Oh no, that means something happened :/
                        std::cerr << "Something happened. Pos: " << correlator.getHighestCorrelationPosition() << std::endl;
                        correlator.correlate(codedData, chunkSize);
                        lastFrameOK = false;
                        flywheelCount = 0;
                    }
                }
                flywheelCount++;

                uint32_t word = correlator.getCorrelationWordNumber();
                uint32_t pos = correlator.getHighestCorrelationPosition();
                uint32_t corr = correlator.getHighestCorrelation();
                SatHelper::PhaseShift phaseShift = word == 0 ? SatHelper::PhaseShift::DEG_0 : SatHelper::PhaseShift::DEG_180;

                if (corr < MINCORRELATIONBITS) {
                    cerr << "Correlation didn't match criteria of " << MINCORRELATIONBITS << " bits." << endl;
                    continue;
                }

                // Sync Frame
                if (pos != 0) {
                    // Shift position
                    char *shiftedPosition = (char *) codedData + pos;
                    memcpy(codedData, shiftedPosition, CODEDFRAMESIZE - pos); // Copy from p to chunk size to start of codedData
                    chunkSize = pos; // Read needed bytes to fill a frame.
                    checkTime = SatHelper::Tools::getTimestamp();
                    while (client.AvailableData() < chunkSize) {
                        if (SatHelper::Tools::getTimestamp() - checkTime > TIMEOUT) {
                            throw SatHelper::ClientDisconnectedException();
                        }
                    }

                    client.Receive((char *) (codedData + CODEDFRAMESIZE - pos), chunkSize);
                }

#ifndef HRIT
                // Fix Phase Shift
                // HRIT uses differential encoding (NRZ-M) so it doesn't need phase correction
                packetFixer.fixPacket(codedData, CODEDFRAMESIZE, phaseShift, false);
#endif

#ifdef USE_LAST_FRAME_DATA
                // Shift data and add previous values.
                memcpy(viterbiData, lastFrameEnd, LASTFRAMEDATABITS);
                memcpy(viterbiData + LASTFRAMEDATABITS, codedData, CODEDFRAMESIZE);
#endif

                // Viterbi Decode
                //
#ifdef USE_LAST_FRAME_DATA
                viterbi.decode(viterbiData, decodedData);
#   ifdef HRIT
                SatHelper::DifferentialEncoding::nrzmDecode(decodedData, FRAMESIZE + LASTFRAMEDATA);
#   endif
#else
                viterbi.decode(codedData, decodedData);
#   ifdef HRIT
                SatHelper::DifferentialEncoding::nrzmDecode(decodedData, FRAMESIZE);
#   endif
#endif
                float signalErrors = viterbi.GetPercentBER();
                signalErrors = 100 - (signalErrors * 10);
                uint8_t signalQuality = signalErrors < 0 ? 0 : (uint8_t) signalErrors;

#ifdef USE_LAST_FRAME_DATA
                // Shift Back
                memmove(decodedData, decodedData + LASTFRAMEDATA / 2, FRAMESIZE);

                // Save last data
                memcpy(lastFrameEnd, viterbiData + CODEDFRAMESIZE, LASTFRAMEDATABITS);
#endif
                memcpy(syncWord, decodedData, 4);
                // DeRandomize Stream
                uint8_t skipsize = (SYNCWORDSIZE / 8);
                memcpy(vitdecData, decodedData, FRAMESIZE);
                memmove(decodedData, decodedData + skipsize, FRAMESIZE - skipsize);
                deRandomizer.DeRandomize(decodedData, FRAMESIZE - skipsize);

                averageVitCorrections += viterbi.GetBER();
                frameCount++;

                // Reed Solomon Error Correction
                int32_t derrors[4] = { 0, 0, 0, 0 };

                for (int i = 0; i < RSBLOCKS; i++) {
                    reedSolomon.deinterleave(decodedData, rsWorkBuffer, i, RSBLOCKS);
                    derrors[i] = reedSolomon.decode_ccsds(rsWorkBuffer);
                    reedSolomon.interleave(rsWorkBuffer, rsCorrectedData, i, RSBLOCKS);
                }

                if (derrors[0] == -1 && derrors[1] == -1 && derrors[2] == -1 && derrors[3] == -1) {
                    droppedPackets++;
#ifdef DUMP_CORRUPTED_PACKETS
                    channelWriter.dumpCorruptedPacket(codedData, CODEDFRAMESIZE, 0);
                    channelWriter.dumpCorruptedPacket(vitdecData, FRAMESIZE, 1);
                    channelWriter.dumpCorruptedPacket(rsCorrectedData, FRAMESIZE, 2);
                    channelWriter.dumpCorruptedPacketStatistics(viterbi.GetBER(), corr, derrors);
#endif
                    isCorrupted = true;
                    lastFrameOK = false;
                } else {
                    averageRSCorrections += derrors[0] != -1 ? derrors[0] : 0;
                    averageRSCorrections += derrors[1] != -1 ? derrors[1] : 0;
                    averageRSCorrections += derrors[2] != -1 ? derrors[2] : 0;
                    averageRSCorrections += derrors[3] != -1 ? derrors[3] : 0;
                    isCorrupted = false;
                    lastFrameOK = true;
                }

                // Packet Header Filtering
                //uint8_t versionNumber = (*rsCorrectedData) & 0xC0 >> 6;
                uint8_t scid = ((*rsCorrectedData) & 0x3F) << 2 | (*(rsCorrectedData + 1) & 0xC0) >> 6;
                uint8_t vcid = (*(rsCorrectedData + 1)) & 0x3F;

                // Packet Counter from Packet
                uint32_t counter = *((uint32_t *) (rsCorrectedData + 2));
                counter = SatHelper::Tools::swapEndianess(counter);
                counter &= 0xFFFFFF00;
                counter = counter >> 8;

                uint8_t phaseCorr = phaseShift == SatHelper::PhaseShift::DEG_180 ? 180 : 0;
                uint16_t partialVitCorrections = (uint16_t) (averageVitCorrections / frameCount);
                uint8_t partialRSCorrections = (uint8_t) (averageRSCorrections / frameCount);

                if (!isCorrupted) {
                    if (dump) {
                        channelWriter.writeChannel(rsCorrectedData, FRAMESIZE - RSPARITYBLOCK - (SYNCWORDSIZE / 8), vcid);
                    }
                    channelDispatcher.add((char *) rsCorrectedData, FRAMESIZE - RSPARITYBLOCK - (SYNCWORDSIZE / 8));

                    if (lastPacketCount[vcid] + 1 != counter && lastPacketCount[vcid] > -1) {
                        int lostCount = counter - lastPacketCount[vcid] - 1;
                        lostPackets += lostCount;
                        lostPacketsPerFrame[vcid] += lostCount;
                    }

                    lastPacketCount[vcid] = counter;
                    receivedPacketsPerFrame[vcid] = receivedPacketsPerFrame[vcid] == -1 ? 1 : receivedPacketsPerFrame[vcid] + 1;

                    statistics.update(scid, vcid, (uint64_t) counter, (int16_t) viterbi.GetBER(), FRAMEBITS, derrors, signalQuality, corr, phaseCorr,
                            lostPackets, partialVitCorrections, partialRSCorrections, droppedPackets, receivedPacketsPerFrame, lostPacketsPerFrame, frameCount,
                            syncWord, true);

                    if (runUi) {
                        display.update(scid, vcid, (uint64_t) counter, (int16_t) viterbi.GetBER(), FRAMEBITS, derrors, signalQuality, corr, phaseCorr,
                                lostPackets, partialVitCorrections, partialRSCorrections, droppedPackets, receivedPacketsPerFrame, lostPacketsPerFrame,
                                frameCount, syncWord, true);

                        display.show();
                    }
                } else {
                    statistics.update(0, 0, 0, (int16_t) viterbi.GetBER(), FRAMEBITS, derrors, 0, corr, 0, lostPackets, partialVitCorrections,
                            partialRSCorrections, droppedPackets, receivedPacketsPerFrame, lostPacketsPerFrame, frameCount, syncWord, false);

                    if (runUi) {
                        display.update(0, 0, 0, (int16_t) viterbi.GetBER(), FRAMEBITS, derrors, signalQuality, corr, phaseCorr, lostPackets,
                                partialVitCorrections, partialRSCorrections, droppedPackets, receivedPacketsPerFrame, lostPacketsPerFrame, frameCount, syncWord,
                                false);

                        display.show();
                    }
                }

                statisticsDispatcher.Update(statistics);
                statisticsDispatcher.Work();

            } catch (SatHelper::SocketException &e) {
                cerr << endl;
                cerr << "Client disconnected" << endl;
                cerr << "   " << e.what() << endl;
                break;
            }
        }

        client.Close();
    }
    return 0;
}
