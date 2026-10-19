/*++

Copyright (c) Microsoft Corporation All Rights Reserved

Module Name:

    micarraywavtable.h

Abstract:-

    Declaration of wave miniport tables for the mic array.

--*/

#ifndef _SIMPLEAUDIOSAMPLE_MICARRAYWAVTABLE_H_
#define _SIMPLEAUDIOSAMPLE_MICARRAYWAVTABLE_H_

//
// Mic range.
//
// WOMIC 端点被改造为 16-bit 单声道麦克风：
//   * MICARRAY_RAW_CHANNELS / MICARRAY_DEVICE_MAX_CHANNELS : 2 -> 1
//   * 位深 32 -> 16（原宏名 MICARRAY_32_BITS_PER_SAMPLE_PCM 会误导，
//     改名为 MICARRAY_BITS_PER_SAMPLE_PCM；下面的 DataRange 同步改用新名）
//
#define MICARRAY_RAW_CHANNELS                   1       // Channels for raw mode
#define MICARRAY_DEVICE_MAX_CHANNELS            1       // Max channels overall
#define MICARRAY_BITS_PER_SAMPLE_PCM            16      // 16 Bits Per Sample
#define MICARRAY_RAW_SAMPLE_RATE                48000   // Raw sample rate
#define MICARRAY_ALT_SAMPLE_RATE                44100   // 第二种支持的采样率

//
// Max # of pin instances.
//
#define MICARRAY_MAX_INPUT_STREAMS              1

//
// 一帧的字节数：16-bit 单声道 = 2。与 womic_ctl.h 的 WOMIC_FRAME_BYTES 必须一致，
// 否则环形缓冲的帧计数与 DMA 字节位移会对不上。
//
#define MICARRAY_FRAME_BYTES                    2

//=============================================================================
//
// 格式表：16-bit 单声道 48 kHz 与 44.1 kHz。
//
// 【为什么 48 kHz 放在 [0]】
//   1) Windows 音频引擎（WASAPI 共享模式 / AudioEndpointBuilder）的默认混音格式
//      绝大多数机器上是 48 kHz，放在 [0] 能让"客户端给了一个宽泛范围"这类的
//      协商请求默认落在 48 kHz，与系统其余部分一致；
//   2) MicArrayPinSupportedDeviceModes[0] 的 default format 引用的是 [0]，
//      RAW 模式的默认格式因此保持 48 kHz，与 MICARRAY_RAW_SAMPLE_RATE 一致；
//   3) 改动面最小：原表第 0 项就是 48 kHz。
//
// 注意：把 44.1 kHz 加进这张表本身是不够的 —— minwavert.cpp 的
// DataRangeIntersection 原本硬编码 `*resultantFormat = *MicArrayPinSupportedDeviceFormats;`
// （即永远只返回 [0]）。那一处必须一起改成"遍历整张表做范围交叉"，
// 否则 Windows 永远只能拿到 48 kHz 一项（坑 2 / N-1）。
//
static
KSDATAFORMAT_WAVEFORMATEXTENSIBLE MicArrayPinSupportedDeviceFormats[] =
{
    // [0] 48 KHz 16-bit mono
    {
        {
            sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE),
            0,
            0,
            0,
            STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
            STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
            STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)
        },
        {
            {
                WAVE_FORMAT_EXTENSIBLE,
                MICARRAY_RAW_CHANNELS,
                48000,
                96000,          // nAvgBytesPerSec = 48000 * 1ch * 2B
                MICARRAY_FRAME_BYTES,
                MICARRAY_BITS_PER_SAMPLE_PCM,
                sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)
            },
            MICARRAY_BITS_PER_SAMPLE_PCM,   // wValidBitsPerSample
            KSAUDIO_SPEAKER_MONO,
            STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM)
        }
    },

    // [1] 44.1 KHz 16-bit mono
    {
        {
            sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE),
            0,
            0,
            0,
            STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
            STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
            STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)
        },
        {
            {
                WAVE_FORMAT_EXTENSIBLE,
                MICARRAY_RAW_CHANNELS,
                44100,
                88200,          // nAvgBytesPerSec = 44100 * 1ch * 2B
                MICARRAY_FRAME_BYTES,
                MICARRAY_BITS_PER_SAMPLE_PCM,
                sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)
            },
            MICARRAY_BITS_PER_SAMPLE_PCM,   // wValidBitsPerSample
            KSAUDIO_SPEAKER_MONO,
            STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM)
        }
    }
};

//
// Supported modes (only on streaming pins).
//
static
MODE_AND_DEFAULT_FORMAT MicArrayPinSupportedDeviceModes[] =
{
    {
        STATIC_AUDIO_SIGNALPROCESSINGMODE_RAW,
        &MicArrayPinSupportedDeviceFormats[0].DataFormat
    }
};

//
// The entries here must follow the same order as the filter's pin
// descriptor array.
//
static
PIN_DEVICE_FORMATS_AND_MODES MicArrayPinDeviceFormatsAndModes[] =
{
    {
        BridgePin,
        NULL,
        0,
        NULL,
        0
    },
    {
        SystemCapturePin,
        MicArrayPinSupportedDeviceFormats,
        SIZEOF_ARRAY(MicArrayPinSupportedDeviceFormats),
        MicArrayPinSupportedDeviceModes,
        SIZEOF_ARRAY(MicArrayPinSupportedDeviceModes)
    }
};

//=============================================================================
// Data ranges
//
// See CMiniportWaveRT::DataRangeIntersection.
//
// 每个支持的采样率给一条独立的 KSDATARANGE_AUDIO（min == max 精确标定），
// 而不是写一条 min=44100/max=48000 的宽范围 —— 这样 KS 自己做
// KSPROPERTY_PIN_DATARANGES 枚举时，也能明确看到两个离散的采样率。
//
// KSDATARANGE_ATTRIBUTES 标志标在"紧邻属性列表之前"的那一条 range 上
// （即 [1]），这是 KS 的约定：该标志表示"本 data range 之后跟着一个属性列表"。
//
static
KSDATARANGE_AUDIO MicArrayPinDataRangesRawStream[] =
{
    // 48 kHz / 16-bit / mono
    {
        {
            sizeof(KSDATARANGE_AUDIO),
            0,
            0,
            0,
            STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
            STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
            STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)
        },
        MICARRAY_RAW_CHANNELS,
        MICARRAY_BITS_PER_SAMPLE_PCM,
        MICARRAY_BITS_PER_SAMPLE_PCM,
        MICARRAY_RAW_SAMPLE_RATE,
        MICARRAY_RAW_SAMPLE_RATE
    },

    // 44.1 kHz / 16-bit / mono
    {
        {
            sizeof(KSDATARANGE_AUDIO),
            KSDATARANGE_ATTRIBUTES,         // An attributes list follows this data range
            0,
            0,
            STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
            STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
            STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)
        },
        MICARRAY_RAW_CHANNELS,
        MICARRAY_BITS_PER_SAMPLE_PCM,
        MICARRAY_BITS_PER_SAMPLE_PCM,
        MICARRAY_ALT_SAMPLE_RATE,
        MICARRAY_ALT_SAMPLE_RATE
    },
};

static
PKSDATARANGE MicArrayPinDataRangePointersStream[] =
{
    // All supported device formats should be listed in the DataRange.
    PKSDATARANGE(&MicArrayPinDataRangesRawStream[0]),
    PKSDATARANGE(&MicArrayPinDataRangesRawStream[1]),
    PKSDATARANGE(&PinDataRangeAttributeList),
};

//=============================================================================
static
KSDATARANGE MicArrayPinDataRangesBridge[] =
{
    {
        sizeof(KSDATARANGE),
        0,
        0,
        0,
        STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
        STATICGUIDOF(KSDATAFORMAT_SUBTYPE_ANALOG),
        STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE)
    }
};

static
PKSDATARANGE MicArrayPinDataRangePointersBridge[] =
{
    &MicArrayPinDataRangesBridge[0]
};

//=============================================================================
static
PCPIN_DESCRIPTOR MicArrayWaveMiniportPins[] =
{
    // Wave In Bridge Pin (Capture - From Topology) KSPIN_WAVE_BRIDGE
    {
        0,
        0,
        0,
        NULL,
        {
            0,
            NULL,
            0,
            NULL,
            SIZEOF_ARRAY(MicArrayPinDataRangePointersBridge),
            MicArrayPinDataRangePointersBridge,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_NONE,
            &KSCATEGORY_AUDIO,
            NULL,
            0
        }
    },
    // Wave In Streaming Pin (Capture) KSPIN_WAVE_HOST
    {
        MICARRAY_MAX_INPUT_STREAMS,
        MICARRAY_MAX_INPUT_STREAMS,
        0,
        NULL,
        {
            0,
            NULL,
            0,
            NULL,
            SIZEOF_ARRAY(MicArrayPinDataRangePointersStream),
            MicArrayPinDataRangePointersStream,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_SINK,
            &KSCATEGORY_AUDIO,
            &KSAUDFNAME_RECORDING_CONTROL,
            0
        }
    }
};

//=============================================================================
static
PCNODE_DESCRIPTOR MicArrayWaveMiniportNodes[] =
{
    // KSNODE_WAVE_ADC
    {
        0,                      // Flags
        NULL,                   // AutomationTable
        &KSNODETYPE_ADC,        // Type
        NULL                    // Name
    }
};

//=============================================================================
static
PCCONNECTION_DESCRIPTOR MicArrayWaveMiniportConnections[] =
{
    { PCFILTER_NODE,        KSPIN_WAVE_BRIDGE,      KSNODE_WAVE_ADC,     1 },
    { KSNODE_WAVE_ADC,      0,                      PCFILTER_NODE,       KSPIN_WAVEIN_HOST },
};

//=============================================================================

static
SIMPLEAUDIOSAMPLE_PROPERTY_ITEM PropertiesMicArrayWaveFilter[] =
{
    {
        {
            &KSPROPSETID_General,
            KSPROPERTY_GENERAL_COMPONENTID,
            KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT,
            PropertyHandler_WaveFilter,
        },
        0,
        0,
        NULL,
        NULL,
        NULL,
        NULL,
        0
    },
    {
        {
            &KSPROPSETID_Pin,
            KSPROPERTY_PIN_PROPOSEDATAFORMAT,
            KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT,
            PropertyHandler_WaveFilter,
        },
        0,
        0,
        NULL,
        NULL,
        NULL,
        NULL,
        0
    },
    {
        {
            &KSPROPSETID_Pin,
            KSPROPERTY_PIN_PROPOSEDATAFORMAT2,
            KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT,
            PropertyHandler_WaveFilter,
        },
        0,
        0,
        NULL,
        NULL,
        NULL,
        NULL,
        0
    },
};

DEFINE_PCAUTOMATION_TABLE_PROP(AutomationMicArrayWaveFilter, PropertiesMicArrayWaveFilter);

//=============================================================================
static
PCFILTER_DESCRIPTOR MicArrayWaveMiniportFilterDescriptor =
{
    0,                                              // Version
    &AutomationMicArrayWaveFilter,                  // AutomationTable
    sizeof(PCPIN_DESCRIPTOR),                       // PinSize
    SIZEOF_ARRAY(MicArrayWaveMiniportPins),         // PinCount
    MicArrayWaveMiniportPins,                       // Pins
    sizeof(PCNODE_DESCRIPTOR),                      // NodeSize
    SIZEOF_ARRAY(MicArrayWaveMiniportNodes),        // NodeCount
    MicArrayWaveMiniportNodes,                      // Nodes
    SIZEOF_ARRAY(MicArrayWaveMiniportConnections),  // ConnectionCount
    MicArrayWaveMiniportConnections,                // Connections
    0,                                              // CategoryCount
    NULL                                            // Categories  - use defaults (audio, render, capture)
};

#endif // _SIMPLEAUDIOSAMPLE_MICARRAYWAVTABLE_H_
