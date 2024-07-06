/*
 * sst-effects - an open source library of audio effects
 * built by Surge Synth Team.
 *
 * Copyright 2018-2023, various authors, as described in the GitHub
 * transaction log.
 *
 * sst-effects is released under the GNU General Public Licence v3
 * or later (GPL-3.0-or-later). The license is found in the "LICENSE"
 * file in the root of this repository, or at
 * https://www.gnu.org/licenses/gpl-3.0.en.html
 *
 * The majority of these effects at initiation were factored from
 * Surge XT, and so git history prior to April 2023 is found in the
 * surge repo, https://github.com/surge-synthesizer/surge
 *
 * All source in sst-effects available at
 * https://github.com/surge-synthesizer/sst-effects
 */

#ifndef INCLUDE_SST_VOICE_EFFECTS_GENERATOR_GENVA_H
#define INCLUDE_SST_VOICE_EFFECTS_GENERATOR_GENVA_H

#include "sst/basic-blocks/params/ParamMetadata.h"
#include "sst/basic-blocks/dsp/BlockInterpolators.h"
#include "sst/basic-blocks/dsp/QuadratureOscillators.h"
#include "sst/basic-blocks/dsp/DPWSawPulseOscillator.h"
#include "sst/basic-blocks/tables/SincTableProvider.h"

#include "../VoiceEffectCore.h"

#include <iostream>

#include "sst/basic-blocks/mechanics/block-ops.h"

namespace sst::voice_effects::generator
{
template <typename VFXConfig> struct GenVA : core::VoiceEffectTemplateBase<VFXConfig>
{
    static constexpr const char *effectName{"VA Oscillator"};

    static constexpr int numFloatParams{5};
    static constexpr int numIntParams{3};
    
    static constexpr int maxUnison{9};

    using SincTable = sst::basic_blocks::tables::ShortcircuitSincTableProvider;
    const SincTable &sSincTable;

    enum FloatParams
    {
        fpOffset,
        fpLevel,
        fpSync,
        fpWidth,
        fpUniDetune
    };

    enum IntParams
    {
        ipStereo,
        ipWaveform,
        ipUnison,
    };

    basic_blocks::params::ParamMetaData paramAt(int idx) const
    {
        using pmd = basic_blocks::params::ParamMetaData;

        switch (idx)
        {
        case fpOffset:
            if (keytrackOn)
            {
                return pmd()
                    .asFloat()
                    .withRange(-48, 48)
                    .withDefault(0)
                    .withLinearScaleFormatting("semitones")
                    .withName("Tune");
            }
            return pmd().asAudibleFrequency().withName("Frequency");
        case fpLevel:
            return pmd().asCubicDecibelAttenuation().withDefault(0.5f).withName("Level");
        case fpWidth:
            return pmd().asPercent().withName("Pulse Width").withDefault(0.5);
        case fpSync:
            return pmd()
                .asFloat()
                .withRange(0, 96)
                .withName("Sync")
                .withDefault(0)
                .withLinearScaleFormatting("semitones");
        case fpUniDetune:
            return pmd()
                .asFloat()
                .withRange(0.f, 1.f)
                .withDefault(.01f)
                .withLinearScaleFormatting("cents", 100.f)
                .withName("Unison Detune");
        }
        return pmd().withName("Unknown " + std::to_string(idx)).asPercent();
    }

    basic_blocks::params::ParamMetaData intParamAt(int idx) const
    {
        using pmd = basic_blocks::params::ParamMetaData;

        switch (idx)
        {
        case ipStereo:
            return pmd().asBool().withDefault(false).withName("Stereo");
        case ipWaveform:
            return pmd()
                .asInt()
                .withRange(0, 2)
                .withUnorderedMapFormatting({{0, "Sine"}, {1, "Saw"}, {2, "Pulse"}})
                .withName("Waveform");
            case ipUnison:
                return pmd()
                .asInt()
                .withRange(1, maxUnison)
                .withDefault(1)
                .withName("Unison Count");
        }
        return pmd().withName("error");
    }

    GenVA(const SincTable &st) : sSincTable(st), core::VoiceEffectTemplateBase<VFXConfig>()
    {
        for (float &v : oscSum)
            v = 0.f;

        for (int i = 0; i < maxUnison; ++i)
        {
            mPulseOsc[i].initialize(&sSincTable, this->getSampleRate());
        }

    }

    ~GenVA() {}

    void initVoiceEffect() {}
    void initVoiceEffectParams() { this->initToParamMetadataDefault(this); }

    
    class PulseOscillator
    {
        bool mFirstRun{true};
        float samplerate;
        
        sst::basic_blocks::dsp::lipol<float, VFXConfig::blockSize, true> mFreqLerp, mWidthLerp, mSyncLerp, mLevelLerp;
        
        static constexpr int64_t kLarge = 0x10000000000;
        static constexpr float kIntegratorHPF = 0.99999999f;

        float mOscBuffer alignas(16)[VFXConfig::blockSize];
        int64_t mOscState{0}, mSyncState{0};
        bool mPolarity{false};
        float mOscOut{0};
        size_t mBufPos{0};
        
        const SincTable* localSincTable;
        
        void convolute()
        {
            int32_t ipos = (int32_t)(((kLarge + mOscState) >> 16) & 0xFFFFFFFF);
            bool sync = false;
            double freq = mFreqLerp.v;

            if (mSyncState < mOscState)
            {
                ipos = (int32_t)(((kLarge + mSyncState) >> 16) & 0xFFFFFFFF);
                double t =
                    std::max(0.5, samplerate / freq);
                int64_t syncrate = (int64_t)(double)(65536.0 * 16777216.0 * t);
                mOscState = mSyncState;
                mSyncState += syncrate;
                sync = true;
            }
            // generate pulse
            float fpol = mPolarity ? -1.0f : 1.0f;
            int32_t m = ((ipos >> 16) & 0xff) * SincTable::FIRipol_N;
            float lipol = ((float)((uint32_t)(ipos & 0xffff)));

            if (!sync || !mPolarity)
            {
                for (auto k = 0U; k < SincTable::FIRipol_N; k++)
                {
                    mOscBuffer[mBufPos + k & (VFXConfig::blockSize - 1)] +=
                        fpol *
                        (localSincTable->SincTableF32[m + k] + lipol * localSincTable->SincOffsetF32[m + k]);
                }
            }

            if (sync)
                mPolarity = false;

            // add time until next statechange
            double width = (0.5 - 0.499f * std::clamp(mWidthLerp.v, 0.01f, 0.99f));
            double t = std::max(0.5, samplerate / (freq + sync));
            if (mPolarity)
            {
                width = 1 - width;
            }
            int64_t rate = (int64_t)(double)(65536.0 * 16777216.0 * t * width);

            mOscState += rate;
            mPolarity = !mPolarity;
        }
        
    public:
        void initialize(const SincTable* sinc, const float sr)
        {
            localSincTable = sinc;
            samplerate = sr;
            
            mFirstRun = true;
            
            mOscState = 0;
            mSyncState = 0;
            mOscOut = 0;
            mPolarity = false;
            mBufPos = 0;
            
            for (float &v : mOscBuffer)
                v = 0.f;
        }
        
        void setParams(const float freq, const float width, const float sync, const float level)
        {
            mFreqLerp.newValue(freq);
            mWidthLerp.newValue(width);
            mSyncLerp.newValue(sync);
            mLevelLerp.newValue(level);
        }
        
        void run(float *block)
        {
            if (mFirstRun)
            {
                mFirstRun = false;
                
                // initial antipulse
                convolute();
                mOscState -= kLarge;
                for (auto i = 0U; i < VFXConfig::blockSize; i++)
                {
                    mOscBuffer[i] *= -0.5f;
                }
                mOscState = 0;
                mPolarity = 0;
            }

            for (auto k = 0U; k < VFXConfig::blockSize; k++)
            {
                mOscState -= kLarge;
                mSyncState -= kLarge;
                while (mSyncState < 0)
                    this->convolute();
                while (mOscState < 0)
                    this->convolute();
                mOscOut = mOscOut * kIntegratorHPF + mOscBuffer[mBufPos];
                block[k] = mOscOut * mLevelLerp.v * mLevelLerp.v * mLevelLerp.v;
                mOscBuffer[mBufPos] = 0.f;

                mBufPos++;
                mBufPos = mBufPos & (VFXConfig::blockSize - 1);

                mWidthLerp.process();
                mSyncLerp.process();
                mFreqLerp.process();
                mLevelLerp.process();
            }
        }
    }; // PulseOscillator
    
    void detuneStrategy(const int count, const float pitch, float *res)
    {
        float tune = (keytrackOn) ? this->getFloatParam(fpOffset) + pitch : this->getFloatParam(fpOffset);
        float detune = this->getFloatParam(fpUniDetune);
        if (count % 2 == 1) //odd
        {
            res[0] = tune;
            if (count > 2)
            {
                float offsetUnit = 1.f / (count / 2);
                
                int counter = 0;
                for (int i = 1; i < count; ++i)
                {
                    if (i % 2 == 0)
                    {
                        counter--;
                    }
                    float tid = (i + counter) * offsetUnit * detune; //this index's detune

                    res[i] = (i % 2 == 0) ? tune + tid : tune + -tid;
                }
            }
            return;
        }
        else //even
        {
            float offsetUnit = 1.f / (count / 2);

            int counter = 1;
            for (int i = 0; i < count; ++i)
            {
                if (i % 2 == 1)
                {
                    counter--;
                }

                float tid = (i + counter) * offsetUnit * detune;

                res[i] = (i % 2 == 0) ? tune + tid : tune + -tid;
            }
            return;
        }
    } // detuneStrategy
    
    using SineOsc = sst::basic_blocks::dsp::QuadratureOscillator<float>;
    using SawOsc = sst::basic_blocks::dsp::DPWSawOscillator<    sst::basic_blocks::dsp::BlockInterpSmoothingStrategy<VFXConfig::blockSize>>;
    using PulseOsc = GenVA::PulseOscillator;

    void runSine(SineOsc &osc, float *output, const float tune)
    {
        osc.setRate(440.0 * 2 * M_PI * this->note_to_pitch_ignoring_tuning(tune) * this->getSampleRateInv());
        
        for (int k = 0; k < VFXConfig::blockSize; k++)
        {
            output[k] = osc.v;
            osc.step();
        }
    }

    void runSaw(SawOsc &osc, float *output, const float tune)
    {
        osc.setFrequency(440.0 * this->note_to_pitch_ignoring_tuning(tune), this->getSampleRateInv());

        for (int k = 0; k < VFXConfig::blockSize; k++)
        {
            output[k] = osc.step();
        }
    }

    void runPulse(PulseOsc &osc, float *output, const float tune)
    {
        float freq = 440.f * this->note_to_pitch_ignoring_tuning(tune);
        float sync = 440.f * this->note_to_pitch_ignoring_tuning(this->getFloatParam(fpSync));
        osc.setParams(freq, this->getFloatParam(fpWidth), sync, this->getFloatParam(fpLevel));
        osc.run(output);
    }

    void processMonoToMono(float *datainL, float *dataoutL, float pitch)
    {
        namespace mech = sst::basic_blocks::mechanics;
        float level = std::clamp(this->getFloatParam(fpLevel), 0.f, 1.f);
        level = level * level * level;
        levelLerp.set_target(level);
        
        float tune = (keytrackOn) ? this->getFloatParam(fpOffset) + pitch : this->getFloatParam(fpOffset);
        float detuneParam = this->getFloatParam(fpUniDetune);
        
        int unisonCount = this->getIntParam(ipUnison);
        float detune[unisonCount];
        detuneStrategy(unisonCount, tune, detune);
        
        int wave = this->getIntParam(ipWaveform);
        if (wave == 0)
        {
            for (int i = 0; i < unisonCount; i++)
            {
                float tmp alignas(16)[VFXConfig::blockSize];
                runSine(mQuadOsc[i], tmp, detune[i]);
                if (i == 0)
                {
                    mech::copy_from_to<VFXConfig::blockSize>(tmp, oscSum);
                }
                else
                {
                mech::accumulate_from_to<VFXConfig::blockSize>(tmp, oscSum);
                }
            }
            mech::scale_by<VFXConfig::blockSize>(level, oscSum);
        }
        else if (wave == 1)
        {
            for (int i = 0; i < unisonCount; i++)
            {
                float tmp alignas(16)[VFXConfig::blockSize];
                runSaw(mSawOsc[i], tmp, detune[i]);
                if (i == 0)
                {
                    mech::copy_from_to<VFXConfig::blockSize>(tmp, oscSum);
                }
                else
                {
                mech::accumulate_from_to<VFXConfig::blockSize>(tmp, oscSum);
                }
            }
            mech::scale_by<VFXConfig::blockSize>(level, oscSum);
        }
        else
        {
            for (int i = 0; i < unisonCount; i++)
            {
                float tmp alignas(16)[VFXConfig::blockSize];
                runPulse(mPulseOsc[i], tmp, detune[i]);
                if (i == 0)
                {
                    mech::copy_from_to<VFXConfig::blockSize>(tmp, oscSum);
                }
                else
                {
                mech::accumulate_from_to<VFXConfig::blockSize>(tmp, oscSum);
                }
            }
        }

        sst::basic_blocks::mechanics::copy_from_to<VFXConfig::blockSize>(oscSum, dataoutL);
    }
    
    void processStereo(float *datainL, float *datainR, float *dataoutL, float *dataoutR,
                       float pitch)
    {
        processMonoToMono(datainL, dataoutL, pitch);
        sst::basic_blocks::mechanics::copy_from_to<VFXConfig::blockSize>(dataoutL, dataoutR);
    }

    bool enableKeytrack(bool b)
    {
        auto res = (b != keytrackOn);
        keytrackOn = b;
        return res;
    }
    bool getKeytrack() const { return keytrackOn; }
    bool getKeytrackDefault() const { return true; }

  protected:
    bool keytrackOn{true};
    
    std::array<SineOsc, maxUnison> mQuadOsc;
    std::array<SawOsc, maxUnison> mSawOsc;
    std::array<PulseOsc, maxUnison> mPulseOsc;
    
    float oscSum alignas(16)[VFXConfig::blockSize];

    sst::basic_blocks::dsp::lipol_sse<VFXConfig::blockSize, true> levelLerp;
};
} // namespace sst::voice_effects::generator

#endif // SHORTCIRCUITXT_GENVA_H
