
/*
 * Copyright (C) Alex Zhang
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include <zstd.h>


typedef struct {
    ngx_flag_t                   enable;
    ngx_int_t                    level;
    ssize_t                      min_length;

    ngx_hash_t                   types;

    ngx_bufs_t                   bufs;

    ngx_array_t                 *types_keys;

} ngx_http_zstd_loc_conf_t;


typedef struct {
    ngx_chain_t                 *in;
    ngx_chain_t                 *free;
    ngx_chain_t                 *busy;
    ngx_chain_t                 *out;
    ngx_chain_t                **last_out;

    ngx_buf_t                   *in_buf;
    ngx_buf_t                   *out_buf;
    ngx_int_t                    bufs;

    ZSTD_inBuffer                buffer_in;
    ZSTD_outBuffer               buffer_out;

    ZSTD_CStream                *cstream;

    size_t                       bytes_in;
    size_t                       bytes_out;
    size_t                       consumed;
    size_t                       limit;

    unsigned                     last:1;
    unsigned                     flush:1;
    unsigned                     done:1;
    unsigned                     nomem:1;
    unsigned                     cached:1;
    unsigned                     yield:1;
} ngx_http_zstd_ctx_t;


typedef struct {
    ngx_conf_post_handler_pt  post_handler;
} ngx_http_zstd_comp_level_bounds_t;


static ngx_http_output_header_filter_pt  ngx_http_next_header_filter;
static ngx_http_output_body_filter_pt  ngx_http_next_body_filter;

static ngx_str_t  ngx_http_zstd_ratio = ngx_string("zstd_ratio");

static ZSTD_CStream  *ngx_http_zstd_cached_cstream;
static ngx_uint_t     ngx_http_zstd_cached_cstream_busy;


static ngx_int_t ngx_http_zstd_header_filter(ngx_http_request_t *r);
static ngx_int_t ngx_http_zstd_body_filter(ngx_http_request_t *r,
    ngx_chain_t *in);
static ngx_int_t ngx_http_zstd_filter_add_data(ngx_http_request_t *r,
    ngx_http_zstd_ctx_t *ctx);
static ngx_int_t ngx_http_zstd_filter_get_buf(ngx_http_request_t *r,
    ngx_http_zstd_ctx_t *ctx);
static ZSTD_CStream *ngx_http_zstd_filter_create_cstream(ngx_http_request_t *r,
    ngx_http_zstd_ctx_t *ctx);
static ngx_int_t ngx_http_zstd_filter_release_cstream(ngx_http_zstd_ctx_t *ctx,
    ngx_log_t *log, ngx_uint_t discard);
static void ngx_http_zstd_filter_cleanup(void *data);
static void ngx_http_zstd_filter_exit_process(ngx_cycle_t *cycle);
static ngx_int_t ngx_http_zstd_filter_compress(ngx_http_request_t *r,
    ngx_http_zstd_ctx_t *ctx);
static ngx_int_t ngx_http_zstd_accept_encoding(ngx_str_t *ae);
static ngx_int_t ngx_http_zstd_ok(ngx_http_request_t *r);
static ngx_int_t ngx_http_zstd_filter_init(ngx_conf_t *cf);
static void *ngx_http_zstd_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_zstd_merge_loc_conf(ngx_conf_t *cf, void *parent,
    void *child);
static ngx_int_t ngx_http_zstd_add_variables(ngx_conf_t *cf);
static ngx_int_t ngx_http_zstd_ratio_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *vv, uintptr_t data);
static char *ngx_http_zstd_comp_level(ngx_conf_t *cf, void *post, void *data);
static char *ngx_conf_zstd_set_num_slot_with_negatives(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);


static ngx_http_zstd_comp_level_bounds_t  ngx_http_zstd_comp_level_bounds = {
    ngx_http_zstd_comp_level
};


static ngx_command_t  ngx_http_zstd_filter_commands[] = {

    { ngx_string("zstd"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_HTTP_LIF_CONF
      |NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_zstd_loc_conf_t, enable),
      NULL },

    { ngx_string("zstd_comp_level"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_zstd_set_num_slot_with_negatives,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_zstd_loc_conf_t, level),
      &ngx_http_zstd_comp_level_bounds },

    { ngx_string("zstd_types"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_1MORE,
      ngx_http_types_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_zstd_loc_conf_t, types_keys),
      &ngx_http_html_default_types[0] },

    { ngx_string("zstd_buffers"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE2,
      ngx_conf_set_bufs_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_zstd_loc_conf_t, bufs),
      NULL },

    { ngx_string("zstd_min_length"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_size_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_zstd_loc_conf_t, min_length),
      NULL },

    ngx_null_command
};


static ngx_http_module_t  ngx_http_zstd_filter_module_ctx = {
    ngx_http_zstd_add_variables,            /* preconfiguration */
    ngx_http_zstd_filter_init,              /* postconfiguration */

    NULL,                                   /* create main configuration */
    NULL,                                   /* init main configuration */

    NULL,                                   /* create server configuration */
    NULL,                                   /* merge server configuration */

    ngx_http_zstd_create_loc_conf,          /* create location configuration */
    ngx_http_zstd_merge_loc_conf,           /* merge location configuration */
};


ngx_module_t  ngx_http_zstd_filter_module = {
    NGX_MODULE_V1,
    &ngx_http_zstd_filter_module_ctx,       /* module context */
    ngx_http_zstd_filter_commands,          /* module directives */
    NGX_HTTP_MODULE,                        /* module type */
    NULL,                                   /* init master */
    NULL,                                   /* init module */
    NULL,                                   /* init process */
    NULL,                                   /* init thread */
    NULL,                                   /* exit thread */
    ngx_http_zstd_filter_exit_process,      /* exit process */
    NULL,                                   /* exit master */
    NGX_MODULE_V1_PADDING
};


static ngx_int_t
ngx_http_zstd_header_filter(ngx_http_request_t *r)
{
    ngx_table_elt_t           *h;
    ngx_pool_cleanup_t        *cln;
    ngx_http_zstd_loc_conf_t  *zlcf;
    ngx_http_zstd_ctx_t       *ctx;

    zlcf = ngx_http_get_module_loc_conf(r, ngx_http_zstd_filter_module);

    if (!zlcf->enable
        || (r->headers_out.status != NGX_HTTP_OK
            && r->headers_out.status != NGX_HTTP_FORBIDDEN
            && r->headers_out.status != NGX_HTTP_NOT_FOUND
            && r->headers_out.status != 207) // WebDAV Multi-Status
       || (r->headers_out.content_encoding
           && r->headers_out.content_encoding->value.len)
       || (r->headers_out.content_length_n != -1
           && r->headers_out.content_length_n < zlcf->min_length)
       || ngx_http_test_content_type(r, &zlcf->types) == NULL
       || r->header_only)
    {
        return ngx_http_next_header_filter(r);
    }

    r->gzip_vary = 1;

    if (ngx_http_zstd_ok(r) != NGX_OK) {
        return ngx_http_next_header_filter(r);
    }

    ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_zstd_ctx_t));
    if (ctx == NULL) {
        return NGX_ERROR;
    }

    cln = ngx_pool_cleanup_add(r->pool, 0);
    if (cln == NULL) {
        return NGX_ERROR;
    }

    cln->handler = ngx_http_zstd_filter_cleanup;
    cln->data = ctx;

    ngx_http_set_ctx(r, ctx, ngx_http_zstd_filter_module);
    ctx->last_out = &ctx->out;

    h = ngx_list_push(&r->headers_out.headers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    h->hash = 1;
    ngx_str_set(&h->key, "Content-Encoding");
    ngx_str_set(&h->value, "zstd");
    r->headers_out.content_encoding = h;

    r->main_filter_need_in_memory = 1;

    ngx_http_clear_content_length(r);
    ngx_http_clear_accept_ranges(r);
    ngx_http_weak_etag(r);

    return ngx_http_next_header_filter(r);
}


static ngx_int_t
ngx_http_zstd_body_filter(ngx_http_request_t *r, ngx_chain_t *in)
{
    ngx_int_t                  flush, rc, output;
    ngx_chain_t               *cl;
    ngx_http_zstd_ctx_t       *ctx;
    ngx_http_core_loc_conf_t  *clcf;


    ctx = ngx_http_get_module_ctx(r, ngx_http_zstd_filter_module);

    if (ctx == NULL || ctx->done || r->header_only) {
        return ngx_http_next_body_filter(r, in);
    }

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "http zstd filter");

    if (ctx->cstream == NULL) {
        ctx->cstream = ngx_http_zstd_filter_create_cstream(r, ctx);
        if (ctx->cstream == NULL) {
            goto failed;
        }
    }

    if (in) {
        if (ngx_chain_add_copy(r->pool, &ctx->in, in) != NGX_OK) {
            goto failed;
        }

        r->connection->buffered |= NGX_HTTP_GZIP_BUFFERED;
    }

    if (ctx->yield) {
        if (r->connection->write->posted) {
            return NGX_AGAIN;
        }

        ctx->yield = 0;
        ctx->limit = ctx->consumed;
    }

    clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);

    if (ctx->nomem) {

        /* flush busy buffers */

        if (ngx_http_next_body_filter(r, NULL) == NGX_ERROR) {
            goto failed;
        }

        cl = NULL;

        ngx_chain_update_chains(r->pool, &ctx->free, &ctx->busy, &cl,
                                (ngx_buf_tag_t) &ngx_http_zstd_filter_module);

        flush = 0;
        ctx->nomem = 0;

    } else {
        flush = ctx->busy ? 1 : 0;
    }

    for ( ;; ) {

        /* cycle while we can write to a client */

        for ( ;; ) {

            rc = ngx_http_zstd_filter_add_data(r, ctx);

            if (rc == NGX_DECLINED) {
                break;
            }

            if (rc == NGX_AGAIN) {
                continue;
            }

            rc = ngx_http_zstd_filter_get_buf(r, ctx);

            if (rc == NGX_ERROR) {
                goto failed;
            }

            if (rc == NGX_DECLINED) {
                break;
            }

            rc = ngx_http_zstd_filter_compress(r, ctx);

            if (rc == NGX_ERROR) {
                goto failed;
            }

            if (rc == NGX_OK
                || (clcf->sendfile_max_chunk
                    && ctx->consumed - ctx->limit
                       >= clcf->sendfile_max_chunk))
            {
                break;
            }

            /* rc == NGX_AGAIN */
        }

        output = ctx->out != NULL || flush;
        if (!output) {
            goto yield;
        }

        rc = ngx_http_next_body_filter(r, ctx->out);

        if (rc == NGX_ERROR) {
            goto failed;
        }

        ngx_chain_update_chains(r->pool, &ctx->free, &ctx->busy, &ctx->out,
                                (ngx_buf_tag_t) &ngx_http_zstd_filter_module);

        ctx->last_out = &ctx->out;
        ctx->nomem = 0;
        flush = 0;

        if (ctx->done) {
            if (ngx_http_zstd_filter_release_cstream(ctx, r->connection->log,
                                                     0) != NGX_OK)
            {
                rc = NGX_ERROR;
            }

            return rc;
        }

    yield:

        /* Bound CPU work even when compressed output never blocks. */
        if (clcf->sendfile_max_chunk
            && ctx->consumed - ctx->limit >= clcf->sendfile_max_chunk
            && r->connection->write->ready
            && !r->connection->write->delayed)
        {
            ctx->yield = 1;
            r->connection->buffered |= NGX_HTTP_GZIP_BUFFERED;
            ngx_post_event(r->connection->write, &ngx_posted_next_events);
            return NGX_AGAIN;
        }

        if (!output) {
            return ctx->busy ? NGX_AGAIN : NGX_OK;
        }
    }

failed:

    ctx->done = 1;
    (void) ngx_http_zstd_filter_release_cstream(ctx, r->connection->log, 1);

    return NGX_ERROR;
}


static ngx_int_t
ngx_http_zstd_filter_compress(ngx_http_request_t *r, ngx_http_zstd_ctx_t *ctx)
{
    size_t                     rc, pos_in, pos_out, budget;
    ZSTD_EndDirective          action;
    ZSTD_inBuffer              input;
    ngx_uint_t                 finished;
    ngx_chain_t               *cl;
    ngx_buf_t                 *b;
    ngx_http_core_loc_conf_t  *clcf;

    ngx_log_debug7(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "zstd compress in: src:%p pos:%ud size: %ud, "
                   "dst:%p pos:%ud size:%ud flush:%d",
                   ctx->buffer_in.src, ctx->buffer_in.pos, ctx->buffer_in.size,
                   ctx->buffer_out.dst, ctx->buffer_out.pos,
                   ctx->buffer_out.size, ctx->flush);

    pos_in = ctx->buffer_in.pos;
    pos_out = ctx->buffer_out.pos;
    action = ZSTD_e_continue;

    /* Drain input first to preserve streaming compression parameters. */
    if (ctx->last && ctx->in == NULL) {
        action = ZSTD_e_end;

    } else if (ctx->buffer_in.pos == ctx->buffer_in.size && ctx->flush) {
        action = ZSTD_e_flush;
    }

    input = ctx->buffer_in;
    clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);
    if (action == ZSTD_e_continue && clcf->sendfile_max_chunk) {
        budget = clcf->sendfile_max_chunk
                 - ngx_min(ctx->consumed - ctx->limit,
                           clcf->sendfile_max_chunk);
        /* A blocked downstream may postpone the yield. */
        if (budget == 0) {
            budget = clcf->sendfile_max_chunk;
            ctx->limit = ctx->consumed;
        }
        input.size = input.pos + ngx_min(input.size - input.pos, budget);
    }

    rc = ZSTD_compressStream2(ctx->cstream, &ctx->buffer_out, &input, action);
    ctx->buffer_in.pos = input.pos;

    if (ZSTD_isError(rc)) {
        ngx_log_error(NGX_LOG_ALERT, r->connection->log, 0,
                      "ZSTD_compressStream2() failed: %s",
                      ZSTD_getErrorName(rc));

        return NGX_ERROR;
    }

    ngx_log_debug6(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "zstd compress out: src:%p pos:%ud size: %ud, "
                   "dst:%p pos:%ud size:%ud",
                   ctx->buffer_in.src, ctx->buffer_in.pos, ctx->buffer_in.size,
                   ctx->buffer_out.dst, ctx->buffer_out.pos,
                   ctx->buffer_out.size);

    if (ctx->buffer_in.pos != pos_in) {
        ctx->in_buf->pos += ctx->buffer_in.pos - pos_in;
    }
    ctx->consumed += ctx->buffer_in.pos - pos_in;
    ctx->out_buf->last += ctx->buffer_out.pos - pos_out;
    finished = action != ZSTD_e_continue && rc == 0;

    /* A completed flush must be forwarded even if it produced no bytes. */

    if (ngx_buf_size(ctx->out_buf) == 0 && !finished) {
        return NGX_AGAIN;
    }

    cl = ngx_alloc_chain_link(r->pool);
    if (cl == NULL) {
        return NGX_ERROR;
    }

    b = ctx->out_buf;

    if (ngx_buf_size(b) == 0) {
        b = ngx_calloc_buf(r->pool);
        if (b == NULL) {
            return NGX_ERROR;
        }
    }

    if (finished) {
        r->connection->buffered &= ~NGX_HTTP_GZIP_BUFFERED;

        b->flush = ctx->flush;
        b->last_buf = action == ZSTD_e_end;

        ctx->done = b->last_buf;
        ctx->flush = 0;
    }

    ctx->bytes_out += ngx_buf_size(b);

    cl->next = NULL;
    cl->buf = b;

    *ctx->last_out = cl;
    ctx->last_out = &cl->next;

    if (b == ctx->out_buf) {
        ngx_memzero(&ctx->buffer_out, sizeof(ZSTD_outBuffer));
    }

    return ctx->done ? NGX_OK : NGX_AGAIN;
}


static ngx_int_t
ngx_http_zstd_filter_add_data(ngx_http_request_t *r, ngx_http_zstd_ctx_t *ctx)
{
    if (ctx->buffer_in.pos < ctx->buffer_in.size
        || ctx->flush
        || ctx->last)
    {
        return NGX_OK;
    }

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "zstd in: %p", ctx->in);

    if (ctx->in == NULL) {
        return NGX_DECLINED;
    }

    ctx->in_buf = ctx->in->buf;
    ctx->in = ctx->in->next;

    ctx->flush = ctx->in_buf->flush;
    ctx->last = ctx->in_buf->last_buf;

    ctx->buffer_in.src = ctx->in_buf->pos;
    ctx->buffer_in.pos = 0;
    ctx->buffer_in.size = ngx_buf_size(ctx->in_buf);

    ctx->bytes_in += ngx_buf_size(ctx->in_buf);

    if (ctx->buffer_in.size == 0) {
        return NGX_AGAIN;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_zstd_filter_get_buf(ngx_http_request_t *r, ngx_http_zstd_ctx_t *ctx)
{
    ngx_chain_t               *cl;
    ngx_http_zstd_loc_conf_t  *zlcf;

    if (ctx->buffer_out.pos < ctx->buffer_out.size) {
        return NGX_OK;
    }

    zlcf = ngx_http_get_module_loc_conf(r, ngx_http_zstd_filter_module);

    if (ctx->free) {
        cl = ctx->free;
        ctx->free = ctx->free->next;
        ctx->out_buf = cl->buf;
        ngx_free_chain(r->pool, cl);

    } else if (ctx->bufs < zlcf->bufs.num) {
        ctx->out_buf = ngx_create_temp_buf(r->pool, zlcf->bufs.size);
        if (ctx->out_buf == NULL) {
            return NGX_ERROR;
        }

        ctx->out_buf->tag = (ngx_buf_tag_t) &ngx_http_zstd_filter_module;
        ctx->out_buf->recycled = 1;
        ctx->bufs++;

    } else {
        ctx->nomem = 1;
        return NGX_DECLINED;
    }

    ctx->buffer_out.dst = ctx->out_buf->pos;
    ctx->buffer_out.pos = 0;
    ctx->buffer_out.size = ctx->out_buf->end - ctx->out_buf->start;

    return NGX_OK;
}


static ZSTD_CStream *
ngx_http_zstd_filter_create_cstream(ngx_http_request_t *r,
    ngx_http_zstd_ctx_t *ctx)
{
    size_t                      rc;
    ZSTD_CStream               *cstream;
    ngx_http_zstd_loc_conf_t   *zlcf;

    zlcf = ngx_http_get_module_loc_conf(r, ngx_http_zstd_filter_module);

    if (!ngx_http_zstd_cached_cstream_busy) {
        ngx_http_zstd_cached_cstream_busy = 1;
        ctx->cached = 1;
        cstream = ngx_http_zstd_cached_cstream;
        if (cstream != NULL) {
            ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                           "zstd: reusing worker cstream");
        }
    } else {
        cstream = NULL;
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "zstd: worker cstream busy");
    }

    if (cstream == NULL) {
        cstream = ZSTD_createCStream();
        if (cstream == NULL) {
            if (ctx->cached) {
                ngx_http_zstd_cached_cstream_busy = 0;
                ctx->cached = 0;
            }
            ngx_log_error(NGX_LOG_ALERT, r->connection->log, 0,
                          "ZSTD_createCStream() failed");
            return NULL;
        }
        if (ctx->cached) {
            ngx_http_zstd_cached_cstream = cstream;
        }
    }

    ctx->cstream = cstream;

    rc = ZSTD_initCStream(cstream, zlcf->level);
    if (ZSTD_isError(rc)) {
        ngx_log_error(NGX_LOG_ALERT, r->connection->log, 0,
                      "ZSTD_initCStream() failed: %s",
                      ZSTD_getErrorName(rc));

        goto failed;
    }

    return cstream;

failed:
    (void) ngx_http_zstd_filter_release_cstream(ctx, r->connection->log, 1);
    return NULL;
}


static ngx_int_t
ngx_http_zstd_accept_encoding_ends_with_zstd(ngx_str_t *ae)
{
    static const u_char  suffix[] = ", zstd";
    size_t               suffix_len;

    suffix_len = sizeof(suffix) - 1; /* 6 */

    if (ae->len < suffix_len) {
        return NGX_DECLINED;
    }

    if (ngx_strncmp(ae->data + (ae->len - suffix_len),
                    suffix, suffix_len) == 0) {
        return NGX_OK;
    }

    return NGX_DECLINED;
}


static ngx_int_t
ngx_http_zstd_accept_encoding(ngx_str_t *ae)
{
    u_char  *p;

    p = ngx_strlcasestrn(ae->data, ae->data + ae->len,
                         (u_char *) "zstd", sizeof("zstd") - 2);
    if (p == NULL) {
        return NGX_DECLINED;
    }

    if (p == ae->data || (*(p - 1) == ',' || *(p - 1) == ' ')) {

        p += sizeof("zstd") - 1;

        if (p == ae->data + ae->len || *p == ',' || *p == ' ' || *p == ';') {
            return NGX_OK;
        }
    }

    return NGX_DECLINED;
}


static ngx_int_t
ngx_http_zstd_ok(ngx_http_request_t *r)
{
    ngx_table_elt_t  *ae;

    if (r != r->main) {
        return NGX_DECLINED;
    }

    ae = r->headers_in.accept_encoding;
    if (ae == NULL) {
        return NGX_DECLINED;
    }

    if (ae->value.len < sizeof("zstd") - 1) {
        return NGX_DECLINED;
    }

    if (ngx_http_zstd_accept_encoding_ends_with_zstd(&ae->value) != NGX_OK &&
        ngx_http_zstd_accept_encoding(&ae->value) != NGX_OK) {
        return NGX_DECLINED;
    }

    r->gzip_tested = 1;
    r->gzip_ok = 0;

    return NGX_OK;
}


static void *
ngx_http_zstd_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_zstd_loc_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_zstd_loc_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    /*
     * set by ngx_pcalloc():
     *
     *    conf->bufs.num = 0;
     *    conf->types = { NULL };
     *    conf->types_keys = NULL;
     */

    conf->enable = NGX_CONF_UNSET;
    conf->level = NGX_CONF_UNSET;
    conf->min_length = NGX_CONF_UNSET;

    return conf;
}


static char *
ngx_http_zstd_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_zstd_loc_conf_t *prev = parent;
    ngx_http_zstd_loc_conf_t *conf = child;

    ngx_conf_merge_value(conf->enable, prev->enable, 0);
    ngx_conf_merge_value(conf->level, prev->level, 1);
    ngx_conf_merge_value(conf->min_length, prev->min_length, 20);

    if (ngx_http_merge_types(cf, &conf->types_keys, &conf->types,
                             &prev->types_keys, &prev->types,
                             ngx_http_html_default_types))
    {
        return NGX_CONF_ERROR;
    }

    ngx_conf_merge_bufs_value(conf->bufs, prev->bufs,
                              (128 * 1024) / ngx_pagesize, ngx_pagesize);

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_http_zstd_filter_init(ngx_conf_t *cf)
{
    ngx_http_next_header_filter = ngx_http_top_header_filter;
    ngx_http_top_header_filter = ngx_http_zstd_header_filter;

    ngx_http_next_body_filter = ngx_http_top_body_filter;
    ngx_http_top_body_filter = ngx_http_zstd_body_filter;

    return NGX_OK;
}


static ngx_int_t
ngx_http_zstd_filter_release_cstream(ngx_http_zstd_ctx_t *ctx, ngx_log_t *log,
    ngx_uint_t discard)
{
    size_t  rc;

    if (ctx->cstream == NULL) {
        return NGX_OK;
    }

    if (ctx->cached && !discard) {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, log, 0,
                       "zstd: returned worker cstream");
        ngx_http_zstd_cached_cstream_busy = 0;
        ctx->cstream = NULL;
        ctx->cached = 0;
        return NGX_OK;
    }

    if (ctx->cached) {
        ngx_http_zstd_cached_cstream = NULL;
        ngx_http_zstd_cached_cstream_busy = 0;
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, log, 0,
                       "zstd: discarded worker cstream");
    }

    rc = ZSTD_freeCStream(ctx->cstream);
    ctx->cstream = NULL;
    ctx->cached = 0;
    if (ZSTD_isError(rc)) {
        ngx_log_error(NGX_LOG_ALERT, log, 0,
                      "ZSTD_freeCStream() failed: %s", ZSTD_getErrorName(rc));
        return NGX_ERROR;
    }
    return NGX_OK;
}


static void
ngx_http_zstd_filter_cleanup(void *data)
{
    ngx_http_zstd_ctx_t  *ctx = data;

    (void) ngx_http_zstd_filter_release_cstream(ctx, ngx_cycle->log, 1);
}


static void
ngx_http_zstd_filter_exit_process(ngx_cycle_t *cycle)
{
    size_t  rc;

    if (ngx_http_zstd_cached_cstream == NULL) {
        return;
    }

    rc = ZSTD_freeCStream(ngx_http_zstd_cached_cstream);
    ngx_http_zstd_cached_cstream = NULL;
    ngx_http_zstd_cached_cstream_busy = 0;
    if (ZSTD_isError(rc)) {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                      "ZSTD_freeCStream() failed: %s", ZSTD_getErrorName(rc));
    }
}


static ngx_int_t
ngx_http_zstd_add_variables(ngx_conf_t *cf)
{
    ngx_http_variable_t  *v;

    v = ngx_http_add_variable(cf, &ngx_http_zstd_ratio,
                              NGX_HTTP_VAR_NOCACHEABLE);
    if (v == NULL) {
        return NGX_ERROR;
    }

    v->get_handler = ngx_http_zstd_ratio_variable;

    return NGX_OK;
}


static ngx_int_t
ngx_http_zstd_ratio_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *vv, uintptr_t data)
{
    ngx_uint_t            ratio_int, ratio_frac;
    ngx_http_zstd_ctx_t  *ctx;

    ctx = ngx_http_get_module_ctx(r, ngx_http_zstd_filter_module);
    if (ctx == NULL || !ctx->done || ctx->bytes_out == 0) {
        vv->not_found = 1;
        return NGX_OK;
    }

    vv->data = ngx_pnalloc(r->pool, NGX_INT32_LEN + 3);
    if (vv->data == NULL) {
        return NGX_ERROR;
    }

    ratio_int = (ngx_uint_t) ctx->bytes_in / ctx->bytes_out;
    ratio_frac = (ngx_uint_t) (ctx->bytes_in * 1000 / ctx->bytes_out % 1000);

    vv->len = ngx_sprintf(vv->data, "%ui.%03ui", ratio_int, ratio_frac)
              - vv->data;

    vv->valid = 1;
    vv->no_cacheable = 1;

    return NGX_OK;
}


static char *
ngx_http_zstd_comp_level(ngx_conf_t *cf, void *post, void *data)
{
    ngx_int_t  *np = data;

    if (*np == 0 || *np < (ngx_int_t)ZSTD_minCLevel() || *np > ZSTD_maxCLevel()) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "zstd compress level must between %i and %i excluding 0",
                           (ngx_int_t)ZSTD_minCLevel(), ZSTD_maxCLevel());

        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}

static char *
ngx_conf_zstd_set_num_slot_with_negatives(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    char  *p = conf;

    ngx_int_t        *np;
    ngx_str_t        *value;
    ngx_conf_post_t  *post;


    np = (ngx_int_t *) (p + cmd->offset);

    if (*np != NGX_CONF_UNSET) {
        return "is duplicate";
    }

    value = cf->args->elts;

    if (*(value[1].data) == '-') {
        // Parse ignoring the leading '-' character
        *np = ngx_atoi(value[1].data + 1, value[1].len - 1);

        // NGX_ERROR is -1 so we need to check for that before making the parsed
        // result negative
        if (*np == NGX_ERROR) {
            return "invalid number";
        }

        *np = -*np;
    } else {
        *np = ngx_atoi(value[1].data, value[1].len);

        if (*np == NGX_ERROR) {
            return "invalid number";
        }
    }

    if (cmd->post) {
        post = cmd->post;
        return post->post_handler(cf, post, np);
    }

    return NGX_CONF_OK;
}
