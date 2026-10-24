/*++

Copyright (c) Microsoft Corporation All Rights Reserved

Module Name:

    minwavertstream.h

Abstract:

    Definition of wavert miniport class.
--*/

#ifndef _SIMPLEAUDIOSAMPLE_MINWAVERTSTREAM_H_
#define _SIMPLEAUDIOSAMPLE_MINWAVERTSTREAM_H_

#include "savedata.h"

//
// MICLINK 控制设备契约（Source\Inc\miclink\miclink_ctl.h）。
// 它会顺带拉入三方共享的 miclink_ioctl.h / miclink_ring.h / miclink_dsp.h ——
// miclink_declck（去咔哒状态）就定义在后者里。
//
#include "miclink/miclink_ctl.h"

//
// 注意：ToneGenerator.h 的 include、`ToneGenerator` 类型的成员，以及
// Utilities\ToneGenerator.cpp / .h 本身都已被删除（缺陷 D-04：正弦发生器
// 在移除正弦注入后已无任何调用点，继续编译属于死代码，DisableToneGenerator
// 注册表项也会误导运维以为还能切回正弦）。
// 本驱动不再生成任何正弦波；捕获数据全部来自上面的 MICLINK 环形缓冲。
//

//
// Structure to store notifications events in a protected list
//
typedef struct _NotificationListEntry
{
    LIST_ENTRY  ListEntry;
    PKEVENT     NotificationEvent;
} NotificationListEntry;

EXT_CALLBACK   TimerNotifyRT;

//=============================================================================
// Referenced Forward
//=============================================================================
class CMiniportWaveRT;
typedef CMiniportWaveRT *PCMiniportWaveRT;

//=============================================================================
// Classes
//=============================================================================
///////////////////////////////////////////////////////////////////////////////
// CMiniportWaveRTStream 
// 
class CMiniportWaveRTStream : 
    public IDrmAudioStream,
    public IMiniportWaveRTStreamNotification,
    public IMiniportWaveRTInputStream,
    public IMiniportWaveRTOutputStream,
    public CUnknown
{
protected:
    PPORTWAVERTSTREAM           m_pPortStream;
    LIST_ENTRY                  m_NotificationList;
    PEX_TIMER                   m_pNotificationTimer;
    ULONG                       m_ulNotificationIntervalMs;
    ULONG                       m_ulCurrentWritePosition;
    LONG                        m_IsCurrentWritePositionUpdated;
    
public:
    DECLARE_STD_UNKNOWN();
    DEFINE_STD_CONSTRUCTOR(CMiniportWaveRTStream);
    ~CMiniportWaveRTStream();

    IMP_IMiniportWaveRTStream;
    IMP_IMiniportWaveRTStreamNotification;
    IMP_IMiniportWaveRTInputStream;
    IMP_IMiniportWaveRTOutputStream;
    IMP_IMiniportWaveRT;
    IMP_IDrmAudioStream;

    NTSTATUS                    Init
    ( 
        _In_  PCMiniportWaveRT    Miniport,
        _In_  PPORTWAVERTSTREAM   Stream,
        _In_  ULONG               Channel,
        _In_  BOOLEAN             Capture,
        _In_  PKSDATAFORMAT       DataFormat,
        _In_  GUID                SignalProcessingMode
    );

    // Friends
    friend class                CMiniportWaveRT;
    friend EXT_CALLBACK         TimerNotifyRT;
protected:
    CMiniportWaveRT*            m_pMiniport;
    ULONG                       m_ulPin;
    BOOLEAN                     m_bCapture;
    BOOLEAN                     m_bUnregisterStream;
    ULONG                       m_ulDmaBufferSize;
    BYTE*                       m_pDmaBuffer;
    ULONG                       m_ulNotificationsPerBuffer;
    KSSTATE                     m_KsState;
    PKTIMER                     m_pTimer;
    PRKDPC                      m_pDpc;
    ULONGLONG                   m_ullPlayPosition;
    ULONGLONG                   m_ullWritePosition;
    ULONGLONG                   m_ullLinearPosition;
    ULONGLONG                   m_ullPresentationPosition;
    ULONG                       m_ulLastOsReadPacket;
    ULONG                       m_ulLastOsWritePacket;
    LONGLONG                    m_llPacketCounter;
    ULONGLONG                   m_ullDmaTimeStamp;
    LARGE_INTEGER               m_ullPerformanceCounterFrequency;
    ULONGLONG                   m_hnsElapsedTimeCarryForward;
    ULONGLONG                   m_ullLastDPCTimeStamp;
    ULONGLONG                   m_hnsDPCTimeCarryForward;
    ULONG                       m_byteDisplacementCarryForward;
    ULONG                       m_ulDmaMovementRate;
    BOOL                        m_bLfxEnabled;
    PBOOL                       m_pbMuted;
    PLONG                       m_plVolumeLevel;
    PLONG                       m_plPeakMeter;
    PWAVEFORMATEXTENSIBLE       m_pWfExt;
    ULONG                       m_ulContentId;
    CSaveData                   m_SaveData;

    //
    // 去咔哒状态。它会被 WriteBytes 在 DISPATCH_LEVEL 的 DPC 中使用，
    // 因此必须是内联成员（本对象本身分配在 NonPagedPool），
    // 并且只能用 miclink_declck_* 这组纯整数实现（不得引入浮点）。
    //
    miclink_declck                m_Declck;

    GUID                        m_SignalProcessingMode;
    BOOLEAN                     m_bEoSReceived;
    BOOLEAN                     m_bLastBufferRendered;
    KSPIN_LOCK                  m_PositionSpinLock;
    // Member variable as config params for tone generator
    ULONG                       m_ulHostCaptureToneFrequency;
    // If abs(m_dwHostCaptureToneAmplitude) + abs(m_dwHostCaptureToneDCValue) > 100
    // m_dwHostCaptureToneDCValue will be compensated to make the sum equal to 100
    DWORD                       m_dwHostCaptureToneAmplitude;   // must be between -100 to 100
    DWORD                       m_dwLoopbackCaptureToneAmplitude; // must be between -100 to 100
    DWORD                       m_dwHostCaptureToneDCOffset;   // must be between -100 to 100
    DWORD                       m_dwLoopbackCaptureToneDCOffset; // must be between -100 to 100
    DWORD                       m_dwHostCaptureToneInitialPhase;   // must be between -31416 to 31416
    DWORD                       m_dwLoopbackCaptureToneInitialPhase; // must be between -31416 to 31416
    // Member variable as config params for tone generator

public:

    //presentation
    NTSTATUS GetPresentationPosition
    (
        _Out_  KSAUDIO_PRESENTATION_POSITION *_pPresentationPosition
    );
        
    ULONG GetCurrentWaveRTWritePosition() 
    {
        return m_ulCurrentWritePosition;
    };

    // To support simple underrun validation.
    BOOL IsCurrentWaveRTWritePositionUpdated() 
    {
        return InterlockedExchange(&m_IsCurrentWritePositionUpdated, 0) ? TRUE : FALSE;
    };

    GUID GetSignalProcessingMode()
    {
        return m_SignalProcessingMode;
    }
    
private:

    //
    // Helper functions.
    //
    
#pragma code_seg()

    VOID WriteBytes
    (
        _In_ ULONG ByteDisplacement
    );
        
    VOID ReadBytes
    (
        _In_ ULONG ByteDisplacement
    );
    
    VOID UpdatePosition
    (
        _In_ LARGE_INTEGER ilQPC
    );
    
    NTSTATUS SetCurrentWritePositionInternal
    (
        _In_  ULONG ulCurrentWritePosition
    );
    
    NTSTATUS GetPositions
    (
        _Out_opt_  ULONGLONG *      _pullLinearBufferPosition, 
        _Out_opt_  ULONGLONG *      _pullPresentationPosition, 
        _Out_opt_  LARGE_INTEGER *  _pliQPCTime
    );

    NTSTATUS ReadRegistrySettings();
    
};
typedef CMiniportWaveRTStream *PCMiniportWaveRTStream;
#endif // _SIMPLEAUDIOSAMPLE_MINWAVERTSTREAM_H_

