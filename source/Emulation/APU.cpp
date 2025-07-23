#include <cmath>
#include <cstring>
#include <iostream>

#ifdef __DJGPP__
#include <dos.h>
#include <pc.h>
#include <dpmi.h>
#else
#include <allegro.h>
#include <pthread.h>
#endif

#include "../Configuration.hpp"
#include "APU.hpp"
#include "AllegroMidi.hpp"

    APU::MixCache APU::outputCache[256];
    int APU::cacheIndex = 0;


static const uint8_t lengthTable[] = {
    10, 254, 20, 2, 40, 4, 80, 6, 160, 8, 60, 10, 14, 12, 26, 14,
    12, 16, 24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30
};

static const uint8_t dutyTable[][8] = {
    {0, 1, 0, 0, 0, 0, 0, 0},
    {0, 1, 1, 0, 0, 0, 0, 0},
    {0, 1, 1, 1, 1, 0, 0, 0},
    {1, 0, 0, 1, 1, 1, 1, 1}
};

static const uint8_t triangleTable[] = {
    15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
};

static const uint16_t noiseTable[] = {
    4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068
};

/**
 * Pulse waveform generator.
 */
class Pulse
{
    friend class APU;
public:
    Pulse(uint8_t channel)
    {
        enabled = false;
        this->channel = channel;
        lengthEnabled = false;
        lengthValue = 0;
        timerPeriod = 0;
        timerValue = 0;
        dutyMode = 0;
        dutyValue = 0;
        sweepReload = false;
        sweepEnabled = false;
        sweepNegate = false;
        sweepShift = 0;
        sweepPeriod = 0;
        sweepValue = 0;
        envelopeEnabled = false;
        envelopeLoop = false;
        envelopeStart = false;
        envelopePeriod = 0;
        envelopeValue = 0;
        envelopeVolume = 0;
        constantVolume = 0;
    }

    void writeControl(uint8_t value)
    {
        dutyMode = (value >> 6) & 3;
        lengthEnabled = ((value >> 5) & 1) == 0;
        envelopeLoop = ((value >> 5) & 1) == 1;
        envelopeEnabled = ((value >> 4) & 1) == 0;
        envelopePeriod = value & 15;
        constantVolume = value & 15;
        envelopeStart = true;
    }

    void writeSweep(uint8_t value)
    {
        sweepEnabled = ((value >> 7) & 1) == 1;
        sweepPeriod = ((value >> 4) & 7) + 1;
        sweepNegate = ((value >> 3) & 1) == 1;
        sweepShift = value & 7;
        sweepReload = true;
    }

    void writeTimerLow(uint8_t value)
    {
        timerPeriod = (timerPeriod & 0xff00) | (uint16_t)value;
    }

    void writeTimerHigh(uint8_t value)
    {
        lengthValue = lengthTable[value >> 3];
        timerPeriod = (timerPeriod & 0xff) | ((uint16_t)(value & 7) << 8);
        envelopeStart = true;
        dutyValue = 0;
    }

    void stepTimer()
    {
        if (timerValue == 0)
        {
            timerValue = timerPeriod;
            dutyValue = (dutyValue + 1) % 8;
        }
        else
        {
            timerValue--;
        }
    }

    void stepEnvelope()
    {
        if (envelopeStart)
        {
            envelopeVolume = 15;
            envelopeValue = envelopePeriod;
            envelopeStart = false;
        }
        else if (envelopeValue > 0)
        {
            envelopeValue--;
        }
        else
        {
            if (envelopeVolume > 0)
            {
                envelopeVolume--;
            }
            else if (envelopeLoop)
            {
                envelopeVolume = 15;
            }
            envelopeValue = envelopePeriod;
        }
    }

    void stepSweep()
    {
        if (sweepReload)
        {
            if (sweepEnabled && sweepValue == 0)
            {
                sweep();
            }
            sweepValue = sweepPeriod;
            sweepReload = false;
        }
        else if (sweepValue > 0)
        {
            sweepValue--;
        }
        else
        {
            if (sweepEnabled)
            {
                sweep();
            }
            sweepValue = sweepPeriod;
        }
    }

    void stepLength()
    {
        if (lengthEnabled && lengthValue > 0)
        {
            lengthValue--;
        }
    }

    void sweep()
    {
        uint16_t delta = timerPeriod >> sweepShift;
        if (sweepNegate)
        {
            timerPeriod -= delta;
            if (channel == 1)
            {
                timerPeriod--;
            }
        }
        else
        {
            timerPeriod += delta;
        }
    }

    uint8_t output()
    {
        if (!enabled)
        {
            return 0;
        }
        if (lengthValue == 0)
        {
            return 0;
        }
        if (dutyTable[dutyMode][dutyValue] == 0)
        {
            return 0;
        }
        if (timerPeriod < 8 || timerPeriod > 0x7ff)
        {
            return 0;
        }
        if (envelopeEnabled)
        {
            return envelopeVolume;
        }
        else
        {
            return constantVolume;
        }
    }

private:
    bool enabled;
    uint8_t channel;
    bool lengthEnabled;
    uint8_t lengthValue;
    uint16_t timerPeriod;
    uint16_t timerValue;
    uint8_t dutyMode;
    uint8_t dutyValue;
    bool sweepReload;
    bool sweepEnabled;
    bool sweepNegate;
    uint8_t sweepShift;
    uint8_t sweepPeriod;
    uint8_t sweepValue;
    bool envelopeEnabled;
    bool envelopeLoop;
    bool envelopeStart;
    uint8_t envelopePeriod;
    uint8_t envelopeValue;
    uint8_t envelopeVolume;
    uint8_t constantVolume;
};

/**
 * Triangle waveform generator.
 */
class Triangle
{
    friend class APU;
public:
    Triangle()
    {
        enabled = false;
        lengthEnabled = false;
        lengthValue = 0;
        timerPeriod = 0;
        timerValue = 0;  // This was missing!
        dutyValue = 0;
        counterPeriod = 0;
        counterValue = 0;
        counterReload = false;
    }

    void writeControl(uint8_t value)
    {
        lengthEnabled = ((value >> 7) & 1) == 0;
        counterPeriod = value & 0x7f;
    }

    void writeTimerLow(uint8_t value)
    {
        timerPeriod = (timerPeriod & 0xff00) | (uint16_t)value;
    }

    void writeTimerHigh(uint8_t value)
    {
        lengthValue = lengthTable[value >> 3];
        timerPeriod = (timerPeriod & 0x00ff) | ((uint16_t)(value & 7) << 8);
        timerValue = timerPeriod;
        counterReload = true;
    }

    void stepTimer()
    {
        if (timerValue == 0)
        {
            timerValue = timerPeriod;
            if (lengthValue > 0 && counterValue > 0)
            {
                dutyValue = (dutyValue + 1) % 32;
            }
        }
        else
        {
            timerValue--;
        }
    }

    void stepLength()
    {
        if (lengthEnabled && lengthValue > 0)
        {
            lengthValue--;
        }
    }

void stepCounter()
{
    if (counterReload)
    {
        counterValue = counterPeriod;
    }
    else if (counterValue > 0)
    {
        counterValue--;
    }
    counterReload = false;  // Move this outside the if block
}

    uint8_t output()
    {
        if (!enabled)
        {
            return 0;
        }
        if (lengthValue == 0)
        {
            return 0;
        }
        if (counterValue == 0)
        {
            return 0;
        }
        return triangleTable[dutyValue];
    }

private:
    bool enabled;
    bool lengthEnabled;
    uint8_t lengthValue;
    uint16_t timerPeriod;
    uint16_t timerValue;
    uint8_t dutyValue;
    uint8_t counterPeriod;
    uint8_t counterValue;
    bool counterReload;
};

class Noise
{
    friend class APU;
public:
    Noise()
    {
        enabled = false;
        mode = false;
        shiftRegister = 1;
        lengthEnabled = false;
        lengthValue = 0;
        timerPeriod = 0;
        timerValue = 0;
        envelopeEnabled = false;
        envelopeLoop = false;
        envelopeStart = false;
        envelopePeriod = 0;
        envelopeValue = 0;
        envelopeVolume = 0;
        constantVolume = 0;
    }

    void writeControl(uint8_t value)
    {
        lengthEnabled = ((value >> 5) & 1) == 0;
        envelopeLoop = ((value >> 5) & 1) == 1;
        envelopeEnabled = ((value >> 4) & 1) == 0;
        envelopePeriod = value & 15;
        constantVolume = value & 15;
        envelopeStart = true;
    }

    void writePeriod(uint8_t value)
    {
        mode = (value & 0x80) == 0x80;
        timerPeriod = noiseTable[value & 0x0f];
    }

    void writeLength(uint8_t value)
    {
        lengthValue = lengthTable[value >> 3];
        envelopeStart = true;
    }

    void stepTimer()
    {
        if (timerValue == 0)
        {
            timerValue = timerPeriod;
            uint8_t shift;
            if (mode)
            {
                shift = 6;
            }
            else
            {
                shift = 1;
            }
            uint16_t b1 = shiftRegister & 1;
            uint16_t b2 = (shiftRegister >> shift) & 1;
            shiftRegister >>= 1;
            shiftRegister |= (b1 ^ b2) << 14;
        }
        else
        {
            timerValue--;
        }
    }

    void stepEnvelope()
    {
        if (envelopeStart)
        {
            envelopeVolume = 15;
            envelopeValue = envelopePeriod;
            envelopeStart = false;
        }
        else if (envelopeValue > 0)
        {
            envelopeValue--;
        }
        else
        {
            if (envelopeVolume > 0)
            {
                envelopeVolume--;
            }
            else if (envelopeLoop)
            {
                envelopeVolume = 15;
            }
            envelopeValue = envelopePeriod;
        }
    }

    void stepLength()
    {
        if (lengthEnabled && lengthValue > 0)
        {
            lengthValue--;
        }
    }

    uint8_t output()
    {
        if (!enabled)
        {
            return 0;
        }
        if (lengthValue == 0)
        {
            return 0;
        }
        if ((shiftRegister & 1) == 0)
        {
            return 0;
        }
        if (envelopeEnabled)
        {
            return envelopeVolume;
        }
        else
        {
            return constantVolume;
        }
    }

private:
    bool enabled;
    bool mode;
    uint16_t shiftRegister;
    bool lengthEnabled;
    uint8_t lengthValue;
    uint16_t timerPeriod;
    uint16_t timerValue;
    bool envelopeEnabled;
    bool envelopeLoop;
    bool envelopeStart;
    uint8_t envelopePeriod;
    uint8_t envelopeValue;
    uint8_t envelopeVolume;
    uint8_t constantVolume;
};

APU::APU()
{
    frameValue = 0;
    audioBufferLength = 0;

    // Initialize pointers to null first for safety
    pulse1 = nullptr;
    pulse2 = nullptr;
    triangle = nullptr;
    noise = nullptr;
    gameAudio = nullptr;

    // Clear audio buffer
    memset(audioBuffer, 0, AUDIO_BUFFER_LENGTH);

    try {
        pulse1 = new Pulse(1);
        pulse2 = new Pulse(2);
        triangle = new Triangle;
        noise = new Noise;
        
        // Initialize the enhanced audio system
        gameAudio = new AllegroMIDIAudioSystem(this);
        
        #ifdef __DJGPP__
        printf("APU initialized for DOS - all objects created successfully\n");
        #else
        printf("APU initialized for Linux\n");
        #endif
    } catch (...) {
        printf("ERROR: Failed to create APU objects\n");
        // Clean up any partially created objects
        if (pulse1) { delete pulse1; pulse1 = nullptr; }
        if (pulse2) { delete pulse2; pulse2 = nullptr; }
        if (triangle) { delete triangle; triangle = nullptr; }
        if (noise) { delete noise; noise = nullptr; }
        if (gameAudio) { delete gameAudio; gameAudio = nullptr; }
    }
}

APU::~APU()
{
    if (gameAudio) {
        delete gameAudio;
        gameAudio = nullptr;
    }
    
    if (pulse1) {
        delete pulse1;
        pulse1 = nullptr;
    }
    if (pulse2) {
        delete pulse2;
        pulse2 = nullptr;
    }
    if (triangle) {
        delete triangle;
        triangle = nullptr;
    }
    if (noise) {
        delete noise;
        noise = nullptr;
    }
}

    uint8_t APU::getOutput()
    {
        if (!pulse1 || !pulse2 || !triangle || !noise) {
            return 128;
        }

        uint8_t p1 = pulse1->output();
        uint8_t p2 = pulse2->output();
        uint8_t tri = triangle->output();
        uint8_t noi = noise->output();

        // Check if we've computed this combination before
        for (int i = 0; i < 256; i++) {
            MixCache& cache = outputCache[i];
            if (cache.valid && 
                cache.pulse1_val == p1 && cache.pulse2_val == p2 && 
                cache.triangle_val == tri && cache.noise_val == noi) {
                return cache.result;  // Cache hit - return stored result
            }
        }
 
        // Cache miss - compute the floating point math
        double pulse_sum = p1 + p2;
        double pulse_out = (pulse_sum > 0) ? 95.52 / (8128.0 / pulse_sum + 100.0) : 0.0;
        
        double tnd_sum = tri / 8227.0 + noi / 12241.0;
        double tnd_out = (tnd_sum > 0) ? 163.67 / (1.0 / tnd_sum + 100.0) : 0.0;
        
        uint8_t result = (uint8_t)((pulse_out + tnd_out) * 255.0);

        // Store in cache for next time
        MixCache& cache = outputCache[cacheIndex];
        cache.pulse1_val = p1;
        cache.pulse2_val = p2;
        cache.triangle_val = tri;
        cache.noise_val = noi;
        cache.result = result;
        cache.valid = true;
        cacheIndex = (cacheIndex + 1) & 255;  // Wrap around

        return result;
    }


void APU::output(uint8_t* buffer, int len)
{
    // CHANGE FROM: if (gameAudio && gameAudio->isMIDIMode()) {
    // CHANGE TO:   if (gameAudio && gameAudio->isFMMode()) {
    if (gameAudio && gameAudio->isFMMode()) {
        // Use FM synthesis
        gameAudio->generateAudio(buffer, len);
    } else {
        // Use original APU output
        len = (len > audioBufferLength) ? audioBufferLength : len;
        if (len > audioBufferLength)
        {
            memcpy(buffer, audioBuffer, audioBufferLength);
            audioBufferLength = 0;
        }
        else
        {
            memcpy(buffer, audioBuffer, len);
            audioBufferLength -= len;
            memcpy(audioBuffer, audioBuffer + len, audioBufferLength);
        }
    }
}


void APU::stepFrame()
{
    // Safety check - if objects aren't created, don't crash
    if (!pulse1 || !pulse2 || !triangle || !noise) {
        return;
    }

    // Step the frame counter 4 times per frame, for 240Hz (same as SDL)
    for (int i = 0; i < 4; i++)
    {
        frameValue = (frameValue + 1) % 5;
        switch (frameValue)
        {
        case 1:
        case 3:
            stepEnvelope();
            break;
        case 0:
        case 2:
            stepEnvelope();
            stepSweep();
            stepLength();
            break;
        }

        // Calculate the number of samples needed per 1/4 frame (same as SDL)
        int frequency = Configuration::getAudioFrequency();
        int samplesToWrite = frequency / (Configuration::getFrameRate() * 4);
        if (i == 3)
        {
            // Handle the remainder on the final tick of the frame counter
            samplesToWrite = (frequency / Configuration::getFrameRate()) - 3 * (frequency / (Configuration::getFrameRate() * 4));
        }
        
        // Bounds check
        if (samplesToWrite <= 0 || audioBufferLength + samplesToWrite >= AUDIO_BUFFER_LENGTH) {
            continue;
        }
        
#ifndef __DJGPP__
        // Audio locking for thread safety (like SDL's SDL_LockAudio)
        //static pthread_mutex_t audio_mutex = PTHREAD_MUTEX_INITIALIZER;
        //pthread_mutex_lock(&audio_mutex);
#endif

        // Step the timer ~3729 times per quarter frame (same as SDL)
        int j = 0;
        for (int stepIndex = 0; stepIndex < 3729 && j < samplesToWrite; stepIndex++)
        {
            if ((stepIndex / 3729.0) > (j / (double)samplesToWrite))
            {
                uint8_t sample = getOutput();
                audioBuffer[audioBufferLength + j] = sample;
                j++;
            }

            pulse1->stepTimer();
            pulse2->stepTimer();
            noise->stepTimer();
            triangle->stepTimer();
            triangle->stepTimer(); // Triangle steps twice like in SDL
        }
        audioBufferLength += j; // Use j instead of samplesToWrite
        
#ifndef __DJGPP__
        //pthread_mutex_unlock(&audio_mutex);
#endif        
    }
}


void APU::stepEnvelope()
{
    if (pulse1) pulse1->stepEnvelope();
    if (pulse2) pulse2->stepEnvelope();
    if (triangle) triangle->stepCounter();
    if (noise) noise->stepEnvelope();
}

void APU::stepSweep()
{
    if (pulse1) pulse1->stepSweep();
    if (pulse2) pulse2->stepSweep();
}

void APU::stepLength()
{
    if (pulse1) pulse1->stepLength();
    if (pulse2) pulse2->stepLength();
    if (triangle) triangle->stepLength();
    if (noise) noise->stepLength();
}

void APU::writeControl(uint8_t value)
{
    if (pulse1) pulse1->enabled = (value & 1) == 1;
    if (pulse2) pulse2->enabled = (value & 2) == 2;
    if (triangle) triangle->enabled = (value & 4) == 4;
    if (noise) noise->enabled = (value & 8) == 8;
    
    if (pulse1 && !pulse1->enabled) {
        pulse1->lengthValue = 0;
    }
    if (pulse2 && !pulse2->enabled) {
        pulse2->lengthValue = 0;
    }
    if (triangle && !triangle->enabled) {
        triangle->lengthValue = 0;
    }
    if (noise && !noise->enabled) {
        noise->lengthValue = 0;
    }
}

void APU::writeRegister(uint16_t address, uint8_t value)
{
    // FIRST: Let the enhanced audio system intercept the register write
    if (gameAudio) {
        gameAudio->interceptAPURegister(address, value);
    }
    
    // THEN: Process normally for APU emulation (in case we want to fall back)
    switch (address)
    {
    case 0x4000:
        if (pulse1) pulse1->writeControl(value);
        break;
    case 0x4001:
        if (pulse1) pulse1->writeSweep(value);
        break;
    case 0x4002:
        if (pulse1) pulse1->writeTimerLow(value);
        break;
    case 0x4003:
        if (pulse1) pulse1->writeTimerHigh(value);
        break;
    case 0x4004:
        if (pulse2) pulse2->writeControl(value);
        break;
    case 0x4005:
        if (pulse2) pulse2->writeSweep(value);
        break;
    case 0x4006:
        if (pulse2) pulse2->writeTimerLow(value);
        break;
    case 0x4007:
        if (pulse2) pulse2->writeTimerHigh(value);
        break;
    case 0x4008:
        if (triangle) triangle->writeControl(value);
        break;
    case 0x400a:
        if (triangle) triangle->writeTimerLow(value);
        break;
    case 0x400b:
        if (triangle) triangle->writeTimerHigh(value);
        break;
    case 0x400c:
        if (noise) noise->writeControl(value);
        break;
    case 0x400e:  // FIXED: Remove case 0x400d, only 0x400e
        if (noise) noise->writePeriod(value);
        break;
    case 0x400f:
        if (noise) noise->writeLength(value);
        break;
    case 0x4015:
        writeControl(value);
        break;
    case 0x4017:
        stepEnvelope();
        stepSweep();
        stepLength();
        break;
    default:
        break;
    }
}

void APU::toggleAudioMode() {
    if (gameAudio) {
        // CHANGE FROM: gameAudio->isMIDIMode()
        // CHANGE TO:   gameAudio->isFMMode()
        printf("Current audio mode: %s\n", gameAudio->isFMMode() ? "FM Synthesis" : "APU");
        gameAudio->toggleAudioMode();
        printf("New audio mode: %s\n", gameAudio->isFMMode() ? "FM Synthesis" : "APU");
    } else {
        printf("Enhanced audio system not available\n");
    }
}


bool APU::isUsingMIDI() const {
    return gameAudio && gameAudio->isFMMode();
}

void APU::debugAudio() {
    if (gameAudio) {
        gameAudio->debugPrintChannels();
    } else {
        printf("Enhanced audio system not available\n");
    }
}
