/*
 *  <:copyright-BRCM:2016-2020:Apache:standard
 *  
 *   Copyright (c) 2016-2020 Broadcom. All Rights Reserved
 *  
 *   The term “Broadcom” refers to Broadcom Inc. and/or its subsidiaries
 *  
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *  
 *       http://www.apache.org/licenses/LICENSE-2.0
 *  
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 *  
 *  :>
 *
 *****************************************************************************/

/*
 * bbf-xpon.c
 */
#include <bcmos_system.h>
#include <bcmolt_netconf_module_utils.h>
#include <bcmolt_netconf_constants.h>
#include <bbf-vomci.h>
#include <bcm_tr451_polt.h>
#include <bcmolt_netconf_notifications.h>

#ifndef TR451_VOMCI
#error TR451_VOMCI support is required
#endif

#define BCM_MAX_YANG_NAME_LENGTH    32

static sr_subscription_ctx_t *sr_ctx;
static sr_subscription_ctx_t *sr_ctx_state;

static const char* bbf_polt_vomci_features[] = {
    "nf-client-supported",
    "nf-server-supported",
    NULL
};

#define  CLIENT_REMOTE_ENDPOINT_XPATH "client-parameters/nf-initiate/remote-endpoints"

/* Data store change indication callback */
static int bbf_polt_vomci_server_change_cb(sr_session_ctx_t *srs, const char *module_name,
    const char *xpath, sr_event_t event, uint32_t request_id, void *private_ctx)
{
    sr_change_iter_t *sr_iter = NULL;
    sr_change_oper_t sr_oper;
    const struct lyd_node *node;
    const char *prev_val, *prev_list;
    bool prev_dflt;
    tr451_endpoint endpoint = {};
    tr451_polt_filter filter = {};
    const char *filter_endpoint_name = NULL;
    int sr_rc;
    bcmos_errno err = BCM_ERR_OK;

    NC_LOG_INFO("xpath=%s event=%d\n", xpath, event);

    /* We only handle CHANGE and ABORT events.
     * Since there is no way to reserve resources in advance and no way to fail the APPLY event,
     * configuration is applied in VERIFY event.
     * There are no other verifiers, but if there are and they fail,
     * ABORT event will roll-back the changes.
     */
    if (event == SR_EV_DONE)
        return SR_ERR_OK;

    sr_rc = sr_get_changes_iter(srs, "//.", &sr_iter);
    while ((err == BCM_ERR_OK) && (sr_rc == SR_ERR_OK) &&
           ((sr_rc = sr_get_change_tree_next(srs, sr_iter, &sr_oper,
                &node, &prev_val, &prev_list, &prev_dflt)) == SR_ERR_OK))
    {
        const char *node_name = node->schema->name;

        NC_LOG_DBG("op=%s node=%s\n", sr_op_name(sr_oper), node_name);

        /* Handle attributes */
        if (!strcmp(node_name, "enabled"))
        {
            bcm_tr451_polt_grpc_server_enable_disable(
                (sr_oper != SR_OP_DELETED) &&
                 ((const struct lyd_node_leaf_list *)node)->value.bln);
            continue;
        }

        if (sr_oper != SR_OP_DELETED)
        {
            if (!strcmp(node_name, "local-port") || !strcmp(node_name, "local-address"))
            {
                /* access points */
                const struct lyd_node *endpoint_name_node;
                const char *endpoint_name;

                endpoint_name_node = nc_ly_get_sibling_or_parent_node(node, "name");
                if (endpoint_name_node == NULL)
                {
                    NC_LOG_ERR("can't find listen-endpoint node\n");
                    continue;
                }
                NC_LOG_DBG("Handling listen-endpoint[%s]\n", endpoint_name_node->schema->name);
                endpoint_name = ((const struct lyd_node_leaf_list *)endpoint_name_node)->value.string;
                if (endpoint.name != NULL && strcmp(endpoint.name, endpoint_name))
                {
                    tr451_server_endpoint server_ep = { .endpoint = endpoint };
                    err = bcm_tr451_polt_grpc_server_create(&server_ep);
                    memset(&endpoint, 0, sizeof(endpoint));
                }
                endpoint.name = endpoint_name;
                if (!strcmp(node_name, "local-port"))
                    endpoint.port = ((const struct lyd_node_leaf_list *)node)->value.uint16;
                else if (!strcmp(node_name, "local-address"))
                    endpoint.host_name = ((const struct lyd_node_leaf_list *)node)->value_str;
            }
            else if (!strcmp(node_name, "priority") || !strcmp(node_name, "resulting-endpoint") ||
                    !strcmp(node_name, "any-onu") || !strcmp(node_name, "onu-vendor") ||
                    !strcmp(node_name, "onu-serial-number"))
            {
                /* endpoint filter */
                const struct lyd_node *filter_name_node;
                const char *rule_name;

                /* Commit endpoint if any */
                if (endpoint.name)
                {
                    tr451_server_endpoint server_ep = { .endpoint = endpoint };
                    err = bcm_tr451_polt_grpc_server_create(&server_ep);
                    memset(&endpoint, 0, sizeof(endpoint));
                }

                filter_name_node = nc_ly_get_sibling_or_parent_node(node, "name");
                if (filter_name_node == NULL)
                {
                    NC_LOG_ERR("can't find nf-endpoint-filter/rule\n");
                    continue;
                }
                rule_name = ((const struct lyd_node_leaf_list *)filter_name_node)->value.string;
                /* Filter rule changed ? */
                if (filter.name && strcmp(filter.name, rule_name))
                {
                    if (filter_endpoint_name)
                    {
                        err = bcm_tr451_polt_grpc_server_filter_set(&filter, filter_endpoint_name);
                    }
                    memset(&filter, 0, sizeof(filter));
                    filter_endpoint_name = NULL;
                }
                filter.name = rule_name;
                if (!strcmp(node_name, "priority"))
                    filter.priority = ((const struct lyd_node_leaf_list *)node)->value.uint16;
                else if (!strcmp(node_name, "resulting-endpoint"))
                    filter_endpoint_name = ((const struct lyd_node_leaf_list *)node)->value_str;
                else if (!strcmp(node_name, "any-onu"))
                    filter.type = TR451_FILTER_TYPE_ANY;
                else if (!strcmp(node_name, "onu-vendor"))
                {
                    filter.type = TR451_FILTER_TYPE_VENDOR_ID;
                    strncpy((char *)&filter.serial_number[0], ((const struct lyd_node_leaf_list *)node)->value.string, 4);
                }
                else if (!strcmp(node_name, "onu-serial-number"))
                {
                    const char *serial_number = ((const struct lyd_node_leaf_list *)node)->value.string;
                    if (serial_number == NULL || strlen(serial_number) < 6)
                    {
                        NC_LOG_ERR("invalid onu-serial-number: NULL or too short\n");
                        continue;
                    }
                    strncpy((char *)&filter.serial_number[0], serial_number, 4);
                    /* Serial number consists of 4xASCII vendor_id + 8xHex string vendor-specific id */
                    if (nc_hex_to_bin(serial_number + 4, &filter.serial_number[4], 4) < 0)
                    {
                        NC_LOG_ERR("invalid onu-serial-number format: %s\n", serial_number);
                        continue;
                    }
                    filter.type = TR451_FILTER_TYPE_SERIAL_NUMBER;
                }
            }
        }
        else
        {
            const struct lyd_node *endpoint_node;
            const struct lyd_node *filter_node;
            const char *entity_name = ((const struct lyd_node_leaf_list *)node)->value.string;

            /* Deleted */
            if (!strcmp(node_name, "name"))
            {
                endpoint_node = nc_ly_get_sibling_or_parent_node(node, "listen-endpoint");
                filter_node = nc_ly_get_sibling_or_parent_node(node, "rule");
                if (filter_node)
                {
                    err = bcm_tr451_polt_grpc_server_filter_delete(entity_name);
                }
                else if (endpoint_node != NULL)
                {
                    err = bcm_tr451_polt_grpc_server_delete(entity_name);
                }
            }
        }
    }

    if (endpoint.name && err == BCM_ERR_OK)
    {
        tr451_server_endpoint server_ep = { .endpoint = endpoint };
        err = bcm_tr451_polt_grpc_server_create(&server_ep);
    }
    if (filter.name)
    {
        err = bcm_tr451_polt_grpc_server_filter_set(&filter, filter_endpoint_name);
    }

    sr_free_change_iter(sr_iter);

    return nc_bcmos_errno_to_sr_errno(err);
}

/* Data store change indication callback */
static int bbf_polt_vomci_client_change_cb(sr_session_ctx_t *srs, const char *module_name,
    const char *xpath, sr_event_t event, uint32_t request_id, void *private_ctx)
{
    sr_change_iter_t *sr_iter = NULL;
    sr_change_oper_t sr_oper;
    const struct lyd_node *node;
    const char *prev_val, *prev_list;
    bool prev_dflt;
    tr451_client_endpoint *endpoint = NULL;
    tr451_endpoint entry = {};
    tr451_polt_filter filter = {};
    const char *filter_endpoint_name = NULL;
    int sr_rc;
    bcmos_errno err = BCM_ERR_OK;

    NC_LOG_INFO("xpath=%s event=%d\n", xpath, event);

    /* We only handle CHANGE and ABORT events.
     * Since there is no way to reserve resources in advance and no way to fail the APPLY event,
     * configuration is applied in VERIFY event.
     * There are no other verifiers, but if there are and they fail,
     * ABORT event will roll-back the changes.
     */
    if (event == SR_EV_DONE)
        return SR_ERR_OK;

    sr_rc = sr_get_changes_iter(srs, "//.", &sr_iter);
    while ((err == BCM_ERR_OK) && (sr_rc == SR_ERR_OK) &&
           ((sr_rc = sr_get_change_tree_next(srs, sr_iter, &sr_oper,
                &node, &prev_val, &prev_list, &prev_dflt)) == SR_ERR_OK))
    {
        const char *node_name = node->schema->name;

        NC_LOG_DBG("op=%s node=%s\n", sr_op_name(sr_oper), node_name);

        /* Handle attributes */
        if (!strcmp(node_name, "enabled"))
        {
            bcm_tr451_polt_grpc_client_enable_disable(
                (sr_oper != SR_OP_DELETED) &&
                 ((const struct lyd_node_leaf_list *)node)->value.bln);
            continue;
        }

        if (sr_oper != SR_OP_DELETED)
        {
            if (!strcmp(node_name, "remote-port") || !strcmp(node_name, "remote-address"))
            {
                /* access points */
                const struct lyd_node *access_name_node;
                const struct lyd_node *endpoint_name_node;
                const char *endpoint_name;
                const char *access_point_name;

                access_name_node = nc_ly_get_sibling_or_parent_node(node, "name");
                NC_LOG_DBG("access-node %s\n", access_name_node ? access_name_node->schema->name : "*undefined*");
                if (access_name_node == NULL)
                {
                    NC_LOG_ERR("can't find access-node\n");
                    continue;
                }
                endpoint_name_node = nc_ly_get_sibling_or_parent_node(access_name_node, "name");
                if (endpoint_name_node == NULL)
                {
                    NC_LOG_ERR("can't find remote-endpoint node\n");
                    continue;
                }
                NC_LOG_DBG("Handling remote-endpoint[%s]/access-points[%s]\n",
                    endpoint_name_node->schema->name, access_name_node->schema->name);
                endpoint_name = ((const struct lyd_node_leaf_list *)endpoint_name_node)->value.string;
                access_point_name = ((const struct lyd_node_leaf_list *)access_name_node)->value.string;
                if (endpoint != NULL && strcmp(endpoint->name, endpoint_name))
                {
                    err = bcm_tr451_polt_grpc_client_create(endpoint);
                    endpoint = NULL;
                }
                if (endpoint == NULL)
                {
                    endpoint = bcm_tr451_client_endpoint_alloc(endpoint_name);
                }
                /* access-point name changed ? */
                if (entry.name && strcmp(entry.name, access_point_name))
                {
                    bcm_tr451_client_endpoint_add_entry(endpoint, &entry);
                    memset(&entry, 0, sizeof(entry));
                }
                entry.name = access_point_name;
                if (!strcmp(node_name, "remote-port"))
                    entry.port = ((const struct lyd_node_leaf_list *)node)->value.uint16;
                else if (!strcmp(node_name, "remote-address"))
                    entry.host_name = ((const struct lyd_node_leaf_list *)node)->value_str;
            }
            else if (!strcmp(node_name, "priority") || !strcmp(node_name, "resulting-endpoint") ||
                    !strcmp(node_name, "any-onu") || !strcmp(node_name, "onu-vendor") ||
                    !strcmp(node_name, "onu-serial-number"))
            {
                /* endpoint filter */
                const struct lyd_node *filter_name_node;
                const char *rule_name;

                /* Commit endpoint if any */
                if (endpoint)
                {
                    if (entry.name)
                    {
                        bcm_tr451_client_endpoint_add_entry(endpoint, &entry);
                        memset(&entry, 0, sizeof(entry));
                    }
                    err = bcm_tr451_polt_grpc_client_create(endpoint);
                    if (err != BCM_ERR_OK)
                        break;
                    endpoint = NULL;
                }

                filter_name_node = nc_ly_get_sibling_or_parent_node(node, "name");
                if (filter_name_node == NULL)
                {
                    NC_LOG_ERR("can't find nf-endpoint-filter/rule\n");
                    continue;
                }
                rule_name = ((const struct lyd_node_leaf_list *)filter_name_node)->value.string;
                /* Filter rule changed ? */
                if (filter.name && strcmp(filter.name, rule_name))
                {
                    if (filter_endpoint_name)
                    {
                        err = bcm_tr451_polt_grpc_client_filter_set(&filter, filter_endpoint_name);
                    }
                    memset(&filter, 0, sizeof(filter));
                    filter_endpoint_name = NULL;
                }
                filter.name = rule_name;
                if (!strcmp(node_name, "priority"))
                    filter.priority = ((const struct lyd_node_leaf_list *)node)->value.uint16;
                else if (!strcmp(node_name, "resulting-endpoint"))
                    filter_endpoint_name = ((const struct lyd_node_leaf_list *)node)->value_str;
                else if (!strcmp(node_name, "any-onu"))
                    filter.type = TR451_FILTER_TYPE_ANY;
                else if (!strcmp(node_name, "onu-vendor"))
                {
                    filter.type = TR451_FILTER_TYPE_VENDOR_ID;
                    strncpy((char *)&filter.serial_number[0], ((const struct lyd_node_leaf_list *)node)->value.string, 4);
                }
                else if (!strcmp(node_name, "onu-serial-number"))
                {
                    const char *serial_number = ((const struct lyd_node_leaf_list *)node)->value.string;
                    if (serial_number == NULL || strlen(serial_number) < 6)
                    {
                        NC_LOG_ERR("invalid onu-serial-number: NULL or too short\n");
                        continue;
                    }
                    strncpy((char *)&filter.serial_number[0], serial_number, 4);
                    /* Serial number consists of 4xASCII vendor_id + 8xHex string vendor-specific id */
                    if (nc_hex_to_bin(serial_number + 4, &filter.serial_number[4], 4) < 0)
                    {
                        NC_LOG_ERR("invalid onu-serial-number format: %s\n", serial_number);
                        continue;
                    }
                    filter.type = TR451_FILTER_TYPE_SERIAL_NUMBER;
                }
            }
        }
        else
        {
            const struct lyd_node *endpoint_node;
            const struct lyd_node *access_node;
            const struct lyd_node *filter_node;
            const char *entity_name = ((const struct lyd_node_leaf_list *)node)->value.string;

            /* Deleted */
            if (!strcmp(node_name, "name"))
            {
                access_node = nc_ly_get_sibling_or_parent_node(node, "access-points");
                endpoint_node = nc_ly_get_sibling_or_parent_node(node, "remote-endpoints");
                filter_node = nc_ly_get_sibling_or_parent_node(node, "rule");
                if (filter_node)
                {
                    err = bcm_tr451_polt_grpc_client_filter_delete(entity_name);
                }
                else if (access_node)
                {
                    NC_LOG_DBG("Deleting access-points [%s] is not supported. Request ignored\n",
                        entity_name);
                }
                else if (endpoint_node != NULL)
                {
                    err = bcm_tr451_polt_grpc_client_delete(entity_name);
                }
            }
        }
    }

    if (endpoint && err == BCM_ERR_OK)
    {
        if (entry.name)
        {
            bcm_tr451_client_endpoint_add_entry(endpoint, &entry);
            memset(&entry, 0, sizeof(entry));
        }
        err = bcm_tr451_polt_grpc_client_create(endpoint);
        endpoint = NULL;
    }
    if (filter.name)
    {
        err = bcm_tr451_polt_grpc_client_filter_set(&filter, filter_endpoint_name);
    }
    if (endpoint != NULL)
        bcm_tr451_client_endpoint_free(endpoint);

    sr_free_change_iter(sr_iter);

    return nc_bcmos_errno_to_sr_errno(err);
}

bcmos_errno bbf_polt_vomci_module_init(sr_session_ctx_t *srs, struct ly_ctx *ly_ctx)
{

    bcmos_errno err = BCM_ERR_INTERNAL;
    const struct lys_module *bbf_polt_mod;
    int i;

    do  {
        /* make sure that ietf-interfaces module is loaded */
        bbf_polt_mod = ly_ctx_get_module(ly_ctx, BBF_POLT_VOMCI_MODULE_NAME, NULL, 1);
        if (bbf_polt_mod == NULL)
        {
            bbf_polt_mod = ly_ctx_load_module(ly_ctx, BBF_POLT_VOMCI_MODULE_NAME, NULL);
            if (bbf_polt_mod == NULL)
            {
                NC_LOG_ERR(BBF_POLT_VOMCI_MODULE_NAME ": can't find the schema in sysrepo\n");
                break;
            }
        }

        /* Enable all relevant features are enabled in sysrepo */
        for (i = 0; bbf_polt_vomci_features[i]; i++)
        {
            if (lys_features_enable(bbf_polt_mod, bbf_polt_vomci_features[i]))
            {
                NC_LOG_ERR("%s: can't enable feature %s\n", BBF_POLT_VOMCI_MODULE_NAME, bbf_polt_vomci_features[i]);
                break;
            }
        }
        if (bbf_polt_vomci_features[i])
            break;

        err = BCM_ERR_OK;
    } while (0);

    return err;
}

static char *_get_date_time_string(char *buf, uint32_t buf_size)
{
    time_t t = time(NULL);
    struct tm tm = *localtime(&t);
    snprintf(buf, buf_size,
        "%.4d-%.2d-%.2dT%.2d:%.2d:%.2dZ",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

/* Server endpoint connect/disconnect notification */
static void _server_connect_disconnect_cb(void *data, const char *server_name,
    const char *client_name, bcmos_bool is_connected)
{
    sr_session_ctx_t *session = (sr_session_ctx_t *)data;
    const struct ly_ctx *ctx = sr_get_context(sr_session_get_connection(session));
    struct lyd_node *notif = NULL;
    char notif_xpath[200];
    char node_xpath[256];
    char date_time_string[64];
    int sr_rc;

    do
    {
        snprintf(notif_xpath, sizeof(notif_xpath)-1,
            "%s[name='%s']/remote-endpoint-status-change",
            BBF_POLT_VOMCI_SERVER_LISTEN_ENDPOINTS_PATH, server_name);
        notif = lyd_new_path(NULL, ctx, notif_xpath, NULL, 0, 0);
        if (notif == NULL)
        {
            NC_LOG_ERR("Failed to create notification %s.\n", notif_xpath);
            break;
        }

        snprintf(node_xpath, sizeof(node_xpath)-1, "%s/remote-endpoint", notif_xpath);
        if (lyd_new_path(notif, NULL, node_xpath, (void *)(long)client_name, 0, 0) == NULL)
        {
            NC_LOG_ERR("Failed to add 'remote-endpoint' node to notification %s.\n", notif_xpath);
            break;
        }

        snprintf(node_xpath, sizeof(node_xpath)-1, "%s/connected", notif_xpath);
        if (lyd_new_path(notif, NULL, node_xpath, is_connected ? "true" : "false", 0, 0) == NULL)
        {
            NC_LOG_ERR("Failed to add 'connected' node to notification %s.\n", notif_xpath);
            break;
        }

        snprintf(node_xpath, sizeof(node_xpath)-1, "%s/remote-endpoint-state-last-change", notif_xpath);
        if (lyd_new_path(notif, NULL, node_xpath,
                _get_date_time_string(date_time_string, sizeof(date_time_string)), 0, 0) == NULL)
        {
            NC_LOG_ERR("Failed to add 'remote-endpoint-state-last-change' node to notification %s.\n", notif_xpath);
            break;
        }

        sr_rc = sr_event_notif_send_tree(session, notif);
        if (sr_rc != SR_ERR_OK)
        {
            NC_LOG_ERR("Failed to sent %s notification. Error '%s'\n",
                notif_xpath, sr_strerror(sr_rc));
            break;
        }

        NC_LOG_DBG("Sent %s notification: remote_endpoint %s: %sconnected\n",
            notif_xpath, client_name, is_connected ? "" : "dis");

    } while (0);

    if (notif != NULL)
        lyd_free_withsiblings(notif);
}

/* Client endpoint connected/disconnected notification */
static void _client_connect_disconnect_cb(void *data, const char *remote_endpoint_name,
    const char *access_point_name, bcmos_bool is_connected)
{
    sr_session_ctx_t *session = (sr_session_ctx_t *)data;
    const struct ly_ctx *ctx = sr_get_context(sr_session_get_connection(session));
    struct lyd_node *notif = NULL;
    char notif_xpath[200];
    char node_xpath[256];
    char date_time_string[64];
    int sr_rc;

    do
    {
        snprintf(notif_xpath, sizeof(notif_xpath)-1,
            "%s[name='%s']/remote-endpoint-status-change",
            BBF_POLT_VOMCI_CLIENT_REMOTE_ENDPOINTS_PATH, remote_endpoint_name);
        notif = lyd_new_path(NULL, ctx, notif_xpath, NULL, 0, 0);
        if (notif == NULL)
        {
            NC_LOG_ERR("Failed to create notification %s.\n", notif_xpath);
            break;
        }

        snprintf(node_xpath, sizeof(node_xpath)-1, "%s/access-point", notif_xpath);
        if (lyd_new_path(notif, NULL, node_xpath, (void *)(long)access_point_name, 0, 0) == NULL)
        {
            NC_LOG_ERR("Failed to add 'access-point' node to notification %s.\n", notif_xpath);
            break;
        }

        snprintf(node_xpath, sizeof(node_xpath)-1, "%s/connected", notif_xpath);
        if (lyd_new_path(notif, NULL, node_xpath, is_connected ? "true" : "false", 0, 0) == NULL)
        {
            NC_LOG_ERR("Failed to add 'connected' node to notification %s.\n", notif_xpath);
            break;
        }

        snprintf(node_xpath, sizeof(node_xpath)-1, "%s/remote-endpoint-state-last-change", notif_xpath);
        if (lyd_new_path(notif, NULL, node_xpath,
                _get_date_time_string(date_time_string, sizeof(date_time_string)), 0, 0) == NULL)
        {
            NC_LOG_ERR("Failed to add 'remote-endpoint-state-last-change' node to notification %s.\n", notif_xpath);
            break;
        }

        sr_rc = sr_event_notif_send_tree(session, notif);
        if (sr_rc != SR_ERR_OK)
        {
            NC_LOG_ERR("Failed to sent %s notification. Error '%s'\n",
                notif_xpath, sr_strerror(sr_rc));
            break;
        }

        NC_LOG_DBG("Sent %s notification: remote_endpoint %s, access_point %s: %sconnected\n",
            notif_xpath, remote_endpoint_name, access_point_name, is_connected ? "" : "dis");

    } while (0);

    if (notif != NULL)
        lyd_free_withsiblings(notif);
}

/* Get server/remote-endpoints list */
static int _server_remote_endpoints_get_cb(sr_session_ctx_t *session, const char *module_name,
    const char *xpath, const char *request_path, uint32_t request_id,
    struct lyd_node **parent, void *private_data)
{
    const struct ly_ctx *ctx = sr_get_context(sr_session_get_connection(session));
    char full_xpath[256];
    const char *ep_name = NULL;

    NC_LOG_INFO("module=%s xpath=%s request=%s\n", module_name, xpath, request_path);

    while ((ep_name = bcm_tr451_polt_grpc_server_client_get_next(ep_name)) != NULL)
    {
        snprintf(full_xpath, sizeof(full_xpath)-1, "%s[name='%s']", xpath, ep_name);
        *parent = nc_ly_sub_value_add(ctx, *parent, full_xpath, "name", ep_name);
    }

    return SR_ERR_OK;
}

bcmos_errno bbf_polt_vomci_module_start(sr_session_ctx_t *srs, struct ly_ctx *ly_ctx)
{
    int sr_rc;
    bcmos_errno err;

    /* subscribe to events */
    sr_rc = sr_module_change_subscribe(srs, BBF_POLT_VOMCI_MODULE_NAME, BBF_POLT_VOMCI_SERVER_PATH,
            bbf_polt_vomci_server_change_cb, NULL, 0, SR_SUBSCR_ENABLED | SR_SUBSCR_CTX_REUSE,
            &sr_ctx);
    if (SR_ERR_OK == sr_rc)
    {
        NC_LOG_INFO("Subscribed to %s subtree changes.\n", BBF_POLT_VOMCI_SERVER_PATH);
    }
    else
    {
        NC_LOG_ERR("Failed to subscribe to %s subtree changes (%s).\n",
            BBF_POLT_VOMCI_SERVER_PATH, sr_strerror(sr_rc));
        return nc_sr_errno_to_bcmos_errno(sr_rc);
    }

    /* subscribe to events */
    sr_rc = sr_module_change_subscribe(srs, BBF_POLT_VOMCI_MODULE_NAME, BBF_POLT_VOMCI_CLIENT_PATH,
            bbf_polt_vomci_client_change_cb, NULL, 0, SR_SUBSCR_ENABLED | SR_SUBSCR_CTX_REUSE,
            &sr_ctx);
    if (SR_ERR_OK == sr_rc)
    {
        NC_LOG_INFO("Subscribed to %s subtree changes.\n", BBF_POLT_VOMCI_CLIENT_PATH);
    }
    else
    {
        NC_LOG_ERR("Failed to subscribe to %s subtree changes (%s).\n",
            BBF_POLT_VOMCI_CLIENT_PATH, sr_strerror(sr_rc));
        sr_unsubscribe(sr_ctx);
        sr_ctx = NULL;
        return nc_sr_errno_to_bcmos_errno(sr_rc);
    }

    /* Subscribe for server's remote-endpoint retrieval */
    sr_rc = sr_oper_get_items_subscribe(srs, BBF_POLT_VOMCI_MODULE_NAME,
        BBF_POLT_VOMCI_SERVER_REMOTE_ENDPOINTS_PATH,
        _server_remote_endpoints_get_cb, NULL, 0, &sr_ctx_state);
    if (SR_ERR_OK == sr_rc)
    {
        NC_LOG_INFO("Subscribed to %s subtree operation data retrieval.\n",
            BBF_POLT_VOMCI_SERVER_REMOTE_ENDPOINTS_PATH);
    }
    else
    {
        NC_LOG_ERR("Failed to subscribe to %s subtree operation data retrieval (%s).\n",
            BBF_POLT_VOMCI_SERVER_REMOTE_ENDPOINTS_PATH, sr_strerror(sr_rc));
        sr_unsubscribe(sr_ctx);
        sr_ctx = NULL;
        return nc_sr_errno_to_bcmos_errno(sr_rc);
    }

    /* Register for gRPC connect/disconnect notifications */
    err = bcm_tr451_polt_grpc_server_connect_disconnect_cb_register(_server_connect_disconnect_cb, srs);
    err = err ? err : bcm_tr451_polt_grpc_client_connect_disconnect_cb_register(_client_connect_disconnect_cb, srs);
    err = err ? err : bcm_tr451_onu_state_change_notify_cb_register(bcmolt_xpon_v_ani_state_change);

    if (err != BCM_ERR_OK)
    {
        NC_LOG_ERR("Failed to subscribe to pOLT connect/disconnect notifications (%s).\n",
            bcmos_strerror(err));
        sr_unsubscribe(sr_ctx);
        sr_ctx = NULL;
        return err;
    }

    return BCM_ERR_OK;
}

void bbf_polt_vomci_module_exit(sr_session_ctx_t *srs, struct ly_ctx *ly_ctx)
{
    if (sr_ctx != NULL)
    {
        sr_unsubscribe(sr_ctx);
        sr_ctx = NULL;
    }
    if (sr_ctx_state != NULL)
    {
        sr_unsubscribe(sr_ctx_state);
        sr_ctx_state = NULL;
    }
    bcm_tr451_polt_grpc_server_connect_disconnect_cb_register(NULL, srs);
    bcm_tr451_polt_grpc_client_connect_disconnect_cb_register(NULL, srs);
}
