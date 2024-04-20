#include "protobuf_server.h"
#include "loggers/network_logger.h"
/*
    we shall not use nanopb or any protobuf lib because they need atleast 1 memcopy
    i have read the byte array implemntation of the protoc and
    we do encoding/decoding right to the buffer
*/
#include "buffer_stream.h"
// #include <pb_encode.h>
// #include <pb_decode.h>
// #include "packet.pb.h"
#include "uleb128.h"

#define MAX_PACKET_SIZE (65536 * 1)

typedef struct protobuf_server_state_s
{

} protobuf_server_state_t;

typedef struct protobuf_server_con_state_s
{
    buffer_stream_t *stream_buf;
    size_t           wanted;
    bool             first_sent;

} protobuf_server_con_state_t;

static void cleanup(protobuf_server_con_state_t *cstate)
{
    destroyBufferStream(cstate->stream_buf);
    free(cstate);
}

static inline void upStream(tunnel_t *self, context_t *c, TunnelFlowRoutine upstream, TunnelFlowRoutine downstream)
{
    protobuf_server_con_state_t *cstate = CSTATE(c);

    if (c->payload != NULL)
    {
        buffer_stream_t *bstream = cstate->stream_buf;

        bufferStreamPush(cstate->stream_buf, c->payload);
        c->payload = NULL;

        while (true)
        {
            if (bufferStreamLen(bstream) < 2)
            {
                destroyContext(c);
                return;
            }
            unsigned int  read_len = (bufferStreamLen(bstream) >= 4 ? 4 : 2);
            unsigned char uleb_encoded_buf[4];
            bufferStreamViewBytesAt(bstream, uleb_encoded_buf, 1, read_len);

            size_t data_len     = 0;
            size_t bytes_passed = readUleb128ToUint64(uleb_encoded_buf, uleb_encoded_buf + read_len, &data_len);
            if (data_len == 0)
            {
                if (uleb_encoded_buf[0] = 0x0)
                {
                    // invalid
                    goto disconnect;
                }

                // keep waiting for more data to come
                destroyContext(c);
                return;
            }
            if (data_len > MAX_PACKET_SIZE)
            {
                // overflow
                goto disconnect;
            }
            if (! (bufferStreamLen(bstream) >= 1 + bytes_passed + data_len))
            {
                destroyContext(c);
                return;
            }
            context_t *upstream_ctx = newContextFrom(c);
            upstream_ctx->payload   = bufferStreamRead(cstate->stream_buf, 1 + bytes_passed + data_len);
            shiftr(upstream_ctx->payload, 1 + bytes_passed);
            if (! cstate->first_sent)
            {
                upstream_ctx->first = true;
                cstate->first_sent  = true;
            }
            upstream(self->up, upstream_ctx);
            if (! isAlive(c->line))
            {
                destroyContext(c);
                return;
            }
        }
    }
    else
    {
        if (c->init)
        {
            cstate             = malloc(sizeof(protobuf_server_con_state_t));
            cstate->wanted     = 0;
            cstate->first_sent = false;
            cstate->stream_buf = newBufferStream(buffer_pools[c->line->tid]);
            CSTATE_MUT(c)      = cstate;
        }
        else if (c->fin)
        {
            cleanup(cstate);
            CSTATE_MUT(c) = NULL;
        }
        upstream(self->up, c);
    }

    return;
disconnect:
    cleanup(cstate);
    CSTATE_MUT(c) = NULL;
    upstream(self->up, newFinContext(c->line));
    downstream(self->dw, newFinContext(c->line));
    destroyContext(c);
}

static inline void downStream(tunnel_t *self, context_t *c, TunnelFlowRoutine downstream)
{

    if (c->payload != NULL)
    {
        size_t blen             = bufLen(c->payload);
        size_t calculated_bytes = size_uleb128(blen);
        shiftl(c->payload, calculated_bytes + 1);
        write_uleb128(rawBufMut(c->payload) + 1, blen);
        writeUI8(c->payload, '\n');
    }
    else
    {
        if (c->fin)
        {
            protobuf_server_con_state_t *cstate = cstate;
            cleanup(cstate);
            CSTATE_MUT(c) = NULL;
        }
    }
    downstream(self->dw, c);
}



tunnel_t *newProtoBufServer(node_instance_context_t *instance_info)
{

    (void) instance_info;
    tunnel_t *t   = newTunnel();
    t->upStream   = &upStream;
    t->downStream = &downStream;
    atomic_thread_fence(memory_order_release);

    return t;
}

api_result_t apiProtoBufServer(tunnel_t *self, const char *msg)
{
    (void) (self);
    (void) (msg);
    return (api_result_t){0};
}

tunnel_t *destroyProtoBufServer(tunnel_t *self)
{
    (void) (self);
    return NULL;
}
tunnel_metadata_t getMetadataProtoBufServer()
{
    return (tunnel_metadata_t){.version = 0001, .flags = 0x0};
}