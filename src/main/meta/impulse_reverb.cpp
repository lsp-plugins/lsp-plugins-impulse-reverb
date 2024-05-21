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

#include <lsp-plug.in/plug-fw/meta/ports.h>
#include <lsp-plug.in/shared/meta/developers.h>
#include <lsp-plug.in/common/status.h>

#include <private/meta/impulse_reverb.h>

#define LSP_PLUGINS_IMPULSE_REVERB_VERSION_MAJOR       1
#define LSP_PLUGINS_IMPULSE_REVERB_VERSION_MINOR       0
#define LSP_PLUGINS_IMPULSE_REVERB_VERSION_MICRO       20

#define LSP_PLUGINS_IMPULSE_REVERB_VERSION  \
    LSP_MODULE_VERSION( \
        LSP_PLUGINS_IMPULSE_REVERB_VERSION_MAJOR, \
        LSP_PLUGINS_IMPULSE_REVERB_VERSION_MINOR, \
        LSP_PLUGINS_IMPULSE_REVERB_VERSION_MICRO  \
    )

namespace lsp
{
    namespace meta
    {
        //-------------------------------------------------------------------------
        // Impulse reverb

        static const port_item_t ir_files[] =
        {
            { "None",       "file.none" },
            { "File 1",     "file.f1" },
            { "File 2",     "file.f2" },
            { "File 3",     "file.f3" },
            { "File 4",     "file.f4" },
            { NULL, NULL }
        };

        static const port_item_t ir_tracks[] =
        {
            { "Track 1",    "file.t1" },
            { "Track 2",    "file.t2" },
            { "Track 3",    "file.t3" },
            { "Track 4",    "file.t4" },
            { "Track 5",    "file.t5" },
            { "Track 6",    "file.t6" },
            { "Track 7",    "file.t7" },
            { "Track 8",    "file.t8" },
            { NULL, NULL }
        };

        static const port_item_t ir_fft_rank[] =
        {
            { "512",    NULL },
            { "1024",   NULL },
            { "2048",   NULL },
            { "4096",   NULL },
            { "8192",   NULL },
            { "16384",  NULL },
            { "32768",  NULL },
            { "65536",  NULL },
            { NULL, NULL }
        };

        static const port_item_t ir_file_select[] =
        {
            { "File 1",     "file.f1" },
            { "File 2",     "file.f2" },
            { "File 3",     "file.f3" },
            { "File 4",     "file.f4" },
            { NULL, NULL }
        };

        static const port_item_t filter_slope[] =
        {
            { "off",        "eq.slope.off" },
            { "12 dB/oct",  "eq.slope.12dbo" },
            { "24 dB/oct",  "eq.slope.24dbo" },
            { "36 dB/oct",  "eq.slope.36dbo" },
            { NULL, NULL }
        };

        #define IR_PAN_MONO \
            PAN_CTL("p", "Panorama", 0.0f)

        #define IR_PAN_STEREO \
            PAN_CTL("pl", "Left channel panorama", -100.0f), \
            PAN_CTL("pr", "Right channel panorama", 100.0f)

        #define IR_COMMON(pan) \
            BYPASS, \
            COMBO("fsel", "File selector", 0, ir_file_select), \
            COMBO("fft", "FFT size", impulse_reverb_metadata::FFT_RANK_DEFAULT, ir_fft_rank), \
            CONTROL("pd", "Pre-delay", U_MSEC, impulse_reverb_metadata::PREDELAY), \
            pan, \
            DRY_GAIN(1.0f), \
            WET_GAIN(1.0f), \
            DRYWET(100.0f), \
            OUT_GAIN

        #define IR_SAMPLE_FILE(id, label)   \
            PATH("ifn" id, "Impulse file" label),    \
            CONTROL("ihc" id, "Head cut" label, U_MSEC, impulse_reverb_metadata::CONV_LENGTH), \
            CONTROL("itc" id, "Tail cut" label, U_MSEC, impulse_reverb_metadata::CONV_LENGTH), \
            CONTROL("ifi" id, "Fade in" label, U_MSEC, impulse_reverb_metadata::CONV_LENGTH), \
            CONTROL("ifo" id, "Fade out" label, U_MSEC, impulse_reverb_metadata::CONV_LENGTH), \
            TRIGGER("ils" id, "Impulse listen" label), \
            SWITCH("irv" id, "Impulse reverse" label, 0.0f), \
            STATUS("ifs" id, "Load status" label), \
            METER("ifl" id, "Impulse length" label, U_MSEC, impulse_reverb_metadata::CONV_LENGTH), \
            MESH("ifd" id, "Impulse file contents" label, impulse_reverb_metadata::TRACKS_MAX, impulse_reverb_metadata::MESH_SIZE)

        #define IR_CONVOLVER_MONO(id, label, file, track, mix) \
            COMBO("csf" id, "Channel source file" label, file, ir_files), \
            COMBO("cst" id, "Channel source track" label, track, ir_tracks), \
            AMP_GAIN100("mk" id, "Makeup gain" label, 1.0f), \
            SWITCH("cam" id, "Channel mute" label, 0.0f), \
            BLINK("ca" id, "Channel activity" label), \
            CONTROL("pd" id, "Channel pre-delay" label, U_MSEC, impulse_reverb_metadata::PREDELAY), \
            PAN_CTL("com" id, "Channel Left/Right output mix" label, mix)

        #define IR_CONVOLVER_STEREO(id, label, file, track, in_mix, out_mix) \
            PAN_CTL("cim" id, "Left/Right input mix" label, in_mix), \
            IR_CONVOLVER_MONO(id, label, file, track, out_mix)

        #define IR_EQ_BAND(id, freq)    \
            CONTROL("eq_" #id, "Band " freq "Hz gain", U_GAIN_AMP, impulse_reverb_metadata::BA)

        #define IR_EQUALIZER    \
            SWITCH("wpp", "Wet post-process", 0),    \
            SWITCH("eqv", "Equalizer visibility", 0),    \
            COMBO("lcm", "Low-cut mode", 0, filter_slope),      \
            LOG_CONTROL("lcf", "Low-cut frequency", U_HZ, impulse_reverb_metadata::LCF),   \
            IR_EQ_BAND(0, "50"), \
            IR_EQ_BAND(1, "107"), \
            IR_EQ_BAND(2, "227"), \
            IR_EQ_BAND(3, "484"), \
            IR_EQ_BAND(4, "1 k"), \
            IR_EQ_BAND(5, "2.2 k"), \
            IR_EQ_BAND(6, "4.7 k"), \
            IR_EQ_BAND(7, "10 k"), \
            COMBO("hcm", "High-cut mode", 0, filter_slope),      \
            LOG_CONTROL("hcf", "High-cut frequency", U_HZ, impulse_reverb_metadata::HCF)

        static const port_t impulse_reverb_mono_ports[] =
        {
            // Input audio ports
            AUDIO_INPUT_MONO,
            AUDIO_OUTPUT_LEFT,
            AUDIO_OUTPUT_RIGHT,
            IR_COMMON(IR_PAN_MONO),

            // Input controls
            IR_SAMPLE_FILE("0", " 0"),
            IR_SAMPLE_FILE("1", " 1"),
            IR_SAMPLE_FILE("2", " 2"),
            IR_SAMPLE_FILE("3", " 3"),
            IR_CONVOLVER_MONO("0", " 0", 1, 0, -100.0f),
            IR_CONVOLVER_MONO("1", " 1", 1, 1, +100.0f),
            IR_CONVOLVER_MONO("2", " 2", 2, 0, -100.0f),
            IR_CONVOLVER_MONO("3", " 3", 2, 1, +100.0f),

            // Impulse response equalizer
            IR_EQUALIZER,

            PORTS_END
        };

        static const port_t impulse_reverb_stereo_ports[] =
        {
            // Input audio ports
            PORTS_STEREO_PLUGIN,
            IR_COMMON(IR_PAN_STEREO),

            // Input controls
            IR_SAMPLE_FILE("0", " 0"),
            IR_SAMPLE_FILE("1", " 1"),
            IR_SAMPLE_FILE("2", " 2"),
            IR_SAMPLE_FILE("3", " 3"),
            IR_CONVOLVER_STEREO("0", " 0", 1, 0, -100.0f, -100.0f),
            IR_CONVOLVER_STEREO("1", " 1", 1, 1, -100.0f, +100.0f),
            IR_CONVOLVER_STEREO("2", " 2", 2, 0, +100.0f, -100.0f),
            IR_CONVOLVER_STEREO("3", " 3", 2, 1, +100.0f, +100.0f),

            // Impulse response equalizer
            IR_EQUALIZER,

            PORTS_END
        };

        static const int plugin_classes[]           = { C_REVERB, -1 };
        static const int clap_features_mono[]       = { CF_AUDIO_EFFECT, CF_REVERB, CF_MONO, -1 };
        static const int clap_features_stereo[]     = { CF_AUDIO_EFFECT, CF_REVERB, CF_STEREO, -1 };

        const meta::bundle_t impulse_reverb_bundle =
        {
            "impulse_reverb",
            "Impulse Reverb",
            B_CONVOLUTION,
            "hEUauzc_j3U",
            "This plugin performs highly optimized real time zero-latency convolution\nto the input signal. It can be used as a cabinet emulator, some sort of\nequalizer or as a reverb simulation plugin. All what is needed is audio\nfile(s) with impulse response taken from the linear system (cabinet, equalizer\nor hall/room). The flexible configuration allows one to use True Reverb processing."
        };

        const meta::plugin_t impulse_reverb_mono =
        {
            "Impulsnachhall Mono",
            "Impulse Reverb Mono",
            "Impulse Reverb Mono",
            "INH1M",
            &developers::v_sadovnikov,
            "impulse_reverb_mono",
            LSP_LV2_URI("impulse_reverb_mono"),
            LSP_LV2UI_URI("impulse_reverb_mono"),
            "fggq",
            LSP_VST3_UID("inh1m   fggq"),
            LSP_VST3UI_UID("inh1m   fggq"),
            0,
            NULL,
            LSP_CLAP_URI("impulse_reverb_mono"),
            LSP_PLUGINS_IMPULSE_REVERB_VERSION,
            plugin_classes,
            clap_features_mono,
            E_DUMP_STATE | E_FILE_PREVIEW,
            impulse_reverb_mono_ports,
            "convolution/impulse_reverb/mono.xml",
            NULL,
            mono_to_stereo_plugin_port_groups,
            &impulse_reverb_bundle
        };

        const meta::plugin_t impulse_reverb_stereo =
        {
            "Impulsnachhall Stereo",
            "Impulse Reverb Stereo",
            "Impulse Reverb Stereo",
            "INH1S",
            &developers::v_sadovnikov,
            "impulse_reverb_stereo",
            LSP_LV2_URI("impulse_reverb_stereo"),
            LSP_LV2UI_URI("impulse_reverb_stereo"),
            "o9zj",
            LSP_VST3_UID("inh1s   o9zj"),
            LSP_VST3UI_UID("inh1s   o9zj"),
            0,
            NULL,
            LSP_CLAP_URI("impulse_reverb_stereo"),
            LSP_PLUGINS_IMPULSE_REVERB_VERSION,
            plugin_classes,
            clap_features_stereo,
            E_DUMP_STATE | E_FILE_PREVIEW,
            impulse_reverb_stereo_ports,
            "convolution/impulse_reverb/stereo.xml",
            NULL,
            stereo_plugin_port_groups,
            &impulse_reverb_bundle
        };
    } // namespace meta
} // namespace lsp
