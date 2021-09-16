#include "tcp.h"
#include "tcp_timer.h"
#include "tcp_sock.h"
#include "log.h"

#include <stdio.h>
#include <unistd.h>
#include <pthread.h>

static struct list_head timer_list;

pthread_mutex_t timer_lock;

#ifndef max
#define max(x, y) ((x) > (y) ? (x) : (y))
#endif

// scan the timer_list, find the tcp sock which stays for at 2*MSL, release it
void tcp_scan_timer_list()
{
	// fprintf(stdout, "TODO: implement %s please.\n", __FUNCTION__);
	pthread_mutex_lock(&timer_lock);
	struct tcp_timer *tmr = NULL, *tmr_tmp = NULL;
	list_for_each_entry_safe(tmr, tmr_tmp, &timer_list, list)
	{
		struct tcp_sock *tsk = retranstimer_to_tcp_sock(tmr);
		tmr->timeout -= TCP_TIMER_SCAN_INTERVAL;
		if (tmr->timeout <= 0)
		{
			if (tmr->type == 0)
			{
				// Time wait timer
				tmr->enable = 0;
				list_delete_entry(&tmr->list);
				log(DEBUG, "Wait for 2*MSL, close connection");
				(timewait_to_tcp_sock(tmr))->state = TCP_CLOSED;
			}
			else if (tmr->type == 1)
			{
				// Retransmission timer
				// Every time a retransmission occurs, sshthresh should be updated.
				tsk->ssthresh = max(1, tsk->cwnd / 2);
				tsk->cwnd = 1;
				log_cwnd_update(tsk->cwnd, tsk->ssthresh);
				tsk->cong_state = loss;

				tmr->timeout = TCP_RETRANS_INTERVAL_INITIAL << tmr->enable;
				tmr->enable += 1;
				pthread_mutex_lock(&tsk->send_buf_lock);
				if (list_empty(&tsk->send_buf))
				{
					log(ERROR, "No unacked packet pended. Ignore.");
					list_delete_entry(&tmr->list);
					continue;
				}
				struct pended_packet *ppkt =
					list_entry(tsk->send_buf.next, struct pended_packet, list);
				pthread_mutex_unlock(&tsk->send_buf_lock);
				if (tmr->enable > 3)
				{
					log(ERROR, "Retransmission max retries.");
					u32 rel_seq = ppkt->seq - tsk->iss;
					log(DEBUG, "Relative seq=%u", rel_seq);
					tcp_send_control_packet(tsk, TCP_RST);
					tmr->enable = 0;
					list_delete_entry(&tmr->list);
					tsk->state = TCP_CLOSED;
				}
				else
				{
					// log(DEBUG, "Retransmitting packet.");
					// u32 rel_seq = ppkt->seq - tsk->iss;
					// log(DEBUG, "Relative seq=%u", rel_seq);
					char *packet = malloc(ppkt->len);
					if (packet == NULL)
					{
						log(ERROR, "Malloc failed during %s", __FUNCTION__);
						exit(-1);
					}
					memcpy(packet, ppkt->packet, ppkt->len);
					ip_send_packet(packet, ppkt->len);
				}
				// Reset congestion state to open after retransmissions
				tsk->cong_state = open;
			}
		}
	}
	pthread_mutex_unlock(&timer_lock);
}

// set the timewait timer of a tcp sock, by adding the timer into timer_list
void tcp_set_timewait_timer(struct tcp_sock *tsk)
{
	// fprintf(stdout, "TODO: implement %s please.\n", __FUNCTION__);
	pthread_mutex_lock(&timer_lock);
	struct tcp_timer *tmr = &tsk->timewait;
	if (tsk->state == TCP_TIME_WAIT)
	{
		tmr->type = 0;
		tmr->enable = 1;
		tmr->timeout = TCP_TIMEWAIT_TIMEOUT;
		list_add_head(&tmr->list, &timer_list);
	}
	pthread_mutex_unlock(&timer_lock);
}

// scan the timer_list periodically by calling tcp_scan_timer_list
void *tcp_timer_thread(void *arg)
{
	init_list_head(&timer_list);
	pthread_mutex_init(&timer_lock, NULL);
	while (1)
	{
		usleep(TCP_TIMER_SCAN_INTERVAL);
		tcp_scan_timer_list();
	}

	return NULL;
}

// Set retrans timer of a tcp sock, by adding the timer into timer_list
void tcp_set_retrans_timer(struct tcp_sock *tsk)
{
	pthread_mutex_lock(&timer_lock);
	if (tsk->retrans_timer.enable == 0)
	{
		struct tcp_timer *tmr = &tsk->retrans_timer;
		tmr->type = 1;
		tmr->enable = 1;
		tmr->timeout = TCP_RETRANS_INTERVAL_INITIAL;
		list_add_head(&tmr->list, &timer_list);
	}
	pthread_mutex_unlock(&timer_lock);
}

void tcp_unset_retrans_timer(struct tcp_sock *tsk)
{
	pthread_mutex_lock(&timer_lock);
	struct tcp_timer *tmr = NULL, *tmp_tmr = NULL;
	list_for_each_entry_safe(tmr, tmp_tmr, &timer_list, list)
	{
		if (retranstimer_to_tcp_sock(tmr) == tsk)
		{
			list_delete_entry(&tmr->list);
			tmr->enable = 0;
		}
	}
	pthread_mutex_unlock(&timer_lock);
}

// Clear acked packets out of send_buf and update the retransmission timer
void tcp_update_retrans_timer(struct tcp_sock *tsk, u32 ack)
{
	pthread_mutex_lock(&timer_lock);
	pthread_mutex_lock(&tsk->send_buf_lock);

	struct pended_packet *ppkt = NULL, *tmp_ppkt = NULL;
	list_for_each_entry_safe(ppkt, tmp_ppkt, &tsk->send_buf, list)
	{
		if (ppkt->seq_end <= ack)
		{
			// log(DEBUG, "Removed acked packet.");
			// u32 rel_seq = ppkt->seq - tsk->iss;
			// log(DEBUG, "Relative seq=%u", rel_seq);
			list_delete_entry(&ppkt->list);
			free(ppkt->packet);
			free(ppkt);
			tsk->retrans_timer.enable = 1;
		}
	}

	if (tsk->retrans_timer.enable && list_empty(&tsk->send_buf))
	{
		// log(DEBUG, "All pended packet(s) acked, unset retransmission timer.");
		list_delete_entry(&tsk->retrans_timer.list);
		tsk->retrans_timer.enable = 0;
	}

	pthread_mutex_unlock(&tsk->send_buf_lock);
	pthread_mutex_unlock(&timer_lock);
}
