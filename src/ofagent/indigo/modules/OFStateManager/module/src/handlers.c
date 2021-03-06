/****************************************************************
 *
 *        Copyright 2013, Big Switch Networks, Inc. 
 * 
 * Licensed under the Eclipse Public License, Version 1.0 (the
 * "License"); you may not use this file except in compliance
 * with the License. You may obtain a copy of the License at
 * 
 *        http://www.eclipse.org/legal/epl-v10.html
 * 
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
 * either express or implied. See the License for the specific
 * language governing permissions and limitations under the
 * License.
 *
 ****************************************************************/

/**
 * @file
 * @brief First line of defense against OpenFlow messages
 *
 * See detailed documentation in the Indigo architecture headers.
 */

#include "ofstatemanager_log.h"

#include <OFStateManager/ofstatemanager_config.h>
#include <OFConnectionManager/ofconnectionmanager.h>
#include <indigo/indigo.h>
#include <indigo/of_state_manager.h>
#include <indigo/port_manager.h>
#include <indigo/forwarding.h>
#include <loci/loci.h>
#include <loci/loci_obj_dump.h>
#include "ofstatemanager_decs.h"
#include "ofstatemanager_int.h"
#include "handlers.h"
#include "ft.h"

static void
flow_mod_err_msg_send(indigo_error_t indigo_err, of_version_t ver,
                      indigo_cxn_id_t cxn_id, of_flow_modify_t *flow_mod);

/****************************************************************
 *
 * Utility functions
 *
 ****************************************************************/

int
ind_core_xid_alloc(void)
{
    static uint32_t xid = 1000;

    return xid++;
}

/****************************************************************
 *
 * Message handling
 *
 ****************************************************************/

/**
 * Function for unhandled message
 */
indigo_error_t
ind_core_unhandled_message(of_object_t *obj, indigo_cxn_id_t cxn_id)
{
    uint32_t xid = 0;

    LOG_ERROR("Unhandled message %p from %d.  Object id %d",
              obj, cxn_id, obj->object_id);

    of_object_xid_get(obj, &xid);
    /* Generate error message */
    if (indigo_cxn_send_error_msg(obj->version, cxn_id, xid,
                                  OF_ERROR_TYPE_BAD_REQUEST,
                                  OF_REQUEST_FAILED_BAD_TYPE, NULL) < 0) {
        LOG_ERROR("Error sending error message for unhandled msg, cxn id %d",
                  cxn_id);
    }

    of_object_delete(obj);

    return INDIGO_ERROR_UNKNOWN;
}

/****************************************************************/

/**
 * Handle a hello message
 * @param cxn_id Connection handler for the owning connection
 * @param _obj Generic type object for the message to be coerced
 * @returns Error code
 */

indigo_error_t
ind_core_hello_handler(of_object_t *_obj, indigo_cxn_id_t cxn_id)
{
    LOG_TRACE("Received HELLO message from %d", cxn_id);

    of_object_delete(_obj);

    return INDIGO_ERROR_NONE;
}

/****************************************************************/

/**
 * Handle a packet_out message
 * @param cxn_id Connection handler for the owning connection
 * @param _obj Generic type object for the message to be coerced
 * @returns Error code
 *
 * Note:  Deletes packet out object, so data from obj should be copied
 */

indigo_error_t
ind_core_packet_out_handler(of_object_t *_obj, indigo_cxn_id_t cxn_id)
{
    of_packet_out_t *obj;

    obj = (of_packet_out_t *)_obj;
    LOG_TRACE("Handling of_packet_out message: %p.", obj);

    (void)indigo_fwd_packet_out(obj);

    of_packet_out_delete(obj);

    return INDIGO_ERROR_NONE;
}

/****************************************************************/

/**
 * Handle a port_mod message
 * @param cxn_id Connection handler for the owning connection
 * @param _obj Generic type object for the message to be coerced
 * @returns Error code
 */

indigo_error_t
ind_core_port_mod_handler(of_object_t *_obj, indigo_cxn_id_t cxn_id)
{
    of_port_mod_t *obj = _obj;
    indigo_error_t rv;

    LOG_TRACE("Handling of_port_mod message.");

    rv = indigo_port_modify(obj);

    if (rv != INDIGO_ERROR_NONE) {
        of_version_t ver = obj->version;
        uint32_t xid = 0;

        LOG_ERROR("Port modify failed: %d", rv);
        of_port_mod_xid_get(obj, &xid);
        if (ind_core_send_error_msg(ver, cxn_id, xid,
                OF_ERROR_TYPE_PORT_MOD_FAILED_BY_VERSION(ver),
                OF_PORT_MOD_FAILED_BAD_PORT, obj, NULL) < 0) {
            LOG_ERROR("Error sending port mod error message");
        }
    }

    of_port_mod_delete(obj);

    return rv;
}

/****************************************************************/

/**
 * Handle a port_stats_request message
 * @param cxn_id Connection handler for the owning connection
 * @param _obj Generic type object for the message to be coerced
 * @returns Error code
 */

indigo_error_t
ind_core_port_stats_request_handler(of_object_t *_obj, indigo_cxn_id_t cxn_id)
{
    of_port_stats_request_t *obj = _obj;
    of_port_stats_reply_t *reply;
    indigo_error_t rv;
    uint32_t xid = 0;

    LOG_TRACE("Handling of_port_stats_request message.");

    rv = indigo_port_stats_get(obj, &reply);
    if (rv == INDIGO_ERROR_NONE) {
        /* Set the XID to match the request */
        of_port_stats_request_xid_get(obj, &xid);
        of_port_stats_reply_xid_set(reply, xid);

        if ((rv = IND_CORE_MSG_SEND(cxn_id, reply)) < 0) {
            LOG_ERROR("Error %d sending port_stats_get reply to %d", rv, cxn_id);
        }
    } else {
        of_port_no_t port_no;
        of_port_stats_request_port_no_get(obj, &port_no);

        LOG_ERROR("Failed to get stats for port %u: %d", port_no, rv);
        /* @todo sending type 0, code 0 error message */
        if (indigo_cxn_send_error_msg(obj->version, cxn_id, xid,
                                      0, 0, NULL) < 0) {
            LOG_ERROR("Error sending error message for port stats get msg, "
                      "cxn id %d", cxn_id);
        }
    }

    of_port_stats_request_delete(obj);

    return rv;
}

/****************************************************************/

/**
 * Handle a queue_get_config_request message
 * @param cxn_id Connection handler for the owning connection
 * @param _obj Generic type object for the message to be coerced
 * @returns Error code
 */

indigo_error_t
ind_core_queue_get_config_request_handler(of_object_t *_obj,
                                          indigo_cxn_id_t cxn_id)
{
    of_queue_get_config_request_t *obj = _obj;
    of_queue_get_config_reply_t *reply;
    indigo_error_t rv;
    uint32_t xid;
    of_port_no_t port;

    of_queue_get_config_request_xid_get(obj, &xid);
    of_queue_get_config_request_port_get(obj, &port);

    LOG_TRACE("Handling of_queue_get_config_request message.");

    rv = indigo_port_queue_config_get(obj, &reply);
    if (rv == INDIGO_ERROR_NONE) {
        of_queue_get_config_reply_xid_set(reply, xid);
        of_queue_get_config_reply_port_set(reply, port);

        if ((rv = IND_CORE_MSG_SEND(cxn_id, reply)) < 0) {
#ifdef OFDPA_FIXUP
            LOG_ERROR("Error %d sending queue_config_get reply to %d", rv, cxn_id);
#else
            LOG_ERROR("Error %d sending queue_stats_get reply to %d", rv, cxn_id);
#endif
        }
    } else {
#ifdef OFDPA_FIXUP
        LOG_ERROR("Failed to get queue config on port %u: %d",
                  port, rv);

        if (indigo_cxn_send_error_msg(obj->version, cxn_id, xid,
                                      OF_ERROR_TYPE_QUEUE_OP_FAILED_BY_VERSION(obj->version),
                                      OF_QUEUE_OP_FAILED_BAD_PORT_BY_VERSION(obj->version), 
                                      NULL) < 0) {
#else
        uint32_t queue_id;
        of_queue_stats_request_queue_id_get(obj, &queue_id);
        LOG_ERROR("Failed to get config for queue %u on port %u: %d",
                  queue_id, port, rv);
        /* @todo sending type 0, code 0 error message */
        if (indigo_cxn_send_error_msg(obj->version, cxn_id, xid,
                                      0, 0, NULL) < 0) {
#endif
        LOG_ERROR("Error sending error message for queue config get msg, "
                      "cxn id %d", cxn_id);
        }

    }

    of_queue_get_config_request_delete(obj);

    return rv;
}

/****************************************************************/

/**
 * Handle a queue_stats_request message
 * @param cxn_id Connection handler for the owning connection
 * @param _obj Generic type object for the message to be coerced
 * @returns Error code
 */

indigo_error_t
ind_core_queue_stats_request_handler(of_object_t *_obj, indigo_cxn_id_t cxn_id)
{
    of_queue_stats_request_t *obj = _obj;
    of_queue_stats_reply_t *reply;
    uint32_t xid;
    indigo_error_t rv;
#ifdef OFDPA_FIXUP
    uint16_t code;
#endif

    of_queue_stats_request_xid_get(obj, &xid);

    LOG_TRACE("Handling of_queue_stats_request message");

    rv = indigo_port_queue_stats_get(obj, &reply);
    if (rv == INDIGO_ERROR_NONE) {
        /* Set the XID to match the request */
        of_queue_stats_reply_xid_set(reply, xid);

        if ((rv = IND_CORE_MSG_SEND(cxn_id, reply)) < 0) {
            LOG_ERROR("Error %d sending queue_stats reply to %d", rv, cxn_id);
            of_queue_stats_reply_delete(reply);
        }
    } else {
        of_port_no_t port_no;
        uint32_t queue_id;
        of_queue_stats_request_port_no_get(obj, &port_no);
        of_queue_stats_request_queue_id_get(obj, &queue_id);

        LOG_ERROR("Failed to get stats for queue %u on port %u: %d",
                  queue_id, port_no, rv);
#ifdef OFDPA_FIXUP
        if (rv == INDIGO_ERROR_NOT_FOUND)
        {
          code = OF_QUEUE_OP_FAILED_BAD_PORT_BY_VERSION(obj->version);
        }
        else if (rv == INDIGO_ERROR_RANGE)
        {
          code = OF_QUEUE_OP_FAILED_BAD_QUEUE_BY_VERSION(obj->version);
        }
        else
        {
          code = OF_QUEUE_OP_FAILED_BAD_PORT_BY_VERSION(obj->version);
        }

        if (indigo_cxn_send_error_msg(obj->version, cxn_id, xid,
                                      OF_ERROR_TYPE_QUEUE_OP_FAILED_BY_VERSION(obj->version), 
                                      code, NULL) < 0) {
#else
        /* @todo sending type 0, code 0 error message */
        if (indigo_cxn_send_error_msg(obj->version, cxn_id, xid,
                                      0, 0, NULL) < 0) {
#endif
            LOG_ERROR("Error sending error message for queue stats get msg, "
                      "cxn id %d", cxn_id);
        }
    }

    of_queue_stats_request_delete(obj);

    return rv;
}

/****************************************************************/

static indigo_error_t
flow_mod_setup_query(of_flow_modify_t *obj, /* Works with add, mod, del */
                     of_meta_match_t *query,
                     int query_mode,
                     int force_wildcard_port)
{
    INDIGO_MEM_SET(query, 0, sizeof(*query));
    if (obj->version > OF_VERSION_1_0) {
        of_flow_modify_table_id_get(obj, &query->table_id);
    } else {
        query->table_id = TABLE_ID_ANY;
    }
    _TRY(of_flow_modify_match_get(obj, &(query->match)));
    query->mode = query_mode;
    if ((query_mode == OF_MATCH_STRICT) || (query_mode == OF_MATCH_OVERLAP)) {
        query->check_priority = 1;
        of_flow_add_priority_get(obj, &(query->priority));
    }
    if (force_wildcard_port) {
        query->out_port = OF_PORT_DEST_WILDCARD;
    } else {
        /* Could check object_id is delete or delete_strict */
        of_flow_add_out_port_get(obj, &(query->out_port));
    }
    if (query_mode != OF_MATCH_OVERLAP && obj->version >= OF_VERSION_1_1) {
        of_flow_add_cookie_get(obj, &query->cookie);
        of_flow_add_cookie_mask_get(obj, &query->cookie_mask);
    }

    return INDIGO_ERROR_NONE;
}

/**
 * @brief Check if overlap is found with the flow modify object
 *
 * Return 1 if overlap found, 0 if not found, <0 on error
 */

static int
overlap_found(of_flow_modify_t *obj)
{
    ft_entry_t *entry;
    list_links_t *cur, *next;
    of_meta_match_t query;

    _TRY(flow_mod_setup_query(obj, &query, OF_MATCH_OVERLAP, 1));

    FT_ITER(ind_core_ft, entry, cur, next) {
        if (ft_entry_meta_match(&query, entry)) {
            return 1;
        }
    }

    return 0;
}

static indigo_flow_id_t
flow_id_next(void)
{
    static indigo_flow_id_t next_flow_id = 1;

    indigo_flow_id_t result = next_flow_id;

    if (++next_flow_id == 0)  next_flow_id = 1;

    return (result);
}

/**
 * Handle a flow_add message
 * @param cxn_id Connection handler for the owning connection
 * @param _obj Generic type object for the message to be coerced
 * @returns Error code
 */

indigo_error_t
ind_core_flow_add_handler(of_object_t *_obj, indigo_cxn_id_t cxn_id)
{
    indigo_error_t rv = INDIGO_ERROR_NONE;
    of_flow_modify_t *obj; /* Coerce to modify object */
    of_meta_match_t query;
    uint16_t flags;
    of_version_t ver;
    uint32_t xid = 0;
    ft_entry_t        *entry = 0;
    indigo_flow_id_t  flow_id;
    uint16_t idle_timeout, hard_timeout;
    uint8_t table_id;

    obj = (of_flow_modify_t *)_obj;
    ver = obj->version;
    LOG_TRACE("Handling of_flow_add message: %p, ver %d.", obj, ver);

    of_flow_modify_flags_get(obj, &flags);
    of_flow_modify_xid_get(obj, &xid);
    of_flow_modify_idle_timeout_get(obj, &idle_timeout);
    of_flow_modify_hard_timeout_get(obj, &hard_timeout);

    if (flags & OF_FLOW_MOD_FLAG_CHECK_OVERLAP_BY_VERSION(ver)) {
        if (overlap_found(obj)) {
            LOG_TRACE("Overlap found when adding flow");
            if (ind_core_send_error_msg(ver, cxn_id, xid,
                    OF_ERROR_TYPE_FLOW_MOD_FAILED_BY_VERSION(ver),
                    OF_FLOW_MOD_FAILED_OVERLAP_BY_VERSION(ver),
                    obj, NULL) < 0) {
                LOG_ERROR("Error sending overlap error message");
            }
            goto done;
        }
    }

    if ((flags & OF_FLOW_MOD_FLAG_EMERG_BY_VERSION(ver)) &&
        (idle_timeout != 0 || hard_timeout != 0)) {
        LOG_TRACE("Attempted to set timeout on an emergency flow");
        if (ind_core_send_error_msg(ver, cxn_id, xid,
                OF_ERROR_TYPE_FLOW_MOD_FAILED_BY_VERSION(ver),
                OF_FLOW_MOD_FAILED_BAD_EMERG_TIMEOUT_BY_VERSION(ver),
                obj, NULL) < 0) {
            LOG_ERROR("Error sending bad emergency timeout error message");
        }
        rv = INDIGO_ERROR_PARAM;
        goto done;
    }

    /* Search table; if match found, replace entry */
    rv = flow_mod_setup_query(obj, &query, OF_MATCH_STRICT, 1);
    if (rv != INDIGO_ERROR_NONE) {
        LOG_ERROR("flow_mod_setup_query() failed");
        goto done;
    }

    /* Delete existing flow if any */
    if (ft_strict_match(ind_core_ft, &query, &entry) == INDIGO_ERROR_NONE) {
        ind_core_flow_entry_delete(entry, INDIGO_FLOW_REMOVED_OVERWRITE, cxn_id);
    }

    /* No match found, add as normal */
    LOG_TRACE("Adding new flow");

    flow_id = flow_id_next();

    rv = ft_add(ind_core_ft, flow_id, obj, &entry);
    if (rv != INDIGO_ERROR_NONE) {
        LOG_ERROR("ft_add() failed");
        goto done;
    }

    rv = indigo_fwd_flow_create(flow_id, (of_flow_add_t *)obj, &table_id);
    if (rv == INDIGO_ERROR_NONE) {
        LOG_TRACE("Flow table now has %d entries",
                  FT_STATUS(ind_core_ft)->current_count);
        entry->table_id = table_id;
    } else { /* Error during insertion at forwarding layer */
       uint32_t xid;

       LOG_VERBOSE("Error from forwarding while inserting flow: %d", rv);
       ind_core_ft->status.forwarding_add_errors += 1;

       of_flow_add_xid_get(obj, &xid);
       flow_mod_err_msg_send(rv, obj->version, cxn_id,
                             (of_flow_modify_t *)obj);

       /* Free entry in local flow table */
       ft_delete(ind_core_ft, entry);
    }

done:
    of_object_delete(_obj);

    return INDIGO_ERROR_NONE;
}

/**
 * Translate the error status into the correct error code for the given
 * OpenFlow version, and send the error message to the controller.
 * @param indigo_err Error status
 * @param ver OpenFlow version
 * @param cxn_id Connection to send to
 * @param flow_mod Request that failed
 */

static void
flow_mod_err_msg_send(indigo_error_t indigo_err, of_version_t ver,
                      indigo_cxn_id_t cxn_id, of_flow_modify_t *flow_mod)
{
    unsigned char errmsgf = 0;
    unsigned code;
    uint32_t xid;

    of_flow_modify_xid_get(flow_mod, &xid);

    switch (indigo_err) {
    case INDIGO_ERROR_NONE:
        break;

    case INDIGO_ERROR_RESOURCE:
        code    = OF_FLOW_MOD_FAILED_ALL_TABLES_FULL_BY_VERSION(ver);
        /* @fixme this will return the wrong code for ver >1.0 */
        errmsgf = 1;
        break;

    case INDIGO_ERROR_NOT_SUPPORTED:
        code    = OF_FLOW_MOD_FAILED_UNSUPPORTED_BY_VERSION(ver);
        /* @fixme this will return the wrong code for ver >1.0 */
        errmsgf = 1;
        break;

    default:
        code    = OF_FLOW_MOD_FAILED_EPERM_BY_VERSION(ver);
        /* @fixme use OF_FLOW_MOD_FAILED_UNKNOWN for ver >1.0 */
        errmsgf = 1;
    }

    if (errmsgf) {
        int rv;

        rv = ind_core_send_error_msg(ver, cxn_id, xid,
            OF_ERROR_TYPE_FLOW_MOD_FAILED_BY_VERSION(ver),
            code, flow_mod, NULL);
        if (INDIGO_FAILURE(rv)) {
            LOG_ERROR("Error sending flow mod error message");
        }
    }
}

/****************************************************************/

/* State for non-strict flow-modify iteration */
struct flow_modify_state {
    of_flow_modify_t *request;
    indigo_cxn_id_t cxn_id;
    int num_matched;
};

/* Flowtable iterator for ind_core_flow_modify_handler */
static void
modify_iter_cb(void *cookie, ft_entry_t *entry)
{
    struct flow_modify_state *state = cookie;

    if (entry != NULL) {
        indigo_error_t rv;
        state->num_matched++;
        rv = indigo_fwd_flow_modify(entry->id, state->request);
        if (rv == INDIGO_ERROR_NONE) {
            ft_entry_modify_effects(ind_core_ft, entry, state->request);
        } else {
            LOG_TRACE("Flow modify error: %d", rv);
            flow_mod_err_msg_send(rv, state->request->version,
                                  state->cxn_id, state->request);
        }
    } else {
        if (state->num_matched == 0) {
            LOG_TRACE("No entries to modify, treat as add");
            /* OpenFlow 1.0.0, section 4.6, page 14.  Treat as an add */
            ind_core_flow_add_handler(state->request, state->cxn_id);
        } else {
            LOG_TRACE("Finished flow modify task");
            of_object_delete(state->request);
        }
        INDIGO_MEM_FREE(state);
    }
}

/**
 * Handle a flow_modify message
 * @param cxn_id Connection handler for the owning connection
 * @param _obj Generic type object for the message to be coerced
 * @returns Error code
 */

indigo_error_t
ind_core_flow_modify_handler(of_object_t *_obj, indigo_cxn_id_t cxn_id)
{
    of_flow_modify_t *obj;
    int rv;
    of_meta_match_t query;

    obj = (of_flow_modify_t *)_obj;
    LOG_TRACE("Handling of_flow_modify message: %p.", obj);

    struct flow_modify_state *state = INDIGO_MEM_ALLOC(sizeof(*state));
    if (state == NULL) {
        return INDIGO_ERROR_RESOURCE;
    }
    state->request = obj;
    state->num_matched = 0;
    state->cxn_id = cxn_id;

    rv = flow_mod_setup_query(obj, &query, OF_MATCH_NON_STRICT, 1);
    if (rv != INDIGO_ERROR_NONE) {
        of_object_delete(_obj);
        INDIGO_MEM_FREE(state);
        return rv;
    }

    rv = ft_spawn_iter_task(ind_core_ft, &query, modify_iter_cb, state,
                            IND_SOC_DEFAULT_PRIORITY);
    if (rv != INDIGO_ERROR_NONE) {
        of_object_delete(_obj);
        INDIGO_MEM_FREE(state);
        return rv;
    }

    return INDIGO_ERROR_NONE;
}

/**
 * Handle a flow_modify_strict message
 * @param cxn_id Connection handler for the owning connection
 * @param _obj Generic type object for the message to be coerced
 * @returns Error code
 *
 * Checks that only one entry in local table matches.  See modify_handler
 * above for more info.
 */

indigo_error_t
ind_core_flow_modify_strict_handler(of_object_t *_obj, indigo_cxn_id_t cxn_id)
{
    of_flow_modify_strict_t *obj = _obj;
    indigo_error_t rv;
    of_meta_match_t query;
    ft_entry_t *entry;

    LOG_TRACE("Handling of_flow_modify_strict message.");

    /* Form the query */
    rv = flow_mod_setup_query(obj, &query, OF_MATCH_STRICT, 1);
    if (rv != INDIGO_ERROR_NONE) {
        goto done;
    }

    rv = ft_strict_match(ind_core_ft, &query, &entry);
    if (rv == INDIGO_ERROR_NOT_FOUND) {
        LOG_TRACE("No entries to modify strict, treat as add.");
        /* OpenFlow 1.0.0, section 4.6, page 14.  Treat as an add */
        return ind_core_flow_add_handler(_obj, cxn_id);
    }

    rv = indigo_fwd_flow_modify(entry->id, obj);
    if (rv == INDIGO_ERROR_NONE) {
        ft_entry_modify_effects(ind_core_ft, entry, obj);
    } else {
        LOG_TRACE("Flow modify error: %d", rv);
        flow_mod_err_msg_send(rv, obj->version, cxn_id, obj);
    }

 done:
    of_object_delete(obj);

    return rv;
}

/****************************************************************/

/* Flowtable iterator for ind_core_flow_delete_handler */
static void
delete_iter_cb(void *cookie, ft_entry_t *entry)
{
    struct flow_modify_state *state = cookie;

    if (entry != NULL) {
        ind_core_flow_entry_delete(entry, INDIGO_FLOW_REMOVED_DELETE,
                                   state->cxn_id);
    } else {
        LOG_TRACE("Finished flow delete task");
        of_object_delete(state->request);
        INDIGO_MEM_FREE(state);
    }
}


/**
 * Handle a flow_delete message
 * @param cxn_id Connection handler for the owning connection
 * @param _obj Generic type object for the message to be coerced
 * @returns Error code
 */

indigo_error_t
ind_core_flow_delete_handler(of_object_t *_obj, indigo_cxn_id_t cxn_id)
{
    of_flow_delete_t *flow_del;
    of_meta_match_t query;
    indigo_error_t rv;

    flow_del = (of_flow_delete_t *)_obj;
    LOG_TRACE("Handling of_flow_delete message: %p.", flow_del);

    struct flow_modify_state *state = INDIGO_MEM_ALLOC(sizeof(*state));
    if (state == NULL) {
        of_object_delete(_obj);
        return INDIGO_ERROR_RESOURCE;
    }
    state->request = _obj;
    state->num_matched = 0;
    state->cxn_id = cxn_id;

    /* Form the query and call mark entries */
    rv = flow_mod_setup_query((of_flow_modify_t *)flow_del, &query,
                              OF_MATCH_NON_STRICT, 0);
    if (rv != INDIGO_ERROR_NONE) {
        of_object_delete(state->request);
        INDIGO_MEM_FREE(state);
        return rv;
    }

    rv = ft_spawn_iter_task(ind_core_ft, &query, delete_iter_cb, state,
                            IND_SOC_DEFAULT_PRIORITY);
    if (rv != INDIGO_ERROR_NONE) {
        of_object_delete(state->request);
        INDIGO_MEM_FREE(state);
        return rv;
    }

    /* Ownership of _obj is passed to the iterator for barrier tracking */
    return INDIGO_ERROR_NONE;
}

/**
 * Handle a flow_delete_strict message
 * @param cxn_id Connection handler for the owning connection
 * @param _obj Generic type object for the message to be coerced
 * @returns Error code
 */

indigo_error_t
ind_core_flow_delete_strict_handler(of_object_t *_obj, indigo_cxn_id_t cxn_id)
{
    of_flow_delete_strict_t *obj;
    int rv;
    of_meta_match_t query;
    ft_entry_t *entry;

    obj = (of_flow_delete_strict_t *)_obj;
    LOG_TRACE("Handling of_flow_delete_strict message: %p.", obj);

    /* Form the query and call mark entries */
    rv = flow_mod_setup_query((of_flow_modify_t *)obj, &query, OF_MATCH_STRICT, 0);
    if (rv != INDIGO_ERROR_NONE) {
        of_object_delete(_obj);
        return rv;
    }

    if (ft_strict_match(ind_core_ft, &query, &entry) == INDIGO_ERROR_NONE) {
        ind_core_flow_entry_delete(entry, INDIGO_FLOW_REMOVED_DELETE, cxn_id);
    }

    /* ind_core_flow_entry_delete copied _obj for barrier tracking */
    of_object_delete(_obj);

    return INDIGO_ERROR_NONE;
}


/****************************************************************/

static indigo_error_t
get_config_reply_setup(of_get_config_reply_t *reply, of_get_config_request_t *req)
{
    uint32_t xid;

    of_get_config_reply_flags_set(reply, ind_core_of_config.flags);
    of_get_config_reply_miss_send_len_set(reply,
         ind_core_of_config.miss_send_len);

    of_get_config_request_xid_get(req, &xid);
    of_get_config_reply_xid_set(reply, xid);

    return INDIGO_ERROR_NONE;
}

/**
 * Handle a get_config_request message
 * @param cxn_id Connection handler for the owning connection
 * @param _obj Generic type object for the message to be coerced
 * @returns Error code
 */

indigo_error_t
ind_core_get_config_request_handler(of_object_t *_obj, indigo_cxn_id_t cxn_id)
{
    of_get_config_request_t *obj;
    of_get_config_reply_t *reply = NULL;
    int rv;

    obj = (of_get_config_request_t *)_obj;
    LOG_TRACE("Handling of_get_config_request message: %p.", obj);

    /* Allocate reply */
    reply = of_get_config_reply_new(obj->version);
    if (reply == NULL) {
        LOG_TRACE("Could not alloc get_config_reply.");
        of_object_delete(_obj);
        return INDIGO_ERROR_RESOURCE;
    }

    rv = get_config_reply_setup(reply, obj);
    if (rv != INDIGO_ERROR_NONE) {
        of_get_config_reply_delete(reply);
        of_object_delete(_obj);
        return rv;
    }

    rv = IND_CORE_MSG_SEND(cxn_id, reply);

    /* If error, free the reply pointer */
    if (rv != INDIGO_ERROR_NONE) {
        of_get_config_reply_delete(reply);
    }
    of_object_delete(_obj);

    return INDIGO_ERROR_NONE;
}

/****************************************************************/

struct ind_core_flow_stats_state {
    indigo_cxn_id_t cxn_id;
    of_flow_stats_request_t *req;
    indigo_time_t current_time;
    of_flow_stats_reply_t *reply;
};

static void
ind_core_flow_stats_iter(void *cookie, ft_entry_t *entry)
{
    struct ind_core_flow_stats_state *state = cookie;
    uint32_t secs, nsecs;
    indigo_fi_flow_stats_t flow_stats;
    indigo_error_t rv;

    /* Allocate a reply if we don't already have one. */
    if (state->reply == NULL) {
        uint32_t xid;

        state->reply = of_flow_stats_reply_new(state->req->version);
        if (state->reply == NULL) {
            LOG_ERROR("Failed to allocate of_flow_stats_reply.");
            if (entry == NULL) {
                /* This is the last callback, so need to clean up
                 * before returning. */
                of_flow_stats_request_delete(state->req);
                INDIGO_MEM_FREE(state);
            }
            return;
        }

        of_flow_stats_request_xid_get(state->req, &xid);
        of_flow_stats_reply_xid_set(state->reply, xid);
        of_flow_stats_reply_flags_set(state->reply, 1);
    }

    if (entry == NULL) {
        /* Send last reply */
        of_flow_stats_reply_flags_set(state->reply, 0);
        IND_CORE_MSG_SEND(state->cxn_id, state->reply);

        /* Clean up state */
        of_flow_stats_request_delete(state->req);
        INDIGO_MEM_FREE(state);
        return;
    }

    rv = indigo_fwd_flow_stats_get(entry->id, &flow_stats);
    if (rv != INDIGO_ERROR_NONE) {
        LOG_ERROR("Failed to get stats for flow "INDIGO_FLOW_ID_PRINTF_FORMAT": %d",
                  entry->id, rv);
        return;
    }

    /* Skip entry if stats request version is not equal to entry version */
    if (state->req->version != entry->effects.actions->version) {
        LOG_TRACE("Stats request version (%d) differs from entry version (%d). "
                  "Entry is skipped.",
                  state->req->version, entry->effects.actions->version);
        return;
    }

    /* TODO use time from flow_stats? */
    calc_duration(state->current_time, entry->insert_time, &secs, &nsecs);

    /* Set up the structures to append an entry to the list */
    {
        of_list_flow_stats_entry_t list;
        of_flow_stats_entry_t stats_entry;
        of_flow_stats_reply_entries_bind(state->reply, &list);
        of_flow_stats_entry_init(&stats_entry, state->reply->version, -1, 1);
        if (of_list_flow_stats_entry_append_bind(&list, &stats_entry)) {
            LOG_ERROR("failed to append to flow stats list");
            return;
        }

        of_flow_stats_entry_cookie_set(&stats_entry, entry->cookie);
        of_flow_stats_entry_priority_set(&stats_entry, entry->priority);
        of_flow_stats_entry_idle_timeout_set(&stats_entry, entry->idle_timeout);
        of_flow_stats_entry_hard_timeout_set(&stats_entry, entry->hard_timeout);

        if (stats_entry.version >= OF_VERSION_1_3) {
            of_flow_stats_entry_flags_set(&stats_entry, entry->flags);
        }

        if (of_flow_stats_entry_match_set(&stats_entry, &entry->match)) {
            LOG_ERROR("Failed to set match in flow stats entry");
            return;
        }

        if (stats_entry.version == entry->effects.actions->version) {
            if (stats_entry.version == OF_VERSION_1_0) {
                if (of_flow_stats_entry_actions_set(
                        &stats_entry, entry->effects.actions) < 0) {
                    LOG_ERROR("Failed to set actions list of flow stats entry");
                    return;
                }
            } else {
                if (of_flow_stats_entry_instructions_set(
                        &stats_entry, entry->effects.instructions) < 0) {
                    LOG_ERROR("Failed to set instructions list of flow stats entry");
                    return;
                }
            }
        }

        of_flow_stats_entry_table_id_set(&stats_entry, entry->table_id);
        of_flow_stats_entry_duration_sec_set(&stats_entry, secs);
        of_flow_stats_entry_duration_nsec_set(&stats_entry, nsecs);
        of_flow_stats_entry_packet_count_set(&stats_entry, flow_stats.packets);
        of_flow_stats_entry_byte_count_set(&stats_entry, flow_stats.bytes);
    }

    if (state->reply->length > (1 << 15)) { /* Last object would get too big */
        IND_CORE_MSG_SEND(state->cxn_id, state->reply);
        state->reply = NULL;
    }
}

/**
 * Handle a flow_stats_request message
 * @param _obj Generic type object for the message to be coerced
 * @param cxn_id Connection handler for the owning connection
 * @returns Error code
 */

indigo_error_t
ind_core_flow_stats_request_handler(of_object_t *_obj, indigo_cxn_id_t cxn_id)
{
    of_flow_stats_request_t *obj;
    of_meta_match_t query;
    struct ind_core_flow_stats_state *state;
    indigo_error_t rv;

    obj = (of_flow_stats_request_t *)_obj;
    LOG_TRACE("Handling of_flow_stats_request message: %p.", obj);

    /* Set up the query structure */
    INDIGO_MEM_SET(&query, 0, sizeof(query));
    if (of_flow_stats_request_match_get(obj, &(query.match)) < 0) {
        LOG_ERROR("Failed to get flow stats match.");
        of_object_delete(_obj);
        return INDIGO_ERROR_UNKNOWN;
    }
    of_flow_stats_request_out_port_get(obj, &(query.out_port));
    of_flow_stats_request_table_id_get(obj, &(query.table_id));
    if (obj->version >= OF_VERSION_1_1) {
        of_flow_stats_request_cookie_get(obj, &query.cookie);
        of_flow_stats_request_cookie_mask_get(obj, &query.cookie_mask);
    }

    /* Non strict; do not check priority or overlap */
    query.mode = OF_MATCH_NON_STRICT;

    state = INDIGO_MEM_ALLOC(sizeof(*state));
    if (state == NULL) {
       LOG_ERROR("Failed to allocate flow stats state object.");
       of_object_delete(_obj);
       return INDIGO_ERROR_RESOURCE;
    }

    state->req = obj; /* ownership transferred */
    state->cxn_id = cxn_id;
    state->current_time = INDIGO_CURRENT_TIME;
    state->reply = NULL;

    rv = ft_spawn_iter_task(ind_core_ft, &query, ind_core_flow_stats_iter, 
                            state, IND_SOC_DEFAULT_PRIORITY);
    if (rv != INDIGO_ERROR_NONE) {
        LOG_ERROR("Failed to start flow stats iter.");
        of_object_delete(_obj);
        INDIGO_MEM_FREE(state);
        return rv;
    }

    /* Ownership of _obj is passed to the iterator for barrier tracking */
    return INDIGO_ERROR_NONE;
}

/****************************************************************/

/**
 * Handle a echo_reply message
 * @param cxn_id Connection handler for the owning connection
 * @param _obj Generic type object for the message to be coerced
 * @returns Error code
 */

indigo_error_t
ind_core_echo_reply_handler(of_object_t *_obj, indigo_cxn_id_t cxn_id)
{
    of_echo_reply_t *obj;

    obj = (of_echo_reply_t *)_obj;
    LOG_TRACE("Handling of_echo_reply message: %p.", obj);

    /* Handle object of type of_echo_reply_t */

    return INDIGO_ERROR_NONE;
}

/****************************************************************/

struct ind_core_aggregate_stats_state {
    uint64_t packets;
    uint64_t bytes;
    uint32_t flows;
    indigo_cxn_id_t cxn_id;
    of_aggregate_stats_request_t *req;
};

static void
ind_core_aggregate_stats_iter(void *cookie, ft_entry_t *entry)
{
    struct ind_core_aggregate_stats_state *state = cookie;
    indigo_error_t rv;

    if (entry != NULL) {
        indigo_fi_flow_stats_t flow_stats;
        rv = indigo_fwd_flow_stats_get(entry->id, &flow_stats);
        if (rv != INDIGO_ERROR_NONE) {
            LOG_ERROR("Failed to get stats for flow "INDIGO_FLOW_ID_PRINTF_FORMAT": %d",
                      entry->id, rv);
            return;
        }

        state->bytes += flow_stats.bytes;
        state->packets += flow_stats.packets;
        state->flows += 1;
    } else {
        uint32_t xid;
        of_aggregate_stats_reply_t* reply;
        of_aggregate_stats_request_xid_get(state->req, &xid);
        reply = of_aggregate_stats_reply_new(state->req->version);
        if (reply != NULL) {
            of_aggregate_stats_reply_xid_set(reply, xid);
            of_aggregate_stats_reply_byte_count_set(reply, state->bytes);
            of_aggregate_stats_reply_packet_count_set(reply, state->packets);
            of_aggregate_stats_reply_flow_count_set(reply, state->flows);
            IND_CORE_MSG_SEND(state->cxn_id, reply);
        } else {
            LOG_ERROR("Failed to allocate aggregate stats reply.");
        }
        of_aggregate_stats_request_delete(state->req);
        INDIGO_MEM_FREE(state);
    }
}

/**
 * Handle a aggregate_stats_request message
 * @param cxn_id Connection handler for the owning connection
 * @param _obj Generic type object for the message to be coerced
 * @returns Error code
 */

indigo_error_t
ind_core_aggregate_stats_request_handler(of_object_t *_obj,
                                         indigo_cxn_id_t cxn_id)
{
    of_aggregate_stats_request_t *obj;
    of_meta_match_t query;
    struct ind_core_aggregate_stats_state *state;
    indigo_error_t rv;

    obj = (of_aggregate_stats_request_t *)_obj;
    LOG_TRACE("Handling of_aggregate_stats_request message: %p.", obj);

    /* Set up the query structure */
    INDIGO_MEM_SET(&query, 0, sizeof(query));
    if (of_aggregate_stats_request_match_get(obj, &(query.match)) < 0) {
        LOG_ERROR("Failed to get aggregate stats match.");
        of_object_delete(_obj);
        return INDIGO_ERROR_UNKNOWN;
    }
    of_aggregate_stats_request_out_port_get(obj, &(query.out_port));
    of_aggregate_stats_request_table_id_get(obj, &(query.table_id));
    if (obj->version >= OF_VERSION_1_1) {
        of_aggregate_stats_request_cookie_get(obj, &query.cookie);
        of_aggregate_stats_request_cookie_mask_get(obj, &query.cookie_mask);
    }

    /* Non strict; do not check priority or overlap */
    query.mode = OF_MATCH_NON_STRICT;

    state = INDIGO_MEM_ALLOC(sizeof(*state));
    if (state == NULL) {
       LOG_ERROR("Failed to allocate flow stats state object.");
       of_object_delete(_obj);
       return INDIGO_ERROR_RESOURCE;
    }

    state->cxn_id = cxn_id;
    state->req = obj; /* ownership transferred */
    state->packets = 0;
    state->bytes = 0;
    state->flows = 0;

    rv = ft_spawn_iter_task(ind_core_ft, &query, ind_core_aggregate_stats_iter,
                            state, IND_SOC_DEFAULT_PRIORITY);
    if (rv != INDIGO_ERROR_NONE) {
        LOG_ERROR("Failed to start aggregate stats iter.");
        of_object_delete(_obj);
        INDIGO_MEM_FREE(state);
        return rv;
    }

    /* Ownership of _obj is passed to the iterator for barrier tracking */
    return INDIGO_ERROR_NONE;
}

/****************************************************************/

/**
 * Handle a desc_stats_request message
 * @param cxn_id Connection handler for the owning connection
 * @param _obj Generic type object for the message to be coerced
 * @returns Error code
 */

indigo_error_t
ind_core_desc_stats_request_handler(of_object_t *_obj, indigo_cxn_id_t cxn_id)
{
    of_desc_stats_request_t *obj;
    of_desc_stats_reply_t *reply;
    uint32_t xid;
    int rv;
    ind_core_desc_stats_t *data;

    obj = (of_desc_stats_request_t *)_obj;
    LOG_TRACE("Handling of_desc_stats_request message: %p.", obj);

    /* Create reply and send to controller */
    if ((reply = of_desc_stats_reply_new(obj->version)) == NULL) {
        LOG_ERROR("Failed to create desc stats reply message");
        of_object_delete(_obj);
        return INDIGO_ERROR_NONE; /* @fixme Error handling in this case? */
    }

    of_desc_stats_request_xid_get(obj, &xid);
    of_desc_stats_reply_xid_set(reply, xid);

    data = &ind_core_of_config.desc_stats;
    of_desc_stats_reply_sw_desc_set(reply, data->sw_desc);
    of_desc_stats_reply_hw_desc_set(reply, data->hw_desc);
    of_desc_stats_reply_dp_desc_set(reply, data->dp_desc);
    of_desc_stats_reply_mfr_desc_set(reply, data->mfr_desc);
    of_desc_stats_reply_serial_num_set(reply, data->serial_num);
    of_desc_stats_reply_flags_set(reply, 0);

    if ((rv = IND_CORE_MSG_SEND(cxn_id, reply)) < 0) {
        LOG_ERROR("Error sending desc stats response to %d", cxn_id);
        of_object_delete(_obj);
        return rv;
    }

    of_object_delete(_obj);
    return INDIGO_ERROR_NONE;
}

/****************************************************************/

/**
 * Handle a table_stats_request message
 * @param cxn_id Connection handler for the owning connection
 * @param _obj Generic type object for the message to be coerced
 * @returns Error code
 */

indigo_error_t
ind_core_table_stats_request_handler(of_object_t *_obj, indigo_cxn_id_t cxn_id)
{
    of_table_stats_request_t *obj = _obj;
    of_table_stats_request_t *reply = NULL;
    indigo_error_t rv;

    LOG_TRACE("Handling of_table_stats_request message.");

    rv = indigo_fwd_table_stats_get(obj, &reply);
    if (rv < 0) {
        reply = NULL;
        LOG_ERROR("Table stats returned error %d", rv);
        goto done;
    }

    rv = IND_CORE_MSG_SEND(cxn_id, reply);
    reply = NULL;
    if (rv < 0) {
        LOG_ERROR("Error %d sending table_stats reply to cxn %d", rv, cxn_id);
        goto done;
    }

done:
    of_table_stats_reply_delete(reply);
    of_table_stats_request_delete(obj);

    return rv;
}

/****************************************************************/

/**
 * Handle a port_desc_stats_request message
 * @param cxn_id Connection handler for the owning connection
 * @param _obj Generic type object for the message to be coerced
 * @returns Error code
 */

indigo_error_t
ind_core_port_desc_stats_request_handler(of_object_t *_obj, indigo_cxn_id_t cxn_id)
{
    uint32_t xid;
    of_port_desc_stats_request_t *obj;
    of_port_desc_stats_reply_t *reply;
    int rv;

    obj = (of_port_desc_stats_request_t *)_obj;
    LOG_TRACE("Handling of_port_desc_stats_request message: %p.", obj);

    /* Generate a port_desc_stats reply and send to controller */
    if ((reply = of_port_desc_stats_reply_new(obj->version)) == NULL) {
        LOG_ERROR("Failed to create port_desc_stats reply message");
        of_object_delete(_obj);
#ifdef OFDPA_FIXUP
        return INDIGO_ERROR_RESOURCE;
#else
        return INDIGO_ERROR_NONE; /* @fixme Error handling in this case? */
#endif
    }

    of_port_desc_stats_request_xid_get(obj, &xid);
    of_port_desc_stats_reply_xid_set(reply, xid);
    indigo_port_desc_stats_get(reply);

    of_port_desc_stats_request_delete(obj);

    if ((rv = IND_CORE_MSG_SEND(cxn_id, reply)) < 0) {
        LOG_ERROR("Error sending port_desc_stats response to %d", cxn_id);
        return rv;
    }

    return INDIGO_ERROR_NONE;
}

/****************************************************************/

/**
 * Handle a features_request message
 * @param cxn_id Connection handler for the owning connection
 * @param _obj Generic type object for the message to be coerced
 * @returns Error code
 */

indigo_error_t
ind_core_features_request_handler(of_object_t *_obj, indigo_cxn_id_t cxn_id)
{
    of_features_request_t *obj;
    of_features_reply_t *reply;
    uint32_t xid;
    int rv;
    of_dpid_t dpid;

    obj = (of_features_request_t *)_obj;
    LOG_TRACE("Handling of_features_request message: %p.", obj);

    /* Generate a features reply and send to controller */
    if ((reply = of_features_reply_new(obj->version)) == NULL) {
        LOG_ERROR("Failed to create features reply message");
        of_object_delete(_obj);
#ifdef OFDPA_FIXUP
        return INDIGO_ERROR_RESOURCE;
#else
        return INDIGO_ERROR_NONE; /* @fixme Error handling in this case? */
#endif
    }

    of_features_request_xid_get(obj, &xid);
    of_features_reply_xid_set(reply, xid);
    _TRY_NR(indigo_core_dpid_get(&dpid));
    of_features_reply_datapath_id_set(reply, dpid);
    of_features_reply_n_buffers_set(reply, 0);
    _TRY_NR(indigo_fwd_forwarding_features_get(reply));
    _TRY_NR(indigo_port_features_get(reply));

    of_features_request_delete(obj);

    if ((rv = IND_CORE_MSG_SEND(cxn_id, reply)) < 0) {
        LOG_ERROR("Error sending features response to %d", cxn_id);
        return rv;
    }

    return INDIGO_ERROR_NONE;
}

/**
 * Handle a table_mod message
 * @param cxn_id Connection handler for the owning connection
 * @param _obj Generic type object for the message to be coerced
 * @returns Error code
 */

indigo_error_t
ind_core_table_mod_handler(of_object_t *_obj, indigo_cxn_id_t cxn_id)
{
    of_table_mod_t *obj;

    obj = (of_table_mod_t *)_obj;
    LOG_TRACE("Handling of_table_mod message: %p.", obj);

    /* Handle object of type of_table_mod_t */

    of_object_delete(_obj);

    return INDIGO_ERROR_NONE;
}

/**
 * Handle a set_config message
 * @param cxn_id Connection handler for the owning connection
 * @param _obj Generic type object for the message to be coerced
 * @returns Error code
 */

indigo_error_t
ind_core_set_config_handler(of_object_t *_obj, indigo_cxn_id_t cxn_id)
{
    of_set_config_t *obj;

    obj = (of_set_config_t *)_obj;
    LOG_TRACE("Handling of_set_config message: %p.", obj);

    /* Handle object of type of_set_config_t */
    ind_core_of_config.config_set_done = 1;
    of_set_config_flags_get(obj, &ind_core_of_config.flags);
    LOG_VERBOSE("Config flags set to 0x%x", ind_core_of_config.flags);
    of_set_config_miss_send_len_get(obj, &ind_core_of_config.miss_send_len);
    LOG_VERBOSE("Miss send len set to %d", ind_core_of_config.miss_send_len);

    of_object_delete(_obj);

    return INDIGO_ERROR_NONE;
}

/**
 * Handle an experimenter message
 * @param cxn_id Connection handler for the owning connection
 * @param _obj Generic type object for the message to be coerced
 * @returns Error code
 *
 * The state manager has no specific support for experimenter messages.
 * However, the port or forwarding modules may have that support
 * independent of the state manager.  For this reason, the state
 * manager calls both the port manager and forwarding modules with
 * the request.
 *
 * Currently there is no support for asynchronous experimenter message
 * handling at this layer (so barriers currently will not track experimenter
 * messages correctly).  However, the port and forwarding modules
 * each get their own copy of the message.
 *
 * The port and forwarding modules must respond as follows:
 *    INDIGO_ERROR_NONE:  Supported request, no error.
 *    INDIGO_ERROR_NOT_SUPPORTED:  Module does not support the request
 *    INDIGO_ERROR_xxx: Supported request, but error occurred.
 *
 * If both modules respond with NOT_SUPPORTED, then an unknown message
 * type error is generated and sent back to the controller connection.
 *
 * If either module responds with NONE, the operation is considered
 * successful.
 *
 * Otherwise, if either module responds with an error other than
 * supported, behavior is TBD.
 *
 */

indigo_error_t
ind_core_experimenter_handler(of_object_t *_obj, indigo_cxn_id_t cxn_id)
{
    of_experimenter_t *fwd_obj;
    of_experimenter_t *port_obj;
    indigo_error_t fwd_rv;
    indigo_error_t port_rv;
    indigo_error_t rv = INDIGO_ERROR_NONE;
    uint32_t xid = 0;
    of_version_t version;

    fwd_obj = (of_experimenter_t *)_obj;
    port_obj = of_object_dup(_obj);

    if (port_obj == NULL) {
        LOG_ERROR("Could not allocate port experimenter object");
        of_object_delete(_obj);
        return INDIGO_ERROR_RESOURCE;
    }

    /* Record data before calling datapath funs which own objs */
    version = fwd_obj->version;
    of_object_xid_get(_obj, &xid);

    LOG_TRACE("Handling of_experimenter message: %p.", fwd_obj);

    /* Handle object of type of_experimenter_t */
    if ((fwd_rv = indigo_fwd_experimenter(fwd_obj, cxn_id)) < 0) {
        LOG_TRACE("Error %d from fwd_experimenter", fwd_rv);
    }
    if ((port_rv = indigo_port_experimenter(port_obj, cxn_id)) < 0) {
        LOG_TRACE("Error %d from port_experimenter", port_rv);
    }

    if ((fwd_rv == INDIGO_ERROR_NOT_SUPPORTED) &&
        (port_rv == INDIGO_ERROR_NOT_SUPPORTED)) {
        indigo_cxn_send_error_msg(version, cxn_id, xid,
                                  OF_ERROR_TYPE_BAD_REQUEST,
                                  OF_REQUEST_FAILED_BAD_EXPERIMENTER, NULL);
    } else if ((fwd_rv != INDIGO_ERROR_NONE) &&
               (port_rv != INDIGO_ERROR_NONE)) {
        /* Not handled and some error */
        if (port_rv != INDIGO_ERROR_NONE) {
            rv = port_rv;
        }
        if (fwd_rv != INDIGO_ERROR_NONE) {
            rv = fwd_rv;
        }
    }

    if (rv < 0) {
        LOG_VERBOSE("Error handling experimenter message in port or fwding");
    }

    of_experimenter_delete(fwd_obj);
    of_experimenter_delete(port_obj);

    return rv;
}


/**
 * Handle a experimenter_stats_request message
 * @param cxn_id Connection handler for the owning connection
 * @param _obj Generic type object for the message to be coerced
 * @returns Error code
 *
 * Currently not handled; returns error message to controller
 */

indigo_error_t
ind_core_experimenter_stats_request_handler(of_object_t *_obj,
                                            indigo_cxn_id_t cxn_id)
{
    of_experimenter_stats_request_t *obj;

    obj = (of_experimenter_stats_request_t *)_obj;
    LOG_TRACE("Handling of_experimenter_stats_request message: %p.", obj);

    /* Handle object of type of_experimenter_stats_request_t */

    ind_core_unhandled_message(_obj, cxn_id);

    return INDIGO_ERROR_NONE;
}

/****************************************************************
 *
 * Extension message handling
 *
 ****************************************************************/

/**
 * Handle a BSN set IP mask
 * @param cxn_id Connection handler for the owning connection
 * @param _obj Generic type object for the message to be coerced
 * @returns Error code
 */

indigo_error_t
ind_core_bsn_set_ip_mask_handler(of_object_t *_obj, indigo_cxn_id_t cxn_id)
{
    of_bsn_set_ip_mask_t *obj;
    uint8_t index;
    uint32_t mask;
    uint32_t xid;

    obj = (of_bsn_set_ip_mask_t *)_obj;
    of_bsn_set_ip_mask_xid_get(obj, &xid);

    LOG_TRACE("Received BSN set IP mask message from %d", cxn_id);

    of_bsn_set_ip_mask_index_get(obj, &index);
    of_bsn_set_ip_mask_mask_get(obj, &mask);
    of_bsn_set_ip_mask_delete(obj);

    if (of_ip_mask_map_set((int)index, mask) < 0) {
        LOG_ERROR("Bad index for set ip_mask: %d", index);
        /* @todo sending type 0, code 0 error message */
        if (indigo_cxn_send_error_msg(obj->version, cxn_id, xid,
                                      0, 0, NULL) < 0) {
            LOG_ERROR("Error sending error message for set ip mask msg, "
                      "cxn id %d", cxn_id);
        }
        return INDIGO_ERROR_NONE;
    }
    LOG_TRACE("ip_mask: Set index %d to 0x%x", index, mask);

    return INDIGO_ERROR_NONE;
}

/**
 * Handle a BSN get IP mask request
 * @param cxn_id Connection handler for the owning connection
 * @param _obj Generic type object for the message to be coerced
 * @returns Error code
 */

indigo_error_t
ind_core_bsn_get_ip_mask_request_handler(of_object_t *_obj,
                                         indigo_cxn_id_t cxn_id)
{
    of_bsn_get_ip_mask_request_t *obj;
    of_bsn_get_ip_mask_reply_t *reply;
    uint32_t val32;
    uint8_t index;
    int rv;
    uint32_t xid;

    obj = (of_bsn_get_ip_mask_request_t *)_obj;

    LOG_TRACE("Received BSN get IP mask request message from %d", cxn_id);

    /* Create reply and send to controller */
    if ((reply = of_bsn_get_ip_mask_reply_new(obj->version)) == NULL) {
        LOG_ERROR("Failed to create ip mask reply message");
        of_bsn_get_ip_mask_request_delete(obj);
        return INDIGO_ERROR_NONE; /* @fixme Error handling in this case? */
    }

    of_bsn_get_ip_mask_request_xid_get(obj, &xid);
    of_bsn_get_ip_mask_reply_xid_set(reply, xid);

    of_bsn_get_ip_mask_request_index_get(obj, &index);
    of_bsn_get_ip_mask_reply_index_set(reply, index);

    of_bsn_get_ip_mask_request_delete(obj);

    if (of_ip_mask_map_get((int)index, &val32) < 0) {
        LOG_ERROR("Bad index for get ip_mask: %d", index);
        /* @todo sending type 0, code 0 error message */
        if (indigo_cxn_send_error_msg(obj->version, cxn_id, xid,
                                      0, 0, NULL) < 0) {
            LOG_ERROR("Error sending error message for get ip mask msg, "
                      "cxn id %d", cxn_id);
        }
        return INDIGO_ERROR_NONE;
    }
    of_bsn_get_ip_mask_reply_mask_set(reply, val32);

    if ((rv = IND_CORE_MSG_SEND(cxn_id, reply)) < 0) {
        LOG_ERROR("Error sending get ip mask response to %d", cxn_id);
        return rv;
    }
    return INDIGO_ERROR_NONE;
}

/**
 * Handle a BSN hybrid get request
 * @param cxn_id Connection handler for the owning connection
 * @param _obj Generic type object for the message to be coerced
 * @returns Error code
 */

indigo_error_t
ind_core_bsn_hybrid_get_request_handler(of_object_t *_obj,
                                        indigo_cxn_id_t cxn_id)
{
    of_bsn_hybrid_get_request_t *obj;
    of_bsn_hybrid_get_reply_t *reply;
    int rv;
    uint32_t xid;

    obj = (of_bsn_hybrid_get_request_t *)_obj;

    LOG_TRACE("Received BSN hybrid_get message from %d", cxn_id);

    /* Create reply and send to controller */
    if ((reply = of_bsn_hybrid_get_reply_new(obj->version)) == NULL) {
        LOG_ERROR("Failed to create hybrid_get reply message");
        of_bsn_hybrid_get_request_delete(obj);
        return INDIGO_ERROR_NONE; /* @fixme Error handling in this case? */
    }

    of_bsn_hybrid_get_request_xid_get(obj, &xid);
    of_bsn_hybrid_get_reply_xid_set(reply, xid);
    of_bsn_hybrid_get_reply_hybrid_enable_set(reply, 1);
    of_bsn_hybrid_get_reply_hybrid_version_set(reply, 0);

    of_bsn_hybrid_get_request_delete(obj);

    if ((rv = IND_CORE_MSG_SEND(cxn_id, reply)) < 0) {
        LOG_ERROR("Error sending hybrid_get response to %d", cxn_id);
        return rv;
    }
    return INDIGO_ERROR_NONE;
}
