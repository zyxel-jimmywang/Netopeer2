/**
 * @file op_generic.c
 * @author Radek Krejci <rkrejci@cesnet.cz>
 * @brief Implementation of NETCONF Event Notifications handling
 *
 * Copyright (c) 2016 CESNET, z.s.p.o.
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */

#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#include <libyang/libyang.h>
#include <nc_server.h>
#include <sysrepo.h>

#include "common.h"
#include "operations.h"

struct subscriber_s {
    struct nc_session *session;
    const struct lys_module *stream;
    time_t start;
    time_t stop;
};

struct {
    uint16_t size;
    uint16_t num;
    struct subscriber_s *list;
    pthread_mutex_t lock;
} subscribers = {0, 0, NULL, PTHREAD_MUTEX_INITIALIZER};

struct nc_server_reply *
op_ntf_subscribe(struct lyd_node *rpc, struct nc_session *ncs)
{
    uint16_t i;
    uint32_t idx;
    time_t now = time(NULL), start = 0, stop = 0;
    const char *stream;
    struct lyd_node *node;
    struct lys_node *snode;
    struct subscriber_s *new = NULL;
    struct nc_server_error *e = NULL;
    const struct lys_module *mod, *pstream;

    /*
     * parse RPC to get params
     */
    /* stream is always present - as explicit or default node */
    stream = ((struct lyd_node_leaf_list *)rpc->child)->value_str;

    /* check for the correct stream name */
    if (!strcmp(stream, "NETCONF")) {
        /* default stream */
        pstream = NULL;
    } else {
        /* stream name is supposed to match the name of a schema in the context having some
         * notifications defined */
        idx = 0;
        while ((mod = ly_ctx_get_module_iter(np2srv.ly_ctx, &idx))) {
            LY_TREE_FOR(mod->data, snode) {
                if (snode->nodetype == LYS_NOTIF) {
                    break;
                }
            }
            if (!snode) {
                /* module has no notification */
                continue;
            }

            if (!strcmp(stream, mod->name)) {
                /* we have a match */
                pstream = mod;
                break;
            }
        }
        if (!mod) {
            /* requested stream does not match any schema with a notification */
            e = nc_err(NC_ERR_BAD_ELEM, NC_ERR_TYPE_PROT, "stream");
            nc_err_set_msg(e, "Requested stream name does not match any of the provided streams.", "en");
            goto error;
        }
    }

    /* get optional parameters */
    LY_TREE_FOR(rpc->child->next, node) {
        if (strcmp(node->schema->module->ns, "urn:ietf:params:xml:ns:netconf:notification:1.0")) {
            /* ignore */
            continue;
        } else if (!strcmp(node->schema->name, "startTime")) {
            start = nc_datetime2time(((struct lyd_node_leaf_list *)node)->value_str);
        } else if (!strcmp(node->schema->name, "stopTime")) {
            stop = nc_datetime2time(((struct lyd_node_leaf_list *)node)->value_str);
        }
        /* TODO support subtree and XPath filters */
    }

    /* check for the correct time boundaries */
    if (start > now) {
        /* it is not valid to specify future start time */
        e = nc_err(NC_ERR_BAD_ELEM, NC_ERR_TYPE_PROT, "startTime");
        nc_err_set_msg(e, "Requested startTime is later than the current time.", "en");
        goto error;
    } else if (!start && stop) {
        /* stopTime must be used with startTime */
        e = nc_err(NC_ERR_MISSING_ELEM, NC_ERR_TYPE_PROT, "startTime");
        nc_err_set_msg(e, "The stopTime element must be used with the startTime element.", "en");
        goto error;
    } else if (start > stop) {
        /* stopTime must be later than startTime */
        e = nc_err(NC_ERR_BAD_ELEM, NC_ERR_TYPE_PROT, "stopTime");
        nc_err_set_msg(e, "Requested stopTime is earlier than the specified startTime.", "en");
        goto error;
    }

    pthread_mutex_lock(&subscribers.lock);

    /* check that the session is not in the current subscribers list */
    for (i = 0; i < subscribers.num; i++) {
        if (subscribers.list[i].session == ncs) {
            /* we have match, check times */
            if (subscribers.list[i].stop && subscribers.list[i].stop < now) {
                /* previous subscription ended, update it and use it */
                subscribers.list[i].start = start;
                subscribers.list[i].stop = stop;
                subscribers.list[i].stream = pstream;
                new = &subscribers.list[i];
                break;
            } else {
                /* already subscribed */
                pthread_mutex_unlock(&subscribers.lock);
                e = nc_err(NC_ERR_IN_USE, NC_ERR_TYPE_PROT);
                nc_err_set_msg(e, "Already subscribed.", "en");
                goto error;
            }
        } else {
            /* check times for the subscribers list maintenance */
            if (subscribers.list[i].stop && subscribers.list[i].stop < now) {
                /* expired subscriber, clean it */
                subscribers.num--;
                if (i + 1 < subscribers.num) {
                    /* replace it by the last subscriber */
                    memcpy(&subscribers.list[i], &subscribers.list[subscribers.num], sizeof *subscribers.list);
                } /* else just decrease the number of subscribers and forget */
                i--;
            }
        }
    }
    if (!new) {
        /* new subscriber, add it into the list */
        if (subscribers.num == subscribers.size) {
            subscribers.size += 4;
            new = realloc(subscribers.list, subscribers.size * sizeof *subscribers.list);
            if (!new) {
                /* realloc failed */
                pthread_mutex_unlock(&subscribers.lock);
                EMEM;
                e = nc_err(NC_ERR_OP_FAILED, NC_ERR_TYPE_APP);
                subscribers.size -= 4;
                goto error;
            }
            subscribers.list = new;
        }
        new = &subscribers.list[subscribers.num];
        subscribers.num++;
    }

    /* store information about the new subscriber */
    new->session = ncs;
    new->start = start;
    new->stop = stop;
    new->stream = pstream;

    pthread_mutex_unlock(&subscribers.lock);

    nc_session_set_notif_status(ncs, 1);

    return nc_server_reply_ok();

error:
    return nc_server_reply_err(e);
}

void
op_ntf_unsubscribe(struct nc_session *session)
{
    unsigned int i;
    time_t now = time(NULL);

    pthread_mutex_lock(&subscribers.lock);

    for (i = 0; i < subscribers.num; i++) {
        if (subscribers.list[i].session == session) {
            /* we have match */
            subscribers.num--;
            if (i < subscribers.num) {
                /* move here the subscriber from the end of the list */
                memcpy(&subscribers.list[i], &subscribers.list[subscribers.num], sizeof *subscribers.list);
            }
            /* we are done */
            break;
        } else {
            /* check times for the subscribers list maintenance */
            if (subscribers.list[i].stop && subscribers.list[i].stop < now) {
                /* expired subscriber, remove it */
                subscribers.num--;
                if (i < subscribers.num) {
                    /* replace it by the last subscriber */
                    memcpy(&subscribers.list[i], &subscribers.list[subscribers.num], sizeof *subscribers.list);
                } /* else just decrease the number of subscribers and forget */
                i--;
            }
        }
    }

    if (!subscribers.num) {
        free(subscribers.list);
        subscribers.list = NULL;
        subscribers.size = 0;
    }

    pthread_mutex_unlock(&subscribers.lock);

    nc_session_set_notif_status(session, 0);
}

struct lyd_node *
ntf_get_data(void)
{
    uint32_t idx = 0;
    struct lyd_node *root, *stream;
    struct lys_node *snode;
    const struct lys_module *mod;

    root = lyd_new_path(NULL, np2srv.ly_ctx, "/nc-notifications:netconf/streams", NULL, 0, 0);
    if (!root || !root->child) {
        goto error;
    }

    /* generic stream */
    stream = lyd_new_path(root, np2srv.ly_ctx, "/nc-notifications:netconf/streams/stream[name='NETCONF']", NULL, 0, 0);
    if (!stream) {
        goto error;
    }
    if (!lyd_new_leaf(stream, stream->schema->module, "description",
                      "Default NETCONF stream containing all the Event Notifications.")) {
        goto error;
    }
    if (!lyd_new_leaf(stream, stream->schema->module, "replaySupport", "false")) {
        goto error;
    }

    /* local streams - matching a module specifying a notifications */
    while ((mod = ly_ctx_get_module_iter(np2srv.ly_ctx, &idx))) {
        LY_TREE_FOR(mod->data, snode) {
            if (snode->nodetype == LYS_NOTIF) {
                break;
            }
        }
        if (!snode) {
            /* module has no notification */
            continue;
        }

        /* generate information about the stream/module */
        stream = lyd_new(root->child, root->schema->module, "stream");
        if (!stream) {
            goto error;
        }
        if (!lyd_new_leaf(stream, stream->schema->module, "name", mod->name)) {
            goto error;
        }
        if (!lyd_new_leaf(stream, stream->schema->module, "replaySupport", "false")) {
            goto error;
        }
    }

    return root;

error:
    lyd_free(root);
    return NULL;
}

void
np2srv_ntf_send(struct lyd_node **ntf, time_t timestamp)
{
    size_t i;
    char *datetime;
    struct nc_server_notif *ntf_msg = NULL;
    struct lys_module *mod;
    time_t now;

    /* build the notification */
    datetime = nc_time2datetime(timestamp, NULL, NULL);
    ntf_msg = nc_server_notif_new(*ntf, datetime, 0);
    if (!ntf_msg) {
        free(datetime);
        return;
    }
    mod = (*ntf)->schema->module;
    *ntf = NULL;

    /* get the current time */
    now = time(NULL);

    /* send notification to the all subscribed receivers */
    pthread_mutex_lock(&subscribers.lock);
    for (i = 0; i < subscribers.num; i++) {
        /* maintain subscribers list by checking subscription stop times */
        if (subscribers.list[i].stop && subscribers.list[i].stop < now) {
            /* expired subscriber, remove it */
            subscribers.num--;
            if (i < subscribers.num) {
                /* replace it by the last subscriber */
                memcpy(&subscribers.list[i], &subscribers.list[subscribers.num], sizeof *subscribers.list);
            } /* else just decrease the number of subscribers and forget */
            i--;
            continue;
        }

        /* check subscribed stream */
        if (!subscribers.list[i].stream || /* generic NETCONF session where we send all the notifications */
                subscribers.list[i].stream == mod) { /* notification from the subscribed schema */
            nc_server_notif_send(subscribers.list[i].session, ntf_msg, 5000);
        }
    }
    pthread_mutex_unlock(&subscribers.lock);

    nc_server_notif_free(ntf_msg);
}

void
np2srv_ntf_clb(const sr_ev_notif_type_t notif_type, const char *xpath, const sr_node_t *trees, const size_t tree_cnt,
               time_t timestamp, void *UNUSED(private_ctx))
{
    const struct lys_node *snode;
    struct lyd_node *ntf = NULL, *parent, *node;
    const sr_node_t *srnode, *srnext;
    const struct lys_module *mod;
    size_t i;
    char numstr[21];

    VRB("Received notification \"%s\" (%d).", xpath, timestamp);

    /* if we have no subscribers, it is not needed to do anything here */
    if (!subscribers.num) {
        return;
    }

    ntf = lyd_new_path(NULL, np2srv.ly_ctx, xpath, NULL, 0, 0);
    if (!ntf) {
        ERR("Creating notification \"%s\" failed.", xpath);
        goto error;
    }

    for (i = 0; i < tree_cnt; i++) {
        parent = ntf;

        for (srnode = srnext = &trees[i]; srnode; srnode = srnext) {
            mod = ly_ctx_get_module(np2srv.ly_ctx, srnode->module_name, NULL);
            if (!mod) {
                ERR("Data from unknown module (%s%s) received in sysrepo notification \"%s\"", srnode->module_name,
                    srnode->name, xpath);
                goto error;
            } else if (!mod->implemented) {
                mod = lys_implemented_module(mod);
                if (!mod->implemented) {
                    ERR("Non-implemented data (%s:%s) received in sysrepo notification \"%s\"", srnode->module_name,
                        srnode->name, xpath);
                    goto error;
                }
            }

            snode = NULL;
            while ((snode = lys_getnext(snode, parent->schema, mod, 0))) {
                if (strcmp(srnode->name, snode->name) || strcmp(srnode->module_name, snode->module->name)) {
                    continue;
                }
                /* match */
                break;
            }
            if (!snode) {
                ERR("Unknown data (%s:%s) received in sysrepo notification \"%s\"", srnode->module_name,
                    srnode->name, xpath);
                goto error;
            }
            switch (snode->nodetype) {
            case LYS_LEAFLIST:
            case LYS_LEAF:
                node = lyd_new_leaf(parent, mod, srnode->name, op_get_srval(np2srv.ly_ctx, (sr_val_t *)srnode, numstr));
                break;
            case LYS_CONTAINER:
            case LYS_LIST:
                node = lyd_new(parent, mod, srnode->name);
                break;
            case LYS_ANYXML:
            case LYS_ANYDATA:
                node = lyd_new_anydata(parent, mod, srnode->name,
                                       op_get_srval(np2srv.ly_ctx, (sr_val_t *)srnode, numstr), LYD_ANYDATA_SXML);
                break;
            default:
                ERR("Invalid node type (%d) received in sysrepo notification \"%s\"", snode->nodetype, xpath);
                goto error;
            }

            if (!node) {
                ERR("Creating notification (%s) data (%d: %s:%s) failed.", xpath, snode->nodetype, srnode->module_name,
                    srnode->name);
                goto error;
            }

            /* select element for the next run - children first */
            srnext = srnode->first_child;
            if (!srnext) {
                /* no children, try siblings */
                srnext = srnode->next;
            } else {
                parent = node;
            }
            while (!srnext) {
                /* parent is already processed, go to its sibling */
                srnode = srnode->parent;

                if (srnode == trees->parent) {
                    /* we are done, no next element to process */
                    break;
                }
                srnext = srnode->next;
                parent = parent->parent;
            }
        }
    }

    /* send the notification */
    np2srv_ntf_send(&ntf, timestamp);

error:
    lyd_free(ntf);
}
