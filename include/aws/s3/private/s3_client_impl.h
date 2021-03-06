#ifndef AWS_S3_CLIENT_IMPL_H
#define AWS_S3_CLIENT_IMPL_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include "aws/s3/s3_client.h"

#include <aws/common/atomics.h>
#include <aws/common/byte_buf.h>
#include <aws/common/linked_list.h>
#include <aws/common/mutex.h>
#include <aws/common/ref_count.h>
#include <aws/common/task_scheduler.h>
#include <aws/http/connection_manager.h>

struct aws_http_connection;
struct aws_http_connection_manager;

typedef void(aws_s3_client_get_http_connection_callback)(
    struct aws_http_connection *http_connection,
    int error_code,
    void *user_data);

typedef void(aws_s3_client_sign_callback)(int error_code, void *user_data);

/* Represents one Virtual IP (VIP) in S3, including a connection manager that points directly at that VIP. */
struct aws_s3_vip {
    struct aws_linked_list_node node;

    /* True if this VIP is currently being cleaned up.  The work event loop will check for this flag and clean up
     * related VIP connections. */
    struct aws_atomic_var active;

    struct aws_ref_count internal_ref_count;

    /* S3 Client that owns this vip. */
    struct aws_s3_client *owning_client;

    /* Address this VIP represents. */
    struct aws_string *host_address;

    /* Connection manager shared by all VIP connections. */
    struct aws_http_connection_manager *http_connection_manager;
};

/* Represents one connection on a particular VIP. */
struct aws_s3_vip_connection {

    struct aws_linked_list_node node;

    /* The VIP that this connection belongs to. */
    struct aws_s3_vip *owning_vip;

    /* The underlying, currently in-use HTTP connection. */
    struct aws_http_connection *http_connection;

    /* Number of requests we have made on this particular connection. Important for the request service limit. */
    uint32_t request_count;

    /* Request currently being processed on the VIP connection. */
    struct aws_s3_request *request;
};

struct aws_s3_client_vtable {

    struct aws_s3_meta_request *(
        *meta_request_factory)(struct aws_s3_client *client, const struct aws_s3_meta_request_options *options);

    void (*push_meta_request)(struct aws_s3_client *client, struct aws_s3_meta_request *meta_request);

    void (*remove_meta_request)(struct aws_s3_client *client, struct aws_s3_meta_request *meta_request);

    void (*get_http_connection)(
        struct aws_s3_client *client,
        struct aws_s3_vip_connection *vip_connection,
        aws_http_connection_manager_on_connection_setup_fn *on_connection_acquired_callback);
};

/* Represents the state of the S3 client. */
struct aws_s3_client {
    struct aws_allocator *allocator;

    /* Small block allocator for our small allocations. */
    struct aws_allocator *sba_allocator;

    struct aws_s3_client_vtable *vtable;

    struct aws_ref_count ref_count;

    /* Internal ref count is used for tracking the lifetime of resources owned by the client that have asynchronous
     * clean up.  In those cases, we don't want to prevent clean up from being initiated (which is what would happen
     * with a normal reference), but we do want to know when we can completely clean up (ie: regular ref count and
     * internal ref count are both 0). */
    struct aws_ref_count internal_ref_count;

    /* Client bootstrap for setting up connection managers. */
    struct aws_client_bootstrap *client_bootstrap;

    /* Event loop on the client bootstrap ELG for processing work/dispatching requests. */
    struct aws_event_loop *process_work_event_loop;

    /* Event loop group for streaming request bodies back to the user. */
    struct aws_event_loop_group *body_streaming_elg;

    /* Region of the S3 bucket. */
    struct aws_string *region;

    /* Size of parts for files when doing gets or puts.  This exists on the client as configurable option that is passed
     * to meta requests for use. */
    const uint64_t part_size;

    /* Size of parts for files when doing gets or puts.  This exists on the client as configurable option that is passed
     * to meta requests for use. */
    const uint64_t max_part_size;

    /* TLS Options to be used for each connection. */
    struct aws_tls_connection_options *tls_connection_options;

    /* Cached signing config. Can be NULL if no signing config was specified. */
    struct aws_cached_signing_config_aws *cached_signing_config;

    /* Throughput target in Gbps that we are trying to reach. */
    const double throughput_target_gbps;

    /* The calculated ideal number of VIP's based on throughput target and throughput per vip. */
    const uint32_t ideal_vip_count;

    /* Retry strategy used for scheduling request retries. */
    struct aws_retry_strategy *retry_strategy;

    /* Shutdown callbacks to notify when the client is completely cleaned up. */
    aws_s3_client_shutdown_complete_callback_fn *shutdown_callback;
    void *shutdown_callback_user_data;

    struct {
        struct aws_mutex lock;

        /* Endpoint to use for the bucket. */
        struct aws_string *endpoint;

        uint32_t vip_count;

        /* Linked list of active VIP's. */
        struct aws_linked_list vips;

        /* VIP Connections that need added or updated in the work event loop. */
        struct aws_linked_list pending_vip_connection_updates;

        /* Meta requests that need added in the work event loop. */
        struct aws_linked_list pending_meta_request_work;

        /* Task for processing requests from meta requests on vip connections. */
        struct aws_task process_work_task;

        /* Counter for number of requests that have been finished/released, allowing us to create new requests. */
        uint32_t pending_request_count;

        /* Host listener to get new IP addresses. */
        struct aws_host_listener *host_listener;

        /* Whether or not work processing is currently scheduled. */
        uint32_t process_work_task_scheduled : 1;

        /* Whether or not the client has started cleaning up all of its resources */
        uint32_t active : 1;

    } synced_data;

    struct {
        /* List of all VIP Connections for each VIP. */
        struct aws_linked_list idle_vip_connections;

        /* Client list of on going meta requests. */
        struct aws_linked_list meta_requests;

        struct aws_s3_meta_request *next_meta_request;

        uint32_t num_requests_in_flight;

    } threaded_data;
};

void aws_s3_client_push_meta_request(struct aws_s3_client *client, struct aws_s3_meta_request *meta_request);

void aws_s3_client_remove_meta_request(struct aws_s3_client *client, struct aws_s3_meta_request *meta_request);

int aws_s3_client_make_request(struct aws_s3_client *client, struct aws_s3_vip_connection *vip_connection);

void aws_s3_client_notify_connection_finished(
    struct aws_s3_client *client,
    struct aws_s3_vip_connection *vip_connection);

void aws_s3_client_notify_request_destroyed(struct aws_s3_client *client);

void aws_s3_client_stream_response_body(
    struct aws_s3_client *client,
    struct aws_s3_meta_request *meta_request,
    struct aws_linked_list *requests);

#endif /* AWS_S3_CLIENT_IMPL_H */
