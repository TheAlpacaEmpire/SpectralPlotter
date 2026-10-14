#pragma once

#include <JuceHeader.h>
#include <array>
#include <string>

//==============================================================================
// Rich per-band audio features produced by the analysis (capture) thread and consumed
// on the message thread by the terrain/river models. Replaces the old raw bandLevels /
// bandLevelsFast arrays. Everything here is already smoothed and normalised 0..1 unless
// noted, so consumers can read it directly per frame.
struct AudioFeatures
{
    static constexpr int numBands = 64;

    std::array<float, numBands> harmonic   {};  // HPSS tonal energy   (perceptually weighted, adaptive-smoothed)
    std::array<float, numBands> percussive {};  // HPSS transient energy (fast-smoothed)
    std::array<float, numBands> flux       {};  // per-band positive spectral flux (raw onset strength, 0..~1)
    std::array<float, numBands> level      {};  // harmonic+percussive combined (back-compat for river/hue)

    float centroidNorm  = 0.0f;   // 0..1 overall spectral brightness (spectral centroid, log-mapped)
    float overallEnergy = 0.0f;   // mean(level)
    float bassEnergy    = 0.0f;   // mean(level[0..6])
    float highEnergy    = 0.0f;   // mean(level[37..])
};

//==============================================================================
// Captures the system's default audio output via WASAPI loopback and, on that same
// capture thread, runs an overlapped FFT pipeline (log-frequency band energy, A-weighting,
// HPSS harmonic/percussive separation, spectral flux, adaptive smoothing) to produce an
// AudioFeatures snapshot. The message thread reads the latest snapshot lock-free-ish via
// a short SpinLock copy.
class AudioAnalyzer
{
public:
    static constexpr int fftOrder = 12;
    static constexpr int fftSize  = 1 << fftOrder;   // 4096
    static constexpr int hopSize  = fftSize / 4;     // 1024 samples (75% overlap)
    static constexpr int numBands = AudioFeatures::numBands;
    // HPSS median-in-time window (frames, ~21 ms each at 48 kHz). A causal median only
    // flips once half the window agrees, so this sets the harmonic stream's latency:
    // 9 frames ~= 190 ms window / ~85 ms lag (was 17 => ~180 ms lag). Still long enough
    // that drum hits (< ~60 ms) stay out of the harmonic stream.
    static constexpr int histLen  = 9;

    // Band ranges summarised into bass/high energy scalars (of the 64 log-spaced bands):
    // bass = bands [0, bassBandCount), high = bands [highBandStart, numBands).
    static constexpr int bassBandCount = 7;          // ~20–120 Hz lump
    static constexpr int highBandStart = 37;         // ~2 kHz and up

    // Live state of the WASAPI loopback capture, published for the on-screen telemetry so a
    // dead/reconnecting capture is visible instead of a silent frozen scene.
    enum class CaptureState { Starting, Running, Reconnecting, Failed };

    AudioAnalyzer();
    ~AudioAnalyzer();

    void start();
    void stop();

    // Call once per frame on the message thread — copies the latest published features
    // into the local snapshot that getFeatures() returns.
    void update();

    // Requests that the analysis thread clear its transient/envelope state on its next
    // block — called on reseed so old audio state doesn't flash against fresh terrain.
    void resetFastEnvelope() noexcept { resetRequested.store (true); }

    const AudioFeatures& getFeatures()   const noexcept { return snapshot; }
    float                getSampleRate() const noexcept { return capturedSampleRate.load(); }
    CaptureState         getCaptureState() const noexcept { return captureState.load(); }

    // Audio-source cycling. The capture thread enumerates every active endpoint — output
    // devices (captured via WASAPI loopback) and input devices (mics/line-in, captured
    // directly) — and cycleInput() advances to the next one, restarting capture on it. The
    // current source's friendly name is published for the on-screen control.
    void         cycleInput (int delta = 1) noexcept { inputCycleRequest.fetch_add (delta); }
    juce::String getCurrentInputName() const;

    // Persistence: the WASAPI endpoint id of the source being captured ("" = system default
    // output), and a preferred id to open on the first session. Call setPreferredDeviceId
    // before start(); an id that no longer exists falls back to the default output.
    juce::String getCurrentDeviceId() const;
    void         setPreferredDeviceId (const juce::String& id);

private:
    // Called from the loopback thread — accumulates into the ring and triggers analysis.
    void pushSample (float sample) noexcept;
    void analyzeBlock();                         // runs on the capture thread
    void ensureBandTables (float sampleRate);    // (re)builds per-band bin ranges + weights + alphas

    juce::dsp::FFT                       fft;
    juce::dsp::WindowingFunction<float>  hannWindow;

    // --- capture-thread sample ring + hop tracking ---
    std::array<float, fftSize>     ring    {};   // circular sample buffer, newest at (writePos-1)
    std::array<float, fftSize * 2> fftData {};   // FFT scratch (real in first half)
    int  writePos    = 0;
    int  hopCounter  = 0;

    // --- per-band tables, rebuilt when the sample rate changes ---
    float bandTableSR = 0.0f;
    std::array<int,   numBands> bandBinLo {};
    std::array<int,   numBands> bandBinHi {};
    std::array<float, numBands> bandCenterFreq {};
    std::array<float, numBands> bandWeightDb {};   // perceptual (A-weighting) offset, floored
    std::array<float, numBands> attackAlpha {};    // per-band adaptive smoothing (harmonic stream)
    std::array<float, numBands> decayAlpha {};

    // --- analysis state (capture thread only) ---
    std::array<std::array<float, numBands>, histLen> bandHistory {};  // rolling spectrogram for HPSS
    int histPos = 0, histCount = 0;
    std::array<float, numBands> prevMag {};        // previous frame's percussive target (for flux)
    std::array<float, numBands> harmEnv {};        // smoothed harmonic output
    std::array<float, numBands> percEnv {};        // smoothed percussive output

    // --- published features (guarded) + message-thread snapshot ---
    juce::SpinLock  featuresLock;
    AudioFeatures   published;   // written by capture thread
    AudioFeatures   snapshot;    // message-thread copy (returned by getFeatures)

    std::atomic<bool>  resetRequested   { false };
    std::atomic<float> capturedSampleRate { 48000.0f };
    std::atomic<CaptureState> captureState { CaptureState::Starting };

    // Audio-source selection. inputCycleRequest is added to by the message thread and drained
    // by the capture thread; currentDeviceName is the guarded friendly name shown in the UI.
    std::atomic<int> inputCycleRequest { 0 };
    mutable juce::SpinLock deviceLock;
    juce::String     currentDeviceName { "DEFAULT OUTPUT" };
    juce::String     currentDeviceId;     // "" => default output (guarded by deviceLock)
    juce::String     preferredDeviceId;   // read once by the capture thread's first session

    // WASAPI capture thread — captures the selected output (loopback) or input device.
    struct LoopbackCapture : public juce::Thread
    {
        explicit LoopbackCapture (AudioAnalyzer& o)
            : juce::Thread ("LoopbackCapture"), owner (o) {}
        void run() override;

        // Outcome of one capture session: a clean requested stop, a device fault (retry after
        // backoff), or a user source-switch (restart immediately on the new device).
        enum class SessionResult { Stopped, Faulted, SwitchDevice };

        // Opens the currently-selected endpoint and pumps audio until the thread is asked to
        // stop, the device faults, or the user cycles the source.
        SessionResult tryCaptureSession();

        AudioAnalyzer& owner;
        std::wstring   selectedId;   // WASAPI endpoint id of the current source ("" => default)
    };

    LoopbackCapture loopback { *this };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioAnalyzer)
};
