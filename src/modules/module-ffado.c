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

#include <pulsecore/modargs.h>
#include <pulsecore/module.h>
#include <pulsecore/rtpoll.h>
#include <pulsecore/sample-util.h>
#include <pulsecore/sink.h>
#include <pulsecore/thread-mq.h>
#include <pulsecore/thread.h>

#include "module-ffado-symdef.h"

PA_MODULE_AUTHOR("Sam Hanes");
PA_MODULE_DESCRIPTION("FFADO Firewire device source/sink");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_USAGE("");

#define DEFAULT_SINK_NAME "firewire_out"

static const char* const valid_modargs[] = {
    "nperiods",
    "period",
    "rate",
    "sink_channel_map",
    "sink_channels",
    "sink_name",
    "verbose",
    NULL
};

struct userdata {
    pa_core *core;
    pa_module *module;

    ffado_device_t *dev;
    int32_t period_size;
    pa_usec_t fixed_latency;

    pa_sink *sink;
    unsigned sink_channels;
    void *sink_buffer[PA_CHANNELS_MAX];
    int sink_channel_map[PA_CHANNELS_MAX];

    pa_thread_mq thread_mq;
    pa_asyncmsgq *io_msgq;
    pa_rtpoll *rtpoll;
    pa_rtpoll_item *rtpoll_io_msgq;

    pa_thread *io_thread;
    pa_thread *msg_thread;
};

enum {
    SINK_MESSAGE_READY = PA_SINK_MESSAGE_MAX,
    SINK_MESSAGE_RENDER,
    SINK_MESSAGE_SHUTDOWN,
};


static int sink_process_msg (pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *memchunk) {
    struct userdata *u = PA_SINK(o)->userdata;

    switch (code) {
    case SINK_MESSAGE_READY:
        pa_log_debug("starting FFADO streams");
        if (ffado_streaming_start(u->dev) < 0) {
            pa_log("error starting FFADO");
            return -1;
        }
        return 0;

    case SINK_MESSAGE_RENDER:
        if (PA_SINK_IS_OPENED(u->sink->thread_info.state)) {
            pa_memchunk chunk;
            size_t nbytes;
            void *p;

            pa_assert(offset > 0);
            nbytes = (size_t) offset * pa_frame_size(&u->sink->sample_spec);

            pa_sink_render_full(u->sink, nbytes, &chunk);

            p = pa_memblock_acquire_chunk(&chunk);
            pa_deinterleave(p, u->sink_buffer, u->sink_channels, sizeof(float), (unsigned) offset);
            pa_memblock_release(chunk.memblock);
            pa_memblock_unref(chunk.memblock);
        } else {
            unsigned c;
            pa_sample_spec ss;

            ss = u->sink->sample_spec;
            ss.channels = 1;

            for (c = 0; c < u->sink_channels; c++)
                pa_silence_memory(u->sink_buffer[c], (size_t) offset * pa_sample_size(&u->sink->sample_spec), &ss);
        }

        ffado_streaming_transfer_buffers(u->dev);
        return 0;

    case PA_SINK_MESSAGE_GET_LATENCY:
        *((pa_usec_t*) data) = u->fixed_latency;
        return 0;

    case SINK_MESSAGE_SHUTDOWN:
        pa_asyncmsgq_post(u->thread_mq.outq, PA_MSGOBJECT(u->core), PA_CORE_MESSAGE_UNLOAD_MODULE, u->module, 0, NULL, NULL);
        return 0;
    }

    return pa_sink_process_msg(o, code, data, offset, memchunk);
}


static void io_thread_func(void *userdata) {
    struct userdata *u = userdata;
    pa_assert(u);

    pa_log_debug("IO thread starting up");

    if (u->core->realtime_scheduling)
        pa_make_realtime(u->core->realtime_priority);

    /* ask the mq thread to bring up FFADO, then wait
     * doing this ensures everyone is ready when it comes up */
    pa_assert_se(0 == pa_asyncmsgq_send(u->io_msgq, PA_MSGOBJECT(u->sink), SINK_MESSAGE_READY, NULL, 0, NULL));

    for (;;) {
        switch (ffado_streaming_wait(u->dev)) {
        case ffado_wait_ok:
            break;

        case ffado_wait_xrun:
            /* handled xrun: process nothing this time, but otherwise OK */
            continue;

        case ffado_wait_error:
            /* probably an unhandled xrun: try to restart */
            if (ffado_streaming_reset(u->dev) < 0) {
                pa_log("unable to recover from FFADO error; shutting down");
                goto finish;
            }
            continue;

        case ffado_wait_shutdown:
            goto finish;

        default:
            pa_log("received nonsense return from ffado_streaming_wait; shutting down");
            goto finish;
        }

        pa_assert_se(0 == pa_asyncmsgq_send(u->io_msgq, PA_MSGOBJECT(u->sink), SINK_MESSAGE_RENDER, NULL, u->period_size, NULL));
    }

finish:
    pa_asyncmsgq_post(u->io_msgq, PA_MSGOBJECT(u->sink), SINK_MESSAGE_SHUTDOWN, NULL, 0, NULL, NULL);
    pa_log_debug("IO thread shutting down");
}


static void msg_thread_func(void *userdata) {
    struct userdata *u = userdata;
    pa_assert(u);

    pa_log_debug("message handling thread starting up");

    if (u->core->realtime_scheduling)
        pa_make_realtime(u->core->realtime_priority);

    pa_thread_mq_install(&u->thread_mq);

    for (;;) {
        int ret;

        if (PA_UNLIKELY(u->sink->thread_info.rewind_requested))
            pa_sink_process_rewind(u->sink, 0);

        if ((ret = pa_rtpoll_run(u->rtpoll, true)) < 0)
            goto fail;

        if (ret == 0)
            goto finish;
    }

fail:
    /* If this was no regular exit from the loop we have to continue
     * processing messages until we received PA_MESSAGE_SHUTDOWN */
    pa_asyncmsgq_post(u->thread_mq.outq, PA_MSGOBJECT(u->core), PA_CORE_MESSAGE_UNLOAD_MODULE, u->module, 0, NULL, NULL);
    pa_asyncmsgq_wait_for(u->thread_mq.inq, PA_MESSAGE_SHUTDOWN);

finish:
    pa_log_debug("message handling thread shutting down");
}


int pa__init(pa_module* m) {
    struct userdata *u = NULL;
    pa_modargs *args = NULL;

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
    u->module = m;
    u->core = m->core;

    u->rtpoll = pa_rtpoll_new();
    pa_thread_mq_init( &u->thread_mq, m->core->mainloop, u->rtpoll );

    u->io_msgq = pa_asyncmsgq_new( 0 );
    u->rtpoll_io_msgq = pa_rtpoll_item_new_asyncmsgq_read( u->rtpoll, PA_RTPOLL_EARLY - 1, u->io_msgq );

    /* ************************************************************** *
     * Initialize FFADO Device                                        *
     * ************************************************************** */

    memset(&dev_info, 0, sizeof(dev_info));
    memset(&dev_opts, 0, sizeof(dev_opts));

    dev_opts.sample_rate = 48000;
    if (pa_modargs_get_value_s32(args, "rate", &(dev_opts.sample_rate)) < 0
            || dev_opts.sample_rate <= 0) {
        pa_log("invalid rate parameter");
        goto fail;
    }
    pa_log_debug("using sample rate %d", dev_opts.sample_rate);

    dev_opts.period_size = 1024;
    if (pa_modargs_get_value_s32(args, "period", &(dev_opts.period_size)) < 0
            || dev_opts.period_size < 1) {
        pa_log("invalid period parameter");
        goto fail;
    }
    pa_log_debug("using period size %d", dev_opts.period_size);
    u->period_size = dev_opts.period_size;

    dev_opts.nb_buffers = 3;
    if (pa_modargs_get_value_s32(args, "nperiods", &(dev_opts.nb_buffers)) < 0
            || dev_opts.nb_buffers < 2) {
        pa_log("invalid nperiods parameter");
        goto fail;
    }
    pa_log_debug("using %d periods of buffer", dev_opts.nb_buffers);

    dev_opts.verbose = 1;
    if (pa_modargs_get_value_s32(args, "verbose", &(dev_opts.verbose)) < 0) {
        pa_log("invalid verbose parameter");
        goto fail;
    }

    dev_opts.realtime = u->core->realtime_scheduling;
    dev_opts.packetizer_priority = u->core->realtime_priority;

    pa_log_debug("initializing FFADO device");

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

    for (idx = 0; idx < raw_sink_channels; idx++) {
        if (ffado_stream_type_audio !=
                ffado_streaming_get_playback_stream_type(u->dev, idx)
                ) {
            pa_log_debug("disabling non-audio FFADO sink stream %d", idx);

            if (ffado_streaming_set_playback_stream_buffer(u->dev, idx, NULL) < 0) {
                pa_log("error disabling non-audio FFADO sink stream %d buffer", idx);
                goto fail;
            }

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

            u->sink_channel_map[dev_sink_channels] = idx;
            u->sink_buffer[dev_sink_channels] = pa_xnew0(float, dev_opts.period_size);

            if (ffado_streaming_set_playback_stream_buffer(u->dev, idx,
                    (char*) u->sink_buffer[dev_sink_channels] ) < 0) {
                pa_log("error setting buffer for FFADO sink stream %d", idx);
                goto fail;
            }
        } else {
            pa_log_debug("not using FFADO sink stream %d", idx);

            if (ffado_streaming_set_playback_stream_buffer(u->dev, idx, NULL) < 0) {
                pa_log("error disabling unused FFADO sink stream %d buffer", idx);
                goto fail;
            }

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

    sink_spec.channels = u->sink_channels = (uint8_t) sink_channels;
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


    u->sink = pa_sink_new(m->core, &sink_data, PA_SINK_LATENCY);
    pa_sink_new_data_done(&sink_data);

    if (!u->sink) {
        pa_log("failed to create PulseAudio sink");
        goto fail;
    }

    u->sink->parent.process_msg = sink_process_msg;
    u->sink->userdata = u;

    pa_sink_set_asyncmsgq( u->sink, u->thread_mq.inq );
    pa_sink_set_rtpoll( u->sink, u->rtpoll );
    pa_sink_set_max_request(u->sink, u->period_size * pa_frame_size(&sink_spec));

    u->fixed_latency = pa_bytes_to_usec(u->period_size * pa_frame_size(&sink_spec) * dev_opts.nb_buffers, &sink_spec);
    pa_sink_set_fixed_latency(u->sink, u->fixed_latency);

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
        if (ffado_streaming_set_capture_stream_buffer(u->dev, idx, NULL) < 0) {
            pa_log("error disabling unused FFADO source stream %d buffer", idx);
            goto fail;
        }

        if (ffado_streaming_capture_stream_onoff(u->dev, idx, 0) < 0) {
            pa_log("error disabling unused FFADO source stream %d", idx);
            goto fail;
        }
    }

    /* *************************************************************** *
     * Start Everything Up                                             *
     * *************************************************************** */

    if (ffado_streaming_prepare(u->dev) < 0) {
        pa_log("error preparing FFADO for streaming");
        goto fail;
    }

    if (!(u->msg_thread = pa_thread_new("ffado-msg", msg_thread_func, u))) {
        pa_log("failed to create message handling thread");
        goto fail;
    }

    if (!(u->io_thread = pa_thread_new("ffado-io", io_thread_func, u))) {
        pa_log("failed to create IO thread");
        goto fail;
    }

    pa_sink_put(u->sink);

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
    unsigned idx;

    pa_assert(m);
    if (!(u = m->userdata))
        return;

    if (u->sink)
        pa_sink_unlink(u->sink);

    if (u->dev) {
        ffado_streaming_stop(u->dev);
        ffado_streaming_finish(u->dev);
    }

    if (u->io_thread) {
        /* the IO thread will have received ffado_wait_shutdown
         * and stopped when we called ffado_streaming_finish above. */
        pa_thread_free(u->io_thread);
    }

    if (u->msg_thread) {
        pa_asyncmsgq_send(u->thread_mq.inq, NULL, PA_MESSAGE_SHUTDOWN, NULL, 0, NULL);
        pa_thread_free(u->msg_thread);
    }

    pa_thread_mq_done(&u->thread_mq);

    if (u->sink)
        pa_sink_unref(u->sink);

    if (u->rtpoll_io_msgq)
        pa_rtpoll_item_free(u->rtpoll_io_msgq);

    if (u->io_msgq)
        pa_asyncmsgq_unref(u->io_msgq);

    if (u->rtpoll)
        pa_rtpoll_free(u->rtpoll);

    for (idx = 0; idx < u->sink_channels; idx++) {
        if (u->sink_buffer[idx])
            pa_xfree(u->sink_buffer[idx]);
    }

    pa_xfree(u);
}
