/***
  This file is part of PulseAudio.

  Copyright 2006 Lennart Poettering

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif


#include <libffado/ffado.h>

#include <pulse/xmalloc.h>

#include <pulsecore/module.h>
#include <pulsecore/modargs.h>
#include <pulsecore/sink.h>

#include "module-ffado-symdef.h"

PA_MODULE_AUTHOR("Sam Hanes");
PA_MODULE_DESCRIPTION("FFADO Firewire device source/sink");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_USAGE("");

#define DEFAULT_SINK_NAME "firewire_out"

static const char* const valid_modargs[] = {
    "device",
    "nperiods",
    "period",
    "rate",
    "sink_channel_map",
    "sink_channels",
    "sink_name",
    NULL
};

struct sink_channel {
    int channel;
    float *buffer;
};

struct userdata {
    ffado_device_t *dev;
    pa_sink *sink;
    int sink_channel_count;
    // null-terminated array of channel structures
    struct sink_channel *sink_channels;
};

int pa__init(pa_module* m) {
    struct userdata *u = NULL;
    pa_modargs *args = NULL;

    char *device_spec;
    ffado_device_info_t dev_info;
    ffado_options_t dev_opts;

    pa_sink_new_data sink_data;
    int raw_sink_channels = 0;
    uint32_t dev_sink_channels = 0;
    uint32_t sink_channels = 0;
    pa_sample_spec sink_spec;
    pa_channel_map sink_map;

    int raw_source_channels = 0;
    int idx;


    if (!(args = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("failed to parse module arguments");
        goto fail;
    }

    m->userdata = u = pa_xnew0(struct userdata, 1);

    /* ************************************************************** *
     * Initialize FFADO Device                                        *
     * ************************************************************** */

    device_spec = (char*) pa_modargs_get_value(args, "device", NULL);
    if (!device_spec) {
        pa_log("device parameter is required");
        goto fail;
    }

    dev_info.nb_device_spec_strings = 1;
    dev_info.device_spec_strings = &device_spec;


    dev_opts.sample_rate = -1;
    if (pa_modargs_get_value_s32(args, "rate", &(dev_opts.sample_rate)) < 0
            || dev_opts.sample_rate < -1) {
        pa_log("invalid rate parameter");
        goto fail;
    }

    dev_opts.period_size = 1024;
    if (pa_modargs_get_value_s32(args, "period", &(dev_opts.period_size)) < 0
            || dev_opts.period_size < 1) {
        pa_log("invalid period parameter");
        goto fail;
    }

    dev_opts.nb_buffers = 3;
    if (pa_modargs_get_value_s32(args, "nperiods", &(dev_opts.nb_buffers)) < 0
            || dev_opts.nb_buffers < 2) {
        pa_log("invalid nperiods parameter");
        goto fail;
    }


    pa_log_debug("initializing FFADO device %s", device_spec);

    u->dev = ffado_streaming_init(dev_info, dev_opts);
    if (!u->dev) {
        pa_log("FFADO device initialization failed");
        goto fail;
    }

    if (ffado_streaming_set_audio_datatype(u->dev, ffado_audio_datatype_float) < 0) {
        pa_log("error setting FFADO audio datatype");
        goto fail;
    }


    /* ************************************************************** *
     * Initialize Sink                                                *
     * ************************************************************** */

    pa_log_debug("initializing FFADO sink streams");

    if (pa_modargs_get_value_u32(args, "sink_channels", &sink_channels) < 0
            || sink_channels > PA_CHANNELS_MAX) {
        pa_log("invalid sink_channels parameter");
        goto fail;
    }

    raw_sink_channels = ffado_streaming_get_nb_playback_streams(u->dev);
    if (raw_sink_channels < 0) {
        pa_log("unable to get sink stream count from FFADO");
        goto fail;
    }

    pa_log_debug("have %d FFADO sink streams", raw_sink_channels);

    u->sink_channels = pa_xnew0(struct sink_channel, raw_sink_channels + 1);

    for (idx = 0; idx < raw_sink_channels; idx++) {
        if (ffado_stream_type_audio !=
                ffado_streaming_get_playback_stream_type(u->dev, idx)
                ) {
            pa_log_debug("disabling non-audio FFADO sink stream %d", idx);

            if (ffado_streaming_playback_stream_onoff(u->dev, idx, 0) < 0) {
                pa_log("error disabling non-audio FFADO sink stream %d", idx);
                goto fail;
            }

            continue;
        }

        if (0 == sink_channels || dev_sink_channels < sink_channels) {
            pa_log_debug("using FFADO sink stream %d as channel %d", idx, dev_sink_channels);

            if (ffado_streaming_playback_stream_onoff(u->dev, idx, 1) < 0) {
                pa_log("error enabling FFADO sink stream %d", idx);
                goto fail;
            }

            u->sink_channels[ dev_sink_channels ].channel = idx;
            u->sink_channels[ dev_sink_channels ].buffer =
                pa_xnew0(float, dev_opts.period_size);

            if (ffado_streaming_set_playback_stream_buffer(u->dev, idx,
                    (char*) u->sink_channels[ dev_sink_channels ].buffer ) < 0) {
                pa_log("error setting buffer for FFADO sink stream %d", idx);
                goto fail;
            }
        } else {
            pa_log_debug("not using FFADO sink stream %d", idx);

            if (ffado_streaming_playback_stream_onoff(u->dev, idx, 0) < 0) {
                pa_log("error disabling unused FFADO sink stream %d", idx);
                goto fail;
            }
        }

        dev_sink_channels++;
    }

    pa_log_debug("have %d FFADO audio sink streams", dev_sink_channels);

    if (0 == sink_channels) {
        sink_channels = dev_sink_channels;
    } else if (dev_sink_channels < sink_channels) {
        pa_log("sink_channels parameter greater than available channels");
        goto fail;
    }


    if (sink_channels == m->core->default_channel_map.channels) {
        sink_map = m->core->default_channel_map;
    } else {
        pa_channel_map_init_extend(&sink_map, sink_channels, PA_CHANNEL_MAP_ALSA);
    }

    if (pa_modargs_get_channel_map(args, "sink_channel_map", &sink_map) < 0
            || sink_map.channels <= 0) {
        pa_log("invalid channel_map parameter");
        goto fail;
    } else if (sink_map.channels != sink_channels) {
        pa_log("channel_map parameter has wrong number of channels");
        goto fail;
    }


    pa_log_debug("initializing PulseAudio sink");

    sink_spec.channels = u->sink_channel_count = (uint8_t) sink_channels;
    sink_spec.rate = dev_opts.sample_rate;
    sink_spec.format = PA_SAMPLE_FLOAT32NE;
    pa_assert(pa_sample_spec_valid(&sink_spec));


    pa_sink_new_data_init(&sink_data);
    sink_data.driver = __FILE__;
    sink_data.module = m;
    pa_sink_new_data_set_name(&sink_data,
            pa_modargs_get_value(args,
                "sink_name", DEFAULT_SINK_NAME
        ));
    pa_sink_new_data_set_sample_spec(&sink_data, &sink_spec);
    pa_sink_new_data_set_channel_map(&sink_data, &sink_map);
    pa_proplist_sets(sink_data.proplist, PA_PROP_DEVICE_API, "ffado");
    pa_proplist_sets(sink_data.proplist, PA_PROP_DEVICE_STRING, device_spec);


    u->sink = pa_sink_new(m->core, &sink_data, PA_SINK_LATENCY);
    pa_sink_new_data_done(&sink_data);

    if (!u->sink) {
        pa_log("failed to create PulseAudio sink");
        goto fail;
    }

    /* ************************************************************** *
     * Initialize Source                                              *
     * ************************************************************** */

    pa_log_debug("initializing FFADO source streams");

    raw_source_channels = ffado_streaming_get_nb_capture_streams(u->dev);
    if (raw_source_channels < 0) {
        pa_log("unable to get sink stream count from FFADO");
        goto fail;
    }

    for (idx = 0; idx < raw_source_channels; idx++) {
        if (ffado_streaming_capture_stream_onoff(u->dev, idx, 0) < 0) {
            pa_log("error disabling unused FFADO source stream %d", idx);
            goto fail;
        }
    }

    pa_modargs_free(args);

    return 0;

fail:
    if (args)
        pa_modargs_free(args);

    pa__done(m);

    return -1;
}

void pa__done(pa_module* m) {
    struct userdata *u;
    int idx;

    pa_assert(m);
    if (!(u = m->userdata))
        return;

    if (u->sink)
        pa_sink_unlink(u->sink);

    if (u->dev) {
        ffado_streaming_stop(u->dev);
        ffado_streaming_finish(u->dev);
    }

    if (u->sink)
        pa_sink_unref(u->sink);

    if (u->sink_channels) {
        for (idx = 0; 0 != u->sink_channels[ idx ].channel; idx++) {
            if (u->sink_channels[ idx ].buffer)
                pa_xfree(u->sink_channels[ idx ].buffer);
        }

        pa_xfree(u->sink_channels);
    }

    pa_xfree(u);
}
