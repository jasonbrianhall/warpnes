#ifndef APU_HPP
#define APU_HPP

#include <cstdint>
#include <functional>

#ifndef __DJGPP__
#include <mutex>
#endif

#define AUDIO_BUFFER_LENGTH 4096

class Pulse;
class Triangle;
class Noise;
class DMC;
class AllegroMIDIAudioSystem; // FM-synthesis alternative output (WarpNES)

/**
 * Audio processing unit emulator.
 *
 * Output format is signed 8-bit mono (matches the AUDIO_S8 SDL device).
 */
class APU
{
public:
    APU();
    ~APU();

    /**
     * Step the APU by one video frame (1/frameRate seconds of NES time).
     */
    void stepFrame();

    /**
     * Advance the APU by a number of CPU cycles. WarpNES calls this before
     * every APU register access and at the end of each frame, so register
     * writes take effect at the right point in the audio.
     */
    void run(uint32_t cpuCycles);

    /**
     * Memory read used by the DMC channel to fetch sample bytes.
     */
    void setMemoryReader(std::function<uint8_t(uint16_t)> reader);

    /**
     * Cartridge expansion audio (VRC6, Sunsoft 5B, Namco 163). The callback
     * advances the mapper's sound by n CPU cycles and returns its output
     * summed over them. Pass nullptr to remove.
     */
    void setExpansionAudio(std::function<float(uint32_t)> source) { expansion = std::move(source); expansionSum = 0.0f; }

    /**
     * Output audio samples to the provided buffer. Always fills all
     * len bytes (pads with the last sample on underrun).
     * @param buffer Output buffer for audio samples
     * @param len Length of the buffer in bytes
     */
    void output(uint8_t* buffer, int len);

    /**
     * Write to an APU register.
     * @param address Register address
     * @param value Value to write
     */
    void writeRegister(uint16_t address, uint8_t value);

    /**
     * Read $4015: bit n set while channel n's length counter is non-zero.
     */
    uint8_t readStatus();

    /**
     * Frames per second the frontend actually runs the emulator at, so each
     * stepFrame() produces matching audio. 0 = use Configuration::getFrameRate().
     */
    void setFrameRate(double fps) { frameRateOverride = fps; }

    /**
     * Toggle between APU and FM-synthesis audio modes.
     */
    void toggleAudioMode();

    /**
     * Check if currently using the FM-synthesis mode.
     */
    bool isUsingMIDI() const;

    /**
     * Debug audio channels.
     */
    void debugAudio();

private:
    AllegroMIDIAudioSystem* gameAudio;  /**< FM-synthesis audio system */
    double frameRateOverride = 0.0;

    // Ring buffer of signed 8-bit samples
    int8_t audioBuffer[AUDIO_BUFFER_LENGTH];
    int readPos;
    int audioBufferLength;      /**< Amount of data currently in buffer */
    int8_t lastSample;

#ifndef __DJGPP__
    std::mutex bufferMutex;
#endif

    // Frame sequencer
    int frameStep;              /**< Next step of the frame sequencer */
    bool fiveStepMode;          /**< $4017 bit 7 */
    bool frameIrqInhibit;       /**< $4017 bit 6 */
    bool frameIrqFlag;          /**< $4015 bit 6 (flag only; no CPU IRQ is raised) */
    uint32_t sequencerCycle;    /**< CPU cycles since the sequencer was reset */

    // Timing / resampling
    double cycleRemainder;      /**< Fractional CPU cycles carried between frames */
    double sampleClock;         /**< CPU cycles still needed for the next sample */
    uint32_t pulse1Sum;         /**< Channel outputs summed over the current sample */
    uint32_t pulse2Sum;
    uint32_t triangleSum;
    uint32_t noiseSum;
    uint32_t dmcSum;
    uint32_t sumCycles;         /**< CPU cycles covered by the sums */
    std::function<float(uint32_t)> expansion;
    float expansionSum = 0.0f;

    // Output filters (approximation of the NES's analog output stage)
    int filterRate;
    float hpCoeff;
    float lpCoeff;
    float hpPrevIn;
    float hpPrevOut;
    float lpPrevOut;

    Pulse* pulse1;
    Pulse* pulse2;
    Triangle* triangle;
    Noise* noise;
    DMC* dmc;

    float pulseTable[32];
    float tndTable[204];

    float mixAverage();
    void pushSample(float value);

    void clockQuarterFrame();
    void clockHalfFrame();
    uint32_t cyclesToSequencerEvent() const;
    void sequencerEvent();
    double effectiveClock() const;
    void writeControl(uint8_t value);
    void writeFrameCounter(uint8_t value);
};

#endif // APU_HPP
