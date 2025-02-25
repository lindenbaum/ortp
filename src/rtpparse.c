/*
 * Copyright (c) 2010-2019 Belledonne Communications SARL.
 *
 * This file is part of oRTP.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */


#ifdef HAVE_CONFIG_H
#include "ortp-config.h"
#endif

#include "ortp/ortp.h"
#include "jitterctl.h"
#include "utils.h"
#include "rtpsession_priv.h"
#include "congestiondetector.h"
#include "videobandwidthestimator.h"

static bool_t queue_packet(queue_t *q, int maxrqsz, mblk_t *mp, rtp_header_t *rtp, int *discarded, int *duplicate)
{
	mblk_t *tmp;
	int header_size;
	*discarded=0;
	*duplicate=0;
	header_size=RTP_FIXED_HEADER_SIZE+ (4*rtp->cc);
	if ((mp->b_wptr - mp->b_rptr)==header_size){
		ortp_debug("Rtp packet contains no data.");
		(*discarded)++;
		freemsg(mp);
		return FALSE;
	}

	/* and then add the packet to the queue */
	if (rtp_putq(q,mp) < 0) {
		/* It was a duplicate packet */
		(*duplicate)++;
	}

	/* make some checks: q size must not exceed RtpStream::max_rq_size */
	while (q->q_mcount > maxrqsz)
	{
		/* remove the oldest mblk_t */
		tmp=getq(q);

		ortp_warning("rtp_putq: Queue is full. Discarding message with ts=%u", rtp_get_timestamp(tmp));
		freemsg(tmp);
		(*discarded)++;
	}
	return TRUE;
}

static void compute_mean_and_deviation(uint32_t nb, double x, double *olds, double *oldm, double *news, double *newm) {
	*newm = *oldm + (x - *oldm) / nb;
	*news = *olds + ((x - *oldm) * (x - *newm));
	*oldm = *newm;
	*olds = *news;
}

static void update_rtcp_xr_stat_summary(RtpSession *session, mblk_t *mp, uint32_t local_str_ts) {
	int64_t diff = (int64_t)rtp_get_timestamp(mp) - (int64_t)local_str_ts;

	/* TTL/HL statistics */
	if (session->rtcp_xr_stats.rcv_since_last_stat_summary == 1) {
		session->rtcp_xr_stats.min_ttl_or_hl_since_last_stat_summary = 255;
		session->rtcp_xr_stats.max_ttl_or_hl_since_last_stat_summary = 0;
		session->rtcp_xr_stats.olds_ttl_or_hl_since_last_stat_summary = 0;
		session->rtcp_xr_stats.oldm_ttl_or_hl_since_last_stat_summary = mp->ttl_or_hl;
		session->rtcp_xr_stats.newm_ttl_or_hl_since_last_stat_summary = mp->ttl_or_hl;
	}
	compute_mean_and_deviation(session->rtcp_xr_stats.rcv_since_last_stat_summary,
		(double)mp->ttl_or_hl,
		&session->rtcp_xr_stats.olds_ttl_or_hl_since_last_stat_summary,
		&session->rtcp_xr_stats.oldm_ttl_or_hl_since_last_stat_summary,
		&session->rtcp_xr_stats.news_ttl_or_hl_since_last_stat_summary,
		&session->rtcp_xr_stats.newm_ttl_or_hl_since_last_stat_summary);
	if (mp->ttl_or_hl < session->rtcp_xr_stats.min_ttl_or_hl_since_last_stat_summary) {
		session->rtcp_xr_stats.min_ttl_or_hl_since_last_stat_summary = mp->ttl_or_hl;
	}
	if (mp->ttl_or_hl > session->rtcp_xr_stats.max_ttl_or_hl_since_last_stat_summary) {
		session->rtcp_xr_stats.max_ttl_or_hl_since_last_stat_summary = mp->ttl_or_hl;
	}

	/* Jitter statistics */
	if (session->rtcp_xr_stats.rcv_since_last_stat_summary == 1) {
		session->rtcp_xr_stats.min_jitter_since_last_stat_summary = 0xFFFFFFFF;
		session->rtcp_xr_stats.max_jitter_since_last_stat_summary = 0;
	} else {
		int64_t signed_jitter = diff - session->rtcp_xr_stats.last_jitter_diff_since_last_stat_summary;
		uint32_t jitter;
		if (signed_jitter < 0) {
			jitter = (uint32_t)(-signed_jitter);
		} else {
			jitter = (uint32_t)(signed_jitter);
		}
		compute_mean_and_deviation(session->rtcp_xr_stats.rcv_since_last_stat_summary - 1,
			(double)jitter,
			&session->rtcp_xr_stats.olds_jitter_since_last_stat_summary,
			&session->rtcp_xr_stats.oldm_jitter_since_last_stat_summary,
			&session->rtcp_xr_stats.news_jitter_since_last_stat_summary,
			&session->rtcp_xr_stats.newm_jitter_since_last_stat_summary);
		if (jitter < session->rtcp_xr_stats.min_jitter_since_last_stat_summary) {
			session->rtcp_xr_stats.min_jitter_since_last_stat_summary = jitter;
		}
		if (jitter > session->rtcp_xr_stats.max_jitter_since_last_stat_summary) {
			session->rtcp_xr_stats.max_jitter_since_last_stat_summary = jitter;
		}
	}
	session->rtcp_xr_stats.last_jitter_diff_since_last_stat_summary = diff;
}

static void check_for_seq_number_gap_immediate(RtpSession *session, rtp_header_t *rtp) {
	uint16_t pid;
	uint16_t i;
	uint16_t seq_number = rtp_header_get_seqnumber(rtp);

	/*don't check anything before first packet delivered*/
	if (session->flags & RTP_SESSION_FIRST_PACKET_DELIVERED
		&& RTP_SEQ_IS_STRICTLY_GREATER_THAN(seq_number, session->rtp.rcv_last_seq + 1)
		&& RTP_SEQ_IS_STRICTLY_GREATER_THAN(seq_number, session->rtp.snd_last_nack + 1)
	) {
		uint16_t first_missed_seq = session->rtp.rcv_last_seq + 1;
		uint16_t diff;

		if (first_missed_seq <= session->rtp.snd_last_nack) {
			first_missed_seq = session->rtp.snd_last_nack + 1;
		}

		diff = seq_number - first_missed_seq;
		pid = first_missed_seq;

		for (i = 0; i <= (diff / 16); i++) {
			uint16_t seq;
			uint16_t blp = 0;
			for (seq = pid + 1; (seq < seq_number) && ((seq - pid) < 16); seq++) {
				blp |= (1 << (seq - pid - 1));
			}
			if (session->rtp.congdetect != NULL && session->rtp.congdetect->state == CongestionStateDetected) {
				/*
				* Do not send NACK in IMMEDIATE_NACK mode in congestion, because the retransmission by the other party of the missing packets
				* will necessarily increase or at least sustain the congestion.
				* Furthermore, due to the congestion, the retransmitted packets have very few chance to arrive in time.
				*/
				ortp_message("Immediate NACK not sent because of congestion.");
				return;
			}
			rtp_session_send_rtcp_fb_generic_nack(session, pid, blp);
			pid = seq;
		}
	}

	if (RTP_SEQ_IS_STRICTLY_GREATER_THAN(seq_number, session->rtp.snd_last_nack)) {
		/* We update the last_nack since we received this packet we don't need a nack for it */
		session->rtp.snd_last_nack = seq_number;
	}
}

void rtp_session_rtp_parse(RtpSession *session, mblk_t *mp, uint32_t local_str_ts, struct sockaddr *addr, socklen_t addrlen)
{
	int discarded;
	int duplicate;
	rtp_header_t *rtp;
	int msgsize;
	RtpStream *rtpstream=&session->rtp;
	rtp_stats_t *stats=&session->stats;
	uint16_t seq_number;
	uint32_t timestamp, ssrc;

	msgsize=(int)(mp->b_wptr-mp->b_rptr);

	if (msgsize<RTP_FIXED_HEADER_SIZE){
		ortp_warning("Packet too small to be a rtp packet (%i)!",msgsize);
		session->stats.bad++;
		ortp_global_stats.bad++;
		freemsg(mp);
		return;
	}
	rtp=(rtp_header_t*)mp->b_rptr;
	if (rtp->version!=2)
	{
		/* try to see if it is a STUN packet */
		uint16_t stunlen=*((uint16_t*)(mp->b_rptr + sizeof(uint16_t)));
		stunlen = ntohs(stunlen);
		if (stunlen+20==mp->b_wptr-mp->b_rptr){
			/* this looks like a stun packet */
			rtp_session_update_remote_sock_addr(session,mp,TRUE,TRUE);
			if (session->eventqs!=NULL){
				OrtpEvent *ev=ortp_event_new(ORTP_EVENT_STUN_PACKET_RECEIVED);
				OrtpEventData *ed=ortp_event_get_data(ev);
				ed->packet=mp;
				memcpy(&ed->source_addr,addr,addrlen);
				ed->source_addrlen=addrlen;
				ed->info.socket_type = OrtpRTPSocket;
				rtp_session_dispatch_event(session,ev);
				return;
			}
		}
		/* discard in two case: the packet is not stun OR nobody is interested by STUN (no eventqs) */
		ortp_debug("Receiving rtp packet with version number %d!=2...discarded", rtp->version);
		stats->bad++;
		ortp_global_stats.bad++;
		freemsg(mp);
		return;
	}

	/* only count non-stun packets. */
	ortp_global_stats.packet_recv++;
	stats->packet_recv++;
	ortp_global_stats.hw_recv+=msgsize;
	stats->hw_recv+=msgsize;
	session->rtp.hwrcv_since_last_SR++;
	session->rtcp_xr_stats.rcv_since_last_stat_summary++;

	/* convert all header data from network order to host order */
	seq_number = rtp_header_get_seqnumber(rtp);
	timestamp = rtp_header_get_timestamp(rtp);
	ssrc = rtp_header_get_ssrc(rtp);
	/* convert csrc if necessary */
	if (rtp->cc*sizeof(uint32_t) > (uint32_t) (msgsize-RTP_FIXED_HEADER_SIZE)){
		ortp_debug("Receiving too short rtp packet.");
		stats->bad++;
		ortp_global_stats.bad++;
		freemsg(mp);
		return;
	}

#ifndef PERF
	/* Write down the last RTP/RTCP packet reception time. */
	bctbx_gettimeofday(&session->last_recv_time, NULL);
#endif

	/*the goal of the following code is to lock on an incoming SSRC to avoid
	receiving "mixed streams"*/
	if (session->ssrc_set){
		/*the ssrc is set, so we must check it */
		if (session->rcv.ssrc!=ssrc){
			if (session->inc_ssrc_candidate==ssrc){
				session->inc_same_ssrc_count++;
			}else{
				session->inc_same_ssrc_count=0;
				session->inc_ssrc_candidate=ssrc;
			}
			if (session->inc_same_ssrc_count>=session->rtp.ssrc_changed_thres){
				/* store the sender rtp address to do symmetric RTP */
				rtp_session_update_remote_sock_addr(session,mp,TRUE,FALSE);
				session->rtp.rcv_last_ts = timestamp;
				session->rcv.ssrc=ssrc;
				rtp_signal_table_emit(&session->on_ssrc_changed);
			}else{
				/*discard the packet*/
				ortp_debug("Receiving packet with unknown ssrc.");
				stats->bad++;
				ortp_global_stats.bad++;
				freemsg(mp);
				return;
			}
		} else{
			/* The SSRC change must not happen if we still receive
			ssrc from the initial source. */
			session->inc_same_ssrc_count=0;
		}
	}else{
		session->ssrc_set=TRUE;
		session->rcv.ssrc=ssrc;
		rtp_session_update_remote_sock_addr(session,mp,TRUE,FALSE);
	}

	/* update some statistics */
	{
		poly32_t *extseq = (poly32_t*)&rtpstream->hwrcv_extseq;
		if (seq_number > extseq->split.lo) {
			extseq->split.lo = seq_number;
		} else if (seq_number < 200 && extseq->split.lo > ((1<<16) - 200)) {
			/* this is a check for sequence number looping */
			extseq->split.lo = seq_number;
			extseq->split.hi++;
		}

		/* the first sequence number received should be initialized at the beginning
		or at any resync, so that the first receiver reports contains valid loss rate*/
		if (!(session->flags & RTP_SESSION_RECV_SEQ_INIT)) {
			rtp_session_set_flag(session, RTP_SESSION_RECV_SEQ_INIT);
			rtpstream->hwrcv_seq_at_last_SR = seq_number - 1;
			session->rtcp_xr_stats.rcv_seq_at_last_stat_summary = seq_number - 1;
		}
		if (stats->packet_recv==1){
			session->rtcp_xr_stats.first_rcv_seq=extseq->one;
		}
		session->rtcp_xr_stats.last_rcv_seq=extseq->one;
	}

	/* check for possible telephone events */
	if (rtp_profile_is_telephone_event(session->snd.profile, rtp->paytype)){
		queue_packet(&session->rtp.tev_rq,session->rtp.jittctl.params.max_packets,mp,rtp,&discarded,&duplicate);
		stats->discarded+=discarded;
		ortp_global_stats.discarded+=discarded;
		stats->packet_dup_recv+=duplicate;
		ortp_global_stats.packet_dup_recv+=duplicate;
		session->rtcp_xr_stats.discarded_count += discarded;
		session->rtcp_xr_stats.dup_since_last_stat_summary += duplicate;
		return;
	}

	/* check for possible payload type change, in order to update accordingly our clock-rate dependant
	parameters */
	if (session->hw_recv_pt!=rtp->paytype){
		rtp_session_update_payload_type(session,rtp->paytype);
	}

	/* Drop the packets while the RTP_SESSION_FLUSH flag is set. */
	if (session->flags & RTP_SESSION_FLUSH) {
		freemsg(mp);
		return;
	}

	jitter_control_new_packet(&session->rtp.jittctl,timestamp,local_str_ts);

	if (session->video_bandwidth_estimator_enabled && session->rtp.video_bw_estimator) {
		int overhead = ortp_stream_is_ipv6(&session->rtp.gs) ? IP6_UDP_OVERHEAD : IP_UDP_OVERHEAD;
		ortp_video_bandwidth_estimator_process_packet(session->rtp.video_bw_estimator, timestamp, &mp->timestamp, msgsize + overhead, rtp->markbit == 1);
	}

	if (session->congestion_detector_enabled && session->rtp.congdetect){
		if (ortp_congestion_detector_record(session->rtp.congdetect,timestamp,local_str_ts)) {
			OrtpEvent *ev=ortp_event_new(ORTP_EVENT_CONGESTION_STATE_CHANGED);
			OrtpEventData *ed=ortp_event_get_data(ev);
			ed->info.congestion_detected = session->rtp.congdetect->state == CongestionStateDetected;
			rtp_session_dispatch_event(session,ev);
		}
	}

	update_rtcp_xr_stat_summary(session, mp, local_str_ts);

	if (session->flags & RTP_SESSION_FIRST_PACKET_DELIVERED) {
		/* detect timestamp important jumps in the future, to workaround stupid rtp senders */
		if (RTP_TIMESTAMP_IS_NEWER_THAN(timestamp,session->rtp.rcv_last_ts+session->rtp.ts_jump)){
			ortp_warning("rtp_parse: timestamp jump in the future detected.");
			rtp_signal_table_emit2(&session->on_timestamp_jump,&timestamp);
		}
		else if (RTP_TIMESTAMP_IS_STRICTLY_NEWER_THAN(session->rtp.rcv_last_ts,timestamp)
			|| RTP_SEQ_IS_STRICTLY_GREATER_THAN(session->rtp.rcv_last_seq, seq_number)) {
			/* don't queue packets older than the last returned packet to the application, or whose sequence number
			 is behind the last packet returned to the application*/
			/* Call timstamp jumb in case of
			 * large negative Ts jump or if ts is set to 0
			*/

			if ( RTP_TIMESTAMP_IS_STRICTLY_NEWER_THAN(session->rtp.rcv_last_ts, timestamp + session->rtp.ts_jump) ){
				ortp_warning("rtp_parse: negative timestamp jump detected");
				rtp_signal_table_emit2(&session->on_timestamp_jump, &timestamp);
			}
			ortp_error("rtp_parse: discarding too old packet (seq=%i, ts=%u, last_delivered was seq=%i, ts=%u)", seq_number, timestamp,
				(int)session->rtp.rcv_last_seq, session->rtp.rcv_last_ts);
			freemsg(mp);
			stats->outoftime++;
			ortp_global_stats.outoftime++;
			session->rtcp_xr_stats.discarded_count++;
			return;
		}
	}

	if ((rtp_session_avpf_enabled(session) == TRUE)
		&& (rtp_session_avpf_feature_enabled(session, ORTP_AVPF_FEATURE_GENERIC_NACK) == TRUE)
		&& (rtp_session_avpf_feature_enabled(session, ORTP_AVPF_FEATURE_IMMEDIATE_NACK) == TRUE)) {
		/*
		 * If immediate nack is enabled then we check for missing packets here instead of
		 * rtp_session_recvm_with_ts
		 */
		check_for_seq_number_gap_immediate(session, rtp);
	}

	if (queue_packet(&session->rtp.rq,session->rtp.jittctl.params.max_packets,mp,rtp,&discarded,&duplicate))
		jitter_control_update_size(&session->rtp.jittctl,&session->rtp.rq);
	stats->discarded+=discarded;
	ortp_global_stats.discarded+=discarded;
	stats->packet_dup_recv+=duplicate;
	ortp_global_stats.packet_dup_recv+=duplicate;
	session->rtcp_xr_stats.discarded_count += discarded;
	session->rtcp_xr_stats.dup_since_last_stat_summary += duplicate;
	if ((discarded == 0) && (duplicate == 0)) {
		session->rtcp_xr_stats.rcv_count++;
	}
	if(session->fec_stream != NULL)
		fec_stream_on_new_source_packet_received(session->fec_stream, mp);
}
