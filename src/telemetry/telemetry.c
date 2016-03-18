/*  
    pmacct (Promiscuous mode IP Accounting package)
    pmacct is Copyright (C) 2003-2016 by Paolo Lucente
*/

/*
    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
*/

/* defines */
#define __TELEMETRY_C

/* includes */
#include "pmacct.h"
#include "thread_pool.h"
#include "telemetry.h"
#if defined WITH_RABBITMQ
#include "amqp_common.h"
#endif
#ifdef WITH_KAFKA
#include "kafka_common.h"
#endif

/* variables to be exported away */
thread_pool_t *telemetry_pool;

/* Functions */
#if defined ENABLE_THREADS
void telemetry_wrapper()
{
  struct telemetry_data *t_data;

  /* initialize threads pool */
  telemetry_pool = allocate_thread_pool(1);
  assert(telemetry_pool);
  Log(LOG_DEBUG, "DEBUG ( %s/core/TELE ): %d thread(s) initialized\n", config.name, 1);

  t_data = malloc(sizeof(struct telemetry_data));
  if (!t_data) {
    Log(LOG_ERR, "ERROR ( %s/core/TELE ): malloc() struct telemetry_data failed. Terminating.\n", config.name);
    exit_all(1);
  }
  telemetry_prepare_thread(t_data);

  /* giving a kick to the telemetry thread */
  send_to_pool(telemetry_pool, telemetry_daemon, t_data);
}
#endif

void telemetry_daemon(void *t_data_void)
{
  struct telemetry_data *t_data = t_data_void;
  int slen, clen, ret, rc, peers_idx, allowed, yes=1, no=0;
  int peers_idx_rr = 0, max_peers_idx = 0, peers_num = 0;
  time_t now;

  telemetry_peer *peer = NULL;

  if (!t_data) {
    Log(LOG_ERR, "ERROR ( %s/%s ): telemetry_daemon(): missing telemetry data. Terminating.\n", config.name, t_data->log_str);
    exit_all(1);
  }

#if defined ENABLE_IPV6
  struct sockaddr_storage server, client;
#else
  struct sockaddr server, client;
#endif
  struct hosts_table allow;
  struct host_addr addr;

  /* select() stuff */
  fd_set read_descs, bkp_read_descs;
  int fd, select_fd, bkp_select_fd, recalc_fds, select_num;

  /* logdump time management */
  time_t dump_refresh_deadline;
  struct timeval dump_refresh_timeout, *drt_ptr;

  /* initial cleanups */
  reload_log_telemetry_thread = FALSE;
  memset(&server, 0, sizeof(server));
  memset(&client, 0, sizeof(client));
  memset(&allow, 0, sizeof(struct hosts_table));
  clen = sizeof(client);

  telemetry_misc_db = &inter_domain_misc_dbs[FUNC_TYPE_TELEMETRY];
  memset(telemetry_misc_db, 0, sizeof(telemetry_misc_structs));

  /* initialize variables */
  if (!config.telemetry_port) config.telemetry_port = TELEMETRY_TCP_PORT;

  /* socket creation for telemetry server: IPv4 only */
#if (defined ENABLE_IPV6)
  if (!config.telemetry_ip) {
    struct sockaddr_in6 *sa6 = (struct sockaddr_in6 *)&server;

    sa6->sin6_family = AF_INET6;
    sa6->sin6_port = htons(config.telemetry_port);
    slen = sizeof(struct sockaddr_in6);
  }
#else
  if (!config.telemetry_ip) {
    struct sockaddr_in *sa4 = (struct sockaddr_in *)&server;

    sa4->sin_family = AF_INET;
    sa4->sin_addr.s_addr = htonl(0);
    sa4->sin_port = htons(config.telemetry_port);
    slen = sizeof(struct sockaddr_in);
  }
#endif
  else {
    trim_spaces(config.telemetry_ip);
    ret = str_to_addr(config.telemetry_ip, &addr);
    if (!ret) {
      Log(LOG_ERR, "ERROR ( %s/%s ): 'telemetry_ip' value is not a valid IPv4/IPv6 address. Terminating.\n", config.name, t_data->log_str);
      exit_all(1);
    }
    slen = addr_to_sa((struct sockaddr *)&server, &addr, config.telemetry_port);
  }

  if (!config.telemetry_max_peers) config.telemetry_max_peers = TELEMETRY_MAX_PEERS_DEFAULT;
  Log(LOG_INFO, "INFO ( %s/%s ): maximum telemetry peers allowed: %d\n", config.name, t_data->log_str, config.telemetry_max_peers);

  telemetry_peers = malloc(config.telemetry_max_peers*sizeof(telemetry_peer));
  if (!telemetry_peers) {
    Log(LOG_ERR, "ERROR ( %s/%s ): Unable to malloc() telemetry peers structure. Terminating.\n", config.name, t_data->log_str);
    exit_all(1);
  }
  memset(telemetry_peers, 0, config.telemetry_max_peers*sizeof(telemetry_peer));

  if (config.telemetry_msglog_file || config.telemetry_msglog_amqp_routing_key || config.telemetry_msglog_kafka_topic) {
    if (config.telemetry_msglog_file) telemetry_misc_db->msglog_backend_methods++;
    if (config.telemetry_msglog_amqp_routing_key) telemetry_misc_db->msglog_backend_methods++;
    if (config.telemetry_msglog_kafka_topic) telemetry_misc_db->msglog_backend_methods++;

    if (telemetry_misc_db->msglog_backend_methods > 1) {
      Log(LOG_ERR, "ERROR ( %s/%s ): telemetry_daemon_msglog_file, telemetry_daemon_msglog_amqp_routing_key and telemetry_daemon_msglog_kafka_topic are mutually exclusive. Terminating.\n", config.name, t_data->log_str);
      exit_all(1);
    }
  }

  if (config.telemetry_dump_file || config.telemetry_dump_amqp_routing_key || config.telemetry_dump_kafka_topic) {
    if (config.telemetry_dump_file) telemetry_misc_db->dump_backend_methods++;
    if (config.telemetry_dump_amqp_routing_key) telemetry_misc_db->dump_backend_methods++;
    if (config.telemetry_dump_kafka_topic) telemetry_misc_db->dump_backend_methods++;

    if (telemetry_misc_db->dump_backend_methods > 1) {
      Log(LOG_ERR, "ERROR ( %s/%s ): telemetry_dump_file, telemetry_dump_amqp_routing_key and telemetry_dump_kafka_topic are mutually exclusive. Terminating.\n", config.name, t_data->log_str);
      exit_all(1);
    }
  }

  if (config.telemetry_dump_file || config.telemetry_dump_amqp_routing_key || config.telemetry_dump_kafka_topic) {
    if (config.telemetry_dump_file) telemetry_misc_db->dump_backend_methods++;
    if (config.telemetry_dump_amqp_routing_key) telemetry_misc_db->dump_backend_methods++;
    if (config.telemetry_dump_kafka_topic) telemetry_misc_db->dump_backend_methods++;

    if (telemetry_misc_db->dump_backend_methods > 1) {
      Log(LOG_ERR, "ERROR ( %s/%s ): telemetry_dump_file, telemetry_dump_amqp_routing_key and telemetry_dump_kafka_topic are mutually exclusive. Terminating.\n", config.name, t_data->log_str);
      exit_all(1);
    }
  }

  if (telemetry_misc_db->msglog_backend_methods) {
    telemetry_misc_db->peers_log = malloc(config.telemetry_max_peers*sizeof(telemetry_peer_log));
    if (!telemetry_misc_db->peers_log) {
      Log(LOG_ERR, "ERROR ( %s/%s ): Unable to malloc() telemetry peers log structure. Terminating.\n", config.name, t_data->log_str);
      exit_all(1);
    }
    memset(telemetry_misc_db->peers_log, 0, config.telemetry_max_peers*sizeof(telemetry_peer_log));
    telemetry_peer_log_seq_init(&telemetry_misc_db->log_seq);

    if (config.telemetry_msglog_amqp_routing_key) {
#ifdef WITH_RABBITMQ
      telemetry_daemon_msglog_init_amqp_host();
      p_amqp_connect_to_publish(&telemetry_daemon_msglog_amqp_host);

      if (!config.telemetry_msglog_amqp_retry)
        config.telemetry_msglog_amqp_retry = AMQP_DEFAULT_RETRY;
#else
      Log(LOG_WARNING, "WARN ( %s/%s ): p_amqp_connect_to_publish() not possible due to missing --enable-rabbitmq\n", config.name, t_data->log_str);
#endif
    }

    if (config.telemetry_msglog_kafka_topic) {
#ifdef WITH_KAFKA
      telemetry_daemon_msglog_init_kafka_host();
#else
      Log(LOG_WARNING, "WARN ( %s/%s ): p_kafka_connect_to_produce() not possible due to missing --enable-kafka\n", config.name, t_data->log_str);
#endif
    }
  }

  config.telemetry_sock = socket(((struct sockaddr *)&server)->sa_family, SOCK_STREAM, 0);
  if (config.telemetry_sock < 0) {
#if (defined ENABLE_IPV6)
    /* retry with IPv4 */
    if (!config.telemetry_ip) {
      struct sockaddr_in *sa4 = (struct sockaddr_in *)&server;

      sa4->sin_family = AF_INET;
      sa4->sin_addr.s_addr = htonl(0);
      sa4->sin_port = htons(config.telemetry_port);
      slen = sizeof(struct sockaddr_in);

      config.telemetry_sock = socket(((struct sockaddr *)&server)->sa_family, SOCK_STREAM, 0);
    }
#endif

    if (config.telemetry_sock < 0) {
      Log(LOG_ERR, "ERROR ( %s/%s ): socket() failed. Terminating.\n", config.name, t_data->log_str);
      exit_all(1);
    }
  }

  if (config.telemetry_ipprec) {
    int opt = config.telemetry_ipprec << 5;

    rc = setsockopt(config.telemetry_sock, IPPROTO_IP, IP_TOS, &opt, sizeof(opt));
    if (rc < 0) Log(LOG_ERR, "WARN ( %s/%s ): setsockopt() failed for IP_TOS (errno: %d).\n", config.name, t_data->log_str, errno);
  }

  rc = setsockopt(config.telemetry_sock, SOL_SOCKET, SO_REUSEADDR, (char *)&yes, sizeof(yes));
  if (rc < 0) Log(LOG_ERR, "WARN ( %s/%s ): setsockopt() failed for SO_REUSEADDR (errno: %d).\n", config.name, t_data->log_str, errno);

#if (defined ENABLE_IPV6) && (defined IPV6_BINDV6ONLY)
  rc = setsockopt(config.telemetry_sock, IPPROTO_IPV6, IPV6_BINDV6ONLY, (char *) &no, (socklen_t) sizeof(no));
  if (rc < 0) Log(LOG_ERR, "WARN ( %s/%s ): setsockopt() failed for IPV6_BINDV6ONLY (errno: %d).\n", config.name, t_data->log_str, errno);
#endif

  if (config.telemetry_pipe_size) {
    int l = sizeof(config.telemetry_pipe_size);
    int saved = 0, obtained = 0;

    getsockopt(config.telemetry_sock, SOL_SOCKET, SO_RCVBUF, &saved, &l);
    Setsocksize(config.telemetry_sock, SOL_SOCKET, SO_RCVBUF, &config.telemetry_pipe_size, sizeof(config.telemetry_pipe_size));
    getsockopt(config.telemetry_sock, SOL_SOCKET, SO_RCVBUF, &obtained, &l);

    Setsocksize(config.telemetry_sock, SOL_SOCKET, SO_RCVBUF, &saved, l);
    getsockopt(config.telemetry_sock, SOL_SOCKET, SO_RCVBUF, &obtained, &l);
    Log(LOG_INFO, "INFO ( %s/%s ): telemetry_daemon_pipe_size: obtained=%d target=%d.\n",
	config.name, t_data->log_str, obtained, config.telemetry_pipe_size);
  }

  rc = bind(config.telemetry_sock, (struct sockaddr *) &server, slen);
  if (rc < 0) {
    char null_ip_address[] = "0.0.0.0";
    char *ip_address;

    ip_address = config.telemetry_ip ? config.telemetry_ip : null_ip_address;
    Log(LOG_ERR, "ERROR ( %s/%s ): bind() to ip=%s port=%d/tcp failed (errno: %d).\n",
	config.name, t_data->log_str, ip_address, config.telemetry_port, errno);
    exit_all(1);
  }

  rc = listen(config.telemetry_sock, 1);
  if (rc < 0) {
    Log(LOG_ERR, "ERROR ( %s/%s ): listen() failed (errno: %d).\n", config.name, t_data->log_str, errno);
    exit_all(1);
  }

  /* Preparing for syncronous I/O multiplexing */
  select_fd = 0;
  FD_ZERO(&bkp_read_descs);
  FD_SET(config.telemetry_sock, &bkp_read_descs);

  {
    char srv_string[INET6_ADDRSTRLEN];
    struct host_addr srv_addr;
    u_int16_t srv_port;

    sa_to_addr(&server, &srv_addr, &srv_port);
    addr_to_str(srv_string, &srv_addr);
    Log(LOG_INFO, "INFO ( %s/%s ): waiting for telemetry data on %s:%u\n", config.name, t_data->log_str, srv_string, srv_port);
  }

  /* Preparing ACL, if any */
  if (config.telemetry_allow_file) load_allow_file(config.telemetry_allow_file, &allow);

  if (telemetry_misc_db->msglog_backend_methods) {
#ifdef WITH_JANSSON
    if (!config.telemetry_msglog_output) config.telemetry_msglog_output = PRINT_OUTPUT_JSON;
#else
    Log(LOG_WARNING, "WARN ( %s/%s ): telemetry_daemon_msglog_output set to json but will produce no output (missing --enable-jansson).\n", config.name, t_data->log_str);
#endif
  }

  if (telemetry_misc_db->dump_backend_methods) {
#ifdef WITH_JANSSON
    if (!config.telemetry_dump_output) config.telemetry_dump_output = PRINT_OUTPUT_JSON;
#else
    Log(LOG_WARNING, "WARN ( %s/%s ): telemetry_table_dump_output set to json but will produce no output (missing --enable-jansson).\n", config.name, t_data->log_str);
#endif
  }

  if (telemetry_misc_db->dump_backend_methods) {
    char dump_roundoff[] = "m";
    time_t tmp_time;

    if (config.telemetry_dump_refresh_time) {
      gettimeofday(&telemetry_misc_db->log_tstamp, NULL);
      dump_refresh_deadline = telemetry_misc_db->log_tstamp.tv_sec;
      tmp_time = roundoff_time(dump_refresh_deadline, dump_roundoff);
      while ((tmp_time+config.telemetry_dump_refresh_time) < dump_refresh_deadline) {
        tmp_time += config.telemetry_dump_refresh_time;
      }
      dump_refresh_deadline = tmp_time;
      dump_refresh_deadline += config.telemetry_dump_refresh_time; /* it's a deadline not a basetime */
    }
    else {
      config.telemetry_dump_file = NULL;
      telemetry_misc_db->dump_backend_methods = FALSE;
      Log(LOG_WARNING, "WARN ( %s/%s ): Invalid 'telemetry_dump_refresh_time'.\n", config.name, t_data->log_str);
    }

    if (config.telemetry_dump_amqp_routing_key) telemetry_dump_init_amqp_host();
    if (config.telemetry_dump_kafka_topic) telemetry_dump_init_kafka_host();
  }

  select_fd = bkp_select_fd = (config.telemetry_sock + 1);
  recalc_fds = FALSE;

  telemetry_link_misc_structs(telemetry_misc_db);

  for (;;) {
    select_again:

    if (recalc_fds) {
      select_fd = config.telemetry_sock;
      max_peers_idx = -1; /* .. since valid indexes include 0 */

      for (peers_idx = 0, peers_num = 0; peers_idx < config.telemetry_max_peers; peers_idx++) {
        if (select_fd < telemetry_peers[peers_idx].fd) select_fd = telemetry_peers[peers_idx].fd;
        if (telemetry_peers[peers_idx].fd) {
	  max_peers_idx = peers_idx;
	  peers_num++;
	}
      }
      select_fd++;
      max_peers_idx++;

      bkp_select_fd = select_fd;
      recalc_fds = FALSE;
    }
    else select_fd = bkp_select_fd;

    memcpy(&read_descs, &bkp_read_descs, sizeof(bkp_read_descs));
    if (telemetry_misc_db->dump_backend_methods) {
      int delta;

      calc_refresh_timeout_sec(dump_refresh_deadline, telemetry_misc_db->log_tstamp.tv_sec, &delta);
      dump_refresh_timeout.tv_sec = delta;
      dump_refresh_timeout.tv_usec = 0;
      drt_ptr = &dump_refresh_timeout;
    }
    else drt_ptr = NULL;

    select_num = select(select_fd, &read_descs, NULL, NULL, drt_ptr);
    if (select_num < 0) goto select_again;

    if (reload_log_telemetry_thread) {
      for (peers_idx = 0; peers_idx < config.telemetry_max_peers; peers_idx++) {
        if (telemetry_misc_db->peers_log[peers_idx].fd) {
          fclose(telemetry_misc_db->peers_log[peers_idx].fd);
          telemetry_misc_db->peers_log[peers_idx].fd = open_output_file(telemetry_misc_db->peers_log[peers_idx].filename, "a", FALSE);
          setlinebuf(telemetry_misc_db->peers_log[peers_idx].fd);
        }
        else break;
      }
    }

    if (telemetry_misc_db->msglog_backend_methods || telemetry_misc_db->dump_backend_methods) {
      gettimeofday(&telemetry_misc_db->log_tstamp, NULL);
      compose_timestamp(telemetry_misc_db->log_tstamp_str, SRVBUFLEN, &telemetry_misc_db->log_tstamp, TRUE, config.timestamps_since_epoch);

      if (telemetry_misc_db->dump_backend_methods) {
        while (telemetry_misc_db->log_tstamp.tv_sec > dump_refresh_deadline) {
          telemetry_handle_dump_event(t_data);
          dump_refresh_deadline += config.telemetry_dump_refresh_time;
        }
      }

#ifdef WITH_RABBITMQ
      if (config.telemetry_msglog_amqp_routing_key) {
        time_t last_fail = P_broker_timers_get_last_fail(&telemetry_daemon_msglog_amqp_host.btimers);

        if (last_fail && ((last_fail + P_broker_timers_get_retry_interval(&telemetry_daemon_msglog_amqp_host.btimers)) <= telemetry_misc_db->log_tstamp.tv_sec)) {
          telemetry_daemon_msglog_init_amqp_host();
          p_amqp_connect_to_publish(&telemetry_daemon_msglog_amqp_host);
        }
      }
#endif

#ifdef WITH_KAFKA
      if (config.telemetry_msglog_kafka_topic) {
        time_t last_fail = P_broker_timers_get_last_fail(&telemetry_daemon_msglog_kafka_host.btimers);

        if (last_fail && ((last_fail + P_broker_timers_get_retry_interval(&telemetry_daemon_msglog_kafka_host.btimers)) <= telemetry_misc_db->log_tstamp.tv_sec))
          telemetry_daemon_msglog_init_kafka_host();
      }
#endif
    }

    /* 
       If select_num == 0 then we got out of select() due to a timeout rather
       than because we had a message from a peeer to handle. By now we did all
       routine checks and can happily return to selet() again.
    */
    if (!select_num) goto select_again;

    /* New connection is coming in */
    if (FD_ISSET(config.telemetry_sock, &read_descs)) {
      int peers_check_idx;

      fd = accept(config.telemetry_sock, (struct sockaddr *) &client, &clen);
      if (fd == ERR) goto read_data;

#if defined ENABLE_IPV6
      ipv4_mapped_to_ipv4(&client);
#endif

      /* If an ACL is defined, here we check against and enforce it */
      if (allow.num) allowed = check_allow(&allow, (struct sockaddr *)&client);
      else allowed = TRUE;

      if (!allowed) {
        close(fd);
        goto read_data;
      }

      for (peer = NULL, peers_idx = 0; peers_idx < config.telemetry_max_peers; peers_idx++) {
        if (!telemetry_peers[peers_idx].fd) {
          now = time(NULL);
	  peer = &telemetry_peers[peers_idx];

	  if (telemetry_peer_init(peer, FUNC_TYPE_TELEMETRY)) peer = NULL;
	  else recalc_fds = TRUE;

	  break;
	}
      }

      if (!peer) {
        int fd;

        /* We briefly accept the new connection to be able to drop it */
        Log(LOG_ERR, "ERROR ( %s/%s ): Insufficient number of telemetry peers has been configured by 'telemetry_max_peers' (%d).\n",
                        config.name, t_data->log_str, config.telemetry_max_peers);
        close(fd);
        goto read_data;
      }

      peer->fd = fd;
      FD_SET(peer->fd, &bkp_read_descs);
      peer->addr.family = ((struct sockaddr *)&client)->sa_family;
      if (peer->addr.family == AF_INET) {
        peer->addr.address.ipv4.s_addr = ((struct sockaddr_in *)&client)->sin_addr.s_addr;
        peer->tcp_port = ntohs(((struct sockaddr_in *)&client)->sin_port);
      }
#if defined ENABLE_IPV6
      else if (peer->addr.family == AF_INET6) {
        memcpy(&peer->addr.address.ipv6, &((struct sockaddr_in6 *)&client)->sin6_addr, 16);
        peer->tcp_port = ntohs(((struct sockaddr_in6 *)&client)->sin6_port);
      }
#endif
      addr_to_str(peer->addr_str, &peer->addr);

      if (telemetry_misc_db->msglog_backend_methods)
        telemetry_peer_log_init(peer, config.telemetry_msglog_output, FUNC_TYPE_TELEMETRY);

      if (telemetry_misc_db->dump_backend_methods)
        telemetry_dump_init_peer(peer);

      peers_num++;
      Log(LOG_INFO, "INFO ( %s/%s ): [%s] telemetry peers usage: %u/%u\n",
	  config.name, t_data->log_str, peer->addr_str, peers_num, config.telemetry_max_peers);
    }

    read_data:

    /*
       We have something coming in: let's lookup which peer is that.
       FvD: To avoid starvation of the "later established" peers, we
       offset the start of the search in a round-robin style.
    */
    for (peer = NULL, peers_idx = 0; peers_idx < max_peers_idx; peers_idx++) {
      int loc_idx = (peers_idx + peers_idx_rr) % max_peers_idx;

      if (telemetry_peers[loc_idx].fd && FD_ISSET(telemetry_peers[loc_idx].fd, &read_descs)) {
        peer = &telemetry_peers[loc_idx];
        peers_idx_rr = (peers_idx_rr + 1) % max_peers_idx;
        break;
      }
    }

    if (!peer) goto select_again;

    ret = recv(peer->fd, &peer->buf.base[peer->buf.truncated_len], (peer->buf.len - peer->buf.truncated_len), 0);
    peer->msglen = (ret + peer->buf.truncated_len);

    if (ret <= 0) {
      Log(LOG_INFO, "INFO ( %s/%s ): [%s] connection reset by peer (%d).\n", config.name, t_data->log_str, peer->addr_str, errno);
      FD_CLR(peer->fd, &bkp_read_descs);
      telemetry_peer_close(peer, FUNC_TYPE_TELEMETRY);
      recalc_fds = TRUE;
      goto select_again;
    }
    else {
      /* XXX: process/handle telemetry data */
    }
  }
}

void telemetry_prepare_thread(struct telemetry_data *t_data) 
{
  if (!t_data) return;

  memset(t_data, 0, sizeof(struct telemetry_data));
  t_data->is_thread = TRUE;
  t_data->log_str = malloc(strlen("core/TELE") + 1);
  strcpy(t_data->log_str, "core/TELE");
}

void telemetry_prepare_daemon(struct telemetry_data *t_data)   
{
  if (!t_data) return;

  memset(t_data, 0, sizeof(struct telemetry_data));
  t_data->is_thread = FALSE;
  t_data->log_str = malloc(strlen("core") + 1);
  strcpy(t_data->log_str, "core");
}

int telemetry_peer_init(telemetry_peer *peer, int type)
{
  return bgp_peer_init(peer, type);
}

void telemetry_peer_close(telemetry_peer *peer, int type)
{
  telemetry_misc_structs *tms;

  if (!peer) return;

  tms = bgp_select_misc_db(peer->type);

  if (!tms) return;
 
  if (tms->dump_file || tms->dump_amqp_routing_key || tms->dump_kafka_topic)
    bmp_dump_close_peer(peer);

  bgp_peer_close(peer, type);
}

void telemetry_peer_log_seq_init(u_int64_t *seq)
{
  bgp_peer_log_seq_init(seq);
}

int telemetry_peer_log_init(telemetry_peer *peer, int output, int type)
{
  return bgp_peer_log_init(peer, output, type);
}

void telemetry_dump_init_peer(telemetry_peer *peer)
{
  bmp_dump_init_peer(peer);
}

void telemetry_handle_dump_event(struct telemetry_data *t_data)
{
  telemetry_misc_structs *tms = bgp_select_misc_db(FUNC_TYPE_TELEMETRY);
  char current_filename[SRVBUFLEN], last_filename[SRVBUFLEN], tmpbuf[SRVBUFLEN];
  char latest_filename[SRVBUFLEN], event_type[] = "dump", *fd_buf = NULL;
  int ret, peers_idx, duration, tables_num;
  pid_t dumper_pid;
  time_t start;
  u_int64_t dump_elems;

  telemetry_peer *peer, *saved_peer;
  telemetry_dump_se_ll *tdsell;
  telemetry_peer_log peer_log;

  /* pre-flight check */
  if (!tms->dump_backend_methods || !config.telemetry_dump_refresh_time)
    return;

  switch (ret = fork()) {
  case 0: /* Child */
    /* we have to ignore signals to avoid loops: because we are already forked */
    signal(SIGINT, SIG_IGN);
    signal(SIGHUP, SIG_IGN);
    pm_setproctitle("%s %s [%s]", config.type, "Core Process -- Telemetry Dump Writer", config.name);

    memset(last_filename, 0, sizeof(last_filename));
    memset(current_filename, 0, sizeof(current_filename));
    fd_buf = malloc(OUTPUT_FILE_BUFSZ);

#ifdef WITH_RABBITMQ
    if (config.telemetry_dump_amqp_routing_key) {
      telemetry_dump_init_amqp_host();
      ret = p_amqp_connect_to_publish(&telemetry_dump_amqp_host);
      if (ret) exit(ret);
    }
#endif

#ifdef WITH_KAFKA
    if (config.telemetry_dump_kafka_topic) {
      ret = telemetry_dump_init_kafka_host();
      if (ret) exit(ret);
    }
#endif

    dumper_pid = getpid();
    Log(LOG_INFO, "INFO ( %s/%s ): *** Dumping telemetry data - START (PID: %u) ***\n", config.name, t_data->log_str, dumper_pid);
    start = time(NULL);
    tables_num = 0;

    for (peer = NULL, saved_peer = NULL, peers_idx = 0; peers_idx < config.telemetry_max_peers; peers_idx++) {
      if (telemetry_peers[peers_idx].fd) {
        peer = &telemetry_peers[peers_idx];
        peer->log = &peer_log; /* abusing struct bgp_peer a bit, but we are in a child */
        tdsell = peer->bmp_se;

        if (config.telemetry_dump_file) bgp_peer_log_dynname(current_filename, SRVBUFLEN, config.telemetry_dump_file, peer);
        if (config.telemetry_dump_amqp_routing_key) bgp_peer_log_dynname(current_filename, SRVBUFLEN, config.telemetry_dump_amqp_routing_key, peer);
        if (config.telemetry_dump_kafka_topic) bgp_peer_log_dynname(current_filename, SRVBUFLEN, config.telemetry_dump_kafka_topic, peer);

        strftime_same(current_filename, SRVBUFLEN, tmpbuf, &tms->log_tstamp.tv_sec);

        /*
          we close last_filename and open current_filename in case they differ;
          we are safe with this approach until $peer_src_ip is the only variable
          supported as part of telemetry_dump_file configuration directive.
        */
        if (config.telemetry_dump_file) {
          if (strcmp(last_filename, current_filename)) {
            if (saved_peer && saved_peer->log && strlen(last_filename)) {
              close_output_file(saved_peer->log->fd);

              if (config.telemetry_dump_latest_file) {
                bgp_peer_log_dynname(latest_filename, SRVBUFLEN, config.telemetry_dump_latest_file, saved_peer);
                link_latest_output_file(latest_filename, last_filename);
              }
            }
            peer->log->fd = open_output_file(current_filename, "w", TRUE);
            if (fd_buf) {
              if (setvbuf(peer->log->fd, fd_buf, _IOFBF, OUTPUT_FILE_BUFSZ))
                Log(LOG_WARNING, "WARN ( %s/%s ): [%s] setvbuf() failed: %s\n", config.name, t_data->log_str, current_filename, errno);
              else memset(fd_buf, 0, OUTPUT_FILE_BUFSZ);
            }
          }
        }

        /*
          a bit pedantic maybe but should come at little cost and emulating
          telemetry_dump_file behaviour will work
        */
#ifdef WITH_RABBITMQ
        if (config.telemetry_dump_amqp_routing_key) {
          peer->log->amqp_host = &telemetry_dump_amqp_host;
          strcpy(peer->log->filename, current_filename);
        }
#endif

#ifdef WITH_KAFKA
        if (config.telemetry_dump_kafka_topic) {
          peer->log->kafka_host = &telemetry_dump_kafka_host;
          strcpy(peer->log->filename, current_filename);
        }
#endif

        bgp_peer_dump_init(peer, config.telemetry_dump_output, FUNC_TYPE_TELEMETRY);
        dump_elems = 0;

	if (tdsell && tdsell->start) {
          telemetry_dump_se_ll_elem *se_ll_elem;
          char event_type[] = "dump";

	  // XXX: dump actual data 
	}

        saved_peer = peer;
        strlcpy(last_filename, current_filename, SRVBUFLEN);
        bgp_peer_dump_close(peer, NULL, config.telemetry_dump_output, FUNC_TYPE_TELEMETRY);
        tables_num++;
      }
    }

#ifdef WITH_RABBITMQ
    if (config.telemetry_dump_amqp_routing_key)
      p_amqp_close(&telemetry_dump_amqp_host, FALSE);
#endif

#ifdef WITH_KAFKA
    if (config.telemetry_dump_kafka_topic)
      p_kafka_close(&telemetry_dump_kafka_host, FALSE);
#endif

    if (config.telemetry_dump_latest_file && peer) {
      bgp_peer_log_dynname(latest_filename, SRVBUFLEN, config.telemetry_dump_latest_file, peer);
      link_latest_output_file(latest_filename, last_filename);
    }

    duration = time(NULL)-start;
    Log(LOG_INFO, "INFO ( %s/%s ): *** Dumping telemetry data - END (PID: %u, PEERS: %u ET: %u) ***\n",
                config.name, t_data->log_str, dumper_pid, tables_num, duration);

    exit(0);
  default: /* Parent */
    if (ret == -1) { /* Something went wrong */
      Log(LOG_WARNING, "WARN ( %s/%s ): Unable to fork telemetry dump writer: %s\n", config.name, t_data->log_str, strerror(errno));
    }

    /* destroy bmp_se linked-list content after dump event */
    for (peer = NULL, peers_idx = 0; peers_idx < config.telemetry_max_peers; peers_idx++) {
      if (telemetry_peers[peers_idx].fd) {
        peer = &telemetry_peers[peers_idx];
        tdsell = peer->bmp_se;

        if (tdsell && tdsell->start) bmp_dump_se_ll_destroy(tdsell);
      }
    }

    break;
  }
}

#if defined WITH_RABBITMQ
void telemetry_daemon_msglog_init_amqp_host()
{
  p_amqp_init_host(&telemetry_daemon_msglog_amqp_host);

  if (!config.telemetry_msglog_amqp_user) config.telemetry_msglog_amqp_user = rabbitmq_user;
  if (!config.telemetry_msglog_amqp_passwd) config.telemetry_msglog_amqp_passwd = rabbitmq_pwd;
  if (!config.telemetry_msglog_amqp_exchange) config.telemetry_msglog_amqp_exchange = default_amqp_exchange;
  if (!config.telemetry_msglog_amqp_exchange_type) config.telemetry_msglog_amqp_exchange_type = default_amqp_exchange_type;
  if (!config.telemetry_msglog_amqp_host) config.telemetry_msglog_amqp_host = default_amqp_host;
  if (!config.telemetry_msglog_amqp_vhost) config.telemetry_msglog_amqp_vhost = default_amqp_vhost;
  if (!config.telemetry_msglog_amqp_retry) config.telemetry_msglog_amqp_retry = AMQP_DEFAULT_RETRY;

  p_amqp_set_user(&telemetry_daemon_msglog_amqp_host, config.telemetry_msglog_amqp_user);
  p_amqp_set_passwd(&telemetry_daemon_msglog_amqp_host, config.telemetry_msglog_amqp_passwd);
  p_amqp_set_exchange(&telemetry_daemon_msglog_amqp_host, config.telemetry_msglog_amqp_exchange);
  p_amqp_set_exchange_type(&telemetry_daemon_msglog_amqp_host, config.telemetry_msglog_amqp_exchange_type);
  p_amqp_set_host(&telemetry_daemon_msglog_amqp_host, config.telemetry_msglog_amqp_host);
  p_amqp_set_vhost(&telemetry_daemon_msglog_amqp_host, config.telemetry_msglog_amqp_vhost);
  p_amqp_set_persistent_msg(&telemetry_daemon_msglog_amqp_host, config.telemetry_msglog_amqp_persistent_msg);
  p_amqp_set_frame_max(&telemetry_daemon_msglog_amqp_host, config.telemetry_msglog_amqp_frame_max);
  p_amqp_set_content_type_json(&telemetry_daemon_msglog_amqp_host);
  p_amqp_set_heartbeat_interval(&telemetry_daemon_msglog_amqp_host, config.telemetry_msglog_amqp_heartbeat_interval);
  P_broker_timers_set_retry_interval(&telemetry_daemon_msglog_amqp_host.btimers, config.telemetry_msglog_amqp_retry);
}
#else
void telemetry_daemon_msglog_init_amqp_host()
{
}
#endif

#if defined WITH_RABBITMQ
void telemetry_dump_init_amqp_host()
{
  p_amqp_init_host(&telemetry_dump_amqp_host);

  if (!config.telemetry_dump_amqp_user) config.telemetry_dump_amqp_user = rabbitmq_user;
  if (!config.telemetry_dump_amqp_passwd) config.telemetry_dump_amqp_passwd = rabbitmq_pwd;
  if (!config.telemetry_dump_amqp_exchange) config.telemetry_dump_amqp_exchange = default_amqp_exchange;
  if (!config.telemetry_dump_amqp_exchange_type) config.telemetry_dump_amqp_exchange_type = default_amqp_exchange_type;
  if (!config.telemetry_dump_amqp_host) config.telemetry_dump_amqp_host = default_amqp_host;
  if (!config.telemetry_dump_amqp_vhost) config.telemetry_dump_amqp_vhost = default_amqp_vhost;

  p_amqp_set_user(&telemetry_dump_amqp_host, config.telemetry_dump_amqp_user);
  p_amqp_set_passwd(&telemetry_dump_amqp_host, config.telemetry_dump_amqp_passwd);
  p_amqp_set_exchange(&telemetry_dump_amqp_host, config.telemetry_dump_amqp_exchange);
  p_amqp_set_exchange_type(&telemetry_dump_amqp_host, config.telemetry_dump_amqp_exchange_type);
  p_amqp_set_host(&telemetry_dump_amqp_host, config.telemetry_dump_amqp_host);
  p_amqp_set_vhost(&telemetry_dump_amqp_host, config.telemetry_dump_amqp_vhost);
  p_amqp_set_persistent_msg(&telemetry_dump_amqp_host, config.telemetry_dump_amqp_persistent_msg);
  p_amqp_set_frame_max(&telemetry_dump_amqp_host, config.telemetry_dump_amqp_frame_max);
  p_amqp_set_content_type_json(&telemetry_dump_amqp_host);
  p_amqp_set_heartbeat_interval(&telemetry_dump_amqp_host, config.telemetry_dump_amqp_heartbeat_interval);
}
#else
void telemetry_dump_init_amqp_host()
{
}
#endif

#if defined WITH_KAFKA
int telemetry_daemon_msglog_init_kafka_host()
{
  int ret;

  p_kafka_init_host(&telemetry_daemon_msglog_kafka_host);
  ret = p_kafka_connect_to_produce(&telemetry_daemon_msglog_kafka_host);

  if (!config.telemetry_msglog_kafka_broker_host) config.telemetry_msglog_kafka_broker_host = default_kafka_broker_host;
  if (!config.telemetry_msglog_kafka_broker_port) config.telemetry_msglog_kafka_broker_port = default_kafka_broker_port;
  if (!config.telemetry_msglog_kafka_retry) config.telemetry_msglog_kafka_retry = PM_KAFKA_DEFAULT_RETRY;

  p_kafka_set_broker(&telemetry_daemon_msglog_kafka_host, config.telemetry_msglog_kafka_broker_host, config.telemetry_msglog_kafka_broker_port);
  p_kafka_set_topic(&telemetry_daemon_msglog_kafka_host, config.telemetry_msglog_kafka_topic);
  p_kafka_set_partition(&telemetry_daemon_msglog_kafka_host, config.telemetry_msglog_kafka_partition);
  p_kafka_set_content_type(&telemetry_daemon_msglog_kafka_host, PM_KAFKA_CNT_TYPE_STR);
  P_broker_timers_set_retry_interval(&telemetry_daemon_msglog_kafka_host.btimers, config.telemetry_msglog_kafka_retry);

  return ret;
}
#else
int telemetry_daemon_msglog_init_kafka_host()
{
  return ERR;
}
#endif

#if defined WITH_KAFKA
int telemetry_dump_init_kafka_host()
{
  int ret;

  p_kafka_init_host(&telemetry_dump_kafka_host);
  ret = p_kafka_connect_to_produce(&telemetry_dump_kafka_host);

  if (!config.telemetry_dump_kafka_broker_host) config.telemetry_dump_kafka_broker_host = default_kafka_broker_host;
  if (!config.telemetry_dump_kafka_broker_port) config.telemetry_dump_kafka_broker_port = default_kafka_broker_port;

  p_kafka_set_broker(&telemetry_dump_kafka_host, config.telemetry_dump_kafka_broker_host, config.telemetry_dump_kafka_broker_port);
  p_kafka_set_topic(&telemetry_dump_kafka_host, config.telemetry_dump_kafka_topic);
  p_kafka_set_partition(&telemetry_dump_kafka_host, config.telemetry_dump_kafka_partition);
  p_kafka_set_content_type(&telemetry_dump_kafka_host, PM_KAFKA_CNT_TYPE_STR);

  return ret;
}
#else
int telemetry_dump_init_kafka_host()
{
  return ERR;
}
#endif

void telemetry_link_misc_structs(telemetry_misc_structs *tms)
{
#if defined WITH_RABBITMQ
  tms->msglog_amqp_host = &telemetry_daemon_msglog_amqp_host;
#endif
#if defined WITH_KAFKA
  tms->msglog_kafka_host = &telemetry_daemon_msglog_kafka_host;
#endif
  tms->max_peers = config.telemetry_max_peers;
  tms->dump_file = config.telemetry_dump_file;
  tms->dump_amqp_routing_key = config.telemetry_dump_amqp_routing_key;
  tms->dump_amqp_routing_key_rr = config.telemetry_dump_amqp_routing_key_rr;
  tms->dump_kafka_topic = config.telemetry_dump_kafka_topic;
  tms->dump_kafka_topic_rr = config.telemetry_dump_kafka_topic_rr;
  tms->msglog_file = config.telemetry_msglog_file;
  tms->msglog_output = config.telemetry_msglog_output;
  tms->msglog_amqp_routing_key = config.telemetry_msglog_amqp_routing_key;
  tms->msglog_amqp_routing_key_rr = config.telemetry_msglog_amqp_routing_key_rr;
  tms->msglog_kafka_topic = config.telemetry_msglog_kafka_topic;
  tms->msglog_kafka_topic_rr = config.telemetry_msglog_kafka_topic_rr;
  tms->peer_str = malloc(strlen("telemetry_node") + 1);
  strcpy(tms->peer_str, "telemetry_node");
  tms->log_thread_str = malloc(strlen("TELE") + 1);
  strcpy(tms->log_thread_str, "TELE");
}

void telemetry_dummy()
{
}