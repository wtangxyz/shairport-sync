/*
 * jack output driver. This file is part of Shairport Sync.
 * Copyright (c) 2019 Mike Brady <mikebrady@iercom.net>,
 *                    Jörn Nettingsmeier <nettings@luchtbeweging.nl>
 *
 * All rights reserved.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "audio.h"
#include "common.h"
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <string.h>

#include <jack/jack.h>
#include <jack/ringbuffer.h>

// Two-channel, 16bit audio:
static const int bytes_per_frame = 4;
// Four seconds buffer -- should be plenty
#define buffer_size (44100 * 4 * bytes_per_frame)

static pthread_mutex_t buffer_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t client_mutex = PTHREAD_MUTEX_INITIALIZER;

int jack_init(int, char **);
void jack_deinit(void);
void jack_start(int, int);
int play(void *, int);
void jack_stop(void);
int jack_delay(long *);
void jack_flush(void);

audio_output audio_jack = {.name = "jack",
                           .help = NULL,
                           .init = &jack_init,
                           .deinit = &jack_deinit,
                           .start = &jack_start,
                           .stop = NULL,
                           .is_running = NULL,
                           .flush = &jack_flush,
                           .delay = &jack_delay,
                           .play = &play,
                           .volume = NULL,
                           .parameters = NULL,
                           .mute = NULL};

// This also affects deinterlacing, so make it exactly the number of incoming audio channels!
#define NPORTS 2
static jack_port_t *port[NPORTS];
static const char* port_name[NPORTS] = { "out_L", "out_R" };

static jack_client_t *client;
static jack_nframes_t sample_rate;
static jack_nframes_t jack_latency;

static jack_ringbuffer_t *jackbuf;
static int flush_please = 0;

static jack_latency_range_t latest_latency_range[NPORTS];
static int64_t time_of_latest_transfer;


static inline jack_default_audio_sample_t sample_conv(short sample) {
  return ((sample < 0) ? (-1.0 * sample / SHRT_MIN) : (1.0 * sample / SHRT_MAX));
}

static void deinterleave_and_convert(const char *interleaved_frames,
                                     jack_default_audio_sample_t* jack_buffer[],
                                     jack_nframes_t offset,
                                     jack_nframes_t nframes) {
  jack_nframes_t f;
  short *ifp = (short *)interleaved_frames; // we're dealing with 16bit audio here
  for (f=offset; f<(nframes + offset); f++) {
    for (int i=0; i<NPORTS; i++) {
      jack_buffer[i][f] = sample_conv(*ifp++);
    }
  }
}

static int process(jack_nframes_t nframes, __attribute__((unused)) void *arg) {
  jack_default_audio_sample_t *buffer[NPORTS];
  for (int i=0; i < NPORTS; i++) {
    buffer[i] = (jack_default_audio_sample_t *)jack_port_get_buffer(port[i], nframes);
  }
  jack_ringbuffer_data_t v[2] = { 0 };
  jack_nframes_t i, thisbuf;
  int frames_written = 0;
  int frames_required = 0;

  if (flush_please) {
    // we just move the read pointer ahead without doing anything with the data.
    jack_ringbuffer_read_advance(jackbuf, jack_ringbuffer_read_space(jackbuf));
    flush_please = 0;
    // since we don't change nframes, the whole buffer will be zeroed later.
  } else {
    jack_ringbuffer_get_read_vector(jackbuf, v); // an array of two elements because of possible ringbuffer wrap-around
    for (i=0; i<2; i++) {
      thisbuf = v[i].len / bytes_per_frame;
      if (thisbuf > nframes) {
        frames_required = nframes;
	    } else {
        frames_required = thisbuf;
      }
      deinterleave_and_convert(v[i].buf, buffer, frames_written, frames_required);
      frames_written += frames_required;
      nframes -= frames_required;
    }
    jack_ringbuffer_read_advance(jackbuf, frames_written * bytes_per_frame);
  }
  // now, if there are any more frames to put into the buffer, fill them with
  // silence
  while (nframes > 0) {
    for (int i=0; i < NPORTS; i++) {
      buffer[i][frames_written] = 0.0;
    }
    frames_written++;
    nframes--;
  }
  return 0;
}

static int graph(__attribute__((unused)) void * arg) {
  int latency = 0;
  debug(1, "JACK graph reorder callback called. Current latencies to terminal downstream port:");
  for (int i=0; i<NPORTS; i++) {
    jack_port_get_latency_range(port[i], JackPlaybackLatency, &latest_latency_range[i]);
    debug(1, "Port %s\tmin: %d\t max: %d", port_name[i], latest_latency_range[i].min, latest_latency_range[i].max);
    latency += latest_latency_range[i].max;
  }
  latency /= NPORTS;
  jack_latency = latency;
  debug(1, "Average maximum latency across all ports: %d", jack_latency);
  return 0;
}

static void error(const char *desc) {
  debug(2, "JACK error: \"%s\"", desc);
}

static void info(const char *desc) {
  inform("JACK information: \"%s\"", desc);
}

int jack_init(__attribute__((unused)) int argc, __attribute__((unused)) char **argv) {
  int i;
  config.audio_backend_latency_offset = 0;
  config.audio_backend_buffer_desired_length = 0.500;
  config.audio_backend_buffer_interpolation_threshold_in_seconds =
      0.25; // below this, soxr interpolation will not occur -- it'll be basic interpolation
            // instead.

  // get settings from settings file first, allow them to be overridden by
  // command line options

  // do the "general" audio  options. Note, these options are in the "general" stanza!
  parse_general_audio_options();

  // other options would be picked up here...

  // now the specific options
  if (config.cfg != NULL) {
    const char *str;
    /* Get the Client Name. */
    if (config_lookup_string(config.cfg, "jack.client_name", &str)) {
      config.jack_client_name = (char *)str;
    }
    /* Get the autoconnect pattern. */
    if (config_lookup_string(config.cfg, "jack.autoconnect_pattern", &str)) {
      config.jack_autoconnect_pattern = (char *)str;
    }
  }
  if (config.jack_client_name == NULL)
    config.jack_client_name = strdup("shairport-sync");

  jackbuf = jack_ringbuffer_create(buffer_size);
  if (jackbuf == NULL)
    die("Can't allocate %d bytes for the JACK ringbuffer.", buffer_size);
  jack_ringbuffer_mlock(jackbuf);		 // lock buffer into memory so that it never gets paged out

  pthread_mutex_lock(&client_mutex);
  jack_status_t status;
  client = jack_client_open(config.jack_client_name, JackNoStartServer, &status);
  if (!client) {
    die("Could not start JACK server. JackStatus is %x", status);
  }
  sample_rate = jack_get_sample_rate(client);
  if (sample_rate != 44100) {
    die("The JACK server is running at the wrong sample rate (%d) for Shairport Sync. Must be 44100 Hz.",
        sample_rate);
  }
  jack_set_process_callback(client, &process, NULL);
  jack_set_graph_order_callback(client, &graph, NULL);
  jack_set_error_function(&error);
  jack_set_info_function(&info);

  for (i=0; i < NPORTS; i++) {
    port[i] = jack_port_register(client, port_name[i], JACK_DEFAULT_AUDIO_TYPE,
                                 JackPortIsOutput, 0);
  }
  if (jack_activate(client)) {
    die("Could not activate %s JACK client.", config.jack_client_name);
  } else {
    debug(2, "JACK client %s activated sucessfully.", config.jack_client_name);
  }

  if (config.jack_autoconnect_pattern != NULL) {
    debug(1, "config.jack_autoconnect_pattern is %s.", config.jack_autoconnect_pattern);
    const char** port_list = jack_get_ports(client, config.jack_autoconnect_pattern,
                                            JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput);
    for (i=0; i<NPORTS ; i++) {
      char* full_port_name[NPORTS];
      full_port_name[i] = malloc(sizeof(char) * jack_port_name_size());
      sprintf(full_port_name[i], "%s:%s", config.jack_client_name, port_name[i]);
      if (port_list[i] != NULL) {
        int err;
        debug(1, "Connecting %s to %s.", full_port_name[i], port_list[i]);
        err = jack_connect(client, full_port_name[i], port_list[i]);
        switch (err) {
        case EEXIST:
          inform("The requested connection from %s to %s already exists.",
                 full_port_name[i], port_list[i]);
          break;
        case 0:
          // success
          break;
        default:
          inform("JACK error no. %d occured while trying to connect %s to %s.",
                 err, full_port_name[i], port_list[i]);
          break;
        }
      } else {
        inform("No matching port found in %s to connect %s to. You may not hear audio.",
               config.jack_autoconnect_pattern, full_port_name[i]);
      }
      free(full_port_name[i]);
    }
    while (port_list[i++] != NULL) {
      inform("Additional matching port %s found. Check that the connections are what you intended.");
    }
    jack_free(port_list);
  }
  pthread_mutex_unlock(&client_mutex);

  return 0;
}

void jack_deinit() {
  pthread_mutex_lock(&client_mutex);
  if (jack_deactivate(client))
    debug(1, "Error deactivating jack client");
  if (jack_client_close(client))
    debug(1, "Error closing jack client");
  pthread_mutex_unlock(&client_mutex);
  jack_ringbuffer_free(jackbuf);
}

void jack_start(__attribute__((unused)) int i_sample_rate,
                __attribute__((unused)) int i_sample_format) {
  // nothing to do, JACK client has already been set up at jack_init()
  // also, we have no say over the sample rate or sample format of JACK
  // we convert the 16bit samples to float, and die if the sample rate is != 44k1.
  // FIXME: later, resampling would be nice. Fold into soxr if possible
}

void jack_flush() {
  // debug(1, "Only the consumer can safely flush a lock-free ringbuffer. Asking the process callback to do it...");
  flush_please = 1;
}

int jack_delay(long *the_delay) {
  // semantics change: we now look at the last transfer into the lock-free ringbuffer, not
  // into the jack buffers directly (because locking those would violate real-time constraints).
  // on average, that should lead to just a constant additional latency. the old comment still applies:

  // without the mutex, we could get the time of what is the last transfer of data to a jack buffer,
  // but then a transfer could occur and we would get the buffer occupancy after another transfer
  // had occurred
  // so we could "lose" a full transfer (e.g. 1024 frames @ 44,100 fps ~ 23.2 milliseconds)
  pthread_mutex_lock(&buffer_mutex);
    int64_t time_now = get_absolute_time_in_fp();
    // this is the time back to the last time data
    // was transferred into a jack buffer
    int64_t delta = time_now - time_of_latest_transfer;
    // this is the buffer occupancy before any
    // subsequent transfer because transfer is blocked
    // by the mutex
    size_t audio_occupancy_now = jack_ringbuffer_read_space(jackbuf) / bytes_per_frame;
    // debug(1, "audio_occupancy_now is %d.", audio_occupancy_now);
  pthread_mutex_unlock(&buffer_mutex);

  int64_t frames_processed_since_latest_latency_check = (delta * 44100) >> 32;
  // debug(1,"delta: %" PRId64 " frames.",frames_processed_since_latest_latency_check);
  // use the average of the left and right maximum latencies. if max is really different from min,
  // there is an anomaly in the graph that we don't have any hope of fixing anyways
  *the_delay = jack_latency + audio_occupancy_now - frames_processed_since_latest_latency_check;
  // debug(1,"reporting a delay of %d frames",*the_delay);
  return 0;
}

int play(void *buf, int samples) {
  // debug(1,"jack_play of %d samples.",samples);
  // copy the samples into the queue
  size_t bytes_to_transfer, bytes_transferred;
  bytes_to_transfer = samples * bytes_per_frame;
  pthread_mutex_lock(&buffer_mutex); // it's ok to lock here since we're not in the realtime callback
    bytes_transferred = jack_ringbuffer_write(jackbuf, buf, bytes_to_transfer);
    // semantics change: we now measure the last time audio was moved into the ringbuffer, not the jack output buffers.
    time_of_latest_transfer = get_absolute_time_in_fp();
  pthread_mutex_unlock(&buffer_mutex);
  if (bytes_transferred < bytes_to_transfer) {
    debug(1, "JACK ringbuffer overrun. Only wrote %d of %d bytes.", bytes_transferred, bytes_to_transfer);
  }
  return 0;
}
