
/* ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is libguac-client-ssh.
 *
 * The Initial Developer of the Original Code is
 * Michael Jumper.
 * Portions created by the Initial Developer are Copyright (C) 2011
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either the GNU General Public License Version 2 or later (the "GPL"), or
 * the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

#include <stdio.h>
#include <stdint.h>
#include <unistd.h>

#include <libssh2.h>
#include <pthread.h>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

#include <guacamole/client.h>

#include "client.h"
#include "ssh_agent.h"
#include "ssh_buffer.h"

void ssh_auth_agent_sign(ssh_auth_agent* agent, char* data, int data_length) {

    LIBSSH2_CHANNEL* channel = agent->channel;
    ssh_key* key = agent->identity;

    char sig[4096];
    int sig_len;

    char buffer[4096];
    char* pos = buffer;

    /* Sign with key */
    sig_len = ssh_key_sign(key, data, data_length, (u_char*) sig);
    if (sig_len < 0)
        return;

    buffer_write_uint32(&pos, 1+4 + 4+7 + 4+sig_len);

    buffer_write_byte(&pos, SSH2_AGENT_SIGN_RESPONSE);
    buffer_write_uint32(&pos, 4+7 + 4+sig_len);

    /* Write key type */
    if (key->type == SSH_KEY_RSA)
        buffer_write_string(&pos, "ssh-rsa", 7);
    else if (key->type == SSH_KEY_DSA)
        buffer_write_string(&pos, "ssh-dsa", 7);
    else
        return;

    /* Write signature */
    buffer_write_string(&pos, sig, sig_len);

    libssh2_channel_write(channel, buffer, pos-buffer);
    libssh2_channel_flush(channel);

    usleep(10000);

}

void ssh_auth_agent_list_identities(ssh_auth_agent* auth_agent) {

    LIBSSH2_CHANNEL* channel = auth_agent->channel;
    ssh_key* key = auth_agent->identity;

    char buffer[4096];
    char* pos = buffer;

    buffer_write_uint32(&pos, 1+4
                              + key->public_key_length+4
                              + sizeof(SSH_AGENT_COMMENT)+3);

    buffer_write_byte(&pos, SSH2_AGENT_IDENTITIES_ANSWER);
    buffer_write_uint32(&pos, 1);

    buffer_write_string(&pos, key->public_key, key->public_key_length);
    buffer_write_string(&pos, SSH_AGENT_COMMENT, sizeof(SSH_AGENT_COMMENT)-1);

    libssh2_channel_write(channel, buffer, pos-buffer);
    libssh2_channel_flush(channel);

    usleep(10000);

}

void ssh_auth_agent_handle_packet(ssh_auth_agent* auth_agent, uint8_t type,
        char* data, int data_length) {

    switch (type) {

        /* List identities */
        case SSH2_AGENT_REQUEST_IDENTITIES:
            ssh_auth_agent_list_identities(auth_agent);
            break;

        /* Sign request */
        case SSH2_AGENT_SIGN_REQUEST: {

            char* pos = data;

            int key_blob_length;
            int sign_data_length;
            char* sign_data;

            /* Skip past key, read data, ignore flags */
            buffer_read_string(&pos, &key_blob_length);
            sign_data = buffer_read_string(&pos, &sign_data_length);

            /* Sign given data */
            ssh_auth_agent_sign(auth_agent, sign_data, sign_data_length);
            break;
        }

        /* Otherwise, return failure */
        default:
            libssh2_channel_write(auth_agent->channel,
                    UNSUPPORTED, sizeof(UNSUPPORTED)-1);

    }

}

void* ssh_auth_agent_read_thread(void* arg) {

    ssh_auth_agent* auth_agent = (ssh_auth_agent*) arg;
    LIBSSH2_CHANNEL* channel = auth_agent->channel;

    int bytes_read;
    char buffer[4096];
    int buffer_length = 0;

    /* Wait for channel to settle */
    usleep(10000);

    do {

        /* Read data into buffer */
        while ((bytes_read = libssh2_channel_read(channel, buffer+buffer_length,
                        sizeof(buffer)-buffer_length)) >= 0
                || bytes_read == LIBSSH2_ERROR_EAGAIN) {

            /* If re-read required, wait a bit and retry */
            if (bytes_read == LIBSSH2_ERROR_EAGAIN) {
                usleep(10000);
                continue;
            }

            /* Update buffer length */
            buffer_length += bytes_read;

            /* Read length and type if given */
            if (buffer_length >= 5) {

                /* Read length */
                uint32_t length =
                      (((unsigned char*) buffer)[0] << 24)
                    | (((unsigned char*) buffer)[1] << 16)
                    | (((unsigned char*) buffer)[2] <<  8)
                    |  ((unsigned char*) buffer)[3];

                /* Read type */
                uint8_t type = ((unsigned char*) buffer)[4];

                /* If enough data read, call handler, shift data */
                if (buffer_length >= length+4) {

                    ssh_auth_agent_handle_packet(auth_agent, type,
                            buffer+5, length-1);

                    buffer_length -= length+4;
                    memmove(buffer, buffer+length+4, buffer_length);

                }

            } /* end if have length and type */

            /* If EOF, stop now */
            if (libssh2_channel_eof(channel))
                break;

        } /* end packet fill */

    } while (bytes_read >= 0 && !libssh2_channel_eof(channel));

    /* Done */
    return NULL;

}

void ssh_auth_agent_callback(LIBSSH2_SESSION *session,
        LIBSSH2_CHANNEL *channel, void **abstract) {

    /* Get client data */
    guac_client* client = (guac_client*) *abstract;
    ssh_guac_client_data* client_data = (ssh_guac_client_data*) client->data;

    /* Get base auth agent thread */
    pthread_t read_thread;
    ssh_auth_agent* auth_agent = malloc(sizeof(ssh_auth_agent));

    auth_agent->channel = channel;
    auth_agent->identity = client_data->key;

    /* Create thread */
    pthread_create(&read_thread, NULL, ssh_auth_agent_read_thread, auth_agent);

}
