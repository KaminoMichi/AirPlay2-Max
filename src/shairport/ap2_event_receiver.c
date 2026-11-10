/*
 * Apple AirPlay 2 Event Receiver. This file is part of Shairport Sync.
 * Copyright (c) Mike Brady 2014--2026
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "ap2_event_receiver.h"
#include "volume.h"
#include "common.h"
#include "player.h"
#include "utilities/network_utilities.h"
#include "utilities/rtsp_message_utilities.h"
#include "utilities/structured_buffer.h"

#ifdef CONFIG_METADATA
#include "metadata/core.h"
#endif

// will return -1 if there is an error, the port is not open or the response is not a valid
// successful RTSP response, 0 if the port was closed and a positive number if okay
ssize_t ap2_event_port_send_message(rtsp_conn_info *conn, char *data, size_t data_length) {
  ssize_t result = -1; // assume a problem
  int bounded_volume=ya_io_get_deadline()!=0;
  int bounded_rejected=0;
  if(bounded_volume){int rc=pthread_mutex_trylock(&conn->event_sender_mutex);if(rc){errno=EBUSY;return -1;}}
  else pthread_mutex_lock(&conn->event_sender_mutex);
  pthread_cleanup_push(mutex_unlock, &conn->event_sender_mutex);
  if (conn->event_channel_fd > 0) {
    if(bounded_volume)ya_volume_mark_echo();
    result = write_encrypted(conn->event_channel_fd, &conn->ap2_pairing_context.event_cipher_bundle,
                             data, data_length);
    if ((result != -1) && ((size_t)result == data_length)) {
      debug(3, "Connection %d: Packet of %zu bytes successfully written on the Event Port.",
            conn->connection_number, result);
      uint8_t packet[4096 + 1];
      if(bounded_volume){
        // One complete RTSP response, including bounded optional body. The TLS
        // deadline covers all encrypted reads, including a slow-drip peer.
        size_t used=0;result=-1;
        while(used<sizeof(packet)-1){
          ssize_t got=read_encrypted(conn->event_channel_fd,&conn->ap2_pairing_context.event_cipher_bundle,packet+used,sizeof(packet)-1-used);
          if(got<=0)break;used+=(size_t)got;packet[used]=0;
          char *end=strstr((char*)packet,"\r\n\r\n");
          if(end){size_t header=(size_t)(end-(char*)packet)+4,body=0;int seen=0;
            for(char*q=strstr((char*)packet,"\r\n");q&&q<end;q=strstr(q+2,"\r\n")){
              if(strncasecmp(q+2,"Content-Length:",15)==0){
                char*tail;errno=0;unsigned long z=strtoul(q+17,&tail,10);
                if(seen++||errno||tail==q+17||tail[0]!='\r'||tail[1]!='\n'||z>2048){seen=-1;break;}body=(size_t)z;
              }
            }
            if(seen<0||header+body>=sizeof(packet))break;
            if(used==header+body){result=(ssize_t)used;break;}
            if(used>header+body)break;
          }
        }
      }else result =
          read_encrypted(conn->event_channel_fd, &conn->ap2_pairing_context.event_cipher_bundle,
                         packet, sizeof(packet) - 1);
      debug(3, "Connection %d: Packet of %zu bytes successfully read on the Event Port.",
            conn->connection_number, result);
      if (result > 0) {
        packet[result] = '\0';
        debug(3, "Connection %d: Packet Received on Event Port with contents: \n--\n%s\n--\n",
              conn->connection_number, packet);

        if ((result < 12) ||
            (((strncmp((char *)packet, "RTSP/1.0 ", 9) != 0) &&
              (strncmp((char *)packet, "RTSP/1.1 ", 9) != 0))) ||
            (packet[9] < '0') || (packet[9] > '9') || (packet[10] < '0') || (packet[10] > '9') ||
            (packet[11] < '0') || (packet[11] > '9') ||
            ((packet[12] != ' ') && (packet[12] != '\r') && (packet[12] != '\n') &&
             (packet[12] != '\0'))) {
          debug(1, "Connection %d: invalid RTSP response from the Event Port.",
                conn->connection_number);
          result = -1;
        } else {
          unsigned int response_code = ((unsigned int)(packet[9] - '0') * 100) +
                                       ((unsigned int)(packet[10] - '0') * 10) +
                                       (unsigned int)(packet[11] - '0');

          if ((response_code < 200) || (response_code >= 300)) {
            bounded_rejected=bounded_volume;
            debug(1, "Connection %d: Event Port returned RTSP status %u.", conn->connection_number,
                  response_code);
            result = -1;
          }
        }
      } else {
        debug(2, "Connection %d: Event Port connection closed by client", conn->connection_number);
      }
    } else {
      result = -1; // this covers a situation where the result is positive but not the same as the
                   // data_length
    }
  } else {
    if(bounded_volume)errno=EBUSY;
    debug(1, "Connection %d: attempt to send a command to the event port over a closed socket",
          conn->connection_number);
  }
  if(bounded_volume&&!bounded_rejected&&result<=0&&conn->event_channel_fd>0){
    shutdown(conn->event_channel_fd,SHUT_RDWR); // owner closes after joined cleanup
    pthread_cancel(conn->thread); // no retry with an advanced/partial cipher state
  }
  pthread_cleanup_pop(1); // unlock the mutex
  if(bounded_rejected)errno=EOPNOTSUPP;
  return result;
}

ssize_t ap2_event_port_post_command(rtsp_conn_info *conn, plist_t command) {
  ssize_t result = 0;
  decodeAndLogPlist(3, command);
  structured_buffer *sbuf = sbuf_new(4096);
  if (sbuf != NULL) {
    pthread_cleanup_push(sbuf_cleanup, sbuf);
    char *plistString = NULL;
    uint32_t plistStringLength = 0;

    plist_to_bin(command, &plistString, &plistStringLength);
    if (plistString != NULL) {
      sbuf_printf(sbuf, "POST /command RTSP/1.0\r\nContent-Length: %u\r\n", plistStringLength);
      sbuf_printf(sbuf, "Content-Type: application/x-apple-binary-plist\r\n\r\n");
      sbuf_append(sbuf, plistString, plistStringLength);
      free(plistString); // should be plist_to_bin_free, but it's not defined in older
                         // libraries
      char *b = 0;
      size_t l = 0;
      sbuf_buf_and_length(sbuf, &b, &l);
      result = ap2_event_port_send_message(conn, b, l);
      debug(3, "Connection %d: POST /command sent on the event port. Result is %zd.",
            conn->connection_number, result);
      sbuf_clear(sbuf);
    }
    pthread_cleanup_pop(1); // delete the structured buffer
  }
  return result;
}

ssize_t ap2_event_send_update_info(rtsp_conn_info *conn) {
  // sends the updateInfo plist on the event port
  ssize_t result = -1;
  plist_t value_plist = generateInfoPlist(conn);
  if (value_plist != NULL) {
    void *txtData = NULL;
    size_t txtDataLength = 0;
    generateTxtDataValueInfo(conn, &txtData, &txtDataLength);
    plist_dict_set_item(value_plist, "txtAirPlay", plist_new_data(txtData, txtDataLength));
    free(txtData);
    plist_t update_info_plist = plist_new_dict();
    if (update_info_plist != NULL) {
      plist_dict_set_item(update_info_plist, "type", plist_new_string("updateInfo"));
      plist_dict_set_item(update_info_plist, "value", value_plist);

      char *plist_as_string = plist_as_xml_text(update_info_plist);
      if (plist_as_string != NULL) {
        debug(3, "update_info_plist is:\n--\n\"%s\"\n--\n", plist_as_string);
        free(plist_as_string);
      }

      result = ap2_event_port_post_command(conn, update_info_plist);
      plist_free(update_info_plist);
    } else {
      debug(1, "Could not build an updateInfo plist");
    }
  } else {
    debug(1, "Could not build an updateInfo value plist");
  }
  return result;
}

void ap2_event_receiver_cleanup_handler(void *arg) {
  pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,NULL); // top-level worker cleanup

  rtsp_conn_info *conn = (rtsp_conn_info *)arg;
  // debug(1, "Connection %d: AP2 Event Receiver Cleanup start.", conn->connection_number);
#ifdef CONFIG_METADATA
  // this is here to ensure it's only performed once during a teardown of a ptp stream
  send_ssnc_metadata('disc', conn->client_ip_string, strlen(conn->client_ip_string), 1);
#endif
  pthread_mutex_lock(&conn->event_sender_mutex);
  pthread_cleanup_push(mutex_unlock, &conn->event_sender_mutex);
  safe_socket_close(&conn->event_channel_fd);
  pthread_cleanup_pop(1); // unlock the mutex

  // Group metadata is owned by the conversation after this worker is joined.
  debug(2, "Connection %d: AP2 Event Receiver Cleanup is complete.", conn->connection_number);
}

void *ap2_event_receiver(void *arg) {
  rtsp_conn_info *conn = (rtsp_conn_info *)arg;
  debug(2, "Connection %d: AP2 Event Receiver started", conn->connection_number);
  SOCKADDR remote_addr;
  memset(&remote_addr, 0, sizeof(remote_addr));
  socklen_t addr_size = sizeof(remote_addr);

  pthread_mutex_lock(&conn->event_sender_mutex);
  pthread_cleanup_push(mutex_unlock, &conn->event_sender_mutex);
  conn->event_channel_fd =
      eintr_checked_accept(conn->event_socket, (struct sockaddr *)&remote_addr, &addr_size);
  pthread_cleanup_pop(1); // unlock the mutex
  if (conn->event_channel_fd > 0) {
    debug(2,
          "Connection %d: ap2_event_receiver accepted a connection on socket %d and moved to a new "
          "socket %d.",
          conn->connection_number, conn->event_socket, conn->event_channel_fd);
    pthread_cleanup_push(ap2_event_receiver_cleanup_handler, arg);

    ap2_event_send_update_info(conn);

    while (1) {
      usleep(100000);
    };

    debug(3, "Connection %d: AP2 Event Receiver RTP thread starting \"normal\" exit.",
          conn->connection_number);
    pthread_cleanup_pop(1); // do the cleanup
  } else {
    debug(1, "Connection %d: could not accept an event socket", conn->connection_number);
    conn->event_channel_fd = -1;
  }
  debug(2, "Connection %d: AP2 Event Receiver RTP thread \"normal\" exit.",
        conn->connection_number);
  conn->ap2_event_receiver_exited = 1;
  pthread_exit(NULL);
}
