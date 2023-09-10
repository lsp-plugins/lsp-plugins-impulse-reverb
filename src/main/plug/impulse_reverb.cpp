/*
 * Copyright (C) 2023 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2023 Vladimir Sadovnikov <sadko4u@gmail.com>
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

#include <private/plugins/impulse_reverb.h>
#include <lsp-plug.in/common/alloc.h>
#include <lsp-plug.in/common/atomic.h>
#include <lsp-plug.in/common/debug.h>
#include <lsp-plug.in/dsp/dsp.h>
#include <lsp-plug.in/dsp-units/units.h>
#include <lsp-plug.in/dsp-units/misc/fade.h>
#include <lsp-plug.in/plug-fw/meta/func.h>

#define TMP_BUF_SIZE            4096
#define CONV_RANK               10

namespace lsp
{
    namespace plugins
    {
        static plug::IPort *TRACE_PORT(plug::IPort *p)
        {
            lsp_trace("  port id=%s", (p)->metadata()->id);
            return p;
        }

        //---------------------------------------------------------------------
        // Plugin factory
        static const meta::plugin_t *plugins[] =
        {
            &meta::impulse_reverb_mono,
            &meta::impulse_reverb_stereo
        };

        static plug::Module *plugin_factory(const meta::plugin_t *meta)
        {
            return new impulse_reverb(meta);
        }

        static plug::Factory factory(plugin_factory, plugins, 2);

        //---------------------------------------------------------------------
        static const float band_freqs[] =
        {
            73.0f,
            156.0f,
            332.0f,
            707.0f,
            1507.0f,
            3213.0f,
            6849.0f
        };

        //---------------------------------------------------------------------
        void impulse_reverb::IRLoader::init(impulse_reverb *base, af_descriptor_t *descr)
        {
            pCore       = base;
            pDescr      = descr;
        }

        impulse_reverb::IRLoader::~IRLoader()
        {
            pCore       = NULL;
            pDescr      = NULL;
        }

        status_t impulse_reverb::IRLoader::run()
        {
            return pCore->load(pDescr);
        }

        void impulse_reverb::IRLoader::dump(dspu::IStateDumper *v) const
        {
            v->write("pCore", pCore);
            v->write("pDescr", pDescr);
        }

        //---------------------------------------------------------------------
        impulse_reverb::IRConfigurator::IRConfigurator(impulse_reverb *base)
        {
            pCore       = base;
        }

        impulse_reverb::IRConfigurator::~IRConfigurator()
        {
            pCore       = NULL;
        }

        status_t impulse_reverb::IRConfigurator::run()
        {
            return pCore->reconfigure();
        }

        void impulse_reverb::IRConfigurator::dump(dspu::IStateDumper *v) const
        {
            v->write("pCore", pCore);
        }

        //-------------------------------------------------------------------------
        impulse_reverb::GCTask::GCTask(impulse_reverb *base)
        {
            pCore       = base;
        }

        impulse_reverb::GCTask::~GCTask()
        {
            pCore       = NULL;
        }

        status_t impulse_reverb::GCTask::run()
        {
            pCore->perform_gc();
            return STATUS_OK;
        }

        void impulse_reverb::GCTask::dump(dspu::IStateDumper *v) const
        {
            v->write("pCore", pCore);
        }

        //-------------------------------------------------------------------------
        impulse_reverb::impulse_reverb(const meta::plugin_t *metadata):
            plug::Module(metadata),
            sConfigurator(this),
            sGCTask(this)
        {
            nInputs         = 0;
            for (const meta::port_t *p = metadata->ports; p->id != NULL; ++p)
                if ((meta::is_in_port(p)) && (meta::is_audio_port(p)))
                    ++nInputs;

            nReconfigReq    = 0;
            nReconfigResp   = -1;
            nRank           = 0;
            pGCList         = NULL;

            for (size_t i=0; i<2; ++i)
            {
                input_t *in     = &vInputs[i];
                in->vIn         = NULL;
                in->pIn         = NULL;
                in->pPan        = NULL;
            }

            for (size_t i=0; i<2; ++i)
            {
                channel_t *c    = &vChannels[i];

                c->vOut         = NULL;
                c->vBuffer      = NULL;
                c->fDryPan[0]   = 0.0f;
                c->fDryPan[1]   = 0.0f;

                c->pOut         = NULL;
                c->pWetEq       = NULL;
                c->pLowCut      = NULL;
                c->pLowFreq     = NULL;
                c->pHighCut     = NULL;
                c->pHighFreq    = NULL;

                for (size_t j=0; j<meta::impulse_reverb_metadata::EQ_BANDS; ++j)
                    c->pFreqGain[j]     = NULL;
            }

            for (size_t i=0; i<meta::impulse_reverb_metadata::CONVOLVERS; ++i)
            {
                convolver_t *c      = &vConvolvers[i];

                c->pCurr            = NULL;
                c->pSwap            = NULL;

                c->nFile            = 0;
                c->nTrack           = 0;

                c->vBuffer          = NULL;
                c->fPanIn[0]        = 0.0f;
                c->fPanIn[1]        = 0.0f;
                c->fPanOut[0]       = 0.0f;
                c->fPanOut[1]       = 0.0f;

                c->pMakeup          = NULL;
                c->pPanIn           = NULL;
                c->pPanOut          = NULL;
                c->pFile            = NULL;
                c->pTrack           = NULL;
                c->pPredelay        = NULL;
                c->pMute            = NULL;
                c->pActivity        = NULL;
            }

            for (size_t i=0; i<meta::impulse_reverb_metadata::FILES; ++i)
            {
                af_descriptor_t *af = &vFiles[i];

                af->pOriginal       = NULL;
                af->pProcessed      = NULL;

                for (size_t j=0; j<meta::impulse_reverb_metadata::TRACKS_MAX; ++j)
                    af->vThumbs[j]      = NULL;

                af->fNorm           = 0.0f;
                af->bRender         = true;
                af->nStatus         = STATUS_UNKNOWN_ERR;
                af->bSync           = true;

                af->fHeadCut        = 0.0f;
                af->fTailCut        = 0.0f;
                af->fFadeIn         = 0.0f;
                af->fFadeOut        = 0.0f;
                af->bReverse        = false;

                af->pFile           = NULL;
                af->pHeadCut        = NULL;
                af->pTailCut        = NULL;
                af->pFadeIn         = NULL;
                af->pFadeOut        = NULL;
                af->pListen         = NULL;
                af->pReverse        = NULL;
                af->pStatus         = NULL;
                af->pLength         = NULL;
                af->pThumbs         = NULL;
            }

            pBypass         = NULL;
            pRank           = NULL;
            pDry            = NULL;
            pWet            = NULL;
            pOutGain        = NULL;
            pPredelay       = NULL;

            pData           = NULL;
            pExecutor       = NULL;
        }

        impulse_reverb::~impulse_reverb()
        {
            do_destroy();
        }

        void impulse_reverb::destroy_file(af_descriptor_t *af)
        {
            // Destroy current file
            destroy_sample(af->pOriginal);
            destroy_sample(af->pProcessed);

            // Forget port
            af->pFile       = NULL;
        }

        void impulse_reverb::destroy_convolver(convolver_t *cv)
        {
            cv->sDelay.destroy();

            if (cv->pCurr != NULL)
            {
                lsp_trace("Destroying pCurr=%p", cv->pSwap);
                cv->pCurr->destroy();
                delete cv->pCurr;
                cv->pCurr    = NULL;
            }

            if (cv->pSwap != NULL)
            {
                lsp_trace("Destroying pSwap=%p", cv->pSwap);
                cv->pSwap->destroy();
                delete cv->pSwap;
                cv->pSwap    = NULL;
            }

            cv->vBuffer     = NULL;
        }

        void impulse_reverb::destroy_channel(channel_t *c)
        {
            dspu::Sample *gc_list = c->sPlayer.destroy(false);
            destroy_samples(gc_list);

            c->sEqualizer.destroy();
            c->vOut     = NULL;
            c->vBuffer  = NULL;
        }

        void impulse_reverb::perform_gc()
        {
            dspu::Sample *gc_list = lsp::atomic_swap(&pGCList, NULL);
            destroy_samples(gc_list);
        }

        void impulse_reverb::destroy_sample(dspu::Sample * &s)
        {
            if (s == NULL)
                return;

            s->destroy();
            delete s;
            lsp_trace("Destroyed sample %p", s);
            s   = NULL;
        }

        void impulse_reverb::destroy_convolver(dspu::Convolver * &c)
        {
            if (c == NULL)
                return;

            c->destroy();
            delete c;
            lsp_trace("Destroyed convolver %p", c);
            c   = NULL;
        }

        void impulse_reverb::destroy_samples(dspu::Sample *gc_list)
        {
            // Iterate over the list and destroy each sample in the list
            while (gc_list != NULL)
            {
                dspu::Sample *next = gc_list->gc_next();
                destroy_sample(gc_list);
                gc_list = next;
            }
        }

        size_t impulse_reverb::get_fft_rank(size_t rank)
        {
            return meta::impulse_reverb_metadata::FFT_RANK_MIN + rank;
        }

        void impulse_reverb::init(plug::IWrapper *wrapper, plug::IPort **ports)
        {
            // Pass wrapper
            plug::Module::init(wrapper, ports);

            // Remember executor service
            pExecutor       = wrapper->executor();
            lsp_trace("Executor = %p", pExecutor);

            // Allocate buffer data
            size_t tmp_buf_size = TMP_BUF_SIZE * sizeof(float);
            size_t thumbs_size  = meta::impulse_reverb_metadata::MESH_SIZE * sizeof(float);
            size_t alloc        = tmp_buf_size * (meta::impulse_reverb_metadata::CONVOLVERS + 2) +
                                  thumbs_size * meta::impulse_reverb_metadata::TRACKS_MAX * meta::impulse_reverb_metadata::FILES;
            pData               = new uint8_t[alloc + DEFAULT_ALIGN];
            if (pData == NULL)
                return;
            uint8_t *ptr    = align_ptr(pData, DEFAULT_ALIGN);

            // Initialize inputs
            for (size_t i=0; i<2; ++i)
            {
                input_t *in     = &vInputs[i];
                in->vIn         = NULL;
                in->pIn         = NULL;
                in->pPan        = NULL;
            }

            // Initialize files
            for (size_t i=0; i<meta::impulse_reverb_metadata::FILES; ++i)
            {
                af_descriptor_t *f    = &vFiles[i];

                f->sListen.init();

                f->pOriginal    = NULL;
                f->pProcessed   = NULL;

                for (size_t j=0; j<meta::impulse_reverb_metadata::TRACKS_MAX; ++j)
                {
                    f->vThumbs[j]   = reinterpret_cast<float *>(ptr);
                    ptr            += thumbs_size;
                }

                f->fNorm        = 1.0f;
                f->bRender      = false;
                f->nStatus      = STATUS_UNSPECIFIED;
                f->bSync        = true;
                f->fHeadCut     = 0.0f;
                f->fTailCut     = 0.0f;
                f->fFadeIn      = 0.0f;
                f->fFadeOut     = 0.0f;
                f->bReverse     = false;

                // Initialize loader
                f->sLoader.init(this, f);

                f->pFile        = NULL;
                f->pHeadCut     = NULL;
                f->pTailCut     = NULL;
                f->pFadeIn      = NULL;
                f->pFadeOut     = NULL;
                f->pListen      = NULL;
                f->pReverse     = NULL;
                f->pStatus      = NULL;
                f->pLength      = NULL;
                f->pThumbs      = NULL;
            }

            // Initialize convolvers
            for (size_t i=0; i<meta::impulse_reverb_metadata::CONVOLVERS; ++i)
            {
                convolver_t *cv     = &vConvolvers[i];

                cv->pCurr           = NULL;
                cv->pSwap           = NULL;
                cv->nFile           = 0;
                cv->nTrack          = 0;

                cv->vBuffer         = reinterpret_cast<float *>(ptr);
                ptr                += tmp_buf_size;

                cv->fPanIn[0]       = 1.0f;
                cv->fPanIn[1]       = 0.0f;
                cv->fPanOut[0]      = 1.0f;
                cv->fPanOut[1]      = 0.0f;

                cv->pMakeup         = NULL;
                cv->pPanIn          = NULL;
                cv->pPanOut         = NULL;
                cv->pFile           = NULL;
                cv->pTrack          = NULL;
                cv->pPredelay       = NULL;
                cv->pMute           = NULL;
                cv->pActivity       = NULL;
            }

            // Initialize output channels
            for (size_t i=0; i<2; ++i)
            {
                channel_t *c    = &vChannels[i];

                if (!c->sPlayer.init(meta::impulse_reverb_metadata::FILES, 32))
                    return;
                if (!c->sEqualizer.init(meta::impulse_reverb_metadata::EQ_BANDS + 2, CONV_RANK))
                    return;
                c->sEqualizer.set_mode(dspu::EQM_BYPASS);

                c->fDryPan[0]   = 0.0f;
                c->fDryPan[1]   = 0.0f;

                c->vOut         = NULL;
                c->vBuffer      = reinterpret_cast<float *>(ptr);
                ptr            += tmp_buf_size;

                c->pOut         = NULL;

                c->pWetEq       = NULL;
                c->pLowCut      = NULL;
                c->pLowFreq     = NULL;
                c->pHighCut     = NULL;
                c->pHighFreq    = NULL;

                for (size_t j=0; j<meta::impulse_reverb_metadata::EQ_BANDS; ++j)
                    c->pFreqGain[j]     = NULL;
            }

            // Bind ports
            size_t port_id = 0;

            lsp_trace("Binding audio ports");
            for (size_t i=0; i<nInputs; ++i)
                vInputs[i].pIn      = TRACE_PORT(ports[port_id++]);
            for (size_t i=0; i<2; ++i)
                vChannels[i].pOut   = TRACE_PORT(ports[port_id++]);

            // Bind common ports
            lsp_trace("Binding common ports");
            pBypass     = TRACE_PORT(ports[port_id++]);
            TRACE_PORT(ports[port_id++]);          // Skip file selector
            pRank       = TRACE_PORT(ports[port_id++]);
            pPredelay   = TRACE_PORT(ports[port_id++]);

            for (size_t i=0; i<nInputs; ++i)        // Panning ports
                vInputs[i].pPan     = TRACE_PORT(ports[port_id++]);

            pDry        = TRACE_PORT(ports[port_id++]);
            pWet        = TRACE_PORT(ports[port_id++]);
            pOutGain    = TRACE_PORT(ports[port_id++]);

            // Bind impulse file ports
            for (size_t i=0; i<meta::impulse_reverb_metadata::FILES; ++i)
            {
                lsp_trace("Binding impulse file #%d ports", int(i));
                af_descriptor_t *f  = &vFiles[i];

                f->pFile        = TRACE_PORT(ports[port_id++]);
                f->pHeadCut     = TRACE_PORT(ports[port_id++]);
                f->pTailCut     = TRACE_PORT(ports[port_id++]);
                f->pFadeIn      = TRACE_PORT(ports[port_id++]);
                f->pFadeOut     = TRACE_PORT(ports[port_id++]);
                f->pListen      = TRACE_PORT(ports[port_id++]);
                f->pReverse     = TRACE_PORT(ports[port_id++]);
                f->pStatus      = TRACE_PORT(ports[port_id++]);
                f->pLength      = TRACE_PORT(ports[port_id++]);
                f->pThumbs      = TRACE_PORT(ports[port_id++]);
            }

            // Bind convolver ports
            for (size_t i=0; i<meta::impulse_reverb_metadata::CONVOLVERS; ++i)
            {
                lsp_trace("Binding convolution #%d ports", int(i));
                convolver_t *c  = &vConvolvers[i];

                if (nInputs > 1)    // Input panning
                    c->pPanIn       = TRACE_PORT(ports[port_id++]);

                c->pFile        = TRACE_PORT(ports[port_id++]);
                c->pTrack       = TRACE_PORT(ports[port_id++]);
                c->pMakeup      = TRACE_PORT(ports[port_id++]);
                c->pMute        = TRACE_PORT(ports[port_id++]);
                c->pActivity    = TRACE_PORT(ports[port_id++]);
                c->pPredelay    = TRACE_PORT(ports[port_id++]);
                c->pPanOut      = TRACE_PORT(ports[port_id++]);
            }

            // Bind wet processing ports
            lsp_trace("Binding wet processing ports");
            size_t port         = port_id;
            for (size_t i=0; i<2; ++i)
            {
                channel_t *c        = &vChannels[i];

                c->pWetEq           = TRACE_PORT(ports[port_id++]);
                TRACE_PORT(ports[port_id++]); // Skip equalizer visibility port
                c->pLowCut          = TRACE_PORT(ports[port_id++]);
                c->pLowFreq         = TRACE_PORT(ports[port_id++]);

                for (size_t j=0; j<meta::impulse_reverb_metadata::EQ_BANDS; ++j)
                    c->pFreqGain[j]     = TRACE_PORT(ports[port_id++]);

                c->pHighCut         = TRACE_PORT(ports[port_id++]);
                c->pHighFreq        = TRACE_PORT(ports[port_id++]);

                port_id             = port;
            }
        }

        void impulse_reverb::destroy()
        {
            plug::Module::destroy();
            do_destroy();
        }

        void impulse_reverb::do_destroy()
        {
            // Destroy files
            for (size_t i=0; i<meta::impulse_reverb_metadata::FILES; ++i)
                destroy_file(&vFiles[i]);

            // Destroy convolvers
            for (size_t i=0; i<meta::impulse_reverb_metadata::CONVOLVERS; ++i)
                destroy_convolver(&vConvolvers[i]);

            // Destroy output channels
            for (size_t i=0; i<2; ++i)
                destroy_channel(&vChannels[i]);

            // Delete all allocated data
            if (pData != NULL)
            {
                delete [] pData;
                pData           = NULL;
            }
        }

        void impulse_reverb::ui_activated()
        {
            // Force file contents to be synchronized with UI
            for (size_t i=0; i<meta::impulse_reverb_metadata::FILES; ++i)
                vFiles[i].bSync     = true;
        }

        void impulse_reverb::update_settings()
        {
            float out_gain      = pOutGain->value();
            float dry_gain      = pDry->value() * out_gain;
            float wet_gain      = pWet->value() * out_gain;
            bool bypass         = pBypass->value() >= 0.5f;
            float predelay      = pPredelay->value();

            // Check that FFT rank has changed
            size_t rank         = get_fft_rank(pRank->value());
            if (rank != nRank)
            {
                nRank               = rank;
                ++nReconfigReq;
            }

            // Adjust volume of dry channel
            if (nInputs == 1)
            {
                float pan               = vInputs[0].pPan->value();
                vChannels[0].fDryPan[0] = (100.0f - pan) * 0.005f * dry_gain;
                vChannels[0].fDryPan[1] = 0.0f;
                vChannels[1].fDryPan[0] = (100.0f + pan) * 0.005f * dry_gain;
                vChannels[1].fDryPan[1] = 0.0f;
            }
            else
            {
                float pan_l             = vInputs[0].pPan->value();
                float pan_r             = vInputs[1].pPan->value();

                vChannels[0].fDryPan[0] = (100.0f - pan_l) * 0.005f * dry_gain;
                vChannels[0].fDryPan[1] = (100.0f - pan_r) * 0.005f * dry_gain;
                vChannels[1].fDryPan[0] = (100.0f + pan_l) * 0.005f * dry_gain;
                vChannels[1].fDryPan[1] = (100.0f + pan_r) * 0.005f * dry_gain;
            }

            // Adjust channel setup
            for (size_t i=0; i<2; ++i)
            {
                channel_t *c        = &vChannels[i];
                c->sBypass.set_bypass(bypass);
                c->sPlayer.set_gain(out_gain);

                // Update equalization parameters
                dspu::Equalizer *eq             = &c->sEqualizer;
                dspu::equalizer_mode_t eq_mode  = (c->pWetEq->value() >= 0.5f) ? dspu::EQM_IIR : dspu::EQM_BYPASS;
                eq->set_mode(eq_mode);

                if (eq_mode != dspu::EQM_BYPASS)
                {
                    dspu::filter_params_t fp;
                    size_t band     = 0;

                    // Set-up parametric equalizer
                    while (band < meta::impulse_reverb_metadata::EQ_BANDS)
                    {
                        if (band == 0)
                        {
                            fp.fFreq        = band_freqs[band];
                            fp.fFreq2       = fp.fFreq;
                            fp.nType        = dspu::FLT_MT_LRX_LOSHELF;
                        }
                        else if (band == (meta::impulse_reverb_metadata::EQ_BANDS - 1))
                        {
                            fp.fFreq        = band_freqs[band-1];
                            fp.fFreq2       = fp.fFreq;
                            fp.nType        = dspu::FLT_MT_LRX_HISHELF;
                        }
                        else
                        {
                            fp.fFreq        = band_freqs[band-1];
                            fp.fFreq2       = band_freqs[band];
                            fp.nType        = dspu::FLT_MT_LRX_LADDERPASS;
                        }

                        fp.fGain        = c->pFreqGain[band]->value();
                        fp.nSlope       = 2;
                        fp.fQuality     = 0.0f;

                        // Update filter parameters
                        eq->set_params(band++, &fp);
                    }

                    // Setup hi-pass filter
                    size_t hp_slope = c->pLowCut->value() * 2;
                    fp.nType        = (hp_slope > 0) ? dspu::FLT_BT_BWC_HIPASS : dspu::FLT_NONE;
                    fp.fFreq        = c->pLowFreq->value();
                    fp.fFreq2       = fp.fFreq;
                    fp.fGain        = 1.0f;
                    fp.nSlope       = hp_slope;
                    fp.fQuality     = 0.0f;
                    eq->set_params(band++, &fp);

                    // Setup low-pass filter
                    size_t lp_slope = c->pHighCut->value() * 2;
                    fp.nType        = (lp_slope > 0) ? dspu::FLT_BT_BWC_LOPASS : dspu::FLT_NONE;
                    fp.fFreq        = c->pHighFreq->value();
                    fp.fFreq2       = fp.fFreq;
                    fp.fGain        = 1.0f;
                    fp.nSlope       = lp_slope;
                    fp.fQuality     = 0.0f;
                    eq->set_params(band++, &fp);
                }
            }

            // Apply panning to each convolver
            for (size_t i=0; i<meta::impulse_reverb_metadata::CONVOLVERS; ++i)
            {
                convolver_t *cv         = &vConvolvers[i];
                float makeup            = cv->pMakeup->value() * wet_gain;
                if (nInputs == 1)
                {
                    cv->fPanIn[0]       = 1.0f;
                    cv->fPanIn[1]       = 0.0f;
                }
                else
                {
                    float pan           = cv->pPanIn->value();
                    cv->fPanIn[0]       = (100.0f - pan) * 0.005f;
                    cv->fPanIn[1]       = (100.0f + pan) * 0.005f;
                }

                float pan           = cv->pPanOut->value();
                cv->fPanOut[0]      = (100.0f - pan) * 0.005f * makeup;
                cv->fPanOut[1]      = (100.0f + pan) * 0.005f * makeup;

                // Set pre-delay
                cv->sDelay.set_delay(dspu::millis_to_samples(fSampleRate, predelay + cv->pPredelay->value()));

                // Analyze source
                size_t file         = (cv->pMute->value() < 0.5f) ? cv->pFile->value() : 0;
                size_t track        = cv->pTrack->value();
                if ((file != cv->nFile) || (track != cv->nTrack))
                {
                    cv->nFile           = file;
                    cv->nTrack          = track;
                    ++nReconfigReq;
                }
            }

            // Apply changes to files
            for (size_t i=0; i<meta::impulse_reverb_metadata::FILES; ++i)
            {
                af_descriptor_t *f  = &vFiles[i];

                // Check that file parameters have changed
                float head_cut      = f->pHeadCut->value();
                float tail_cut      = f->pTailCut->value();
                float fade_in       = f->pFadeIn->value();
                float fade_out      = f->pFadeOut->value();
                bool reverse        = f->pReverse->value() >= 0.5f;
                if ((f->fHeadCut != head_cut) ||
                    (f->fTailCut != tail_cut) ||
                    (f->fFadeIn  != fade_in ) ||
                    (f->fFadeOut != fade_out) ||
                    (f->bReverse != reverse))
                {
                    f->fHeadCut         = head_cut;
                    f->fTailCut         = tail_cut;
                    f->fFadeIn          = fade_in;
                    f->fFadeOut         = fade_out;
                    f->bReverse         = reverse;
                    f->bRender          = true;
                    nReconfigReq        ++;
                }

                // Listen button pressed?
                if (f->pListen != NULL)
                    f->sListen.submit(f->pListen->value());
            }
        }

        void impulse_reverb::update_sample_rate(long sr)
        {
            for (size_t i=0; i<meta::impulse_reverb_metadata::CONVOLVERS; ++i)
                vConvolvers[i].sDelay.init(dspu::millis_to_samples(sr, meta::impulse_reverb_metadata::PREDELAY_MAX * 4.0f));

            for (size_t i=0; i<2; ++i)
            {
                vChannels[i].sBypass.init(sr);
                vChannels[i].sEqualizer.set_sample_rate(sr);
            }

            ++nReconfigReq;
        }

        bool impulse_reverb::has_active_loading_tasks()
        {
            for (size_t i=0; i<meta::impulse_reverb_metadata::FILES; ++i)
                if (!vFiles[i].sLoader.idle())
                    return true;
            return false;
        }

        void impulse_reverb::process_loading_tasks()
        {
            // Do nothing with loading while configurator is active
            if (!sConfigurator.idle())
                return;

            for (size_t i=0; i<meta::impulse_reverb_metadata::FILES; ++i)
            {
                // Get descriptor
                af_descriptor_t *f  = &vFiles[i];
                if (f->pFile == NULL)
                    continue;

                // Get related file path
                plug::path_t *path      = f->pFile->buffer<plug::path_t>();
                if (path == NULL)
                    continue;

                if ((path->pending()) && (f->sLoader.idle())) // There is pending request for file reload
                {
                    // Try to submit task
                    if (pExecutor->submit(&f->sLoader))
                    {
                        lsp_trace("Successfully submitted load task");
                        f->nStatus      = STATUS_LOADING;
                        path->accept();
                    }
                }
                else if ((path->accepted()) && (f->sLoader.completed())) // The reload request has been processed
                {
                    // Update file status and set re-rendering flag
                    f->nStatus      = f->sLoader.code();
                    ++nReconfigReq;

                    // Now we surely can commit changes and reset task state
                    path->commit();
                    f->sLoader.reset();
                }
            }
        }

        void impulse_reverb::process_configuration_tasks()
        {
            // Do nothing if at least one loader is active
            if (has_active_loading_tasks())
                return;

            if ((nReconfigReq != nReconfigResp) && (sConfigurator.idle()))
            {
                // Try to submit task
                if (pExecutor->submit(&sConfigurator))
                {
                    nReconfigResp   = nReconfigReq;
                    lsp_trace("Successfully submitted configuration task");
                }
            }
            else if (sConfigurator.completed())
            {
                // Update samples
                for (size_t i=0; i<meta::impulse_reverb_metadata::FILES; ++i)
                {
                    af_descriptor_t *f = &vFiles[i];

                    // Bind sample player for each output channel
                    for (size_t j=0; j<2; ++j)
                    {
                        channel_t *c = &vChannels[j];
                        c->sPlayer.bind(i, f->pProcessed);
                    }
                    f->pProcessed   = NULL;
                    f->bSync        = true;
                }

                // Update convolvers
                for (size_t i=0; i<meta::impulse_reverb_metadata::CONVOLVERS; ++i)
                {
                    convolver_t *c      = &vConvolvers[i];
                    lsp::swap(c->pCurr, c->pSwap);
                }

                // Reset configurator
                sConfigurator.reset();
            }
        }

        void impulse_reverb::process_gc_events()
        {
            if (sGCTask.completed())
                sGCTask.reset();

            if (sGCTask.idle())
            {
                // Obtain the list of samples for destroy
                if (pGCList == NULL)
                {
                    for (size_t i=0; i<2; ++i)
                        if ((pGCList = vChannels[i].sPlayer.gc()) != NULL)
                            break;
                }
                if (pGCList != NULL)
                    pExecutor->submit(&sGCTask);
            }
        }

        void impulse_reverb::process_listen_events()
        {
            for (size_t i=0; i<meta::impulse_reverb_metadata::FILES; ++i)
            {
                af_descriptor_t *f  = &vFiles[i];

                if (!f->sListen.pending())
                     continue;

                lsp_trace("Submitted listen toggle");
                dspu::Sample *s = vChannels[0].sPlayer.get(i);
                size_t n_c = (s != NULL) ? s->channels() : 0;
                if (n_c > 0)
                {
                    for (size_t j=0; j<2; ++j)
                        vChannels[j].sPlayer.play(i, j%n_c, 1.0f, 0);
                }
                f->sListen.commit();
            }
        }

        void impulse_reverb::perform_convolution(size_t samples)
        {
            // Get pointers to data channels
            for (size_t i=0; i<nInputs; ++i)
                vInputs[i].vIn      = vInputs[i].pIn->buffer<float>();

            for (size_t i=0; i<2; ++i)
                vChannels[i].vOut   = vChannels[i].pOut->buffer<float>();

            // Process samples
            while (samples > 0)
            {
                // Determine number of samples to process
                size_t to_do        = TMP_BUF_SIZE;
                if (to_do > samples)
                    to_do               = samples;

                // Clear temporary channel buffer
                dsp::fill_zero(vChannels[0].vBuffer, to_do);
                dsp::fill_zero(vChannels[1].vBuffer, to_do);

                // Call convolvers
                for (size_t i=0; i<meta::impulse_reverb_metadata::CONVOLVERS; ++i)
                {
                    convolver_t *c      = &vConvolvers[i];

                    // Prepare input buffer: apply panning if present
                    if (nInputs == 1)
                        dsp::copy(c->vBuffer, vInputs[0].vIn, to_do);
                    else
                        dsp::mix_copy2(c->vBuffer, vInputs[0].vIn, vInputs[1].vIn, c->fPanIn[0], c->fPanIn[1], to_do);

                    // Do processing
                    if (c->pCurr != NULL)
                        c->pCurr->process(c->vBuffer, c->vBuffer, to_do);
                    else
                        dsp::fill_zero(c->vBuffer, to_do);
                    c->sDelay.process(c->vBuffer, c->vBuffer, to_do);

                    // Apply processed signal to output channels
                    dsp::fmadd_k3(vChannels[0].vBuffer, c->vBuffer, c->fPanOut[0], to_do);
                    dsp::fmadd_k3(vChannels[1].vBuffer, c->vBuffer, c->fPanOut[1], to_do);
                }

                // Now apply equalization, bypass control and players
                for (size_t i=0; i<2; ++i)
                {
                    channel_t *c        = &vChannels[i];

                    // Apply equalization
                    c->sEqualizer.process(c->vBuffer, c->vBuffer, to_do);

                    // Pass dry sound to output channels
                    if (nInputs == 1)
                        dsp::fmadd_k3(c->vBuffer, vInputs[0].vIn, c->fDryPan[0], to_do);
                    else
                        dsp::mix_add2(c->vBuffer, vInputs[0].vIn, vInputs[1].vIn, c->fDryPan[0], c->fDryPan[1], to_do);

                    // Apply player and bypass
                    c->sPlayer.process(c->vBuffer, c->vBuffer, to_do);
                    c->sBypass.process(c->vOut, vInputs[i%nInputs].vIn, c->vBuffer, to_do);

                    // Update pointers
                    c->vOut            += to_do;
                }

                for (size_t i=0; i<nInputs; ++i)
                    vInputs[i].vIn     += to_do;

                samples            -= to_do;
            }
        }

        void impulse_reverb::output_parameters()
        {
            for (size_t i=0; i<meta::impulse_reverb_metadata::CONVOLVERS; ++i)
            {
                // Output information about the convolver
                convolver_t *c          = &vConvolvers[i];
                c->pActivity->set_value((c->pCurr != NULL) ? 1.0f : 0.0f);
            }

            for (size_t i=0; i<meta::impulse_reverb_metadata::FILES; ++i)
            {
                af_descriptor_t *af     = &vFiles[i];
                if (!af->sLoader.idle())
                    continue;

                // Output information about the file
                dspu::Sample *active    = vChannels[0].sPlayer.get(i);
                size_t channels         = (active != NULL) ? active->channels() : 0;
                channels                = lsp_min(channels, 2u);

                float duration          = (af->pOriginal != NULL) ? af->pOriginal->duration() : 0;
                af->pLength->set_value(duration * 1000.0f);
                af->pStatus->set_value(af->nStatus);

                // Store file dump to mesh
                plug::mesh_t *mesh      = af->pThumbs->buffer<plug::mesh_t>();
                if ((mesh == NULL) || (!mesh->isEmpty()) || (!af->bSync))
                    continue;

                if (channels > 0)
                {
                    // Copy thumbnails
                    for (size_t j=0; j<channels; ++j)
                        dsp::copy(mesh->pvData[j], af->vThumbs[j], meta::impulse_reverb_metadata::MESH_SIZE);
                    mesh->data(channels, meta::impulse_reverb_metadata::MESH_SIZE);
                }
                else
                    mesh->data(0, 0);
                af->bSync           = false;
            }
        }

        void impulse_reverb::process(size_t samples)
        {
            process_loading_tasks();
            process_configuration_tasks();
            process_gc_events();
            process_listen_events();
            perform_convolution(samples);
            output_parameters();
        }

        status_t impulse_reverb::load(af_descriptor_t *descr)
        {
            lsp_trace("descr = %p", descr);
            if (descr == NULL)
                return STATUS_UNKNOWN_ERR;

            // Destroy previously loaded sample
            destroy_sample(descr->pOriginal);

            // Check state
            if (descr->pFile == NULL)
                return STATUS_UNKNOWN_ERR;

            // Get path
            plug::path_t *path = descr->pFile->buffer<plug::path_t>();
            if (path == NULL)
                return STATUS_UNKNOWN_ERR;

            // Get file name
            const char *fname = path->path();
            if (strlen(fname) <= 0)
                return STATUS_UNSPECIFIED;

            // Load audio file
            dspu::Sample *af    = new dspu::Sample();
            if (af == NULL)
                return STATUS_NO_MEM;
            lsp_finally { destroy_sample(af); };

            // Try to load file
            float conv_length_max_seconds = meta::impulse_reverb_metadata::CONV_LENGTH_MAX * 0.001f;
            status_t status = af->load(fname, conv_length_max_seconds);
            if (status != STATUS_OK)
            {
                lsp_trace("load failed: status=%d (%s)", status, get_status(status));
                return status;
            }

            // Try to resample
            status  = af->resample(fSampleRate);
            if (status != STATUS_OK)
            {
                lsp_trace("resample failed: status=%d (%s)", status, get_status(status));
                return status;
            }

            // Determine the normalizing factor
            size_t channels         = af->channels();
            float max = 0.0f;

            for (size_t i=0; i<channels; ++i)
            {
                // Determine the maximum amplitude
                float a_max = dsp::abs_max(af->channel(i), af->samples());
                if (max < a_max)
                    max     = a_max;
            }
            descr->fNorm    = (max != 0.0f) ? 1.0f / max : 1.0f;

            // File was successfully loaded, pass result to the caller
            lsp::swap(descr->pOriginal, af);

            return STATUS_OK;
        }

        status_t impulse_reverb::reconfigure()
        {
            // Re-render files
            for (size_t i=0; i<meta::impulse_reverb_metadata::FILES; ++i)
            {
                // Get audio file
                af_descriptor_t *f  = &vFiles[i];
                destroy_sample(f->pProcessed);

                // Obtain the original sample
                dspu::Sample *af    = f->pOriginal;
                if (af == NULL)
                    continue;

                // Allocate new sample
                dspu::Sample *s     = new dspu::Sample();
                if (s == NULL)
                    return STATUS_NO_MEM;
                lsp_finally { destroy_sample(s); };

                ssize_t flen        = af->samples();
                size_t channels     = lsp_min(af->channels(), meta::impulse_reverb_metadata::TRACKS_MAX);

                // Buffer is present, file is present, check boundaries
                size_t head_cut     = dspu::millis_to_samples(fSampleRate, f->fHeadCut);
                size_t tail_cut     = dspu::millis_to_samples(fSampleRate, f->fTailCut);
                ssize_t fsamples    = flen - head_cut - tail_cut;
                if (fsamples <= 0)
                {
                    for (size_t j=0; j<channels; ++j)
                        dsp::fill_zero(f->vThumbs[j], meta::impulse_reverb_metadata::MESH_SIZE);
                    s->set_length(0);
                    continue;
                }

                // Now ensure that we have enough space for sample
                if (!s->init(channels, flen, fsamples))
                    return STATUS_NO_MEM;

                // Copy data to temporary buffer and apply fading
                for (size_t i=0; i<channels; ++i)
                {
                    float *dst = s->channel(i);
                    const float *src = af->channel(i);

                    // Copy sample data and apply fading
                    if (f->bReverse)
                    {
                        dsp::reverse2(dst, &src[tail_cut], fsamples);
                        dspu::fade_in(dst, dst, dspu::millis_to_samples(fSampleRate, f->fFadeIn), fsamples);
                    }
                    else
                        dspu::fade_in(dst, &src[head_cut], dspu::millis_to_samples(fSampleRate, f->fFadeIn), fsamples);
                    dspu::fade_out(dst, dst, dspu::millis_to_samples(fSampleRate, f->fFadeOut), fsamples);

                    // Now render thumbnail
                    src                 = dst;
                    dst                 = f->vThumbs[i];
                    for (size_t k=0; k<meta::impulse_reverb_metadata::MESH_SIZE; ++k)
                    {
                        size_t first    = (k * fsamples) / meta::impulse_reverb_metadata::MESH_SIZE;
                        size_t last     = ((k + 1) * fsamples) / meta::impulse_reverb_metadata::MESH_SIZE;
                        if (first < last)
                            dst[k]          = dsp::abs_max(&src[first], last - first);
                        else
                            dst[k]          = fabs(src[first]);
                    }

                    // Normalize graph if possible
                    if (f->fNorm != 1.0f)
                        dsp::mul_k2(dst, f->fNorm, meta::impulse_reverb_metadata::MESH_SIZE);
                }

                // Commit sample to the processed list
                lsp::swap(f->pProcessed, s);
            }

            // Randomize phase of the convolver
            uint32_t phase  = seed_addr(this);
            phase           = ((phase << 16) | (phase >> 16)) & 0x7fffffff;
            uint32_t step   = 0x80000000 / (meta::impulse_reverb_metadata::CONVOLVERS + 1);

            // OK, files have been rendered, now need to commutate
            for (size_t i=0; i<meta::impulse_reverb_metadata::CONVOLVERS; ++i)
            {
                convolver_t *c      = &vConvolvers[i];
                destroy_convolver(c->pSwap);

                // Check that routing has changed
                size_t file         = c->nFile;
                size_t track        = c->nTrack;
                if ((file <= 0) || (file > meta::impulse_reverb_metadata::FILES))
                    continue;
                --file;

                // Analyze sample
                dspu::Sample *s = vFiles[file].pProcessed;
                if ((s == NULL) || (!s->valid()) || (s->channels() <= track))
                    continue;

                // Now we can create convolver
                dspu::Convolver *cv   = new dspu::Convolver();
                if (cv == NULL)
                    return STATUS_NO_MEM;
                lsp_finally { destroy_convolver(cv); };

                if (!cv->init(s->channel(track), s->length(), nRank, float((phase + i*step)& 0x7fffffff)/float(0x80000000)))
                    return STATUS_NO_MEM;

                // Commit result
                lsp_trace("Allocated convolver pSwap=%p for channel %d (pCurr=%p)", cv, int(i), c->pCurr);
                lsp::swap(c->pSwap, cv);
            }

            return STATUS_OK;
        }

        void impulse_reverb::dump(dspu::IStateDumper *v) const
        {
            plug::Module::dump(v);

            v->write("nInputs", nInputs);
            v->write("nReconfigReq", nReconfigReq);
            v->write("nReconfigResp", nReconfigResp);
            v->write("nRank", nRank);
            v->write("pGCList", pGCList);

            v->begin_array("vInputs", vInputs, 2);
            {
                for (size_t i=0; i<2; ++i)
                {
                    const input_t *in   = &vInputs[i];
                    v->begin_object(in, sizeof(input_t));
                    {
                        v->write("vIn", in->vIn);
                        v->write("pIn", in->pIn);
                        v->write("pPan", in->pPan);
                    }
                    v->end_object();
                }
            }
            v->end_array();
            v->begin_array("vChannels", vChannels, 2);
            {
                for (size_t i=0; i<2; ++i)
                {
                    const channel_t *c = &vChannels[i];
                    v->begin_object(c, sizeof(channel_t));
                    {
                        v->write_object("sBypass", &c->sBypass);
                        v->write_object("sPlayer", &c->sPlayer);
                        v->write_object("sEqualizer", &c->sEqualizer);

                        v->write("vOut", c->vOut);
                        v->write("vBuffer", c->vBuffer);
                        v->writev("fDryPan", c->fDryPan, 2);

                        v->write("pOut", c->pOut);

                        v->write("pWetEq", c->pWetEq);
                        v->write("pLowCut", c->pLowCut);
                        v->write("pLowFreq", c->pLowFreq);
                        v->write("pHighCut", c->pHighCut);
                        v->write("pHighFreq", c->pHighFreq);

                        v->writev("pFreqGain", c->pFreqGain, meta::impulse_reverb_metadata::EQ_BANDS);
                    }
                    v->end_object();
                }
            }
            v->end_array();
            v->begin_array("vConvolvers", vConvolvers, meta::impulse_reverb_metadata::CONVOLVERS);
            {
                for (size_t i=0; i<meta::impulse_reverb_metadata::CONVOLVERS; ++i)
                {
                    const convolver_t *c = &vConvolvers[i];
                    v->begin_object(c, sizeof(convolver_t));
                    {
                        v->write_object("sDelay", &c->sDelay);

                        v->write_object("pCurr", c->pCurr);
                        v->write_object("pSwap", c->pSwap);

                        v->write("vBuffer", c->vBuffer);
                        v->writev("fPanIn", c->fPanIn, 2);
                        v->writev("fPanOut", c->fPanOut, 2);

                        v->write("pMakeup", c->pMakeup);
                        v->write("pPanIn", c->pPanIn);
                        v->write("pPanOut", c->pPanOut);
                        v->write("pFile", c->pFile);
                        v->write("pTrack", c->pTrack);
                        v->write("pPredelay", c->pPredelay);
                        v->write("pMute", c->pMute);
                        v->write("pActivity", c->pActivity);
                    }
                    v->end_object();
                }
            }
            v->end_array();
            v->begin_array("vFiles", vFiles, meta::impulse_reverb_metadata::FILES);
            {
                for (size_t i=0; i<meta::impulse_reverb_metadata::CONVOLVERS; ++i)
                {
                    const af_descriptor_t *af = &vFiles[i];
                    v->begin_object(af, sizeof(af_descriptor_t));
                    {
                        v->write_object("sListen", &af->sListen);
                        v->write_object("pOriginal", af->pOriginal);
                        v->write_object("pProcessed", af->pProcessed);

                        v->writev("vThumbs", af->vThumbs, meta::impulse_reverb_metadata::TRACKS_MAX);

                        v->write("fNorm", af->fNorm);
                        v->write("bRender", af->bRender);
                        v->write("nStatus", af->nStatus);
                        v->write("bSync", af->bSync);

                        v->write("fHeadCut", af->fHeadCut);
                        v->write("fTailCut", af->fTailCut);
                        v->write("fFadeIn", af->fFadeIn);
                        v->write("fFadeOut", af->fFadeOut);
                        v->write("bReverse", af->bReverse);

                        v->write_object("pLoader", &af->sLoader);

                        v->write("pFile", af->pFile);
                        v->write("pHeadCut", af->pHeadCut);
                        v->write("pTailCut", af->pTailCut);
                        v->write("pFadeIn", af->pFadeIn);
                        v->write("pFadeOut", af->pFadeOut);
                        v->write("pListen", af->pListen);
                        v->write("pReverse", af->pReverse);
                        v->write("pStatus", af->pStatus);
                        v->write("pLength", af->pLength);
                        v->write("pThumbs", af->pThumbs);
                    }
                    v->end_object();
                }
            }
            v->end_array();
            v->write_object("sConfigurator", &sConfigurator);

            v->write("pBypass", pBypass);
            v->write("pRank", pRank);
            v->write("pDry", pDry);
            v->write("pWet", pWet);
            v->write("pOutGain", pOutGain);
            v->write("pPredelay", pPredelay);

            v->write("pData", pData);
            v->write("pExecutor", pExecutor);
        }

    } /* namespace plugins */
} /* namespace lsp */


