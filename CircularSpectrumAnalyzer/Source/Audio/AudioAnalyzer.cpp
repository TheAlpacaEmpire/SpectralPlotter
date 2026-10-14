#include "AudioAnalyzer.h"

#include <algorithm>
#include <cmath>
#include <vector>

// Windows Core Audio headers for WASAPI capture (output loopback + input devices)
#if JUCE_WINDOWS
 #include <mmdeviceapi.h>
 #include <audioclient.h>
 #include <functiondiscoverykeys_devpkey.h>   // PKEY_Device_FriendlyName
 #include <propidl.h>                          // PROPVARIANT / PropVariantClear
#endif

namespace
{
    // Median of the first n entries of scratch (reorders scratch in place). n small (<= histLen).
    float medianInPlace (float* scratch, int n) noexcept
    {
        if (n <= 0) return 0.0f;
        std::nth_element (scratch, scratch + n / 2, scratch + n);
        return scratch[n / 2];
    }

    // Standard A-weighting response in dB, normalised to 0 dB at 1 kHz.
    float aWeightingDb (float f) noexcept
    {
        const float f2  = f * f;
        const float num = 12194.0f * 12194.0f * f2 * f2;
        const float den = (f2 + 20.6f * 20.6f)
                        * std::sqrt ((f2 + 107.7f * 107.7f) * (f2 + 737.9f * 737.9f))
                        * (f2 + 12194.0f * 12194.0f);
        if (den <= 0.0f) return -80.0f;
        return 20.0f * std::log10 (num / den) + 2.0f;
    }
}

//==============================================================================
AudioAnalyzer::AudioAnalyzer()
    : fft (fftOrder),
      hannWindow (fftSize, juce::dsp::WindowingFunction<float>::hann)
{
}

AudioAnalyzer::~AudioAnalyzer()
{
    stop();
}

void AudioAnalyzer::start()
{
    loopback.startThread();
}

void AudioAnalyzer::stop()
{
    loopback.stopThread (2000);
}

//==============================================================================
// Message thread: grab the latest features the capture thread has published.
void AudioAnalyzer::update()
{
    const juce::SpinLock::ScopedLockType sl (featuresLock);
    snapshot = published;
}

//==============================================================================
// Rebuild the per-band bin ranges, perceptual weights, and adaptive-smoothing alphas.
// Cheap; only re-runs when the captured sample rate changes.
void AudioAnalyzer::ensureBandTables (float sr)
{
    if (sr == bandTableSR || sr <= 0.0f)
        return;

    bandTableSR = sr;

    const float minFreq = 20.0f;
    const float maxFreq = juce::jmin (20000.0f, sr * 0.5f);

    // Band b spans [edge(b), edge(b+1)) on a log scale.
    auto edge = [&] (int b) { return minFreq * std::pow (maxFreq / minFreq, (float) b / (float) numBands); };

    for (int b = 0; b < numBands; ++b)
    {
        const float fLo = edge (b);
        const float fHi = edge (b + 1);

        int kLo = (int) std::floor (fLo * (float) fftSize / sr);
        int kHi = (int) std::ceil  (fHi * (float) fftSize / sr);
        kLo = juce::jlimit (1, fftSize / 2 - 1, kLo);
        kHi = juce::jlimit (1, fftSize / 2 - 1, kHi);
        if (kHi < kLo) kHi = kLo;   // very low bands may collapse to a single bin

        bandBinLo[b]      = kLo;
        bandBinHi[b]      = kHi;
        bandCenterFreq[b] = std::sqrt (fLo * fHi);

        // Perceptual tilt: full A-weighting crushes bass by ~50 dB, which would erase the
        // low peaks the visual relies on. Floor it so bass is de-emphasised, not deleted.
        bandWeightDb[b]   = juce::jmax (-18.0f, aWeightingDb (bandCenterFreq[b]));
    }

    // Adaptive smoothing: bass swells slowly, treble ticks fast. Alphas are computed for the
    // analysis frame rate (sr / hopSize), not the 60 Hz paint rate.
    const float analysisRate = sr / (float) hopSize;
    for (int b = 0; b < numBands; ++b)
    {
        const float frac   = (float) b / (float) (numBands - 1);
        const float atkMs  = juce::jmap (frac, 30.0f, 5.0f);    // was 80: bass felt late
        const float decMs  = juce::jmap (frac, 220.0f, 50.0f);  // was 400: bass hung on
        attackAlpha[b] = 1.0f - std::exp (-1.0f / juce::jmax (1.0f, (atkMs * 0.001f) * analysisRate));
        decayAlpha[b]  = 1.0f - std::exp (-1.0f / juce::jmax (1.0f, (decMs * 0.001f) * analysisRate));
    }
}

//==============================================================================
// One overlapped analysis frame — runs on the capture thread every hopSize samples.
void AudioAnalyzer::analyzeBlock()
{
    const float sr = capturedSampleRate.load();
    ensureBandTables (sr);

    if (resetRequested.exchange (false))
    {
        harmEnv = {};  percEnv = {};  prevMag = {};
        bandHistory = {};  histPos = 0;  histCount = 0;
    }

    // Reconstruct the time-ordered window (oldest -> newest) from the ring into the FFT scratch.
    for (int i = 0; i < fftSize; ++i)
        fftData[i] = ring[(writePos + i) % fftSize];
    std::fill (fftData.begin() + fftSize, fftData.end(), 0.0f);

    hannWindow.multiplyWithWindowingTable (fftData.data(), fftSize);
    fft.performFrequencyOnlyForwardTransform (fftData.data());

    // --- E: constant-Q-like log band energy (integrate bin power per band) ---
    std::array<float, numBands> mag {};
    for (int b = 0; b < numBands; ++b)
    {
        float energy = 0.0f;
        for (int k = bandBinLo[b]; k <= bandBinHi[b]; ++k)
            energy += fftData[k] * fftData[k];
        mag[b] = std::sqrt (energy) / (float) fftSize;
    }

    // Push this frame into the rolling spectrogram for HPSS.
    bandHistory[histPos] = mag;
    histPos   = (histPos + 1) % histLen;
    histCount = juce::jmin (histCount + 1, histLen);

    // --- D: HPSS via median filtering in the 64-band domain ---
    // Harmonic estimate = median across time (per band); percussive = median across frequency.
    std::array<float, numBands> harmMag {}, percMag {};
    {
        float tScratch[histLen];
        for (int b = 0; b < numBands; ++b)
        {
            for (int t = 0; t < histCount; ++t)
                tScratch[t] = bandHistory[t][b];
            harmMag[b] = medianInPlace (tScratch, histCount);
        }

        constexpr int F = 17, half = F / 2;
        float fScratch[F];
        for (int b = 0; b < numBands; ++b)
        {
            int n = 0;
            for (int d = -half; d <= half; ++d)
            {
                const int idx = b + d;
                if (idx >= 0 && idx < numBands)
                    fScratch[n++] = mag[idx];
            }
            percMag[b] = medianInPlace (fScratch, n);
        }
    }

    // Automatic gain (see peakRefDb): follow the loudest weighted band, jumping up quickly
    // and falling back slowly, ignoring silence so a pause doesn't crank the boost.
    {
        float framePeakDb = -100.0f;
        for (int b = 0; b < numBands; ++b)
            framePeakDb = juce::jmax (framePeakDb, juce::Decibels::gainToDecibels (mag[b], -100.0f) + bandWeightDb[b]);

        if (framePeakDb > peakRefDb)
            peakRefDb += (framePeakDb - peakRefDb) * 0.3f;   // ~70 ms at ~47 analysis frames/s
        else if (framePeakDb > agcGateDb)
            peakRefDb = juce::jmax (framePeakDb, peakRefDb - agcReleaseDbPerS * (float) hopSize / sr);
    }
    // The reference keeps tracking while auto-gain is off, so re-enabling it doesn't jump.
    // Applied linearly so digital silence stays at the -100 dB floor instead of being lifted.
    const float gainDb  = autoGainOn.load() ? juce::jlimit (0.0f, agcMaxBoostDb, agcTargetDb - peakRefDb)
                                            : manualGainDb.load();
    const float agcGain = juce::Decibels::decibelsToGain (gainDb);

    // Soft Wiener masks split the band magnitude into harmonic/percussive streams, then
    // B (perceptual weighting) + normalisation, then A (adaptive smoothing) per band.
    // C (spectral flux) is measured on the *normalised percussive target* so it is
    // unit-consistent (0..1) and already isolates transients via the HPSS split — a much
    // cleaner onset signal than differencing the full-mix magnitude. prevMag holds the
    // previous frame's percussive target for this purpose.
    auto toNorm = [&] (float m, int b)
    {
        const float db = juce::Decibels::gainToDecibels (m * agcGain, -100.0f) + bandWeightDb[b];
        return juce::jlimit (0.0f, 1.0f, juce::jmap (db, -60.0f, 0.0f, 0.0f, 1.0f));
    };

    constexpr float percAttack = 0.6f, percDecay = 0.25f;   // percussion stays fast

    std::array<float, numBands> fluxBand {};
    for (int b = 0; b < numBands; ++b)
    {
        const float h2    = harmMag[b] * harmMag[b];
        const float p2    = percMag[b] * percMag[b];
        const float denom = h2 + p2 + 1.0e-12f;
        const float hMask = h2 / denom;
        const float pMask = p2 / denom;

        const float hTarget = toNorm (mag[b] * hMask, b);
        const float pTarget = toNorm (mag[b] * pMask, b);

        const float fd = pTarget - prevMag[b];
        fluxBand[b] = fd > 0.0f ? fd : 0.0f;
        prevMag[b]  = pTarget;

        // A: per-band adaptive attack/decay on the harmonic stream.
        const float ha = (hTarget > harmEnv[b]) ? attackAlpha[b] : decayAlpha[b];
        harmEnv[b] += (hTarget - harmEnv[b]) * ha;

        const float pa = (pTarget > percEnv[b]) ? percAttack : percDecay;
        percEnv[b] += (pTarget - percEnv[b]) * pa;
    }

    // --- F support: spectral centroid (brightness), log-mapped to 0..1 ---
    float cNum = 0.0f, cDen = 0.0f;
    for (int b = 0; b < numBands; ++b)
    {
        cNum += bandCenterFreq[b] * mag[b];
        cDen += mag[b];
    }
    float centroidNorm = 0.0f;
    if (cDen > 1.0e-9f)
    {
        const float cHz = cNum / cDen;
        centroidNorm = juce::jlimit (0.0f, 1.0f,
            juce::jmap (std::log (juce::jmax (20.0f, cHz)), std::log (100.0f), std::log (8000.0f), 0.0f, 1.0f));
    }

    // Assemble and publish.
    AudioFeatures f;
    f.harmonic     = harmEnv;
    f.percussive   = percEnv;
    f.flux         = fluxBand;
    f.centroidNorm = centroidNorm;
    f.gainDb       = gainDb;

    float overall = 0.0f, bass = 0.0f, high = 0.0f;
    for (int b = 0; b < numBands; ++b)
    {
        f.level[b] = juce::jlimit (0.0f, 1.0f, harmEnv[b] + percEnv[b]);
        overall += f.level[b];
        if (b < bassBandCount)  bass += f.level[b];
        if (b >= highBandStart) high += f.level[b];
    }
    f.overallEnergy = overall / (float) numBands;
    f.bassEnergy    = bass / (float) bassBandCount;
    f.highEnergy    = high / (float) (numBands - highBandStart);

    {
        const juce::SpinLock::ScopedLockType sl (featuresLock);
        published = f;
    }
}

//==============================================================================
void AudioAnalyzer::pushSample (float sample) noexcept
{
    ring[writePos] = sample;
    writePos = (writePos + 1) % fftSize;

    if (++hopCounter >= hopSize)
    {
        hopCounter = 0;
        analyzeBlock();
    }
}

juce::String AudioAnalyzer::getCurrentInputName() const
{
    const juce::SpinLock::ScopedLockType sl (deviceLock);
    return currentDeviceName;
}

void AudioAnalyzer::getCurrentInputPosition (int& index, int& count) const
{
    const juce::SpinLock::ScopedLockType sl (deviceLock);
    index = currentDeviceIndex;
    count = deviceCount;
}

juce::String AudioAnalyzer::getCurrentDeviceId() const
{
    const juce::SpinLock::ScopedLockType sl (deviceLock);
    return currentDeviceId;
}

void AudioAnalyzer::setPreferredDeviceId (const juce::String& id)
{
    const juce::SpinLock::ScopedLockType sl (deviceLock);
    preferredDeviceId = id;
}

#if JUCE_WINDOWS
namespace
{
    // An enumerable capture source: an output device (tapped via loopback) or an input device.
    struct Endpoint { std::wstring id; juce::String name; bool isCapture; };

    juce::String friendlyName (IMMDevice* device)
    {
        juce::String result;
        IPropertyStore* store = nullptr;
        if (SUCCEEDED (device->OpenPropertyStore (STGM_READ, &store)) && store != nullptr)
        {
            PROPVARIANT pv;
            PropVariantInit (&pv);
            if (SUCCEEDED (store->GetValue (PKEY_Device_FriendlyName, &pv))
                && pv.vt == VT_LPWSTR && pv.pwszVal != nullptr)
                result = juce::String (pv.pwszVal);
            PropVariantClear (&pv);
            store->Release();
        }
        return result;
    }

    std::wstring deviceId (IMMDevice* device)
    {
        std::wstring out;
        LPWSTR id = nullptr;
        if (SUCCEEDED (device->GetId (&id)) && id != nullptr)
            out = id;
        if (id != nullptr)
            ::CoTaskMemFree (id);
        return out;
    }

    void enumerateInto (IMMDeviceEnumerator* en, EDataFlow flow, bool isCapture,
                        std::vector<Endpoint>& out)
    {
        IMMDeviceCollection* coll = nullptr;
        if (FAILED (en->EnumAudioEndpoints (flow, DEVICE_STATE_ACTIVE, &coll)) || coll == nullptr)
            return;

        UINT n = 0;
        coll->GetCount (&n);
        for (UINT i = 0; i < n; ++i)
        {
            IMMDevice* d = nullptr;
            if (SUCCEEDED (coll->Item (i, &d)) && d != nullptr)
            {
                Endpoint ep { deviceId (d), friendlyName (d), isCapture };
                if (! ep.id.empty())
                    out.push_back (std::move (ep));
                d->Release();
            }
        }
        coll->Release();
    }

    // Stable order: outputs (loopback) first, then inputs — so cycling walks a predictable list.
    std::vector<Endpoint> enumerateEndpoints (IMMDeviceEnumerator* en)
    {
        std::vector<Endpoint> out;
        enumerateInto (en, eRender,  false, out);
        enumerateInto (en, eCapture, true,  out);
        return out;
    }

    int indexOfId (const std::vector<Endpoint>& list, const std::wstring& id)
    {
        for (int i = 0; i < (int) list.size(); ++i)
            if (list[(size_t) i].id == id)
                return i;
        return -1;
    }
}
#endif

//==============================================================================
// run() is a supervisor keeping a capture session alive across device changes and
// user source-switches. tryCaptureSession() opens the selected endpoint (an output tapped
// via loopback, or an input captured directly) and pumps audio until the thread stops, the
// device faults (unplug / default-device switch raises AUDCLNT_E_DEVICE_INVALIDATED), or the
// user cycles the source. Faults back off and re-open; source-switches restart immediately.
void AudioAnalyzer::LoopbackCapture::run()
{
#if JUCE_WINDOWS
    ::CoInitializeEx (nullptr, COINIT_MULTITHREADED);

    int backoffMs = 250;   // grows to 1 s so a persistently-missing device isn't hammered

    while (! threadShouldExit())
    {
        const SessionResult r = tryCaptureSession();

        if (r == SessionResult::Stopped || threadShouldExit())
            break;

        if (r == SessionResult::SwitchDevice)
        {
            backoffMs = 250;   // user-initiated: restart immediately on the new source
            continue;
        }

        // Faulted (init failed or device invalidated) — surface it and retry with backoff.
        owner.captureState.store (CaptureState::Reconnecting);

        int waited = 0;
        while (waited < backoffMs && ! threadShouldExit())
        {
            juce::Thread::sleep (25);
            waited += 25;
        }
        backoffMs = juce::jmin (1000, backoffMs * 2);
    }

    ::CoUninitialize();
#else
    owner.captureState.store (CaptureState::Failed);
#endif
}

#if JUCE_WINDOWS
AudioAnalyzer::LoopbackCapture::SessionResult AudioAnalyzer::LoopbackCapture::tryCaptureSession()
{
    owner.captureState.store (CaptureState::Starting);

    // COM objects released in reverse order by the scope guard, so every early exit path
    // frees exactly what it acquired.
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice*           device     = nullptr;
    IAudioClient*        client     = nullptr;
    IAudioCaptureClient* capture    = nullptr;
    bool                 started    = false;

    const juce::ScopeGuard cleanup { [&]
    {
        if (started && client != nullptr) client->Stop();
        if (capture    != nullptr) capture->Release();
        if (client     != nullptr) client->Release();
        if (device     != nullptr) device->Release();
        if (enumerator != nullptr) enumerator->Release();
    } };

    ::CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                        __uuidof (IMMDeviceEnumerator), (void**) &enumerator);
    if (enumerator == nullptr) return SessionResult::Faulted;

    // Resolve which endpoint to open, applying any pending source-cycle requests.
    const std::vector<Endpoint> list = enumerateEndpoints (enumerator);
    const int cycles = owner.inputCycleRequest.exchange (0);

    if (selectedId.empty())   // first run: the saved source if any, else the default output
    {
        const juce::SpinLock::ScopedLockType sl (owner.deviceLock);
        selectedId = owner.preferredDeviceId.toWideCharPointer();
        owner.preferredDeviceId.clear();
    }
    if (selectedId.empty())
    {
        IMMDevice* def = nullptr;
        if (SUCCEEDED (enumerator->GetDefaultAudioEndpoint (eRender, eConsole, &def)) && def != nullptr)
        {
            selectedId = deviceId (def);
            def->Release();
        }
    }
    if (cycles != 0)
        owner.accessDenied.store (false);   // a new source starts with a clean slate
    if (cycles != 0 && ! list.empty())
    {
        const int n   = (int) list.size();
        const int cur = indexOfId (list, selectedId);            // -1 if the source vanished
        const int idx = (((cur < 0 ? 0 : cur) + cycles) % n + n) % n;
        selectedId = list[(size_t) idx].id;
    }

    // Look up the selected endpoint's kind + name (fallback: default output).
    bool         isCapture = false;
    juce::String name;
    for (const auto& ep : list)
        if (ep.id == selectedId) { isCapture = ep.isCapture; name = ep.name; break; }

    if (! selectedId.empty())
        enumerator->GetDevice (selectedId.c_str(), &device);
    const bool usingSelected = (device != nullptr);
    if (device == nullptr)
    {
        enumerator->GetDefaultAudioEndpoint (eRender, eConsole, &device);
        isCapture = false;
        if (name.isEmpty()) name = "DEFAULT OUTPUT";
    }
    if (device == nullptr) return SessionResult::Faulted;   // no device at all yet — retry

    {
        const juce::SpinLock::ScopedLockType sl (owner.deviceLock);
        owner.currentDeviceName = name.isNotEmpty() ? name
                                                    : juce::String (isCapture ? "INPUT" : "OUTPUT");
        owner.currentDeviceId   = usingSelected ? juce::String (selectedId.c_str()) : juce::String();
        owner.currentDeviceIndex = usingSelected ? indexOfId (list, selectedId) : -1;
        owner.deviceCount        = (int) list.size();
    }

    // E_ACCESSDENIED (from Activate or Initialize) on an input = mic privacy switch is off.
    const HRESULT hrAct = device->Activate (__uuidof (IAudioClient), CLSCTX_ALL, nullptr, (void**) &client);
    if (hrAct == E_ACCESSDENIED) owner.accessDenied.store (true);
    if (client == nullptr) return SessionResult::Faulted;

    WAVEFORMATEX* fmt = nullptr;
    client->GetMixFormat (&fmt);
    if (fmt == nullptr) return SessionResult::Faulted;

    // Store sample rate so the FFT mapper uses the correct Hz values
    owner.capturedSampleRate.store ((float) fmt->nSamplesPerSec);
    const int numChannels = (int) fmt->nChannels;

    // Output devices are tapped via loopback; input devices are captured directly (no flag).
    const DWORD streamFlags = isCapture ? 0u : (DWORD) AUDCLNT_STREAMFLAGS_LOOPBACK;
    const HRESULT hr = client->Initialize (AUDCLNT_SHAREMODE_SHARED, streamFlags,
                                           10000000 /*1 s buffer*/, 0, fmt, nullptr);
    ::CoTaskMemFree (fmt);
    owner.accessDenied.store (hr == E_ACCESSDENIED);
    if (FAILED (hr)) return SessionResult::Faulted;

    client->GetService (__uuidof (IAudioCaptureClient), (void**) &capture);
    if (capture == nullptr) return SessionResult::Faulted;

    client->Start();
    started = true;
    owner.captureState.store (CaptureState::Running);

    // Loopback starvation guard. When nothing is playing (e.g. the user pauses), the
    // render endpoint goes idle and loopback simply stops delivering packets — it does
    // NOT send silence. Without samples the analyzer never runs, so the last frame's
    // levels froze and the map stayed raised. Once no packet has arrived for longer than
    // a normal 10 ms period allows, feed real-time digital silence so the analysis clock
    // keeps ticking and the envelopes decay to zero on their normal release times.
    const double sampleRate    = (double) owner.capturedSampleRate.load();
    double       accountedMs   = juce::Time::getMillisecondCounterHiRes();   // audio time covered up to here
    constexpr double starveMs  = 40.0;    // > a few WASAPI periods: the endpoint is idle
    constexpr double maxFillMs = 250.0;   // cap one catch-up burst (e.g. after a stall)

    while (! threadShouldExit())
    {
        if (owner.inputCycleRequest.load() != 0)   // user picked another source
            return SessionResult::SwitchDevice;

        juce::Thread::sleep (10);

        UINT32 packetSize = 0;
        // A failure here (typically AUDCLNT_E_DEVICE_INVALIDATED) means the device went away.
        if (FAILED (capture->GetNextPacketSize (&packetSize))) return SessionResult::Faulted;

        const double nowMs = juce::Time::getMillisecondCounterHiRes();
        if (packetSize > 0)
        {
            accountedMs = nowMs;
        }
        else if (! isCapture && nowMs - accountedMs > starveMs)
        {
            const double fillMs = juce::jmin (maxFillMs, nowMs - accountedMs);
            const int    n      = (int) (fillMs * 0.001 * sampleRate);
            for (int i = 0; i < n; ++i)
                owner.pushSample (0.0f);
            accountedMs = nowMs;
        }

        while (packetSize > 0)
        {
            BYTE*  data         = nullptr;
            UINT32 frames       = 0;
            DWORD  captureFlags = 0;

            if (FAILED (capture->GetBuffer (&data, &frames, &captureFlags, nullptr, nullptr)))
                return SessionResult::Faulted;

            // WASAPI shared-mode always delivers 32-bit IEEE float on Vista+
            const auto* floatData = reinterpret_cast<const float*> (data);

            for (UINT32 f = 0; f < frames; ++f)
            {
                float sample = 0.0f;

                if (! (captureFlags & AUDCLNT_BUFFERFLAGS_SILENT))
                    for (int ch = 0; ch < numChannels; ++ch)
                        sample += floatData[f * numChannels + ch];

                owner.pushSample (numChannels > 0 ? sample / numChannels : 0.0f);
            }

            capture->ReleaseBuffer (frames);

            if (FAILED (capture->GetNextPacketSize (&packetSize))) return SessionResult::Faulted;
        }
    }

    return SessionResult::Stopped;   // threadShouldExit() — clean, requested stop
}
#endif
