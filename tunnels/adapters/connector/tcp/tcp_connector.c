#include "tcp_connector.h"
#include "loggers/network_logger.h"
#include "types.h"
#include "utils/sockutils.h"

static void cleanup(tcp_connector_con_state_t *cstate)
{
    if (cstate->io)
    {
        hevent_set_userdata(cstate->io, NULL);
    }

    hio_t *last_resumed_io = NULL;

    while (contextQueueLen(cstate->data_queue) > 0)
    {
        context_t *cw = contextQueuePop(cstate->data_queue);
        if (cw->src_io != NULL && last_resumed_io != cw->src_io)
        {
            last_resumed_io = cw->src_io;
            hio_read(cw->src_io);
        }
        if (cw->payload)
        {
            reuseContextBuffer(cw);
        }
        destroyContext(cw);
    }

    while (contextQueueLen(cstate->finished_queue) > 0)
    {
        context_t *cw = contextQueuePop(cstate->finished_queue);
        if (cw->src_io != NULL && last_resumed_io != cw->src_io)
        {
            last_resumed_io = cw->src_io;
            hio_read(cw->src_io);
        }

        destroyContext(cw);
    }

    destroyContextQueue(cstate->data_queue);
    destroyContextQueue(cstate->finished_queue);
    free(cstate);
}

static bool resumeWriteQueue(tcp_connector_con_state_t *cstate)
{
    context_queue_t *data_queue     = (cstate)->data_queue;
    context_queue_t *finished_queue = (cstate)->finished_queue;
    hio_t *          io             = cstate->io;
    while (contextQueueLen(data_queue) > 0)
    {
        context_t *cw = contextQueuePop(data_queue);

        unsigned int bytes  = bufLen(cw->payload);
        int          nwrite = hio_write(io, rawBuf(cw->payload), bytes);
        reuseBuffer(cstate->buffer_pool, cw->payload);
        cw->payload = NULL;
        contextQueuePush(cstate->finished_queue, cw);
        if (nwrite >= 0 && nwrite < bytes)
        {
            return false; // write pending}
        }
        // data data_queue is empty
        hio_t *last_resumed_io = NULL;
        while (contextQueueLen(finished_queue) > 0)
        {
            context_t *cw          = contextQueuePop(finished_queue);
            hio_t *    upstream_io = cw->src_io;
            if (upstream_io != NULL && (last_resumed_io != upstream_io))
            {
                last_resumed_io = upstream_io;
                hio_read(upstream_io);
            }
            destroyContext(cw);
        }
        return true;
    }
}
static void onWriteComplete(hio_t *restrict io, const void *restrict buf, int writebytes)
{
    (void) buf;
    (void) writebytes;

    // resume the read on other end of the connection
    tcp_connector_con_state_t *cstate = (tcp_connector_con_state_t *) (hevent_userdata(io));
    if (cstate == NULL)
    {
        return;
    }

    if (hio_write_is_complete(io))
    {
        hio_setcb_write(cstate->io, NULL);
        cstate->write_paused = false;

        context_queue_t *data_queue     = cstate->data_queue;
        context_queue_t *finished_queue = cstate->finished_queue;
        if (contextQueueLen(data_queue) > 0)
        {
            if (! resumeWriteQueue(cstate))
            {
                hio_setcb_write(cstate->io, onWriteComplete);
                cstate->write_paused = true;
                return;
            }
        }
        hio_t *last_resumed_io = NULL;
        while (contextQueueLen(finished_queue) > 0)
        {
            context_t *cw          = contextQueuePop(finished_queue);
            hio_t *    upstream_io = cw->src_io;
            if (upstream_io != NULL && (last_resumed_io != upstream_io))
            {
                last_resumed_io = upstream_io;
                hio_read(upstream_io);
            }
            destroyContext(cw);
        }
    }
}

static void onRecv(hio_t *restrict io, void *restrict buf, int readbytes)
{
    tcp_connector_con_state_t *cstate = (tcp_connector_con_state_t *) (hevent_userdata(io));
    if (cstate == NULL)
    {
        return;
    }
    shift_buffer_t *payload = popBuffer(cstate->buffer_pool);
    setLen(payload, readbytes);
    writeRaw(payload, buf, readbytes);

    tunnel_t *self = (cstate)->tunnel;
    line_t *  line = (cstate)->line;

    context_t *context = newContext(line);
    context->src_io    = io;
    context->payload   = payload;

    self->downStream(self, context);
}

static void onClose(hio_t *io)
{
    tcp_connector_con_state_t *cstate = (tcp_connector_con_state_t *) (hevent_userdata(io));
    if (cstate != NULL)
    {
        LOGD("TcpConnector: received close for FD:%x ", (int) hio_fd(io));
    }
    else
    {
        LOGD("TcpConnector: sent close for FD:%x ", (int) hio_fd(io));
    }
    if (cstate != NULL)
    {
        tunnel_t * self    = (cstate)->tunnel;
        line_t *   line    = (cstate)->line;
        context_t *context = newFinContext(line);
        self->downStream(self, context);
    }
}

static void onOutBoundConnected(hio_t *upstream_io)
{
    tcp_connector_con_state_t *cstate = hevent_userdata(upstream_io);
#ifdef PROFILE
    struct timeval tv2;
    gettimeofday(&tv2, NULL);

    double time_spent = (double) (tv2.tv_usec - (cstate->__profile_conenct).tv_usec) / 1000000 +
                        (double) (tv2.tv_sec - (cstate->__profile_conenct).tv_sec);
    LOGD("TcpConnector: tcp connect took %d ms", (int) (time_spent * 1000));
#endif

    tunnel_t *self = cstate->tunnel;
    line_t *  line = cstate->line;
    hio_setcb_read(upstream_io, onRecv);

    char localaddrstr[SOCKADDR_STRLEN] = {0};
    char peeraddrstr[SOCKADDR_STRLEN]  = {0};

    LOGD("TcpConnector: connection succeed FD:%x [%s] => [%s]", (int) hio_fd(upstream_io),
         SOCKADDR_STR(hio_localaddr(upstream_io), localaddrstr), SOCKADDR_STR(hio_peeraddr(upstream_io), peeraddrstr));

    context_t *est_context = newContext(line);
    est_context->est       = true;
    est_context->src_io    = upstream_io;
    self->downStream(self, est_context);
}

void upStream(tunnel_t *self, context_t *c)
{
    tcp_connector_con_state_t *cstate = CSTATE(c);

    if (c->payload != NULL)
    {
        if (cstate->write_paused)
        {
            if (c->src_io)
            {
                hio_read_stop(c->src_io);
            }
            contextQueuePush(cstate->data_queue, c);
        }
        else
        {
            unsigned int bytes  = bufLen(c->payload);
            int          nwrite = hio_write(cstate->io, rawBuf(c->payload), bytes);
            if (nwrite >= 0 && nwrite < bytes)
            {
                if (c->src_io)
                {
                    hio_read_stop(c->src_io);
                }
                reuseBuffer(cstate->buffer_pool, c->payload);
                c->payload = NULL;

                contextQueuePush(cstate->finished_queue, c);
                cstate->write_paused = true;
                hio_setcb_write(cstate->io, onWriteComplete);
            }
            else
            {
                reuseBuffer(cstate->buffer_pool, c->payload);
                c->payload = NULL;
                destroyContext(c);
            }
        }
    }
    else
    {
        if (c->init)
        {

            CSTATE_MUT(c) = malloc(sizeof(tcp_connector_con_state_t));
            memset(CSTATE(c), 0, sizeof(tcp_connector_con_state_t));
            tcp_connector_con_state_t *cstate = CSTATE(c);
#ifdef PROFILE
            gettimeofday(&(cstate->__profile_conenct), NULL);
#endif

            cstate->buffer_pool    = buffer_pools[c->line->tid];
            cstate->tunnel         = self;
            cstate->line           = c->line;
            cstate->data_queue     = newContextQueue(cstate->buffer_pool);
            cstate->finished_queue = newContextQueue(cstate->buffer_pool);
            cstate->write_paused   = true;

            socket_context_t final_ctx = {0};
            // fill the final_ctx address based on settings
            {
                socket_context_t *     src_ctx  = &(c->line->src_ctx);
                socket_context_t *     dest_ctx = &(c->line->dest_ctx);
                tcp_connector_state_t *state    = STATE(self);

                if (state->dest_addr.status == kCdvsFromSource)
                {
                    copySocketContextAddr(&final_ctx, &src_ctx);
                }
                else if (state->dest_addr.status == kCdvsFromDest)
                {
                    copySocketContextAddr(&final_ctx, &dest_ctx);
                }
                else
                {
                    final_ctx.atype = state->dest_atype;
                    if (state->dest_atype == kSatDomainName)
                    {
                        final_ctx.domain = malloc(state->dest_domain_len + 1);
                        memcpy(final_ctx.domain, state->dest_addr.value_ptr, state->dest_domain_len + 1);
                        final_ctx.resolved          = false;
                        final_ctx.addr.sa.sa_family = AF_INET; // addr resolve will change this
                    }
                    else
                    {
                        sockaddr_set_ip(&(final_ctx.addr), state->dest_addr.value_ptr);
                    }
                }

                if (state->dest_port.status == kCdvsFromSource)
                {
                    sockaddr_set_port(&(final_ctx.addr), sockaddr_port(&(src_ctx->addr)));
                }
                else if (state->dest_port.status == kCdvsFromDest)
                {
                    sockaddr_set_port(&(final_ctx.addr), sockaddr_port(&(dest_ctx->addr)));
                }
                else
                {
                    sockaddr_set_port(&(final_ctx.addr), state->dest_port.value);
                }
            }

            // sockaddr_set_ipport(&(final_ctx.addr), "127.0.0.1", 443);

            LOGD("TcpConnector: initiating connection");
            if (final_ctx.atype == kSatDomainName)
            {
                if (! final_ctx.resolved)
                {
                    if (! tcpConnectorResolvedomain(&final_ctx))
                    {
                        free(final_ctx.domain);
                        cleanup(cstate);

                        CSTATE_MUT(c) = NULL;
                        goto fail;
                    }
                }
                free(final_ctx.domain);
            }
            tcp_connector_state_t *state = STATE(self);

            hloop_t *loop   = loops[c->line->tid];
            int      sockfd = socket(final_ctx.addr.sa.sa_family, SOCK_STREAM, 0);
            if (sockfd < 0)
            {
                LOGE("Connector: socket fd < 0");
                cleanup(cstate);
                CSTATE_MUT(c) = NULL;
                goto fail;
            }
            if (state->tcp_no_delay)
            {
                tcp_nodelay(sockfd, 1);
            }
            if (state->reuse_addr)
            {
                so_reuseport(sockfd, 1);
            }

            if (state->tcp_fast_open)
            {
                const int yes = 1;
                setsockopt(sockfd, SOL_TCP, TCP_FASTOPEN, &yes, sizeof(yes));
            }

            hio_t *upstream_io = hio_get(loop, sockfd);
            assert(upstream_io != NULL);

            hio_set_peeraddr(upstream_io, &(final_ctx.addr.sa), sockaddr_len(&(final_ctx.addr)));
            cstate->io = upstream_io;
            hevent_set_userdata(upstream_io, cstate);

            hio_setcb_connect(upstream_io, onOutBoundConnected);
            hio_setcb_close(upstream_io, onClose);
            hio_connect(upstream_io);
            destroyContext(c);
        }
        else if (c->fin)
        {
            hio_t *io = cstate->io;
            cleanup(cstate);
            CSTATE_MUT(c) = NULL;
            destroyContext(c);
            hio_close(io);
        }
    }
    return;
fail:;
    self->dw->downStream(self->dw, newFinContext(c->line));
    destroyContext(c);
}
void downStream(tunnel_t *self, context_t *c)
{
    tcp_connector_con_state_t *cstate = CSTATE(c);

    if (c->payload != NULL)
    {
#ifdef PROFILE
        struct timeval tv1, tv2;
        gettimeofday(&tv1, NULL);
        {
            self->dw->downStream(self->dw, c);
        }
        gettimeofday(&tv2, NULL);
        double time_spent = (double) (tv2.tv_usec - tv1.tv_usec) / 1000000 + (double) (tv2.tv_sec - tv1.tv_sec);
        LOGD("TcpConnector: tcp downstream took %d ms", (int) (time_spent * 1000));
#else
        self->dw->downStream(self->dw, c);

#endif
    }
    else
    {

        if (c->est)
        {
            cstate->established = true;
            hio_read(cstate->io);
            if (resumeWriteQueue(cstate))
            {
                cstate->write_paused = false;
            }
            else
            {
                hio_setcb_write(cstate->io, onWriteComplete);
            }
            self->dw->downStream(self->dw, c);
        }
        else if (c->fin)
        {
            cleanup(cstate);
            CSTATE_MUT(c) = NULL;
            self->dw->downStream(self->dw, c);
        }
    }
}

tunnel_t *newTcpConnector(node_instance_context_t *instance_info)
{
    tcp_connector_state_t *state = malloc(sizeof(tcp_connector_state_t));
    memset(state, 0, sizeof(tcp_connector_state_t));
    const cJSON *settings = instance_info->node_settings_json;

    if (! (cJSON_IsObject(settings) && settings->child != NULL))
    {
        LOGF("JSON Error: TcpConnector->settings (object field) : The object was empty or invalid");
        return NULL;
    }

    const cJSON *tcp_settings = cJSON_GetObjectItemCaseSensitive(settings, "tcp");
    if ((cJSON_IsObject(tcp_settings) && settings->child != NULL))
    {
        getBoolFromJsonObject(&(state->tcp_no_delay), tcp_settings, "nodelay");
        getBoolFromJsonObject(&(state->tcp_fast_open), tcp_settings, "fastopen");
        getBoolFromJsonObject(&(state->reuse_addr), tcp_settings, "reuseaddr");
        int ds = 0;
        getIntFromJsonObject(&ds, tcp_settings, "domain-strategy");
        state->domain_strategy = ds;
    }
    else
    {
        // memset set everything to 0...
    }

    state->dest_addr =
        parseDynamicStrValueFromJsonObject(settings, "address", 2, "src_context->address", "dest_context->address");

    if (state->dest_addr.status == kDvsEmpty)
    {
        LOGF("JSON Error: TcpConnector->settings->address (string field) : The vaule was empty or invalid");
        return NULL;
    }

    state->dest_port =
        parseDynamicNumericValueFromJsonObject(settings, "port", 2, "src_context->port", "dest_context->port");

    if (state->dest_port.status == kDvsEmpty)
    {
        LOGF("JSON Error: TcpConnector->settings->port (number field) : The vaule was empty or invalid");
        return NULL;
    }
    if (state->dest_addr.status == kDvsConstant)
    {
        state->dest_atype      = getHostAddrType(state->dest_addr.value_ptr);
        state->dest_domain_len = strlen(state->dest_addr.value_ptr);
    }

    tunnel_t *t   = newTunnel();
    t->state      = state;
    t->upStream   = &upStream;
    t->downStream = &downStream;

    atomic_thread_fence(memory_order_release);

    return t;
}
api_result_t apiTcpConnector(tunnel_t *self, const char *msg)
{
    (void) (self);
    (void) (msg);
    return (api_result_t){0};
}
tunnel_t *destroyTcpConnector(tunnel_t *self)
{
    (void) (self);
    return NULL;
}
tunnel_metadata_t getMetadataTcpConnector()
{
    return (tunnel_metadata_t){.version = 0001, .flags = 0x0};
}