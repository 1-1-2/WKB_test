/* opusdec.c — Opus 解码器封装（静态优先 / dll 回退） */
#include "opusdec.h"
#include <string.h>

#ifdef MICLINK_HAVE_OPUS
#include <opus.h>
static OpusDecoder *g_dec = NULL;
#else
typedef struct OpusDecoder OpusDecoder;
typedef OpusDecoder *(*fn_create)(int Fs, int channels, int *error);
typedef int   (*fn_decode)(OpusDecoder *st, const unsigned char *data,
                           int len, int16_t *pcm, int frame_size, int decode_fec);
typedef void  (*fn_destroy)(OpusDecoder *st);
static struct {
    HMODULE   mod;
    fn_create create;
    fn_decode decode;
    fn_destroy destroy;
    OpusDecoder *st;
} g_dll;
#endif

static int g_rate = 0;

const char *opusdec_source(void)
{
#ifdef MICLINK_HAVE_OPUS
    /* 静态链接：编进来就一定可用，与「当前是否已 open」无关 */
    return "static";
#else
    if (!g_dll.mod) {
        g_dll.mod = LoadLibraryW(L"opus.dll");
        if (g_dll.mod) {
            g_dll.create  = (fn_create)(void *)GetProcAddress(g_dll.mod, "opus_decoder_create");
            g_dll.decode  = (fn_decode)(void *)GetProcAddress(g_dll.mod, "opus_decode");
            g_dll.destroy = (fn_destroy)(void *)GetProcAddress(g_dll.mod, "opus_decoder_destroy");
            if (!g_dll.create || !g_dll.decode) g_dll.mod = NULL;
        }
    }
    return g_dll.mod ? "opus.dll" : "none";
#endif
}

int opusdec_ready(void)
{
#ifdef MICLINK_HAVE_OPUS
    return g_dec != NULL;
#else
    return g_dll.st != NULL;
#endif
}

void opusdec_close(void)
{
#ifdef MICLINK_HAVE_OPUS
    if (g_dec) { opus_decoder_destroy(g_dec); g_dec = NULL; }
#else
    if (g_dll.st && g_dll.destroy) g_dll.destroy(g_dll.st);
    g_dll.st = NULL;
#endif
    g_rate = 0;
}

int opusdec_open(int rate)
{
    int err = 0;
    if (rate <= 0) rate = 48000;
    opusdec_close();
    g_rate = rate;

#ifdef MICLINK_HAVE_OPUS
    g_dec = opus_decoder_create(rate, 1, &err);
    if (!g_dec || err != 0) { g_dec = NULL; return 0; }
    return 1;
#else
    if (!g_dll.mod) {
        g_dll.mod = LoadLibraryW(L"opus.dll");
        if (!g_dll.mod) return 0;
        g_dll.create  = (fn_create)(void *)GetProcAddress(g_dll.mod, "opus_decoder_create");
        g_dll.decode  = (fn_decode)(void *)GetProcAddress(g_dll.mod, "opus_decode");
        g_dll.destroy = (fn_destroy)(void *)GetProcAddress(g_dll.mod, "opus_decoder_destroy");
        if (!g_dll.create || !g_dll.decode) { g_dll.mod = NULL; return 0; }
    }
    g_dll.st = g_dll.create(rate, 1, &err);
    if (!g_dll.st || err != 0) { g_dll.st = NULL; return 0; }
    return 1;
#endif
}

int opusdec_decode(const unsigned char *pkt, int len, int16_t *out, int cap, int fec)
{
    int n;
#ifdef MICLINK_HAVE_OPUS
    if (!g_dec) return 0;
    n = opus_decode(g_dec, pkt, len, out, cap, fec);
#else
    if (!g_dll.st) return 0;
    n = g_dll.decode(g_dll.st, pkt, len, out, cap, fec);
#endif
    return n > 0 ? n : 0;
}
