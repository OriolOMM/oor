/*
 * liblisp.c
 *
 * This file is part of LISP Mobile Node Implementation.
 *
 * Copyright (C) 2014 Universitat Politècnica de Catalunya.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * Please send any bug reports or fixes you make to the email address(es):
 *    LISP-MN developers <devel@lispmob.org>
 *
 * Written or modified by:
 *    Florin Coras <fcoras@ac.upc.edu>
 */

#include "liblisp.h"
#include "../lib/cksum.h"
#include "hmac/hmac.h"
#include "../lib/lmlog.h"

static void increment_record_count(lbuf_t *b);

lisp_msg_type_e
lisp_msg_type(lbuf_t *b)
{
    ecm_hdr_t *hdr = lbuf_lisp(b);
    if (!hdr) {
        return(NOT_LISP_MSG);
    }
    return(hdr->type);
}

static void *
lisp_msg_pull_ecm_hdr(lbuf_t *b)
{
    lbuf_reset_lisp_hdr(b);
    return(lbuf_pull(b, sizeof(ecm_hdr_t)));
}

/* Process encapsulated map request header:  lisp header and the interal IP and
 * UDP header */
int
lisp_msg_ecm_decap(lbuf_t *pkt, uint16_t *src_port)
{
    uint16_t ipsum = 0;
    uint16_t udpsum = 0;
    int udp_len = 0;
    struct udphdr *udph;
    struct ip *iph;

    /* this is the new start of the packet */
    lisp_msg_pull_ecm_hdr(pkt);
    /* Set and extract inner layer 3 packet */
    lbuf_reset_l3(pkt);
    iph = pkt_pull_ip(pkt);
    /* Set and extract inner layer 4 packet */
    lbuf_reset_l4(pkt);
    udph = pkt_pull_udp(pkt);

    /* Set the beginning of the LISP msg*/
    lbuf_reset_lisp(pkt);

    /* This should overwrite the external port (dst_port in map-reply =
     * inner src_port in encap map-request) */
    *src_port = ntohs(udph->source);

 #ifdef BSD
    udp_len = ntohs(udph->uh_ulen);
 #else
    udp_len = ntohs(udph->len);
 #endif

    /* Verify the checksums. */
    if (iph->ip_v == IPVERSION) {
        ipsum = ip_checksum((uint16_t *) iph, sizeof(struct ip));
        if (ipsum != 0) {
            LMLOG(LDBG_2, "IP checksum failed.");
        }

    }

    /* Verify UDP checksum only if different from 0.
     * This means we ACCEPT UDP checksum 0! */
    if (udph->check != 0) {
        udpsum = udp_checksum(udph, udp_len, iph,
                ip_version_to_sock_afi(iph->ip_v));
        if (udpsum != 0) {
            LMLOG(LDBG_2, "UDP checksum failed.");
            return (BAD);
        }
    }


    LMLOG(LDBG_2, "%s, inner IP: %s -> %s, inner UDP: %d -> %d",
            lisp_msg_hdr_to_char(pkt),
            ip_to_char(&iph->ip_src, ip_version_to_sock_afi(iph->ip_v)),
            ip_to_char(&iph->ip_dst, ip_version_to_sock_afi(iph->ip_v)),
            ntohs(udph->source), ntohs(udph->dest));

    return (GOOD);
}

int
lisp_msg_parse_addr(lbuf_t *msg, lisp_addr_t *eid)
{
    int len = lisp_addr_parse(lbuf_data(msg), eid);
    if (len < 0) {
        return(BAD);
    }
    lbuf_pull(msg, len);
    return(GOOD);
}

/* Given a message buffer 'msg', extracts the EID out of an EID prefix field
 * and stores it in 'eid'. */
int
lisp_msg_parse_eid_rec(lbuf_t *msg, lisp_addr_t *eid)
{
    eid_record_hdr_t *hdr = lbuf_data(msg);
    int len = lisp_addr_parse(EID_REC_ADDR(hdr), eid);
    lbuf_pull(msg, len);
    lisp_addr_set_plen(eid, EID_REC_MLEN(hdr));

    return(GOOD);
}

int
lisp_msg_parse_itr_rlocs(lbuf_t *b, glist_t *rlocs)
{
    lisp_addr_t *tloc;
    void *mreq_hdr = lbuf_lisp(b);
    int i;

    tloc = lisp_addr_new();
    for (i = 0; i < MREQ_ITR_RLOC_COUNT(mreq_hdr) + 1; i++) {
        if (lisp_msg_parse_addr(b, tloc) != GOOD) {
            return(BAD);
        }
        glist_add(lisp_addr_clone(tloc), rlocs);
        LMLOG(LDBG_1," itr-rloc: %s", lisp_addr_to_char(tloc));
    }
    lisp_addr_del(tloc);
    return(GOOD);
}


int
lisp_msg_parse_loc(lbuf_t *b, locator_t *loc)
{
    int len;
    void *hdr;

    hdr = lbuf_data(b);

    len = locator_parse(lbuf_data(b), loc);
    if (len <= 0) {
        return(BAD);
    }

    lbuf_pull(b, len);

    LMLOG(LDBG_1, "    %s, addr: %s", locator_record_hdr_to_char(hdr),
            lisp_addr_to_char(locator_addr(loc)));

    return(GOOD);
}

int
lisp_msg_parse_mapping_record_split(lbuf_t *b, lisp_addr_t *eid,
        glist_t *loc_list, locator_t **probed_)
{
    void *mrec_hdr = NULL, *loc_hdr = NULL;
    locator_t *loc = NULL, *probed = NULL;
    int i = 0, len = 0;

    probed = NULL;
    mrec_hdr = lbuf_data(b);
    lbuf_pull(b, sizeof(mapping_record_hdr_t));

    len = lisp_addr_parse(lbuf_data(b), eid);
    if (len <= 0) {
        return(BAD);
    }
    lbuf_pull(b, len);
    lisp_addr_set_plen(eid, MAP_REC_EID_PLEN(mrec_hdr));

    LMLOG(LDBG_1, "  %s eid: %s", mapping_record_hdr_to_char(mrec_hdr),
            lisp_addr_to_char(eid));

    for (i = 0; i < MAP_REC_LOC_COUNT(mrec_hdr); i++) {
        loc_hdr = lbuf_data(b);

        loc = locator_new();
        if (lisp_msg_parse_loc(b, loc) != GOOD) {
            return(BAD);
        }
        glist_add(loc, loc_list);

        if (LOC_PROBED(loc_hdr)) {
            if (probed != NULL) {
                LMLOG(LDBG_1, "Multiple probed locators! Aborting");
                return(BAD);
            }
            probed = loc;
        }
    }
    if (probed_ != NULL) {
        *probed_ = probed;
    }

    return(GOOD);
}

/* extracts a mapping record out of lbuf 'b' and stores it into 'm'. 'm' must
 * be preallocated. If a locator is probed, a pointer to it is stored in
 * 'probed'. */
int
lisp_msg_parse_mapping_record(lbuf_t *b, mapping_t *m, locator_t **probed)
{
    glist_t *loc_list;
    glist_entry_t *lit;
    locator_t *loc;
    int ret;
    void *hdr;

    if (!m) {
        return(BAD);
    }

    hdr = lbuf_data(b);
    mapping_set_ttl(m, ntohl(MAP_REC_TTL(hdr)));
    mapping_set_action(m, MAP_REC_ACTION(hdr));
    mapping_set_auth(m, MAP_REC_AUTH(hdr));

    /* no free is called when destroyed*/
    loc_list = glist_new();

    ret = lisp_msg_parse_mapping_record_split(b, mapping_eid(m), loc_list,
                                              probed);
    if (ret != GOOD) {
        goto err;
    }

    glist_for_each_entry(lit, loc_list) {
        loc = glist_entry_data(lit);
        if ((ret = mapping_add_locator(m, loc)) != GOOD) {
            locator_del(loc);
            if (ret != ERR_EXIST){
                goto err;
            }
        }
    }

    glist_destroy(loc_list);
    return(GOOD);

err:
    glist_destroy(loc_list);
    return(BAD);
}

static unsigned int
msg_type_to_hdr_len(lisp_msg_type_e type)
{
    switch(type) {
    case LISP_MAP_REQUEST:
        return(sizeof(map_request_hdr_t));
    case LISP_MAP_REPLY:
        return(sizeof(map_reply_hdr_t));
    case LISP_MAP_REGISTER:
        return(sizeof(map_register_hdr_t));
    case LISP_MAP_NOTIFY:
        return(sizeof(map_notify_hdr_t));
    default:
        return(0);
    }
}

void *
lisp_msg_pull_hdr(lbuf_t *b)
{
    lisp_msg_type_e type = lisp_msg_type(b);
    return(lbuf_pull(b, msg_type_to_hdr_len(type)));
}


void *
lisp_msg_pull_auth_field(lbuf_t *b)
{
    void *hdr;
    lisp_key_type_e keyid;

    hdr = lbuf_pull(b, sizeof(auth_record_hdr_t));
    keyid = ntohs(AUTH_REC_KEY_ID(hdr));
    lbuf_pull(b, auth_data_get_len_for_type(keyid));
    return(hdr);
}


void *
lisp_msg_put_addr(lbuf_t *b, lisp_addr_t *addr)
{
    void *ptr;
    int len;

    /* make sure there's enough space */
    ptr = lbuf_put_uninit(b, lisp_addr_size_to_write(addr));
    if ((len = lisp_addr_write(ptr, addr)) <= 0) {
        LMLOG(LDBG_3, "lisp_msg_put_addr: failed to write address %s",
                lisp_addr_to_char(addr));
        return(NULL);
    }

    return(ptr);
}

void *
lisp_msg_put_locator(lbuf_t *b, locator_t *locator)
{
    locator_hdr_t *loc_ptr;
    lisp_addr_t *addr;

    lcl_locator_extended_info_t * lct_extended_info;

    loc_ptr = lbuf_put_uninit(b, sizeof(locator_hdr_t));

    if (locator->state == UP) {
        loc_ptr->priority    = locator->priority;
    } else {
        /* If the locator is DOWN, set the priority to 255
         * -> Locator should not be used */
        loc_ptr->priority    = UNUSED_RLOC_PRIORITY;
    }
    loc_ptr->weight = locator->weight;
    loc_ptr->mpriority = locator->mpriority;
    loc_ptr->mweight = locator->mweight;
    loc_ptr->local = 1;
    loc_ptr->reachable = locator->state;

    /* TODO: FC should take RTR stuff out in the near future */
    lct_extended_info = locator->extended_info;
    if (lct_extended_info->rtr_locators_list != NULL){
        addr = &(lct_extended_info->rtr_locators_list->locator->address);
    } else {
        addr = locator_addr(locator);
    }

    lisp_msg_put_addr(b, addr);
    return(loc_ptr);
}


static void
increment_record_count(lbuf_t *b)
{
    void *hdr = lbuf_lisp(b);

    switch(lisp_msg_type(b)) {
    case LISP_MAP_REQUEST:
        MREQ_REC_COUNT(hdr) += 1;
        break;
    case LISP_MAP_REPLY:
        MREP_REC_COUNT(hdr) += 1;
        break;
    case LISP_MAP_REGISTER:
        MREG_REC_COUNT(hdr) += 1;
        break;
    case LISP_MAP_NOTIFY:
        MNTF_REC_COUNT(hdr) += 1;
        break;
    default:
        return;
    }

}

void *
lisp_msg_put_mapping_hdr(lbuf_t *b)
{
    void *hdr = lbuf_put_uninit(b, sizeof(mapping_record_hdr_t));
    mapping_record_init_hdr(hdr);
    return(hdr);
}

void *
lisp_msg_put_mapping(
        lbuf_t      *b,
        mapping_t   *m,
        lisp_addr_t *probed_loc)
{
    mapping_record_hdr_t    *rec            = NULL;
    locator_hdr_t           *ploc           = NULL;
    lisp_addr_t             *eid            = NULL;
    glist_entry_t   		*it_list        = NULL;
    glist_entry_t   		*it_loct        = NULL;
    glist_t					*loct_list		= NULL;
    locator_t				*loct			= NULL;

    eid = mapping_eid(m);
    rec = lisp_msg_put_mapping_hdr(b);
    MAP_REC_EID_PLEN(rec) = lisp_addr_get_plen(eid);
    MAP_REC_LOC_COUNT(rec) = m->locator_count;
    MAP_REC_TTL(rec) = htonl(m->ttl);
    MAP_REC_AUTH(rec) = m->authoritative;

    if (lisp_msg_put_addr(b, eid) == NULL) {
        return(NULL);
    }

    /* Add locators */
    glist_for_each_entry(it_list,mapping_locators_lists(m)){
    	loct_list = (glist_t *)glist_entry_data(it_list);
    	loct = (locator_t *)glist_first_data(loct_list);
    	if (lisp_addr_is_no_addr(locator_addr(loct)) == TRUE){
    		continue;
    	}
    	glist_for_each_entry(it_loct,loct_list){
    		loct = (locator_t *)glist_entry_data(it_loct);
    		ploc = lisp_msg_put_locator(b, loct);
    		if (probed_loc)
    		if (probed_loc != NULL
    				&& lisp_addr_cmp(lisp_addr_get_ip_addr(locator_addr(loct)), probed_loc) == 0) {
    			LOC_PROBED(ploc) = 1;
    		}
    	}
    }

    increment_record_count(b);

    return(rec);
}

void *
lisp_msg_put_neg_mapping(lbuf_t *b, lisp_addr_t *eid, int ttl,
        lisp_action_e act)
{
    void *rec;

    rec = lisp_msg_put_mapping_hdr(b);
    MAP_REC_EID_PLEN(rec) = lisp_addr_get_plen(eid);
    MAP_REC_LOC_COUNT(rec) = 0;
    MAP_REC_TTL(rec) = htonl(ttl);
    MAP_REC_ACTION(rec) = act;

    if (lisp_msg_put_addr(b, eid) == NULL) {
        return(NULL);
    }

    increment_record_count(b);

    return(rec);
}

void *
lisp_msg_put_itr_rlocs(lbuf_t *b, glist_t *itr_rlocs)
{
    glist_entry_t *it;
    lisp_addr_t *rloc;
    void *hdr, *data;

    data = lbuf_data(b);
    if(glist_size(itr_rlocs) == 0){
        return (NULL);
    }
    glist_for_each_entry(it, itr_rlocs) {
        rloc = glist_entry_data(it);
        if (lisp_msg_put_addr(b, rloc) == NULL) {
            return(NULL);
        }
    }

    hdr = lisp_msg_hdr(b);
    MREQ_ITR_RLOC_COUNT(hdr) = glist_size(itr_rlocs)-1;
    return(data);
}

void *
lisp_msg_put_eid_rec(lbuf_t *b, lisp_addr_t *eid)
{
    eid_record_hdr_t *hdr;
    hdr = lbuf_put_uninit(b, sizeof(eid_record_hdr_t));
    eid_rec_hdr_init(hdr);
    EID_REC_MLEN(hdr) = lisp_addr_get_plen(eid);
    lisp_msg_put_addr(b, eid);
    increment_record_count(b);
    return(hdr);
}

void *
lisp_msg_encap(lbuf_t *b, int lp, int rp, lisp_addr_t *la,
        lisp_addr_t *ra)
{
    void *hdr;

    /* end of lisp msg */
    lbuf_reset_lisp(b);

    /* push inner ip and udp */
    pkt_push_udp_and_ip(b, lp, rp, lisp_addr_ip(la), lisp_addr_ip(ra));

    /* push lisp ecm hdr */
    hdr = lbuf_push_uninit(b, sizeof(ecm_hdr_t));
    ecm_hdr_init(hdr);
    lbuf_reset_lisp_hdr(b);

    return(lbuf_data(b));
}

lbuf_t *
lisp_msg_create_buf()
{
    lbuf_t* b;

    b = lbuf_new_with_headroom(MAX_IP_PKT_LEN, MAX_LISP_MSG_ENCAP_LEN);
    lbuf_reset_lisp(b);
    return(b);
}

lbuf_t*
lisp_msg_create(lisp_msg_type_e type)
{
    lbuf_t* b;
    void *hdr;

    b = lisp_msg_create_buf();

    switch(type) {
    case LISP_MAP_REQUEST:
        hdr = lbuf_put_uninit(b, sizeof(map_request_hdr_t));
        map_request_hdr_init(hdr);
        break;
    case LISP_MAP_REPLY:
        hdr = lbuf_put_uninit(b, sizeof(map_reply_hdr_t));
        map_reply_hdr_init(hdr);
        break;
    case LISP_MAP_REGISTER:
        hdr = lbuf_put_uninit(b, sizeof(map_register_hdr_t));
        map_register_hdr_init(hdr);
        break;
    case LISP_MAP_NOTIFY:
        hdr = lbuf_put_uninit(b, sizeof(map_notify_hdr_t));
        map_notify_hdr_init(hdr);
        break;
    case LISP_INFO_NAT:
//        hdr = lbuf_put_uninit(b, sizeof(info_nat_hdr_t));
//        info_nat_hdr_init(hdr);
        break;
    case LISP_ENCAP_CONTROL_TYPE:
        /* nothing to do */
        break;
    default:
        LMLOG(LDBG_3, "lisp_msg_create: Unknown LISP message "
                "type %s", type);
    }

    return(b);
}

lbuf_t *
lisp_msg_mreq_create(lisp_addr_t *seid, glist_t *itr_rlocs,
        lisp_addr_t *deid)
{

    lbuf_t *b = lisp_msg_create(LISP_MAP_REQUEST);
    if (lisp_msg_put_addr(b, seid) == NULL) {
        lbuf_del(b);
        return(NULL);
    }

    if (lisp_msg_put_itr_rlocs(b, itr_rlocs) == NULL) {
        lbuf_del(b);
        return(NULL);
    }

    if (lisp_msg_put_eid_rec(b, deid) == NULL) {
        lbuf_del(b);
        return(NULL);
    }

    return(b);
}

lbuf_t *
lisp_msg_neg_mrep_create(lisp_addr_t *eid, int ttl, lisp_action_e ac, uint64_t nonce)
{
    lbuf_t *b;
    void *hdr;
    b = lisp_msg_create(LISP_MAP_REPLY);
    lisp_msg_put_neg_mapping(b, eid, ttl, ac);
    hdr = lisp_msg_hdr(b);
    MREP_NONCE(hdr) = nonce;
    return(b);
}

lbuf_t *
lisp_msg_mreg_create(mapping_t *m, lisp_key_type_e keyid)
{
    lbuf_t *b = lisp_msg_create(LISP_MAP_REGISTER);

    if (!lisp_msg_put_empty_auth_record(b, keyid)) {
        return(NULL);
    }

    if (!lisp_msg_put_mapping(b, m, NULL)) {
        return(NULL);
    }

    return(b);
}

lbuf_t *
lisp_msg_nat_mreg_create(mapping_t *m, char *key, lisp_site_id *site_id,
        lisp_xtr_id *xtr_id, lisp_key_type_e keyid)
{
    void *hdr;
    lbuf_t *b = lisp_msg_create(LISP_MAP_REGISTER);
    hdr = lisp_msg_put_empty_auth_record(b, keyid);

    if (!hdr) {
        return(NULL);
    }

    MREG_PROXY_REPLY(hdr) = 1;
    MREG_IBIT(hdr) = 1;
    MREG_RBIT(hdr) = 1;

    if (!lisp_msg_put_mapping(b, m, NULL)) {
        return(NULL);
    }

    lbuf_put(b, xtr_id, sizeof(lisp_xtr_id));
    lbuf_put(b, site_id, sizeof(lisp_site_id));

    if (lisp_msg_fill_auth_data(b, keyid, key) != GOOD) {
        return(NULL);
    }

    return(b);
}

char *
lisp_msg_hdr_to_char(lbuf_t *b)
{
    void *h = lbuf_lisp(b);

    if (!h) {
        return(NULL);
    }

    switch(lisp_msg_type(b)) {
    case LISP_MAP_REQUEST:
        return(map_request_hdr_to_char(h));
    case LISP_MAP_REPLY:
        return(map_reply_hdr_to_char(h));
    case LISP_MAP_REGISTER:
        return(map_register_hdr_to_char(h));
    case LISP_MAP_NOTIFY:
        return(map_notify_hdr_to_char(h));
    case LISP_INFO_NAT:
        return(NULL);
    case LISP_ENCAP_CONTROL_TYPE:
        return(ecm_hdr_to_char(h));
    default:
        LMLOG(LDBG_3, "Unknown LISP message type %d",
                lisp_msg_type(b));
        return(NULL);
    }
}

char *
lisp_msg_ecm_hdr_to_char(lbuf_t *b)
{
    void *h = lbuf_lisp_hdr(b);
    if (!h) {
        return(NULL);
    }
    return(ecm_hdr_to_char(h));
}

int
lisp_msg_fill_auth_data(lbuf_t *b, lisp_key_type_e keyid, const char *key)
{
    void *hdr = lisp_msg_auth_record(b);

    if (complete_auth_fields(
            keyid,
            key,
            lbuf_lisp(b),
            lbuf_size(b),
            AUTH_REC_DATA(hdr)) != GOOD) {
        return(BAD);
    }

    return(GOOD);
}



/* Checks auth field of Map-Reply and Map-Request messages */
int
lisp_msg_check_auth_field(lbuf_t *b, const char *key)
{
    lisp_key_type_e keyid;
    uint16_t        ad_len  = 0;
    int             ret     = BAD;

    auth_record_hdr_t *hdr;

    hdr = lisp_msg_auth_record(b);

    keyid = ntohs(AUTH_REC_KEY_ID(hdr));
    ad_len = auth_data_get_len_for_type(keyid);
    if (ad_len != ntohs(AUTH_REC_DATA_LEN(hdr))) {
        LMLOG(LDBG_3, "Auth Record record length is wrong: %d instead of %d",
                ntohs(AUTH_REC_DATA_LEN(hdr)), ad_len);
        return(BAD);
    }

    ret = check_auth_field(
            keyid,
            key,
            lbuf_lisp(b),
            lbuf_size(b),
            AUTH_REC_DATA(hdr));

    return(ret);
}

void *
lisp_msg_put_empty_auth_record(lbuf_t *b, lisp_key_type_e keyid)
{
    void *hdr;
    uint16_t len = auth_data_get_len_for_type(keyid);
    hdr = lbuf_put_uninit(b, sizeof(auth_record_hdr_t) + len);
    AUTH_REC_KEY_ID(hdr) = htons(keyid);
    AUTH_REC_DATA_LEN(hdr) = htons(len);
    memset(AUTH_REC_DATA(hdr), 0, len);
    return(hdr);
}


void *
lisp_data_push_hdr(lbuf_t *b)
{
    lisphdr_t *lhdr;
    lhdr = lbuf_push_uninit(b, sizeof(lisphdr_t));
    lisp_data_hdr_init(lhdr);
    return(lhdr);
}

void *
lisp_data_encap(lbuf_t *b, int lp, int rp, lisp_addr_t *la, lisp_addr_t *ra)
{
    int ttl = 0, tos = 0;

    /* read ttl and tos */
    ip_hdr_ttl_and_tos(lbuf_data(b), &ttl, &tos);

    /* push lisp data hdr */
    lisp_data_push_hdr(b);

    /* push outer UDP and IP */
    pkt_push_udp_and_ip(b, lp, rp, lisp_addr_ip(la), lisp_addr_ip(ra));

    ip_hdr_set_ttl_and_tos(lbuf_data(b), ttl, tos);

    return(lbuf_data(b));
}

void *
lisp_data_pull_hdr(lbuf_t *b)
{
    void *dt = lbuf_pull(b, sizeof(lisphdr_t));
    return(dt);
}



/* returns in 'addr_' the first element of the list 'l' to have AFI 'afi'
 * caller must allocate and free 'addr_' */
int
laddr_list_get_addr(glist_t *l, int afi, lisp_addr_t *addr)
{
    glist_entry_t *it;
    lisp_addr_t *ait;
    int found = 0;

    if (!addr) {
        return(BAD);
    }

    glist_for_each_entry(it, l) {
        ait = glist_entry_data(it);
        if (lisp_addr_ip_afi(ait) == afi) {
            lisp_addr_copy(addr, ait);
            found = 1;
            break;
        }
    }

    if (found) {
        return(GOOD);
    } else {
        return(BAD);
    }
}

char *
laddr_list_to_char(glist_t *l)
{
    static char buf[50*INET6_ADDRSTRLEN]; /* 50 addresses */
    int i = 1, n;
    glist_entry_t *it;

    n = glist_size(l);

    *buf = '\0';
    glist_for_each_entry(it, l) {
        if (i < n) {
            sprintf(buf + strlen(buf), "%s, ",
                    lisp_addr_to_char(glist_entry_data(it)));
        } else {
            sprintf(buf + strlen(buf), "%s",
                    lisp_addr_to_char(glist_entry_data(it)));
        }
        i++;
    }
    return(buf);
}



