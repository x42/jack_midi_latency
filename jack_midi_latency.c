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
#define HISTLEN 500
#define TERMWIDTH 50

#ifndef SQUARE
#define SQUARE(a) ( (a) * (a) )
#endif

#ifndef MIN
#define MIN(a,b) ( (a) < (b) ? (a) : (b) )
#endif

#ifndef MAX
#define MAX(a,b) ( (a) > (b) ? (a) : (b) )
#endif

#ifndef RAIL
#define RAIL(v, min, max) (MIN((max), MAX((min), (v))))
#endif


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
static int nperiod = 2; // jackd --nperiod
static int max_events = 10000; // events to receive
static int printinterval = 1; // seconds

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
  {"events", required_argument, 0, 'e'},
  {"input", required_argument, 0, 'i'},
  {"output", required_argument, 0, 'o'},
  {"print", required_argument, 0, 'p'},
  {"version", no_argument, 0, 'V'},
  {NULL, 0, NULL, 0}
};

static void usage (int status) {
  printf ("jack_midi_latency - JACK MIDI Latency Measurement Tool.\n\n");
  printf ("Usage: jack_midi_latency [ OPTIONS ]\n\n");
  printf ("Options:\n\
  -h, --help            display this help and exit\n\
  -e, --events <num>    number of midi-events to send/receive (default: 10000),\n\
                        if <= 0 no limit, run until signalled.\n\
  -i <port-name>, --input <port-name>\n\
                        auto-connect to given jack-midi capture port\n\
  -o <port-name>, --output <port-name>\n\
                        auto-connect to given jack-midi playback port\n\
  -p, --print <num>     print min/max/avg statistics every N seconds,\n\
                        (default : 1 sec), 0 disable.\n\
  -V, --version         print version information and exit\n\
\n");
/*                                  longest help text w/80 chars per line ---->|\n" */
  printf ("\n\
Measure MIDI roundtrip latency...\n\
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
	 "e:" /* events */
	 "i:" /* input */
	 "o:" /* output */
	 "p:" /* printinterval */
	 "V", /* version */
	 long_options, (int *) 0)) != EOF) {
    switch (c) {
      case 'e':
	max_events = atoi(optarg);
	break;
      case 'i':
	inport = optarg;
	break;
      case 'o':
	outport = optarg;
	break;
      case 'p':
	printinterval = atoi(optarg);
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

  if (!inport && !outport) {
    printf("Close the signal-loop to measure JACK MIDI round-trip-latency:\n");
    printf("    jack_midi_latency:out\n -> soundcard midi-port\n -> cable\n -> soundcard midi-port\n -> jack_midi_latency:in\n\n");
  }
  if (max_events > 0 ) {
    printf("Collecting data from %d midi-events; press Ctrl+C to end test early.\n\n", max_events);
  } else {
    printf("Press Ctrl+C to end test.\n\n");
  }

  int cnt_t, cnt_a;
  int min_t, max_t, min_a, max_a;
  double avg_t, avg_a;
  double var_m, var_s;

  unsigned int *history = calloc(HISTLEN, sizeof(unsigned int));
  unsigned int histsize = 0;
  double bin_width = 0;
  double bin_min = 0;
  unsigned int *histogram = NULL;

  cnt_t = 0; cnt_a = 0;
  min_t = min_a = MODX;
  max_t = max_a = 0;
  avg_t = avg_a = 0;
  var_m = var_s = 0;

  time_t last = time(NULL);
  int jack_period = 0;
  int latency = 0;

  while (run && j_client) {
    int i;
    const int mqlen = jack_ringbuffer_read_space (rb) / sizeof(struct timenfo);
    time_t now = time(NULL);
    for (i=0; i < mqlen; ++i) {
      struct timenfo nfo;
      jack_ringbuffer_read(rb, (char*) &nfo, sizeof(struct timenfo));
      if (printinterval > 0 && now >= last + printinterval) {
	last = now;
	printf("\ncurrent: min=%d max=%d avg=%.1f [samples]   --   total events: %d\n", min_t, max_t, avg_t / (double)cnt_t, cnt_a);
	cnt_t = 0;
	min_t = MODX;
	max_t = 0;
	avg_t = 0;
      }

      jack_period = nfo.period;
      latency = capture_latency.max + playback_latency.max;
      if (latency <= 0) latency = nperiod * nfo.period; /* XXX jack does not [yet] report MIDI port latency */

      printf("roundtrip latency: %5lld frames = %6.2fms || non-jack: %5lld frames         \r",
	  nfo.tdiff, nfo.tdiff * 1000.0 / samplerate,
	  nfo.tdiff - latency);

      avg_t += nfo.tdiff;
      avg_a += nfo.tdiff;
      if (nfo.tdiff < min_t) min_t = nfo.tdiff;
      if (nfo.tdiff > max_t) max_t = nfo.tdiff;
      if (nfo.tdiff < min_a) min_a = nfo.tdiff;
      if (nfo.tdiff > max_a) max_a = nfo.tdiff;

      /* running variance */
      if (avg_a == 0) {
	var_m = nfo.tdiff;
      } else {
	const double var_m1 = var_m;
	var_m = var_m + ((double)nfo.tdiff - var_m) / (double)(cnt_a + 1);
	var_s = var_s + ((double)nfo.tdiff - var_m) * ((double)nfo.tdiff - var_m1);
      }

      /* histogram */
      if (cnt_a < HISTLEN) {
	history[cnt_a] = nfo.tdiff;
      } else if (cnt_a == HISTLEN) {
	int j;
	double stddev = 0;
	const double avg = avg_a / (double)HISTLEN;
	for (j = 0; j < HISTLEN; ++j) {
	  stddev += SQUARE((double)history[j] - avg);
	}
	stddev = sqrt(stddev/(double)HISTLEN);
	// Scott's normal reference rule
	bin_width = 3.5 * stddev * pow(HISTLEN, -1.0/3.0);
	int k = ceil((double)(max_a - min_a) / bin_width);

	bin_min = min_a;
	if (bin_min > bin_width) { k++; bin_min -= bin_width; }
	if (bin_min > bin_width) { k++; bin_min -= bin_width; }
	if (bin_min > bin_width) { k++; bin_min -= bin_width; }
	histsize = k+2;

	if (printinterval > 0) {
	  printf("\n -- initializing histogram with %d bins (min:%.2f w:%.2f [samples]) --\n", histsize, bin_min, bin_width);
	}

	histogram = calloc(histsize + 1,sizeof(unsigned int));
	for (j = 0; j < HISTLEN; ++j) {
	  int bin = RAIL(floor(((double)history[j] - bin_min) / bin_width), 0, histsize);
	  histogram[bin]++;
	}
      } else {
	int bin = RAIL(floor(((double)nfo.tdiff - bin_min) / bin_width), 0, histsize);
	histogram[bin]++;
      }

      cnt_t++; cnt_a++;
    }
    fflush(stdout);
    if (max_events > 0 && cnt_a >= max_events) break;
    pthread_cond_wait (&data_ready, &msg_thread_lock);
  }
  pthread_mutex_unlock (&msg_thread_lock);
  printf("\n\n");

  if (cnt_a == 0) {
    printf("No signal was detected.\n");
  } else {
    printf("JACK settings: samplerate: %d, samples/period: %d\n", (int) samplerate, jack_period);
    printf("               probable nominal jack latency: %d [samples] = %.2f [ms]\n", latency, 1000.0 * latency / samplerate);
    double stddev = cnt_a > 1 ? sqrt(var_s / ((double)(cnt_a-1))) : 0;
    printf("TOTAL: %d MIDI events sent+received.\n", cnt_a);
    printf(" min=%6d max=%6d range=%6d avg=%6.1f dev=%6.2f [samples]\n",
	min_a, max_a, max_a-min_a, avg_a / cnt_a, stddev);
    printf(" min=%6.2f max=%6.2f range=%6.2f avg=%6.1f dev=%6.2f [ms]\n",
	1000.0 * min_a / samplerate, 1000.0 * max_a / samplerate,
	1000.0 * (max_a-min_a) / samplerate,
	1000.0 * avg_a / cnt_a / samplerate, 1000.0 * stddev / samplerate
	);
  }

  if (histsize > 0) {
    printf("\n");
    int i,j;
    int binlevel = 0;
    for (i = 0; i < histsize; ++i) {
      if (histogram[i] > binlevel) binlevel = histogram[i];
    }
    if (binlevel > 0) for (i = 0; i <= histsize; ++i) {
      double hmin, hmax;
      if (i == 0) {
	hmin = 0.0;
	hmax = bin_min;
      } else if (i == histsize) {
	hmin = bin_min + (double)(i-1) * bin_width;
	hmax = INFINITY;
      } else {
	hmin = bin_min + (double)(i-1) * bin_width;
	hmax = bin_min + (double)(i) * bin_width;
      }
      printf("%5.2f .. %5.2f [ms]:%7d ", 1000.0 * hmin / samplerate, 1000.0 * hmax / samplerate, histogram[i]);
      int bar_width = (histogram[i] * TERMWIDTH ) / binlevel;
      if (bar_width == 0 && histogram[i] > 0) bar_width = 1;
      for (j = 0; j < bar_width; ++j) printf("#");
      printf("\n");
    }
  }

  free(histogram);
  free(history);

out:
  cleanup();
  return 0;
}
/* vi:set ts=8 sts=2 sw=2: */
