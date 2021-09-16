#include "tcp.h"
#include "tcp_sock.h"
#include "tcp_timer.h"

#include "log.h"
#include "ring_buffer.h"

#include <stdlib.h>
// update the snd_wnd of tcp_sock
//
// if the snd_wnd before updating is zero, notify tcp_sock_send (wait_send)
static inline void tcp_update_window(struct tcp_sock *tsk, struct tcp_cb *cb)
{
	u16 old_snd_wnd = tsk->snd_wnd;
	// tsk->snd_wnd = cb->rwnd;
	tsk->snd_wnd = min(cb->rwnd, tsk->cwnd * TCP_DEFAULT_MSS);
	if (old_snd_wnd == 0)
		wake_up(tsk->wait_send);
}

// update the snd_wnd safely: cb->ack should be between snd_una and snd_nxt
static inline void tcp_update_window_safe(struct tcp_sock *tsk, struct tcp_cb *cb)
{
	if (less_or_equal_32b(tsk->snd_una, cb->ack) && less_or_equal_32b(cb->ack, tsk->snd_nxt))
		tcp_update_window(tsk, cb);
	else
		log(ERROR, "Update window error!");
}

#ifndef max
#define max(x, y) ((x) > (y) ? (x) : (y))
#endif

// check whether the sequence number of the incoming packet is in the receiving
// window
static inline int is_tcp_seq_valid(struct tcp_sock *tsk, struct tcp_cb *cb)
{
	u32 rcv_end = tsk->rcv_nxt + max(tsk->rcv_wnd, 1);
	if (less_than_32b(cb->seq, rcv_end) && less_or_equal_32b(tsk->rcv_nxt, cb->seq_end))
	{
		return 1;
	}
	else
	{
		log(ERROR, "received packet with invalid seq, drop it.");
		return 0;
	}
}

// Process the incoming packet according to TCP state machine.
void tcp_process(struct tcp_sock *tsk, struct tcp_cb *cb, char *packet)
{
	// fprintf(stdout, "TODO: implement %s please.\n", __FUNCTION__);
	if (tsk == NULL)
	{
		log(ERROR, "Socket lookup fail while process tcp socket");
		return;
	}

	if (cb->flags & TCP_RST)
	{
		tsk->state = TCP_CLOSED;
		return;
	}

	// Remember only to update cwnd in established mode

	if (cb->flags & TCP_ACK)
	{
		tcp_update_retrans_timer(tsk, cb->ack);
	}

	if (tsk->state == TCP_SYN_SENT)
	{
		// Receive SYN/ACK from a server in a initiative connection establishment
		if (cb->flags & TCP_SYN)
		{
			tsk->rcv_nxt = cb->seq_end;
			tsk->snd_una = max(tsk->snd_una, cb->ack);
		}
		if (cb->flags & (TCP_SYN | TCP_ACK))
		{
			log(DEBUG, "Connection established");
			tcp_send_control_packet(tsk, TCP_ACK);
			tsk->state = TCP_ESTABLISHED;
			tsk->rcv_buf = alloc_ring_buffer(tsk->rcv_wnd);
			pthread_mutex_init(&tsk->rcv_buf_lock, NULL);
			wake_up(tsk->wait_connect);
		}
	}
	if (tsk->state == TCP_LISTEN)
	{
		if (cb->flags & (TCP_SYN))
		{
			// Receive SYN from a client in a passive connection establishment
			// Create a child socket and put it into listen_queue
			log(DEBUG, "Received TCP_SYN in listen state");
			struct tcp_sock *csk = alloc_tcp_sock();
			csk->state = TCP_SYN_RECV;
			csk->parent = tsk;
			csk->sk_sip = cb->daddr;
			csk->sk_sport = cb->dport;
			csk->sk_dip = cb->saddr;
			csk->sk_dport = cb->sport;
			csk->rcv_nxt = cb->seq_end;
			csk->snd_una = max(csk->snd_una, cb->ack);
			csk->snd_wnd = tsk->snd_wnd;
			list_add_head(&csk->list, &tsk->listen_queue);
			csk->ref_cnt += 1;

			tcp_send_control_packet(csk, TCP_ACK | TCP_SYN);
		}
	}
	if (tsk->state == TCP_LISTEN)
	{
		if (cb->flags & (TCP_ACK))
		{
			// Receive the last ACK from a client in a passive connection establishment
			struct tcp_sock *csk = NULL;
			if (list_empty(&tsk->listen_queue))
			{
				log(ERROR, "No waiting client socket for last ACK in handshaking");
				return;
			}
			log(DEBUG, "Connection established");
			csk = list_entry((&tsk->listen_queue)->next, struct tcp_sock, list);
			list_delete_entry(&csk->list);
			csk->state = TCP_ESTABLISHED;
			list_add_head(&csk->list, &tsk->accept_queue);
			tcp_hash(csk);
			csk->rcv_buf = alloc_ring_buffer(csk->rcv_wnd);
			pthread_mutex_init(&csk->rcv_buf_lock, NULL);
			wake_up(tsk->wait_accept);
		}
	}

	if (tsk->state == TCP_LAST_ACK)
	{
		if (cb->flags == TCP_ACK)
		{
			log(DEBUG, "Received last TCP_ACK, close connection");
			tsk->state = TCP_CLOSED;
		}
	}
	if (tsk->state == TCP_FIN_WAIT_1)
	{
		if (cb->flags & TCP_ACK)
		{
			log(DEBUG, "Receied TCP_ACK in state TCP_FIN_WATI_1, switched to TCP_FIN_WAIT_2");
			tsk->state = TCP_FIN_WAIT_2;
		}
	}
	if (tsk->state == TCP_FIN_WAIT_2)
	{
		if (cb->flags & TCP_FIN)
		{
			tsk->state = TCP_TIME_WAIT;
			tsk->rcv_nxt = cb->seq_end;
			tsk->snd_una = max(tsk->snd_una, cb->ack);
			tcp_send_control_packet(tsk, TCP_ACK);
			log(DEBUG, "Switched to TIME_WAIT state");
			tcp_set_timewait_timer(tsk);
		}
	}
	if (tsk->state == TCP_ESTABLISHED)
	{
		tcp_update_window_safe(tsk, cb);
		// Congestion management
		if (cb->flags & TCP_ACK)
		{
			if (tsk->cong_state == open)
			{
				if (tsk->cwnd < tsk->ssthresh)
				{
					tsk->cwnd += 1;
					log_cwnd_update(tsk->cwnd, tsk->ssthresh);
				}
				else
				{
					tsk->cong_avoid_ack += cb->ack - tsk->snd_una;
					if (tsk->cong_avoid_ack >= tsk->cwnd * TCP_DEFAULT_MSS)
					{
						tsk->cong_avoid_ack = 0;
						tsk->cwnd += 1;
						log_cwnd_update(tsk->cwnd, tsk->ssthresh);
					}
				}
				if (cb->ack == tsk->snd_una)
				{
					tsk->dup_ack += 1;
				}
				if (tsk->dup_ack >= 3)
				{
					tsk->ssthresh = max(1, tsk->cwnd / 2);
					tsk->cwnd = tsk->ssthresh;
					tsk->dup_ack = 0;
					log_cwnd_update(tsk->cwnd, tsk->ssthresh);
					tsk->cong_state = fast_recovery;
					// log(DEBUG, "Fast recovery. Current cwnd=%u", tsk->cwnd);
					tsk->recovery_point = tsk->snd_nxt;
				}
			}
			else if (tsk->cong_state == fast_recovery)
			{
				// Fast recovery
				if (cb->ack == tsk->snd_una)
				{
					tsk->cwnd += 1;
					log_cwnd_update(tsk->cwnd, tsk->ssthresh);
				}
				else if (cb->ack > tsk->snd_una && cb->ack < tsk->recovery_point)
				{
					// Partial ack. Retransmission
					struct pended_packet *ppkt = NULL;
					list_for_each_entry(ppkt, &tsk->send_buf, list)
					{
						if (ppkt->seq == cb->ack)
						{
							char *packet = (char *)malloc(ppkt->len);
							if (packet == NULL)
							{
								log(ERROR, "Malloc failed during %s", __FUNCTION__);
								exit(-1);
							}
							memcpy(packet, ppkt->packet, ppkt->len);
							ip_send_packet(packet, ppkt->len);
						}
					}
				}
				else if (cb->ack == tsk->recovery_point)
				{
					// Full ack
					tsk->cong_state = open;
					// log(DEBUG, "Congestion state: open");
				}
			}
		}
		// Receiving possibly out-of-order packets
		if (tsk->rcv_nxt == cb->seq)
		{
			// in-order receive

			tsk->rcv_nxt = cb->seq_end;
			tsk->rcv_wnd -= cb->pl_len;

			if (cb->flags & TCP_FIN)
			{
				tsk->state = TCP_CLOSE_WAIT;
				log(DEBUG, "Passively close connection");
				// tcp_send_control_packet(tsk, TCP_ACK);
			}
			if (cb->flags & TCP_ACK)
			{
				// Update ack
				tsk->snd_una = max(tsk->snd_una, cb->ack);

				char *data = cb->payload;
				int size = cb->pl_len;

				// You have to write buffer only if size > 0, cause there
				// might be empty ACK packet.
				if (size > 0)
				{
					pthread_mutex_lock(&tsk->rcv_buf_lock);
					write_ring_buffer(tsk->rcv_buf, data, size);
					pthread_mutex_unlock(&tsk->rcv_buf_lock);
				}
				// Check if there are already received following packets
				struct pended_packet *ppkt = NULL, *tmp_ppkt = NULL;
				list_for_each_entry_safe(ppkt, tmp_ppkt, &tsk->rcv_ofo_buf, list)
				{
					if (ppkt->seq == tsk->rcv_nxt)
					{
						// log(DEBUG, "Removing sequential packet into ring buffer. %u-%d", cb->seq, ppkt->seq);

						struct iphdr *ip = (struct iphdr *)packet_to_ip_hdr(ppkt->packet);
						struct tcphdr *tcp = (struct tcphdr *)((char *)ip + IP_HDR_SIZE(ip));
						size = (int)ntohs(ip->tot_len) - (int)IP_HDR_SIZE(ip) - (int)TCP_HDR_SIZE(tcp);
						data = (char *)tcp + tcp->off * 4;
						tsk->rcv_nxt = ppkt->seq_end;
						tsk->rcv_wnd -= size;
						if (size > 0)
						{
							pthread_mutex_lock(&tsk->rcv_buf_lock);
							write_ring_buffer(tsk->rcv_buf, data, size);
							pthread_mutex_unlock(&tsk->rcv_buf_lock);
						}
						list_delete_entry(&ppkt->list);
						free(ppkt->packet);
						free(ppkt);
					}
					else if (ppkt->seq < tsk->rcv_nxt)
					{
						log(ERROR, "Abnormal ofo queue.");
						list_delete_entry(&ppkt->list);
						free(ppkt->packet);
						free(ppkt);
					}
					else
					{
						break;
					}
				}

				// Woken wait won't be woken up again
				wake_up(tsk->wait_recv);
				// tcp_send_control_packet(tsk, TCP_ACK);
			}
		}
		else if (tsk->rcv_nxt < cb->seq && tsk->rcv_nxt + (u32)tsk->rcv_wnd - 1 > cb->seq_end)
		{
			// out of order receive
			if (cb->flags & TCP_ACK)
			{
				// log(DEBUG, "Received an out-of-order packet. Packet loss may occur.");
				struct pended_packet *ppkt = (struct pended_packet *)malloc(sizeof(struct pended_packet));
				if (ppkt == NULL)
				{
					log(ERROR, "Malloc failed during %s", __FUNCTION__);
					exit(-1);
				}
				struct iphdr *ip = packet_to_ip_hdr(packet);
				ppkt->len = (u32)ETHER_HDR_SIZE + (u32)ntohs(ip->tot_len);
				ppkt->seq = cb->seq;
				ppkt->seq_end = cb->seq_end;
				ppkt->packet = malloc(ppkt->len);
				if (ppkt->packet == NULL)
				{
					log(ERROR, "Malloc failed during %s", __FUNCTION__);
					exit(-1);
				}
				memcpy(ppkt->packet, packet, ppkt->len);
				// Insert this packet into out of order packet buffer
				struct pended_packet *true_head =
					list_entry(tsk->rcv_ofo_buf.next, struct pended_packet, list);
				struct pended_packet *true_tail =
					list_entry(tsk->rcv_ofo_buf.prev, struct pended_packet, list);
				if (list_empty(&tsk->rcv_ofo_buf))
				{
					list_add_head(&ppkt->list, &tsk->rcv_ofo_buf);
				}
				else if (true_head == true_tail)
				{
					if (ppkt->seq_end < true_head->seq)
					{
						list_add_head(&ppkt->list, &tsk->rcv_ofo_buf);
					}
					else if (ppkt->seq_end > true_head->seq)
					{
						list_add_tail(&ppkt->list, &tsk->rcv_ofo_buf);
					}
				}
				else
				{
					if (ppkt->seq < true_head->seq)
					{
						list_add_head(&ppkt->list, &tsk->rcv_ofo_buf);
					}
					else if (ppkt->seq > true_tail->seq)
					{
						list_add_tail(&ppkt->list, &tsk->rcv_ofo_buf);
					}
					else
					{
						struct pended_packet *cur =
							list_entry(true_head->list.next, struct pended_packet, list);
						struct pended_packet *nxt = NULL;
						while (cur != true_tail)
						{
							nxt = list_entry(cur->list.next, struct pended_packet, list);
							if (ppkt->seq == cur->seq || ppkt->seq == nxt->seq)
							{
								break;
							}
							if (ppkt->seq > cur->seq && ppkt->seq < nxt->seq)
							{
								list_insert(&ppkt->list, &cur->list, &nxt->list);
								break;
							}
							cur = nxt;
						}
					}
				}
			}
		}
		else
		{
			/*
			 * In this case, some out-of-order packets are successfully accepted.
			 * Others are out of receiving window so are dropped.
			 */
			// log(DEBUG, "Redundant retransmission for ofo packets.");
		}
		if (cb->pl_len || (cb->flags & (TCP_FIN | TCP_SYN)))
		{
			tcp_send_control_packet(tsk, TCP_ACK);
		}
	}
}
