
#include "compat.h"

#ifdef HAVE_XMP

#include "_multivc.h"
#include "multivoc.h"
#include "pitch.h"
#include "pragmas.h"

#define BUILDING_STATIC
#include "libxmp-lite/xmp.h"

typedef struct {
   void * ptr;
   size_t length;

   xmp_context ctx;
} xmp_data;

int  MV_GetXMPPosition(VoiceNode *voice)               { return voice->position; }
void MV_SetXMPPosition(VoiceNode *voice, int position) { xmp_seek_time(((xmp_data *)voice->rawdataptr)->ctx, position); }

static playbackstatus MV_GetNextXMPBlock(VoiceNode *voice)
{
    if (voice->rawdataptr == nullptr)
    {
        LOG_F(ERROR, "MV_GetNextXMPBlock: no XMP context!");
        return NoMoreData;
    }

    auto ctx = ((xmp_data *)voice->rawdataptr)->ctx;

    if (xmp_play_frame(ctx) != 0)
    {
#if 0
        if (voice->Loop.Size > 0)
        {
            xmp_restart_module(ctx);
            if (xmp_play_frame(ctx) != 0)
                return NoMoreData;
        }
        else
#endif
            return NoMoreData;
    }

    xmp_frame_info mi;
    xmp_get_frame_info(ctx, &mi);

    uint32_t const samples = mi.buffer_size / (2 * (16/8)); // since 2-channel, 16-bit is hardcoded
    // uint32_t const samples = mi.buffer_size / (voice->channels * (voice->bits / 8));

    voice->sound    = (char const *)mi.buffer;
    voice->length   = samples << 16;
    voice->position = mi.time;

    MV_SetVoiceMixMode(voice);

    return KeepPlaying;
}

int MV_PlayXMP3D(char *ptr, uint32_t length, int loophow, int pitchoffset, int angle, int distance, int priority, fix16_t volume, intptr_t callbackval)
{
    if (!MV_Installed)
        return MV_SetErrorCode(MV_NotInstalled);

    if (distance < 0)
    {
        distance  = -distance;
        angle    += MV_NUMPANPOSITIONS / 2;
    }

    int vol = MIX_VOLUME(distance);

    // Ensure angle is within 0 - 127
    angle &= MV_MAXPANPOSITION;

    int left  = MV_PanTable[angle][vol].left;
    int right = MV_PanTable[angle][vol].right;
    int mid   = max( 0, 255 - distance );

    return MV_PlayXMP(ptr, length, loophow, -1, pitchoffset, mid, left, right, priority, volume, callbackval);
}

int MV_PlayXMP(char *ptr, uint32_t length, int loopstart, int loopend, int pitchoffset, int vol, int left, int right, int priority, fix16_t volume, intptr_t callbackval)
{
    UNREFERENCED_PARAMETER(loopend);

    if (!MV_Installed)
        return MV_SetErrorCode(MV_NotInstalled);

    // Request a voice from the voice pool
    auto voice = MV_AllocVoice(priority, sizeof(xmp_data));
    if (voice == nullptr)
        return MV_SetErrorCode(MV_NoVoices);

    voice->sound       = 0;
    voice->wavetype    = FMT_XMP;
    voice->Paused      = TRUE;
    voice->GetSound    = MV_GetNextXMPBlock;
    voice->PitchScale  = PITCH_GetScale(pitchoffset);
    voice->priority    = priority;
    voice->callbackval = callbackval;

    voice->bits        = 16;
    voice->channels    = 2;
    voice->SamplingRate = MV_MixRate;

    voice->Loop = { nullptr, nullptr, 0, loopstart >= 0 };

    MV_SetVoiceMixMode(voice);
    MV_SetVoiceVolume(voice, vol, left, right, volume);

    auto xd = (xmp_data *)voice->rawdataptr;

    xd->ptr = ptr;
    xd->length = length;

    voice->task = async::spawn([voice]() -> int
    {
        auto xd = (xmp_data *)voice->rawdataptr;
        auto ctx = xd->ctx;

        if (!ctx)
        {
            ctx = xmp_create_context();
            xd->ctx = ctx;
        }

        int xmp_status = 0;
        if (ctx == nullptr || (xmp_status = xmp_load_module_from_memory(ctx, xd->ptr, xd->length)))
        {
            if (!xmp_status)
                LOG_F(ERROR, "MV_PlayXMP: error in xmp_create_context");
            else
            {
                xmp_free_context(ctx);
                LOG_F(ERROR, "MV_PlayXMP: error %i in xmp_load_module_from_memory", xmp_status);
            }

            ALIGNED_FREE_AND_NULL(voice->rawdataptr);
            voice->rawdatasiz = 0;
            MV_PlayVoice(voice);
            return MV_SetErrorCode(MV_InvalidFile);
        }

        xmp_start_player(ctx, MV_MixRate, 0);
        xmp_set_player(ctx, XMP_PLAYER_INTERP, MV_XMPInterpolation);

        // CODEDUP multivoc.c MV_SetVoicePitch
        voice->RateScale = divideu64((uint64_t)voice->SamplingRate * voice->PitchScale, MV_MixRate);
        voice->FixedPointBufferSize = (voice->RateScale * MV_MIXBUFFERSIZE) - voice->RateScale;
        MV_PlayVoice(voice);
        return MV_Ok;
    }
    );

    return voice->handle;
}

void MV_ReleaseXMPVoice(VoiceNode * voice)
{
    Bassert(voice->wavetype == FMT_XMP && voice->rawdataptr != nullptr && voice->rawdatasiz == sizeof(xmp_data));

    auto xd = (xmp_data *)voice->rawdataptr;

    xmp_end_player(xd->ctx);
    xmp_release_module(xd->ctx);

    if (MV_LazyAlloc)
        return;

    xmp_free_context(xd->ctx);
    voice->rawdataptr = nullptr;
    voice->rawdatasiz = 0;
    ALIGNED_FREE_AND_NULL(xd);
}

void MV_SetXMPInterpolation(int interp)
{
    if (!MV_Installed)
        return;

    for (VoiceNode *voice = VoiceList.next; voice != &VoiceList; voice = voice->next)
        if (voice->wavetype == FMT_XMP)
            xmp_set_player(((xmp_data *)voice->rawdataptr)->ctx, XMP_PLAYER_INTERP, interp);
}

#else

#include "_multivc.h"

static char const NoXMP[] = "MV_PlayXMP: libxmp-lite support not included in this binary.";

int MV_PlayXMP(char *, uint32_t, int, int, int, int, int, int, int, fix16_t, intptr_t)
{
    LOG_F(ERROR, NoXMP);
    return -1;
}

int MV_PlayXMP3D(char *, uint32_t, int, int, int, int, int, fix16_t, intptr_t)
{
    LOG_F(ERROR, NoXMP);
    return -1;
}

#endif

// KEEPINSYNC libxmp-lite/src/*_load.c

static int it_test_memory(char const *ptr, uint32_t ptrlength)
{
    static char const it_magic[] = "IMPM";
    return !!(ptrlength < sizeof(it_magic) - 1 || Bmemcmp(ptr, it_magic, sizeof(it_magic) - 1));
}

static int mod_test_memory(char const *ptr, uint32_t ptrlength)
{
    if (ptrlength < 1084)
        return -1;

    char const * const buf = ptr + 1080;

    if (!Bstrncmp(buf + 2, "CH", 2) && isdigit((int)buf[0]) && isdigit((int)buf[1]))
    {
        int i = (buf[0] - '0') * 10 + buf[1] - '0';
        if (i > 0 && i <= 32)
            return 0;
    }

    if (!Bstrncmp(buf + 1, "CHN", 3) && isdigit((int)*buf))
    {
        if (*buf >= '0' && *buf <= '9')
            return 0;
    }

    if (!Bmemcmp(buf, "M.K.", 4))
        return 0;

    return -1;
}

static int s3m_test_memory(char const *ptr, uint32_t ptrlength)
{
    static char const s3m_magic[] = "SCRM";
    #define s3m_magic_offset 44

    return !!(ptrlength < s3m_magic_offset + sizeof(s3m_magic)-1 ||
        Bmemcmp(ptr + s3m_magic_offset, s3m_magic, sizeof(s3m_magic)-1) ||
        ptr[29] != 0x10);
}

static int xm_test_memory(char const *ptr, uint32_t ptrlength)
{
    static char const xm_magic[] = "Extended Module: ";
    return !!(ptrlength < sizeof(xm_magic) - 1 || Bmemcmp(ptr, xm_magic, sizeof(xm_magic) - 1));
}

static int mtm_test_memory(char const *ptr, uint32_t ptrlength)
{
    static char const mtm_magic[] = "MTM\x10";
    return !!(ptrlength < sizeof(mtm_magic) - 1 || Bmemcmp(ptr, mtm_magic, sizeof(mtm_magic) - 1));
}

int MV_IdentifyXMP(char const *ptr, uint32_t ptrlength)
{
    static decltype(mod_test_memory) * const module_test_functions[] =
    {
        it_test_memory,
        mod_test_memory,
        s3m_test_memory,
        xm_test_memory,
        mtm_test_memory,
    };

    for (auto const test_module : module_test_functions)
    {
        if (test_module(ptr, ptrlength) == 0)
            return 1;
    }

    return 0;
}
