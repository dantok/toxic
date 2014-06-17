/*  dns.c
 *
 *
 *  Copyright (C) 2014 Toxic All Rights Reserved.
 *
 *  This file is part of Toxic.
 *
 *  Toxic is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Toxic is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with Toxic.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <string.h>
#include <netinet/in.h>
#include <arpa/nameser.h>
#include <resolv.h>

#include <tox/toxdns.h>

#include "toxic.h"
#include "windows.h"
#include "line_info.h"
#include "dns.h"
#include "global_commands.h"

#define MAX_DNS_THREADS 10
#define MAX_DNS_REQST_SIZE 256
#define NUM_DNS3_SERVERS 1    /* must correspond to number of items in dns3_servers array */
#define TOX_DNS3_TXT_PREFIX "v=tox3;id="
#define DNS3_KEY_SZ 32

static struct dns3_server {
    uint8_t *name;
    uint8_t key[DNS3_KEY_SZ];
} dns3_servers[] = {
    {
        "utox.org", 
        { 
          0xD3, 0x15, 0x4F, 0x65, 0xD2, 0x8A, 0x5B, 0x41, 0xA0, 0x5D, 0x4A, 0xC7, 0xE4, 0xB3, 0x9C, 0x6B,
          0x1C, 0x23, 0x3C, 0xC8, 0x57, 0xFB, 0x36, 0x5C, 0x56, 0xE8, 0x39, 0x27, 0x37, 0x46, 0x2A, 0x12
        }
    },
};

struct _thread_data {
    ToxWindow *self;
    uint8_t id_bin[TOX_FRIEND_ADDRESS_SIZE];
    uint8_t addr[MAX_STR_SIZE];
    uint8_t msg[MAX_STR_SIZE];
    uint8_t busy;
    Tox *m;
} t_data;

static struct _dns_thread {
    pthread_t tid;
    pthread_mutex_t lock;
} dns_thread;

static int dns_error(ToxWindow *self, uint8_t *errmsg)
{
    uint8_t msg[MAX_STR_SIZE];
    snprintf(msg, sizeof(msg), "DNS lookup failed: %s", errmsg);

    pthread_mutex_lock(&dns_thread.lock);
    line_info_add(self, NULL, NULL, NULL, msg, SYS_MSG, 0, 0);
    pthread_mutex_unlock(&dns_thread.lock);

    return -1;
}

static int cleanup_dns_thread(void *dns_obj)
{
    if (dns_obj)
        tox_dns3_kill(dns_obj);

    memset(&t_data, 0, sizeof(struct _thread_data));
}

/* puts TXT from dns response in buf. Returns length of TXT on success, -1 on fail.*/
static int parse_dns_response(ToxWindow *self, u_char *answer, int ans_len, uint8_t *buf)
{
    uint8_t *ans_pt = answer + sizeof(HEADER);
    uint8_t *ans_end = answer + ans_len;
    uint8_t exp_ans[PACKETSZ];
    
    int len = dn_expand(answer, ans_end, ans_pt, exp_ans, sizeof(exp_ans));

    if (len == -1)
        return dns_error(self, "dn_expand failed."); 

    ans_pt += len;

    if (ans_pt > ans_end - 4)
         return dns_error(self, "Reply was too short."); 

    int type;
    GETSHORT(type, ans_pt);

    if (type != T_TXT)
        return dns_error(self, "Broken reply."); 
 

    ans_pt += INT16SZ;    /* class */
    uint32_t size = 0;

    /* recurse through CNAME rr's */
    do { 
        ans_pt += size;
        len = dn_expand(answer, ans_end, ans_pt, exp_ans, sizeof(exp_ans));

        if (len == -1)
            return dns_error(self, "Second dn_expand failed."); 

        ans_pt += len;

        if (ans_pt > ans_end - 10)
            return dns_error(self, "Reply was too short."); 

        GETSHORT(type, ans_pt);
        ans_pt += INT16SZ;
        ans_pt += 4;
        GETSHORT(size, ans_pt);

        if (ans_pt + size < answer || ans_pt + size > ans_end)
            return dns_error(self, "RR overflow."); 

    } while (type == T_CNAME);

    if (type != T_TXT)
        return dns_error(self, "Not a TXT record."); 

    uint32_t txt_len = *ans_pt;

    if (!size || txt_len >= size || !txt_len)
        return dns_error(self, "No record found.");

    ans_pt++;
    ans_pt[txt_len] = '\0';
    memcpy(buf, ans_pt, txt_len + 1);

    return txt_len;
}

/* Takes address addr in the form "username@domain", puts the username in namebuf, 
   and the domain in dombuf.

   return length of username on success, -1 on failure */
static int parse_addr(uint8_t *addr, uint8_t *namebuf, uint8_t *dombuf)
{
    uint8_t tmpaddr[MAX_STR_SIZE];
    uint8_t *tmpname, *tmpdom;

    strcpy(tmpaddr, addr);
    tmpname = strtok(tmpaddr, "@");
    tmpdom = strtok(NULL, "");

    if (tmpname == NULL || tmpdom == NULL)
        return -1;

    strcpy(namebuf, tmpname);
    strcpy(dombuf, tmpdom);

    return strlen(namebuf);
}

/* Does DNS lookup for addr and puts resulting tox id in id_bin. */
void *dns3_lookup_thread(void *data)
{
    ToxWindow *self = t_data.self;

    uint8_t domain[MAX_STR_SIZE];
    uint8_t name[MAX_STR_SIZE];

    int namelen = parse_addr(t_data.addr, name, domain);

    if (namelen == -1) {
        dns_error(self, "Must be a Tox key or an address in the form username@domain");
        cleanup_dns_thread(NULL);
        return;
    }

    /* get domain name/pub key */
    uint8_t *DNS_pubkey, *domname = NULL;
    int i;

    for (i = 0; i < NUM_DNS3_SERVERS; ++i) {
        if (strcmp(dns3_servers[i].name, domain) == 0) {
            DNS_pubkey = dns3_servers[i].key;
            domname = dns3_servers[i].name;
            break;
        }
    }

    if (domname == NULL) {
        dns_error(self, "Domain not found.");
        cleanup_dns_thread(NULL);
        return;
    }

    void *dns_obj = tox_dns3_new(DNS_pubkey);

    if (dns_obj == NULL) {
        dns_error(self, "Core failed to create DNS object.");
        cleanup_dns_thread(NULL);
        return;
    }

    uint8_t string[MAX_DNS_REQST_SIZE];
    uint32_t request_id;

    int str_len = tox_generate_dns3_string(dns_obj, string, sizeof(string), &request_id, name, namelen);

    if (str_len == -1) {
        dns_error(self, "Core failed to generate dns3 string.");
        cleanup_dns_thread(dns_obj);
        return;
    }

    string[str_len] = '\0';

    u_char answer[PACKETSZ];
    uint8_t d_string[MAX_DNS_REQST_SIZE];

    /* format string and create dns query */
    snprintf(d_string, sizeof(d_string), "_%s._tox.%s", string, domname);
    int ans_len = res_query(d_string, C_IN, T_TXT, answer, sizeof(answer));

    if (ans_len <= 0) {
        dns_error(self, "Query failed.");
        cleanup_dns_thread(dns_obj);
        return;
    }

    uint8_t ans_id[MAX_DNS_REQST_SIZE];

    /* extract TXT from DNS response */
    if (parse_dns_response(self, answer, ans_len, ans_id) == -1) {
        cleanup_dns_thread(dns_obj);
        return;
    }

    uint8_t encrypted_id[MAX_DNS_REQST_SIZE];
    int prfx_len = strlen(TOX_DNS3_TXT_PREFIX);

    /* extract the encrypted ID from TXT response */
    if (strncmp(ans_id, TOX_DNS3_TXT_PREFIX, prfx_len) != 0) {
        dns_error(self, "Bad dns3 TXT response.");
        cleanup_dns_thread(dns_obj);
        return;
    }

    memcpy(encrypted_id, ans_id + prfx_len, ans_len - prfx_len);

    if (tox_decrypt_dns3_TXT(dns_obj, t_data.id_bin, encrypted_id, strlen(encrypted_id), request_id) == -1) {
        dns_error(self, "Core failed to decrypt response.");
        cleanup_dns_thread(dns_obj);
        return;
    }

    pthread_mutex_lock(&dns_thread.lock);
    cmd_add_helper(self, t_data.m, t_data.id_bin, t_data.msg);
    pthread_mutex_unlock(&dns_thread.lock);

    cleanup_dns_thread(dns_obj);
    return;
}

/* creates new thread for dns3 lookup. Only allows one lookup at a time. */
void dns3_lookup(ToxWindow *self, Tox *m, uint8_t *id_bin, uint8_t *addr, uint8_t *msg)
{
    if (t_data.busy) {
        uint8_t *err = "Please wait for previous DNS lookup to finish.";
        line_info_add(self, NULL, NULL, NULL, err, SYS_MSG, 0, 0);
        return;
    }

    t_data.self = self;
    snprintf(t_data.id_bin, sizeof(t_data.id_bin), "%s", id_bin);
    snprintf(t_data.addr, sizeof(t_data.addr), "%s", addr);
    snprintf(t_data.msg, sizeof(t_data.msg), "%s", msg);
    t_data.m = m;
    t_data.busy = 1;

    if (pthread_create(&dns_thread.tid, NULL, dns3_lookup_thread, NULL) != 0) {
        endwin();
        fprintf(stderr, "DNS thread creation failed. Aborting...\n");
        exit(EXIT_FAILURE);
    }

    if (pthread_mutex_init(&dns_thread.lock, NULL) != 0) {
        endwin();
        fprintf(stderr, "DNS thread mutex creation failed. Aborting...\n");
        exit(EXIT_FAILURE);
    }
}