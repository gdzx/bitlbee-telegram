#ifndef TGX_H
#define TGX_H

#define TD_API_KEY "API_KEY"
#define TD_API_HASH "API_HASH"

typedef void tgx_clt_t;

/**
 * tdlib API
 */

tgx_clt_t *td_json_client_create();
void td_json_client_send(tgx_clt_t *, const char *request);
const char *td_json_client_receive(tgx_clt_t *, double timeout);
const char *td_json_client_execute(tgx_clt_t *, const char *request);
void td_json_client_destroy(tgx_clt_t *);
void td_set_log_verbosity_level(int level);

#endif
