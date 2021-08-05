/*
 * Copyright (C) 2021 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2021 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-plugins-impulse-reverb
 * Created on: 3 авг. 2021 г.
 *
 * lsp-plugins-impulse-reverb is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * lsp-plugins-impulse-reverb is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with lsp-plugins-impulse-reverb. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef PRIVATE_PLUGINS_IMPULSE_REVERB_H_
#define PRIVATE_PLUGINS_IMPULSE_REVERB_H_

#include <lsp-plug.in/plug-fw/plug.h>
#include <lsp-plug.in/dsp-units/ctl/Toggle.h>
#include <lsp-plug.in/dsp-units/ctl/Bypass.h>
#include <lsp-plug.in/dsp-units/filters/Equalizer.h>
#include <lsp-plug.in/dsp-units/sampling/Sample.h>
#include <lsp-plug.in/dsp-units/sampling/SamplePlayer.h>
#include <lsp-plug.in/dsp-units/util/Convolver.h>
#include <lsp-plug.in/dsp-units/util/Delay.h>

#include <private/meta/impulse_reverb.h>

namespace lsp
{
    namespace plugins
    {
        /**
         * Impulse Reverb Plugin Series
         */
        class impulse_reverb: public plug::Module
        {
            protected:
                struct af_descriptor_t;

                class IRLoader: public ipc::ITask
                {
                    private:
                        impulse_reverb     *pCore;
                        af_descriptor_t    *pDescr;

                    public:
                        inline IRLoader()
                        {
                            pCore       = NULL;
                            pDescr      = NULL;
                        }

                        void init(impulse_reverb *base, af_descriptor_t *descr);
                        virtual ~IRLoader();

                    public:
                        virtual status_t    run();
                        void                dump(dspu::IStateDumper *v) const;
                };

                typedef struct reconfig_t
                {
                    bool                    bRender[meta::impulse_reverb_metadata::FILES];
                    size_t                  nFile[meta::impulse_reverb_metadata::CONVOLVERS];
                    size_t                  nTrack[meta::impulse_reverb_metadata::CONVOLVERS];
                    size_t                  nRank[meta::impulse_reverb_metadata::CONVOLVERS];
                } reconfig_t;

                class IRConfigurator: public ipc::ITask
                {
                    private:
                        reconfig_t          sReconfig;
                        impulse_reverb     *pCore;

                    public:
                        IRConfigurator(impulse_reverb *base);
                        virtual ~IRConfigurator();

                    public:
                        virtual status_t    run();
                        void                dump(dspu::IStateDumper *v) const;

                        inline void set_render(size_t idx, bool render)     { sReconfig.bRender[idx]    = render;   }
                        inline void set_file(size_t idx, size_t file)       { sReconfig.nFile[idx]      = file;     }
                        inline void set_track(size_t idx, size_t track)     { sReconfig.nTrack[idx]     = track;    }
                        inline void set_rank(size_t idx, size_t rank)       { sReconfig.nRank[idx]      = rank;     }
                };

                typedef struct af_descriptor_t
                {
                    dspu::Sample       *pCurr;          // Current audio file
                    dspu::Sample       *pSwap;          // Pointer to audio file for swapping between RT and non-RT code

                    dspu::Toggle        sListen;        // Listen toggle
                    dspu::Sample       *pSwapSample;
                    dspu::Sample       *pCurrSample;    // Rendered file sample
                    float              *vThumbs[meta::impulse_reverb_metadata::TRACKS_MAX];           // Thumbnails
                    float               fNorm;          // Norming factor
                    bool                bRender;        // Flag that indicates that file needs rendering
                    status_t            nStatus;
                    bool                bSync;          // Synchronize file
                    bool                bSwap;          // Swap samples

                    float               fHeadCut;
                    float               fTailCut;
                    float               fFadeIn;
                    float               fFadeOut;
                    bool                bReverse;

                    IRLoader            sLoader;        // Audio file loader task

                    plug::IPort        *pFile;          // Port that contains file name
                    plug::IPort        *pHeadCut;
                    plug::IPort        *pTailCut;
                    plug::IPort        *pFadeIn;
                    plug::IPort        *pFadeOut;
                    plug::IPort        *pListen;
                    plug::IPort        *pReverse;       // Reverse
                    plug::IPort        *pStatus;        // Status of file loading
                    plug::IPort        *pLength;        // Length of file
                    plug::IPort        *pThumbs;        // Thumbnails of file
                } af_descriptor_t;

                typedef struct convolver_t
                {
                    dspu::Delay         sDelay;         // Delay line

                    dspu::Convolver    *pCurr;          // Currently used convolver
                    dspu::Convolver    *pSwap;          // Swap

                    size_t              nRank;          // Last applied rank
                    size_t              nRankReq;       // Rank request
                    size_t              nSource;        // Source
                    size_t              nFileReq;       // File request
                    size_t              nTrackReq;      // Track request

                    float              *vBuffer;        // Buffer for convolution
                    float               fPanIn[2];      // Input panning of convolver
                    float               fPanOut[2];     // Output panning of convolver

                    plug::IPort        *pMakeup;        // Makeup gain of convolver
                    plug::IPort        *pPanIn;         // Input panning of convolver
                    plug::IPort        *pPanOut;        // Output panning of convolver
                    plug::IPort        *pFile;          // Convolver source file
                    plug::IPort        *pTrack;         // Convolver source file track
                    plug::IPort        *pPredelay;      // Pre-delay
                    plug::IPort        *pMute;          // Mute button
                    plug::IPort        *pActivity;      // Activity indicator
                } convolver_t;

                typedef struct channel_t
                {
                    dspu::Bypass        sBypass;
                    dspu::SamplePlayer  sPlayer;
                    dspu::Equalizer     sEqualizer;     // Wet signal equalizer

                    float              *vOut;
                    float              *vBuffer;        // Rendering buffer
                    float               fDryPan[2];     // Dry panorama

                    plug::IPort        *pOut;

                    plug::IPort        *pWetEq;         // Wet equalization flag
                    plug::IPort        *pLowCut;        // Low-cut flag
                    plug::IPort        *pLowFreq;       // Low-cut frequency
                    plug::IPort        *pHighCut;       // High-cut flag
                    plug::IPort        *pHighFreq;      // Low-cut frequency
                    plug::IPort        *pFreqGain[meta::impulse_reverb_metadata::EQ_BANDS];   // Gain for each band of the Equalizer
                } channel_t;

                typedef struct input_t
                {
                    float              *vIn;            // Input data
                    plug::IPort        *pIn;            // Input port
                    plug::IPort        *pPan;           // Panning
                } input_t;

            protected:
                status_t                load(af_descriptor_t *descr);
                status_t                reconfigure(const reconfig_t *cfg);
                static void             destroy_file(af_descriptor_t *af);
                static void             destroy_channel(channel_t *c);
                static void             destroy_convolver(convolver_t *cv);
                static size_t           get_fft_rank(size_t rank);
                void                    sync_offline_tasks();

            protected:
                size_t                  nInputs;
                size_t                  nReconfigReq;
                size_t                  nReconfigResp;

                input_t                 vInputs[2];
                channel_t               vChannels[2];
                convolver_t             vConvolvers[meta::impulse_reverb_metadata::CONVOLVERS];
                af_descriptor_t         vFiles[meta::impulse_reverb_metadata::FILES];

                IRConfigurator          sConfigurator;

                plug::IPort            *pBypass;
                plug::IPort            *pRank;
                plug::IPort            *pDry;
                plug::IPort            *pWet;
                plug::IPort            *pOutGain;
                plug::IPort            *pPredelay;

                uint8_t                *pData;
                ipc::IExecutor         *pExecutor;

            public:
                explicit impulse_reverb(const meta::plugin_t *metadata);
                virtual ~impulse_reverb();

            public:
                virtual void        init(plug::IWrapper *wrapper);
                virtual void        destroy();

                virtual void        ui_activated();
                virtual void        update_settings();
                virtual void        update_sample_rate(long sr);

                virtual void        process(size_t samples);

                void                dump(dspu::IStateDumper *v) const;
        };
    } // namespace plugins
} // namespace lsp

#endif /* PRIVATE_PLUGINS_IMPULSE_REVERB_H_ */
