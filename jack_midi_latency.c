/* JACK MIDI Latency Tester
 *
 * (C) 2013  Robin Gareus <robin@gareus.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 */

#ifdef WIN32
#include <windows.h>
#include <pthread.h>
#define pthread_t //< override jack.h def
#endif

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <getopt.h>
#include <math.h>
#include <sys/mman.h>

#ifndef WIN32
#include <signal.h>
#include <pthread.h>
#endif

#include <jack/jack.h>
#include <jack/transport.h>
#include <jack/ringbuffer.h>
#include <jack/midiport.h>

#define RBSIZE 20

/* jack connection */
jack_client_t *j_client = NULL;
jack_port_t   *m_input_port;
jack_port_t   *m_output_port;

/* jack port latencies */
jack_latency_range_t capture_latency = {-1, -1};
jack_latency_range_t playback_latency = {-1, -1};

/* threaded communication */
static jack_ringbuffer_t *rb = NULL;
static pthread_mutex_t msg_thread_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t data_ready = PTHREAD_COND_INITIALIZER;

/* application state */
static double samplerate = 48000.0;
static volatile unsigned long long monotonic_cnt = 0;
static int run = 1;

/* options */
static char *inport = NULL;
static char *outport = NULL;

struct timenfo {
  long long tdiff;
  unsigned int period;
};

static void process_jmidi_event(jack_midi_event_t *ev, unsigned long long mfcnt, unsigned int jbs) {
#define MODX (16384)  // 1<<(2*7)
#define MASK (0x3fff) // MODX - 1
  struct timenfo nfo;
  if (ev->size != 3 || ev->buffer[0] != 0xf2 ) return;
  const unsigned long long tc = (mfcnt + ev->time) & MASK;
  const unsigned long long ti = (ev->buffer[2]<<7) | ev->buffer[1];

  nfo.tdiff = (MODX + tc - ti) % MODX;
  nfo.period = jbs;

  if (jack_ringbuffer_write_space(rb) >= sizeof(struct timenfo)) {
    jack_ringbuffer_write(rb, (void *) &nfo, sizeof(struct timenfo));
  }

  if (pthread_mutex_trylock (&msg_thread_lock) == 0) {
    pthread_cond_signal (&data_ready);
    pthread_mutex_unlock (&msg_thread_lock);
  }
}

static void send_rt_message(void* port_buf, jack_nframes_t time, unsigned long long mfcnt) {
  uint8_t *buffer;
  buffer = jack_midi_event_reserve(port_buf, time, 3);
  if(buffer) {
    buffer[0] = 0xf2;
    buffer[1] = (mfcnt)&0x7f;
    buffer[2] = (mfcnt>>7)&0x7f;
  }
}

/**
 * jack process callback
 */
static int process(jack_nframes_t nframes, void *arg) {
  void *out_buf = jack_port_get_buffer(m_output_port, nframes);
  void *in_buf = jack_port_get_buffer(m_input_port, nframes);

  int nevents = jack_midi_get_event_count(in_buf);
  int n;
  jack_midi_clear_buffer(out_buf);

  send_rt_message(out_buf, 0, monotonic_cnt);

  for (n=0; n < nevents; n++) {
    jack_midi_event_t ev;
    jack_midi_event_get(&ev, in_buf, n);
    process_jmidi_event(&ev, monotonic_cnt, nframes);
  }
  monotonic_cnt += nframes;
  return 0;
}

/**
 * callback if jack server terminates
 */
void jack_shutdown(void *arg) {
  j_client=NULL;
  pthread_cond_signal (&data_ready);
  fprintf (stderr, "jack server shutdown\n");
}


static void jack_latency_cb (jack_latency_callback_mode_t mode, void *arg) {
  jack_latency_range_t range;

  range.min = range.max = 0;

  if (mode == JackCaptureLatency) {
    jack_port_set_latency_range (m_output_port, mode, &range);
    jack_port_get_latency_range (m_input_port, mode, &range);
    if ((range.min != capture_latency.min) || (range.max != capture_latency.max)) {
      capture_latency = range;
      printf ("new capture latency: [%d, %d]\n", range.min, range.max);
    }
  } else {
    jack_port_set_latency_range (m_input_port, mode, &range);
    jack_port_get_latency_range (m_output_port, mode, &range);
    if ((range.min != playback_latency.min) || (range.max != playback_latency.max)) {
      playback_latency = range;
      printf ("new playback latency: [%d, %d]\n", range.min, range.max);
    }
  }
}


/**
 * cleanup and exit
 * call this function only after everything has been initialized!
 */
void cleanup(void) {
  if (j_client) {
    jack_deactivate (j_client);
    jack_client_close (j_client);
  }
  if (rb) {
    jack_ringbuffer_free(rb);
  }
  j_client = NULL;
}

/**
 * open a client connection to the JACK server
 */
static int init_jack(const char *client_name) {
  jack_status_t status;
  j_client = jack_client_open (client_name, JackNullOption, &status);
  if (j_client == NULL) {
    fprintf (stderr, "jack_client_open() failed, status = 0x%2.0x\n", status);
    if (status & JackServerFailed) {
      fprintf (stderr, "Unable to connect to JACK server\n");
    }
    return (-1);
  }
  if (status & JackServerStarted) {
    fprintf (stderr, "JACK server started\n");
  }
  if (status & JackNameNotUnique) {
    client_name = jack_get_client_name(j_client);
    fprintf (stderr, "jack-client name: `%s'\n", client_name);
  }
  jack_set_process_callback (j_client, process, 0);
  jack_set_latency_callback (j_client, jack_latency_cb, 0);

#ifndef WIN32
  jack_on_shutdown (j_client, jack_shutdown, NULL);
#endif
  samplerate = (double) jack_get_sample_rate (j_client);

  return (0);
}

static int jack_portsetup(void) {
  if ((m_input_port = jack_port_register(j_client, "in", JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0)) == 0) {
    fprintf (stderr, "cannot register mclk input port !\n");
    return (-1);
  }
  if ((m_output_port = jack_port_register(j_client, "out", JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput, 0)) == 0) {
    fprintf (stderr, "cannot register mclk input port !\n");
    return (-1);
  }
  return (0);
}

static void inport_connect(char *port) {
  if (port && jack_connect(j_client, port, jack_port_name(m_input_port))) {
    fprintf(stderr, "cannot connect port %s to %s\n", port, jack_port_name(m_input_port));
  }
}

static void outport_connect(char *port) {
  if (port && jack_connect(j_client, jack_port_name(m_output_port), port)) {
    fprintf(stderr, "cannot connect port %s to %s\n", jack_port_name(m_output_port), port);
  }
}


static void wearedone(int sig) {
  fprintf(stderr,"caught signal - shutting down.\n");
  run=0;
  pthread_cond_signal (&data_ready);
}


/**************************
 * main application code
 */

static struct option const long_options[] =
{
  {"help", no_argument, 0, 'h'},
  {"input", required_argument, 0, 'i'},
  {"output", required_argument, 0, 'o'},
  {"version", no_argument, 0, 'V'},
  {NULL, 0, NULL, 0}
};

static void usage (int status) {
  printf ("jack_midi_latency - JACK MIDI Latency Measurement Tool.\n\n");
  printf ("Usage: jack_midi_latency [ OPTIONS ]\n\n");
  printf ("Options:\n\
  -h, --help                 display this help and exit\n\
  -i <port-name>, --input <port-name>\n\
                             autoconnect to given jack-midi capture port\n\
  -o <port-name>, --output <port-name>\n\
                             autoconnect to given jack-midi playback port\n\
  -V, --version              print version information and exit\n\
\n");
/*                                  longest help text w/80 chars per line ---->|\n" */
  printf ("\n\
Measure MIDI roundtrip latency\n\
\n");
  printf ("Report bugs to Robin Gareus <robin@gareus.org>\n"
	  "Website and manual: <https://github.com/x42/jack_midi_latency>\n"
    );
  exit (status);
}

static int decode_switches (int argc, char **argv) {
  int c;

  while ((c = getopt_long (argc, argv,
	 "h"  /* help */
	 "i:" /* input */
	 "o:" /* output */
	 "V", /* version */
	 long_options, (int *) 0)) != EOF) {
    switch (c) {
      case 'i':
	inport = optarg;
	break;
      case 'o':
	outport = optarg;
	break;
      case 'V':
	printf ("jack_midi_latency version %s\n\n", VERSION);
	printf ("Copyright (C) GPL 2013 Robin Gareus <robin@gareus.org>\n");
	exit (0);

      case 'h':
	usage (0);

      default:
	usage (EXIT_FAILURE);
    }
  }
  return optind;
}

int main (int argc, char ** argv) {

  decode_switches (argc, argv);

  if (optind > argc) {
    usage (EXIT_FAILURE);
  }

  if (init_jack("jack_midi_latency"))
    goto out;
  if (jack_portsetup())
    goto out;

  rb = jack_ringbuffer_create(RBSIZE * sizeof(struct timenfo));

  if (mlockall (MCL_CURRENT | MCL_FUTURE)) {
    fprintf(stderr, "Warning: Can not lock memory.\n");
  }

  if (jack_activate (j_client)) {
    fprintf (stderr, "cannot activate client.\n");
    goto out;
  }

  inport_connect(inport);
  outport_connect(outport);

#ifndef _WIN32
  signal(SIGHUP, wearedone);
  signal(SIGINT, wearedone);
#endif

  pthread_mutex_lock (&msg_thread_lock);

  /* all systems go */

  while (run && j_client) {
    int i;
    const int mqlen = jack_ringbuffer_read_space (rb) / sizeof(struct timenfo);
    for (i=0; i < mqlen; ++i) {
      struct timenfo nfo;
      jack_ringbuffer_read(rb, (char*) &nfo, sizeof(struct timenfo));

      int latency = capture_latency.max + playback_latency.max;
      if (latency <= 0) latency = 2 * nfo.period; /* jack does not [yet] report MIDI port latency */

      printf("roundtrip latency: %5lld frames = %6.2fms || non-jack: %5lld frames         \r",
	  nfo.tdiff, nfo.tdiff * 1000.0 / samplerate,
	  nfo.tdiff - latency);
      // TODO calc avg, sigma|jitter
    }
    fflush(stdout);
    pthread_cond_wait (&data_ready, &msg_thread_lock);
  }
  pthread_mutex_unlock (&msg_thread_lock);

out:
  cleanup();
  return 0;
}
/* vi:set ts=8 sts=2 sw=2: */
