#include <sys/types.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include "common.h"

#define NR_TIMERS 8		/* number of timers */
#define MAX_QUEUE 100000	/* max number of buffered frames */
#define NO_EVENT -1		/* no event possible */
#define FRAME_SIZE (sizeof(frame))
#define BYTE 0377		/* byte mask */
#define UINT_MAX  0xFFFFFFFF	/* maximum value of an unsigned 32-bit int */
#define INTERVAL 100000		/* interval for periodic printing */
#define AUX 2			/* aux timeout is main timeout/AUX */

/* DEBUG MASKS */
#define SENDS        0x0001	/* frames sent */
#define RECEIVES     0x0002	/* frames received */
#define TIMEOUTS     0x0004	/* timeouts */
#define PERIODIC     0x0008	/* periodic printout for use with long runs */

/* Status variables used by the workers, M0 and M1. */
bigint ack_timer[NR_TIMERS];	/* ack timers */
unsigned int seqs[NR_TIMERS];	/* last sequence number sent per timer */
bigint lowest_timer;		/* lowest of the timers */
bigint aux_timer;		/* value of the auxiliary timer */
int network_layer_status;	/* 0 is disabled, 1 is enabled */
unsigned int next_net_pkt;	/* seq of next network packet to fetch */
unsigned int last_pkt_given= 0xFFFFFFFF;	/* seq of last pkt delivered*/
frame last_frame;		/* arrive frames are kept here */
int offset;			/* to prevent multiple timeouts on same tick*/
bigint tick;			/* current time */
int retransmitting;		/* flag that is set on a timeout */
int nseqs = -1;			/* must be MAX_SEQ + 1 after startup */
extern unsigned int oldest_frame;	/* tells protocol 6 which frame timed out */

char *badgood[] = {"bad ", "good"};
char *tag[] = {"Data", "Ack ", "Nak "};

/* Statistics */
int data_sent;			/* number of data frames sent */
int data_retransmitted;		/* number of data frames retransmitted */
int data_lost;			/* number of data frames lost */
int data_not_lost;		/* number of data frames not lost */
int good_data_recd;		/* number of data frames received */
int cksum_data_recd;		/* number of bad data frames received */

int acks_sent;			/* number of ack frames sent */
int acks_lost;			/* number of ack frames lost */
int acks_not_lost;		/* number of ack frames not lost */
int good_acks_recd;		/* number of ack frames received */
int cksum_acks_recd;		/* number of bad ack frames received */

int payloads_accepted;		/* number of pkts passed to network layer */
int timeouts;			/* number of timeouts */
int ack_timeouts;		/* number of ack timeouts */

/* Incoming frames are buffered here for later processing. */
frame queue[MAX_QUEUE];		/* buffered incoming frames */
frame *inp = &queue[0];		/* where to put the next frame */
frame *outp = &queue[0];	/* where to remove the next frame from */
int nframes;			/* number of queued frames */

/* Prototypes. */
void wait_for_event(event_type *event);
void queue_frames(void);
int pick_event(void);
event_type frametype(void);
void from_network_layer(packet *p);
void to_network_layer(packet *p);
void from_physical_layer(frame *r);
void to_physical_layer(frame *s);
void start_timer(seq_nr k);
void stop_timer(seq_nr k);
void start_ack_timer(void);
void stop_ack_timer(void);
void enable_network_layer(void);
void disable_network_layer(void);
int check_timers(void);
int check_ack_timer(void);
unsigned int pktnum(packet *p);
void fr(frame *f);
void recalc_timers(void);
void print_statistics(void);
void sim_error(char *s);


void wait_for_event(event_type *event)
{
/* Wait_for_event reads the pipe from main to get the time.  Then it
 * fstat's the pipe from the other worker to see if any
 * frames are there.  If so, if collects them all in the queue array.
 * Once the pipe is empty, it makes a decision about what to do next.
 */
 
 bigint ct, word = OK;

  if (nseqs < 0) nseqs = oldest_frame;	/* need MAX_SEQ+1 for protocol 6 */
  offset = 0;			/* prevents two timeouts at the same tick */
  retransmitting = 0;		/* counts retransmissions */
  while (true) {
	queue_frames();		/* go get any newly arrived frames */
	if (write(mwfd, &word, TICK_SIZE) != TICK_SIZE) print_statistics();
	if (read(mrfd, &ct, TICK_SIZE) != TICK_SIZE) print_statistics();
	if (ct == 0) print_statistics();
	tick = ct;		/* update time */
	if ((debug_flags & PERIODIC) && (tick%INTERVAL == 0))
		printf("Tick %u. Proc %d. Data sent=%d  Payloads accepted=%d  Timeouts=%d\n", tick/DELTA, id, data_sent, payloads_accepted, timeouts);

	/* Now pick event. */
	*event = pick_event();
	if (*event == NO_EVENT) {
		word = (lowest_timer == 0 ? NOTHING : OK);
		continue;
	}
	word = OK;
	if (*event == timeout) {
		timeouts++;
		retransmitting = 1;	/* enter retransmission mode */
		if (debug_flags & TIMEOUTS)
		      printf("Tick %u. Proc %d got timeout for frame %d\n",
					       tick/DELTA, id, oldest_frame);
	}

	if (*event == ack_timeout) {
		ack_timeouts++;
		if (debug_flags & TIMEOUTS)
		      printf("Tick %u. Proc %d got ack timeout\n",
					       tick/DELTA, id);
	}
	return;
  }
}


void queue_frames(void)
{
/* See if any frames from the peer have arrived; if so get and queue them.
 * Queue_frames() sucks frames out of the pipe into the circular buffer,
 * queue[]. It first fstats the pipe, to avoid reading from an empty pipe and
 * thus blocking.  If inp is near the top of queue[], a single call here
 * may read a few frames into the top of queue[] and then some more starting
 * at queue[0].  This is done in two read operations.
 */

  int prfd, frct, k;
  frame *top;
  struct stat statbuf;

  prfd = (id == 0 ? r2 : r1);	/* which file descriptor is pipe on */

  if (fstat(prfd, &statbuf) < 0) sim_error("Cannot fstat peer pipe");
  frct = statbuf.st_size/FRAME_SIZE;	/* number of arrived frames */

  if (nframes + frct >= MAX_QUEUE)	/* check for possible queue overflow*/
	sim_error("Out of queue space. Increase MAX_QUEUE and re-make.");  

  /* If frct is 0, the pipe is empty, so don't read from it. */
  if (frct > 0) {
	/* How many frames can be read consecutively? */
	top = (outp <= inp ? &queue[MAX_QUEUE] : outp);/* how far can we rd?*/
	k = top - inp;	/* number of frames that can be read consecutively */
	if (k > frct) k = frct;	/* how many frames to read from peer */
	if (read(prfd, inp, k * FRAME_SIZE) != k * FRAME_SIZE)
		sim_error("Error reading frames from peer");
	frct -= k;		/* residual frames not yet read */
	inp += k;
	if (inp == &queue[MAX_QUEUE]) inp = queue;
	nframes += k;

	/* If frct is still > 0, the queue has been filled to the upper
	 * limit, but there is still space at the bottom.  Continue reading
	 * there.  This mechanism makes queue a circular buffer.
	 */
	if (frct > 0) {
		if (read(prfd, queue, frct * FRAME_SIZE) != frct*FRAME_SIZE)
			sim_error("Error 2 reading frames from peer");
		nframes += frct;
		inp = &queue[frct];
	}
  }
}


int pick_event(void)
{
/* Pick a random event that is now possible for the process.
 * The set of legal events depends on the protocol number and system state.
 * A timeout is not possible, for example, if no frames are outstanding.
 * For each protocol, events from 0 to some protocol-dependent maximum
 * are potentially allowed.  The maximum is given by highest_event.  The
 * events that are theoretically possible are given below.
 *
 *  # Event		Protocols:  1 2 3 4 5 6
 *  0 frame_arrival                 x x x x x x
 *  1 chksum_err                        x x x x
 *  2 timeout                           x x x x
 *  3 network_layer_ready                   x x 
 *  4 ack_timeout                             x (e.g. only 6 gets ack_timeout)
 *
 * Note that the order in which the tests is made is critical, as it gives
 * priority to some events over others.  For example, for protocols 3 and 4
 * frames will be delivered before a timeout will be caused.  This is probably
 * a reasonable strategy, and more closely models how a real line works.
 */

  switch(protocol) {
    case 2:			/* {frame_arrival} */
	if (nframes == 0 && lowest_timer == 0) return(NO_EVENT);
	return(frametype());

    case 3:			/* {frame_arrival, cksum_err, timeout} */
    case 4:
	if (nframes > 0) return((int)frametype());
	if (check_timers() >= 0) return(timeout);	/* timer went off */
	return(NO_EVENT);

    case 5:	/* {frame_arrival, cksum_err, timeout, network_layer_ready} */
	if (nframes > 0) return((int)frametype());
	if (network_layer_status) return(network_layer_ready);
	if (check_timers() >= 0) return(timeout);	/* timer went off */
	return(NO_EVENT);

    case 6:	/* {frame_arrival, cksum_err, timeout, net_rdy, ack_timeout}*/
	if (check_ack_timer() > 0) return(ack_timeout);
	if (nframes > 0) return((int)frametype());
	if (network_layer_status) return(network_layer_ready);
	if (check_timers() >= 0) return(timeout);	/* timer went off */
	return(NO_EVENT);
  }
}


event_type frametype(void)
{
/* This function is called after it has been decided that a frame_arrival
 * event will occur.  The earliest frame is removed from queue[] and copied
 * to last_frame.  This copying is needed to avoid messing up the simulation
 * in the event that the protocol does not actually read the incoming frame.
 * In protocols 2 and 3, the senders do not call from_physical_layer() to
 * collect the incoming frame.  If frametype() did not remove incoming frames
 * from queue[], they never would be removed.  Of course, one could change
 * sender2() and sender3() to have them call from_physical_layer(), but doing
 * it this way is more robust.
 *
 * This function determines (stochastically) whether the arrived frame is good
 * or bad (contains a checksum error).
 */

  int n, i;
  event_type event;

  /* Remove one frame from the queue. */
  last_frame = *outp;		/* copy the first frame in the queue */
  outp++;
  if (outp == &queue[MAX_QUEUE]) outp = queue;
  nframes--;

  /* Generate frames with checksum errors at random. */
  n = rand() & 01777;
  if (n < garbled) {
	/* Checksum error.*/
	event = cksum_err;
	if (last_frame.kind == data) cksum_data_recd++;
	if (last_frame.kind == ack) cksum_acks_recd++;
	i = 0;
  } else {
	event = frame_arrival;
	if (last_frame.kind == data) good_data_recd++;
	if (last_frame.kind == ack) good_acks_recd++;
	i = 1;
  }

  if (debug_flags & RECEIVES) {
	printf("Tick %u. Proc %d got %s frame:  ",
						tick/DELTA,id,badgood[i]);
	fr(&last_frame);
  }
  return(event);
}


void from_network_layer(packet *p)
{
/* Fetch a packet from the network layer for transmission on the channel. */

  p->data[0] = (next_net_pkt >> 24) & BYTE;
  p->data[1] = (next_net_pkt >> 16) & BYTE;
  p->data[2] = (next_net_pkt >>  8) & BYTE;
  p->data[3] = (next_net_pkt      ) & BYTE;
  next_net_pkt++;
}


void to_network_layer(packet *p)
{
/* Deliver information from an inbound frame to the network layer. A check is
 * made to see if the packet is in sequence.  If it is not, the simulation
 * is terminated with a "protocol error" message.
 */

  unsigned int num;

  num = pktnum(p);
  if (num != last_pkt_given + 1) {
	printf("Tick %u. Proc %d got protocol error.  Packet delivered out of order.\n", tick/DELTA, id); 
	printf("Expected payload %d but got payload %d\n",last_pkt_given+1,num);
	exit(0);
  }
  last_pkt_given = num;
  payloads_accepted++;
}

  
void from_physical_layer (frame *r)
{
/* Copy the newly-arrived frame to the user. */
 *r = last_frame;
}


void to_physical_layer(frame *s)
{
/* Pass the frame to the physical layer for writing on pipe 1 or 2. 
 * However, this is where bad packets are discarded: they never get written.
 */

  int fd, got, k;

  /* Fill in fields that that the simulator expects but some protocols do
   * not fill in or use.  This filling is not strictly needed, but makes the
   * simulation trace look better, showing unused fields as zeros.
   */
  switch(protocol) {
    case 2:
	s->seq = 0;

    case 3:
	s->kind = (id == 0 ? data : ack);
	if (s->kind == ack) {
		s->seq = 0;
		s->info.data[0] = 0;
		s->info.data[1] = 0;
		s->info.data[2] = 0;
		s->info.data[3] = 0;
	}
        break;

     case 4:
     case 5:
 	s->kind = data;
	break;

     case 6:
	if (s->kind == nak) {
		s->info.data[0] = 0;
		s->info.data[1] = 0;
		s->info.data[2] = 0;
		s->info.data[3] = 0;
	}

	/* The following statement is essential to protocol 6.  In that
	 * protocol, oldest_frame is automagically set properly to the
	 * sequence number of the frame that has timed out.  Keeping track of
	 * this information is a bit tricky, since the call to start_timer()
	 * does not tell what the sequence number is, just the buffer.  The
	 * simulator keeps track of sequence numbers using the array seqs[],
	 * which records the sequence number of each data frame sent, so on a
	 * timeout, knowing the buffer number makes it possible to determine
	 * the sequence number.
	 */
	if (s->kind==data) seqs[s->seq % (nseqs/2)] = s->seq; /* save seq # */
  }

  if (s->kind == data) data_sent++;
  if (s->kind == ack) acks_sent++;
  if (retransmitting) data_retransmitted++;

  /* Bad transmissions (checksum errors) are simulated here. */
  k = rand() & 01777;		/* 0 <= k <= about 1000 (really 1023) */
  if (k < pkt_loss) {	/* simulate packet loss */
	if (debug_flags & SENDS) {
		printf("Tick %u. Proc %d sent frame that got lost: ",
							    tick/DELTA, id);
		fr(s);
	}
	if (s->kind == data) data_lost++;	/* statistics gathering */
	if (s->kind == ack) acks_lost++;	/* ditto */
	return;

  }
  if (s->kind == data) data_not_lost++;		/* statistics gathering */
  if (s->kind == ack) acks_not_lost++;		/* ditto */
  fd = (id == 0 ? w1 : w2);

  got = write(fd, s, FRAME_SIZE);
  if (got != FRAME_SIZE) print_statistics();	/* must be done */

  if (debug_flags & SENDS) {
	printf("Tick %u. Proc %d sent frame: ", tick/DELTA, id);
	fr(s);
  }
}


void start_timer(seq_nr k)
{
/* Start a timer for a data frame. */

  ack_timer[k] = tick + timeout_interval + offset;
  offset++;
  recalc_timers();		/* figure out which timer is now lowest */
}


void stop_timer(seq_nr k)
{
/* Stop a data frame timer. */

  ack_timer[k] = 0;
  recalc_timers();		/* figure out which timer is now lowest */
}


void start_ack_timer(void)
{
/* Start the auxiliary timer for sending separate acks. The length of the
 * auxiliary timer is arbitrarily set to half the main timer.  This could
 * have been another simulation parameter, but that is unlikely to have
 * provided much extra insight.
 */

  aux_timer = tick + timeout_interval/AUX;
  offset++;
}


void stop_ack_timer(void)
{
/* Stop the ack timer. */

  aux_timer = 0;
}


void enable_network_layer(void)
{
/* Allow network_layer_ready events to occur. */

  network_layer_status = 1;
}


void disable_network_layer(void)
{
/* Prevent network_layer_ready events from occuring. */

  network_layer_status = 0;
}


int check_timers(void)
{
/* Check for possible timeout.  If found, reset the timer. */

  int i;

  /* See if a timeout event is even possible now. */
  if (lowest_timer == 0 || tick < lowest_timer) return(-1);

  /* A timeout event is possible.  Find the lowest timer. Note that it is
   * impossible for two frame timers to have the same value, so that when a
   * hit is found, it is the only possibility.  The use of the offset variable
   * guarantees that each successive timer set gets a higher value than the
   * previous one.
   */
  for (i = 0; i < NR_TIMERS; i++) {
	if (ack_timer[i] == lowest_timer) {
		ack_timer[i] = 0;	/* turn the timer off */
		recalc_timers();	/* find new lowest timer */
                oldest_frame = seqs[i];	/* for protocol 6 */
		return(i);
	}
  }
  printf("Impossible.  check_timers failed at %d\n", lowest_timer);
  exit(1);
}


int check_ack_timer()
{
/* See if the ack timer has expired. */

  if (aux_timer > 0 && tick >= aux_timer) {
	aux_timer = 0;
	return(1);
  } else {
	return(0);
  }
}


unsigned int pktnum(packet *p)
{
/* Extract packet number from packet. */

  unsigned int num, b0, b1, b2, b3;

  b0 = p->data[0] & BYTE;
  b1 = p->data[1] & BYTE;
  b2 = p->data[2] & BYTE;
  b3 = p->data[3] & BYTE;
  num = (b0 << 24) | (b1 << 16) | (b2 << 8) | b3;
  return(num);
}


void fr(frame *f)
{
/* Print frame information for tracing. */

  printf("type=%s  seq=%d  ack=%d  payload=%d\n",
	tag[f->kind], f->seq, f->ack, pktnum(&f->info));
}

void recalc_timers(void)
{
/* Find the lowest timer */

  int i;
  bigint t = UINT_MAX;

  for (i = 0; i < NR_TIMERS; i++) {
	if (ack_timer[i] > 0 && ack_timer[i] < t) t = ack_timer[i];
  }
  lowest_timer = t;
}


void print_statistics(void)
{
/* Display statistics. */

  int word[3];

  sleep(1);
  printf("\nProcess %d:\n", id);
  printf("\tTotal data frames sent:  %9d\n", data_sent);
  printf("\tData frames lost:        %9d\n", data_lost);
  printf("\tData frames not lost:    %9d\n", data_not_lost);
  printf("\tFrames retransmitted:    %9d\n", data_retransmitted);
  printf("\tGood ack frames rec'd:   %9d\n", good_acks_recd);
  printf("\tBad ack frames rec'd:    %9d\n\n", cksum_acks_recd);

  printf("\tGood data frames rec'd:  %9d\n", good_data_recd);
  printf("\tBad data frames rec'd:   %9d\n", cksum_data_recd);
  printf("\tPayloads accepted:       %9d\n", payloads_accepted);
  printf("\tTotal ack frames sent:   %9d\n", acks_sent);
  printf("\tAck frames lost:         %9d\n", acks_lost);
  printf("\tAck frames not lost:     %9d\n", acks_not_lost);

  printf("\tTimeouts:                %9d\n", timeouts);
  printf("\tAck timeouts:            %9d\n", ack_timeouts);
  fflush(stdin);

  word[0] = 0;
  word[1] = payloads_accepted;
  word[2] = data_sent;
  write(mwfd, word, 3*sizeof(int));	/* tell main we are done printing */
  sleep(1);
  exit(0);
}


void sim_error(char *s)
{
/* A simulator error has occurred. */

  int fd;

  printf("%s\n", s);
  fd = (id == 0 ? w4 : w6);
  write(fd, &zero, TICK_SIZE);
  exit(1);
}
