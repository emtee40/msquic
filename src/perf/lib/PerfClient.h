/*++

    Copyright (c) Microsoft Corporation.
    Licensed under the MIT License.

Abstract:

    Defines the types used for the performance client-side.

--*/

#pragma once

#include "PerfHelpers.h"
#include "PerfBase.h"
#include "PerfCommon.h"

struct PerfClientConnection;
struct PerfClientStream;
struct PerfClientWorker;
class PerfClient;

struct PerfClientConnection : public MsQuicConnection {
    CXPLAT_LIST_ENTRY Link; // For Worker's connection queue
    PerfClient& Client;
    PerfClientWorker& Worker;
    uint64_t TotalStreamCount {0};
    uint64_t ActiveStreamCount {0};
    PerfClientConnection(_In_ const MsQuicRegistration& Registration, _In_ PerfClient& Client, _In_ PerfClientWorker& Worker)
        : MsQuicConnection(Registration, CleanUpAutoDelete, s_ConnectionCallback), Client(Client), Worker(Worker) { }
    QUIC_STATUS ConnectionCallback(_Inout_ QUIC_CONNECTION_EVENT* Event);
    static QUIC_STATUS s_ConnectionCallback(MsQuicConnection* /* Conn */, void* Context, QUIC_CONNECTION_EVENT* Event) {
        return ((PerfClientConnection*)Context)->ConnectionCallback(Event);
    }
    QUIC_STATUS StreamCallback(_In_ PerfClientStream* StrmContext, _In_ HQUIC StreamHandle, _Inout_ QUIC_STREAM_EVENT* Event);
    void StartNewStream(bool DelaySend = false);
    void SendData(_In_ PerfClientStream* Stream);
};

struct PerfClientStream {
    PerfClientStream(_In_ PerfClientConnection& Connection)
        : Connection{Connection} { }
    ~PerfClientStream() {
        if (Handle) {
            MsQuic->StreamClose(Handle);
        }
    }
    static QUIC_STATUS
    s_StreamCallback(HQUIC Stream, void* Context, QUIC_STREAM_EVENT* Event) {
        return ((PerfClientStream*)Context)->Connection.StreamCallback((PerfClientStream*)Context, Stream, Event);
    }
    PerfClientConnection& Connection;
    HQUIC Handle {nullptr};
    uint64_t StartTime {CxPlatTimeUs64()};
    uint64_t OutstandingBytes {0};
    uint64_t BytesSent {0};
    uint64_t BytesCompleted {0};
    bool Complete {false};
    QUIC_BUFFER LastBuffer;
#if DEBUG
    uint8_t Padding[12];
#endif
};

struct PerfClientWorker {
    PerfClient* Client {nullptr};
    CxPlatLock Lock;
    CXPLAT_THREAD Thread;
    CxPlatEvent WakeEvent;
    bool ThreadStarted {false};
    uint16_t Processor {UINT16_MAX};
    uint64_t TotalConnectionCount {0};
    uint64_t ConnnectedConnectionCount {0};
    uint64_t ActiveConnectionCount {0};
    uint64_t StartedRequests {0};
    uint64_t SendCompletedRequests {0};
    uint64_t CompletedRequests {0};
    UniquePtr<char[]> Target;
    QuicAddr LocalAddr;
    QuicAddr RemoteAddr;
    QuicPoolAllocator<PerfClientConnection> ConnectionAllocator;
    QuicPoolAllocator<PerfClientStream> StreamAllocator;
    PerfClientWorker() { }
    ~PerfClientWorker() { WaitForThread(); }
    void Uninitialize() { WaitForThread(); }
    void QueueNewConnection() {
        InterlockedIncrement((unsigned*)&TotalConnectionCount);
        WakeEvent.Set();
    }
    static CXPLAT_THREAD_CALLBACK(s_WorkerThread, Context) {
        ((PerfClientWorker*)Context)->WorkerThread();
        CXPLAT_THREAD_RETURN(QUIC_STATUS_SUCCESS);
    }
private:
    void WaitForThread() {
        if (ThreadStarted) {
            WakeEvent.Set();
            CxPlatThreadWait(&Thread);
            CxPlatThreadDelete(&Thread);
            ThreadStarted = false;
        }
    }
    void StartNewConnection();
    void WorkerThread();
};

class PerfClient : public PerfBase {
public:

    PerfClient() {
        CxPlatZeroMemory(LocalAddresses, sizeof(LocalAddresses));
        for (uint32_t i = 0; i < PERF_MAX_THREAD_COUNT; ++i) {
            Workers[i].Client = this;
        }
    }

    ~PerfClient() override {
        Running = false;
    }

    QUIC_STATUS
    Init(
        _In_ int argc,
        _In_reads_(argc) _Null_terminated_ char* argv[]
        ) override;

    QUIC_STATUS Start(_In_ CXPLAT_EVENT* StopEvent) override;
    QUIC_STATUS Wait(_In_ int Timeout) override;
    void GetExtraDataMetadata(_Out_ PerfExtraDataMetadata* Result) override;

    QUIC_STATUS
    GetExtraData(
        _Out_writes_bytes_(*Length) uint8_t* Data,
        _Inout_ uint32_t* Length
        ) override;

    MsQuicRegistration Registration {
        "perf-client",
        PerfDefaultExecutionProfile,
        true};
    MsQuicConfiguration Configuration {
        Registration,
        MsQuicAlpn(PERF_ALPN),
        MsQuicSettings()
            .SetDisconnectTimeoutMs(PERF_DEFAULT_DISCONNECT_TIMEOUT)
            .SetIdleTimeoutMs(PERF_DEFAULT_IDLE_TIMEOUT)
            .SetSendBufferingEnabled(false)
            .SetCongestionControlAlgorithm(PerfDefaultCongestionControl)
            .SetEcnEnabled(PerfDefaultEcnEnabled)
            .SetEncryptionOffloadAllowed(PerfDefaultQeoAllowed),
        MsQuicCredentialConfig(
            QUIC_CREDENTIAL_FLAG_CLIENT |
            QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION)};
    // Target parameters
    UniquePtr<char[]> Target;
    QUIC_ADDRESS_FAMILY TargetFamily {QUIC_ADDRESS_FAMILY_UNSPEC};
    uint16_t TargetPort {PERF_DEFAULT_PORT};
    uint32_t CibirIdLength {0};
    uint8_t CibirId[7]; // {offset, values}
    uint8_t IncrementTarget {FALSE};
    // Local execution parameters
    QUIC_ADDR LocalAddresses[PERF_MAX_CLIENT_PORT_COUNT];
    uint32_t MaxLocalAddrCount {PERF_MAX_CLIENT_PORT_COUNT};
    uint32_t WorkerCount;
    uint8_t AffinitizeWorkers {FALSE};
    uint8_t SpecificLocalAddresses {FALSE};
#ifdef QUIC_COMPARTMENT_ID
    uint16_t CompartmentId {UINT16_MAX};
#endif
    // General parameters
    uint8_t UseEncryption {TRUE};
    uint8_t UsePacing {TRUE};
    uint8_t UseSendBuffering {FALSE};
    uint8_t PrintStats {FALSE};
    uint8_t PrintStreamStats {FALSE};
    uint8_t PrintLatencyStats {FALSE};
    // Scenario parameters
    uint32_t ConnectionCount {1};
    uint32_t StreamCount {0};
    uint32_t IoSize {PERF_DEFAULT_IO_SIZE};
    uint32_t Upload {0};
    uint32_t Download {0};
    uint8_t Timed {FALSE};
    uint32_t HandshakeWaitTime {0};
    uint8_t SendInline {FALSE};
    uint8_t RepeateConnections {FALSE};
    uint8_t RepeatStreams {FALSE};
    uint32_t RunTime {0};

    struct PerfIoBuffer {
        QUIC_BUFFER* Buffer {nullptr};
        operator QUIC_BUFFER* () noexcept { return Buffer; }
        ~PerfIoBuffer() noexcept { if (Buffer) { CXPLAT_FREE(Buffer, QUIC_POOL_PERF); } }
        void Init(uint32_t IoSize, uint64_t Initial) noexcept {
            Buffer = (QUIC_BUFFER*)CXPLAT_ALLOC_NONPAGED(sizeof(QUIC_BUFFER) + sizeof(uint64_t) + IoSize, QUIC_POOL_PERF);
            Buffer->Length = sizeof(uint64_t) + IoSize;
            Buffer->Buffer = (uint8_t*)(Buffer + 1);
            *(uint64_t*)(Buffer->Buffer) = CxPlatByteSwapUint64(Initial);
            for (uint32_t i = 0; i < IoSize; ++i) {
                Buffer->Buffer[sizeof(uint64_t) + i] = (uint8_t)i;
            }
        }
    } RequestBuffer;

    CXPLAT_EVENT* CompletionEvent {nullptr};
    uint64_t CachedCompletedRequests {0};
    UniquePtr<uint32_t[]> LatencyValues {nullptr};
    uint64_t MaxLatencyIndex {0};
    PerfClientWorker Workers[PERF_MAX_THREAD_COUNT];
    bool Running {true};

    uint32_t GetConnectedConnections() const {
        uint32_t ConnectedConnections = 0;
        for (uint32_t i = 0; i < WorkerCount; ++i) {
            ConnectedConnections += Workers[i].ConnnectedConnectionCount;
        }
        return ConnectedConnections;
    }
    uint32_t GetActiveConnections() const {
        uint32_t ActiveConnections = 0;
        for (uint32_t i = 0; i < WorkerCount; ++i) {
            ActiveConnections += Workers[i].ActiveConnectionCount;
        }
        return ActiveConnections;
    }
    uint64_t GetStartedRequests() const {
        uint64_t StartedRequests = 0;
        for (uint32_t i = 0; i < WorkerCount; ++i) {
            StartedRequests += Workers[i].StartedRequests;
        }
        return StartedRequests;
    }
    uint64_t GetSendCompletedRequests() const {
        uint64_t SendCompletedRequests = 0;
        for (uint32_t i = 0; i < WorkerCount; ++i) {
            SendCompletedRequests += Workers[i].SendCompletedRequests;
        }
        return SendCompletedRequests;
    }
    uint64_t GetCompletedRequests() const {
        uint64_t CompletedRequests = 0;
        for (uint32_t i = 0; i < WorkerCount; ++i) {
            CompletedRequests += Workers[i].CompletedRequests;
        }
        return CompletedRequests;
    }
};
