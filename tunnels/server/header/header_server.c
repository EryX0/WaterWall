#include "header_server.h"
#include "buffer_stream.h"
#include "hv/hsocket.h"
#include "loggers/network_logger.h"

enum header_dynamic_value_status
{
    kHdvsEmpty = 0x0,
    kHdvsConstant,
    kHdvsDestPort,
};

typedef struct header_server_state_s
{
    dynamic_value_t data;

} header_server_state_t;

typedef struct header_server_con_state_s
{
    bool init_sent;

} header_server_con_state_t;

static void upStream(tunnel_t *self, context_t *c)
{
    header_server_state_t *    state  = STATE(self);
    header_server_con_state_t *cstate = CSTATE(c);
    if (c->payload != NULL)
    {
        if (c->first)
        {

            shift_buffer_t *buf = c->payload;
            if (bufLen(buf) < 2)
            {
                reuseContextBuffer(c);
                self->dw->downStream(self->dw, newFinContext(c->line));
                destroyContext(c);
                return;
            }

            uint16_t port = 0;
            switch ((enum header_dynamic_value_status) state->data.status)
            {
            case kHdvsConstant:

                readUI16(buf, &port);
                sockaddr_set_port(&(c->line->dest_ctx.addr), port);
                shiftr(c->payload, sizeof(uint16_t));
                if (port < 10)
                {
                    reuseContextBuffer(c);
                    self->dw->downStream(self->dw, newFinContext(c->line));
                    destroyContext(c);
                    return;
                }

                break;

            default:
                break;
            }

            cstate->init_sent = true;
            self->up->upStream(self->up, newInitContext(c->line));
            if (! isAlive(c->line))
            {
                reuseContextBuffer(c);
                destroyContext(c);
                return;
            }
        }
    }
    else if (c->init)
    {
        cstate            = malloc(sizeof(header_server_con_state_t));
        cstate->init_sent = false;
        CSTATE_MUT(c)     = cstate;
        destroyContext(c);
        return;
    }
    else if (c->fin)
    {
        bool send_fin = cstate->init_sent;
        free(cstate);
        CSTATE_MUT(c) = NULL;
        if (send_fin)
        {
            self->up->upStream(self->up, c);
        }
        else
        {
            destroyContext(c);
        }
        return;
    }

    self->up->upStream(self->up, c);
}

static inline void downStream(tunnel_t *self, context_t *c)
{

    if (c->fin)
    {
        free(CSTATE(c));
        CSTATE_MUT(c) = NULL;
    }

    self->dw->downStream(self->dw, c);
}

tunnel_t *newHeaderServer(node_instance_context_t *instance_info)
{

    header_server_state_t *state = malloc(sizeof(header_server_state_t));
    memset(state, 0, sizeof(header_server_state_t));
    const cJSON *settings = instance_info->node_settings_json;
    state->data           = parseDynamicNumericValueFromJsonObject(settings, "override", 1, "dest_context->port");
    tunnel_t *t           = newTunnel();
    t->state              = state;
    t->upStream           = &upStream;
    t->downStream         = &downStream;
    atomic_thread_fence(memory_order_release);

    return t;
}

api_result_t apiHeaderServer(tunnel_t *self, const char *msg)
{
    (void) (self);
    (void) (msg);
    return (api_result_t){0}; // TODO(root):
}

tunnel_t *destroyHeaderServer(tunnel_t *self)
{
    (void) (self);
    return NULL;
}
tunnel_metadata_t getMetadataHeaderServer()
{
    return (tunnel_metadata_t){.version = 0001, .flags = 0x0};
}