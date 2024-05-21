/*
 * Copyright (C) 2024 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2024 Vladimir Sadovnikov <sadko4u@gmail.com>
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
                        virtual ~IRLoader() override;

                    public:
                        virtual status_t    run() override;
                        void                dump(dspu::IStateDumper *v) const;
                };

                class IRConfigurator: public ipc::ITask
                {
                    private:
                        impulse_reverb     *pCore;

                    public:
                        IRConfigurator(impulse_reverb *base);
                        virtual ~IRConfigurator() override;

                    public:
                        virtual status_t    run() override;
                        void                dump(dspu::IStateDumper *v) const;
                };

                class GCTask: public ipc::ITask
                {
                    private:
                        impulse_reverb     *pCore;

                    public:
                        explicit GCTask(impulse_reverb *base);
                        virtual ~GCTask() override;

                    public:
                        virtual status_t run() override;

                        void        dump(dspu::IStateDumper *v) const;
                };

                typedef struct af_descriptor_t
                {
                    dspu::Toggle        sListen;        // Listen toggle
                    dspu::Sample       *pOriginal;      // Original audio file
                    dspu::Sample       *pProcessed;     // Processed audio file for sampler
                    float              *vThumbs[meta::impulse_reverb_metadata::TRACKS_MAX];           // Thumbnails
                    float               fNorm;          // Norming factor
                    bool                bRender;        // Flag that indicates that file needs rendering
                    status_t            nStatus;
                    bool                bSync;          // Synchronize file

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

                    size_t              nFile;          // File
                    size_t              nTrack;         // Track

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
                static void             destroy_sample(dspu::Sample * &s);
                static void             destroy_samples(dspu::Sample *gc_list);
                static void             destroy_convolver(dspu::Convolver * &c);
                static void             destroy_file(af_descriptor_t *af);
                static void             destroy_channel(channel_t *c);
                static void             destroy_convolver(convolver_t *cv);
                static size_t           get_fft_rank(size_t rank);

            protected:
                bool                    has_active_loading_tasks();
                status_t                load(af_descriptor_t *descr);
                status_t                reconfigure();
                void                    process_loading_tasks();
                void                    process_configuration_tasks();
                void                    process_gc_events();
                void                    process_listen_events();
                void                    perform_convolution(size_t samples);
                void                    output_parameters();
                void                    perform_gc();

            protected:
                size_t                  nInputs;
                size_t                  nReconfigReq;
                size_t                  nReconfigResp;
                size_t                  nRank;
                dspu::Sample           *pGCList;        // Garbage collection list

                input_t                 vInputs[2];
                channel_t               vChannels[2];
                convolver_t             vConvolvers[meta::impulse_reverb_metadata::CONVOLVERS];
                af_descriptor_t         vFiles[meta::impulse_reverb_metadata::FILES];

                IRConfigurator          sConfigurator;
                GCTask                  sGCTask;

                plug::IPort            *pBypass;
                plug::IPort            *pRank;
                plug::IPort            *pDry;
                plug::IPort            *pWet;
                plug::IPort            *pDryWet;
                plug::IPort            *pOutGain;
                plug::IPort            *pPredelay;

                uint8_t                *pData;
                ipc::IExecutor         *pExecutor;

            protected:
                void                do_destroy();

            public:
                explicit impulse_reverb(const meta::plugin_t *metadata);
                impulse_reverb(const impulse_reverb &) = delete;
                impulse_reverb(impulse_reverb &&) = delete;
                virtual ~impulse_reverb() override;

                impulse_reverb & operator = (const impulse_reverb &) = delete;
                impulse_reverb & operator = (impulse_reverb &&) = delete;

                virtual void        init(plug::IWrapper *wrapper, plug::IPort **ports) override;
                virtual void        destroy() override;

            public:
                virtual void        ui_activated() override;
                virtual void        update_settings() override;
                virtual void        update_sample_rate(long sr) override;

                virtual void        process(size_t samples) override;

                void                dump(dspu::IStateDumper *v) const override;
        };
    } /* namespace plugins */
} /* namespace lsp */

#endif /* PRIVATE_PLUGINS_IMPULSE_REVERB_H_ */
