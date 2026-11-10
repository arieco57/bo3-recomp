// XAudio render driver -> SDL audio, XMA context allocation, voice (no headset).
//
// The console's audio driver calls each registered client's callback every 256 samples
// (48 kHz -> 5.33 ms); the client mixes and calls XAudioSubmitRenderDriverFrame with
// 256 samples x 6 channels (FL FR C LFE BL BR, planar) of big-endian floats. We downmix to
// stereo and queue it on an SDL audio stream; the driver thread paces the callbacks by the
// stream's fill level (no drift between the guest and the sound card). Without an audio
// device it falls back to a 5.33 ms timer. BO3_NO_AUDIO=1 disables output, BO3_VOLUME=0..1.
#include "guest.h"
#include "stubs.h"
#include "thread.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>

namespace
{
// The driver calls callback(wrappedArg) where wrappedArg points at a dword holding the client's
// argument (the callback starts with `lwz r3,0(r3)`; same as Xenia's wrapped_callback_arg).
struct Client { uint32_t callback = 0, arg = 0, wrappedArg = 0; bool active = false; };
std::mutex s_lock;
Client s_clients[8];
std::atomic<uint64_t> s_framesSubmitted{ 0 };
float s_peak = 0;   // since the last log line
bool s_threadStarted = false;
SDL_AudioStream* s_stream = nullptr;
float s_volume = 1.0f;
constexpr int kFrameSamples = 256;
constexpr int kFrameBytes = kFrameSamples * 2 * sizeof(float);   // stereo float

void OpenOutput()
{
    if (getenv("BO3_NO_AUDIO") || getenv("BO3_NO_WINDOW")) return;
    if (const char* v = getenv("BO3_VOLUME")) s_volume = std::clamp(float(atof(v)), 0.0f, 2.0f);
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) { LogWarn("audio: SDL audio init failed: %s", SDL_GetError()); return; }
    SDL_AudioSpec spec{ SDL_AUDIO_F32, 2, 48000 };
    s_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!s_stream) { LogWarn("audio: no output device: %s", SDL_GetError()); return; }
    SDL_ResumeAudioStreamDevice(s_stream);
    Log("audio: output 48 kHz stereo on \"%s\"", SDL_GetAudioDeviceName(SDL_GetAudioStreamDevice(s_stream)));
}

void CallClients(GuestThread& t)
{
    for (int i = 0; i < 8; i++)
    {
        Client c;
        {
            std::lock_guard lock(s_lock);
            c = s_clients[i];
        }
        if (c.active && c.callback) GuestCallOnThread(t, c.callback, c.wrappedArg);
    }
}

void AudioThread(GuestThread& t)
{
    // Give the client time to finish initialising after registration (real hardware also
    // starts calling back asynchronously, after the DMA engine spins up).
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    OpenOutput();
    uint64_t ticks = 0;
    auto next = std::chrono::steady_clock::now();
    for (;;)
    {
        if (s_stream)
        {
            // keep ~3 frames (16 ms) queued: call the clients whenever the queue drains below it
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            int calls = 0;
            while (SDL_GetAudioStreamQueued(s_stream) < 3 * kFrameBytes && calls < 4)
            {
                uint64_t before = s_framesSubmitted;
                CallClients(t);
                calls++;
                ticks++;
                if (s_framesSubmitted == before) break;   // nothing mixed: don't spin
            }
        }
        else
        {
            next += std::chrono::microseconds(5333);
            std::this_thread::sleep_until(next);
            CallClients(t);
            ticks++;
        }
        if (ticks && ticks % 940 == 0)
        {
            static uint64_t lastLogged = 0;
            if (lastLogged != ticks)
            {
                lastLogged = ticks;
                Log("audio: %llu callbacks, %llu frames submitted, peak %.3f", (unsigned long long)ticks, (unsigned long long)s_framesSubmitted.load(), s_peak);
                s_peak = 0;
            }
        }
    }
}

float ToFloat(uint32_t bits) { float f; memcpy(&f, &bits, 4); return f; }
uint32_t FromFloat(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }
}

// XAudioRegisterRenderDriverClient(callback_info* {callback, arg}, driver*)
GUEST_IMPORT(XAudioRegisterRenderDriverClient)
{
    uint32_t info = ARG(3);
    std::lock_guard lock(s_lock);
    for (uint32_t i = 0; i < 8; i++)
        if (!s_clients[i].active)
        {
            uint32_t wrapped = GuestAlloc(4);
            GuestWrite32(wrapped, GuestRead32(info + 4));
            s_clients[i] = { GuestRead32(info), GuestRead32(info + 4), wrapped, true };
            GuestWrite32(ARG(4), 0x41550000 | i);
            Log("audio: render client %u registered (callback 0x%08X arg 0x%08X)", i, s_clients[i].callback, s_clients[i].arg);
            if (!s_threadStarted)
            {
                s_threadStarted = true;
                ThreadCreateHost("audio-driver", AudioThread);
            }
            RETURN(0);
        }
    RETURN(0x80004005);
}

GUEST_IMPORT(XAudioUnregisterRenderDriverClient)
{
    std::lock_guard lock(s_lock);
    uint32_t i = ARG(3) & 0xFFFF;
    if (i < 8) s_clients[i].active = false;
    RETURN(0);
}

// XAudioSubmitRenderDriverFrame(driver, samples): 6 planar channels x 256 big-endian floats.
GUEST_IMPORT(XAudioSubmitRenderDriverFrame)
{
    s_framesSubmitted++;
    if (s_stream && ARG(4))
    {
        const uint8_t* src = g_base + ARG(4);
        auto sample = [&](int ch, int i) {
            uint32_t b;
            memcpy(&b, src + (ch * kFrameSamples + i) * 4, 4);
            b = __builtin_bswap32(b);
            float f;
            memcpy(&f, &b, 4);
            return std::isfinite(f) ? f : 0.0f;
        };
        float out[kFrameSamples * 2];
        const float k = 0.7071f;
        for (int i = 0; i < kFrameSamples; i++)
        {
            float c = sample(2, i) * k, lfe = sample(3, i) * 0.5f;
            float l = sample(0, i) + c + sample(4, i) * k + lfe;
            float r = sample(1, i) + c + sample(5, i) * k + lfe;
            out[i * 2] = std::clamp(l * s_volume, -1.0f, 1.0f);
            out[i * 2 + 1] = std::clamp(r * s_volume, -1.0f, 1.0f);
            s_peak = std::max({ s_peak, std::fabs(l), std::fabs(r) });
        }
        SDL_PutAudioStreamData(s_stream, out, sizeof(out));
        static FILE* dump = [] { const char* p = getenv("BO3_AUDIO_DUMP"); return p ? fopen(p, "wb") : nullptr; }();
        if (dump) fwrite(out, 1, sizeof(out), dump);   // raw 48 kHz stereo float (diagnostics)
    }
    RETURN(0);
}

GUEST_IMPORT(XAudioGetSpeakerConfig) { GuestWrite32(ARG(3), 0x00010001); RETURN(0); }   // stereo

// XAudioGetVoiceCategoryVolume(category, float* volume)
GUEST_IMPORT(XAudioGetVoiceCategoryVolume) { GuestWrite32(ARG(4), FromFloat(1.0f)); RETURN(0); }

GUEST_IMPORT(XAudioEnableDucker) { RETURN(0); }
GUEST_IMPORT(XAudioGetDuckerLevel) { if (ARG(3)) GuestWrite32(ARG(3), FromFloat(0.0f)); RETURN(0); }
GUEST_IMPORT(XAudioGetDuckerThreshold) { if (ARG(3)) GuestWrite32(ARG(3), FromFloat(0.0f)); RETURN(0); }
GUEST_IMPORT(XAudioGetDuckerAttackTime) { if (ARG(3)) GuestWrite32(ARG(3), 0); RETURN(0); }
GUEST_IMPORT(XAudioGetDuckerReleaseTime) { if (ARG(3)) GuestWrite32(ARG(3), 0); RETURN(0); }
GUEST_IMPORT(XAudioGetDuckerHoldTime) { if (ARG(3)) GuestWrite32(ARG(3), 0); RETURN(0); }

// XMA hardware decoder contexts. The decoder itself is not implemented yet: contexts are
// handed out so the engine can initialise; XMA streams will play silence.
// Voice: no headset connected.
GUEST_IMPORT(XamVoiceIsActiveProcess) { RETURN(0); }
GUEST_IMPORT(XamVoiceHeadsetPresent) { RETURN(0); }
GUEST_IMPORT(XamVoiceCreate) { if (ARG(5)) GuestWrite32(ARG(5), 0); RETURN(ERROR_DEVICE_NOT_CONNECTED); }
GUEST_IMPORT(XamVoiceClose) { RETURN(0); }
GUEST_IMPORT(XamVoiceSubmitPacket) { RETURN(ERROR_DEVICE_NOT_CONNECTED); }
