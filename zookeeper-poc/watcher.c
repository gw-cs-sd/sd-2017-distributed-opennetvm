#include <string.h>
#include <zookeeper/proto.h>
#include <zookeeper/zookeeper.h>

/**
 * ZK Status Watcher Functions
 * From: https://github.com/apache/zookeeper/blob/master/src/c/src/cli.c#L78
 */
static clientid_t myid;
static int is_connected;

static const char* state2String(int state){
  if (state == 0)
    return "CLOSED_STATE";
  if (state == ZOO_CONNECTING_STATE)
    return "CONNECTING_STATE";
  if (state == ZOO_ASSOCIATING_STATE)
    return "ASSOCIATING_STATE";
  if (state == ZOO_CONNECTED_STATE)
    return "CONNECTED_STATE";
  if (state == ZOO_EXPIRED_SESSION_STATE)
    return "EXPIRED_SESSION_STATE";
  if (state == ZOO_AUTH_FAILED_STATE)
    return "AUTH_FAILED_STATE";

  return "INVALID_STATE";
}

static const char* type2String(int state){
  if (state == ZOO_CREATED_EVENT)
    return "CREATED_EVENT";
  if (state == ZOO_DELETED_EVENT)
    return "DELETED_EVENT";
  if (state == ZOO_CHANGED_EVENT)
    return "CHANGED_EVENT";
  if (state == ZOO_CHILD_EVENT)
    return "CHILD_EVENT";
  if (state == ZOO_SESSION_EVENT)
    return "SESSION_EVENT";
  if (state == ZOO_NOTWATCHING_EVENT)
    return "NOTWATCHING_EVENT";

  return "UNKNOWN_EVENT_TYPE";
}

static const char* status2String(int status) {
    if (status == ZOK)
        return "OK";
    if (status == ZNONODE)
        return "NO_NODE";
    if (status == ZNODEEXISTS)
        return "NODE_EXISTS";
    if (status == ZNOAUTH)
        return "NO_AUTH";
    if (status == ZBADVERSION)
        return "BAD_VERSION";
    if (status == ZNOTEMPTY)
        return "NOT_EMPTY";
    if (status == ZBADARGUMENTS)
        return "BAD_ARGUMENTS";
    if (status == ZINVALIDSTATE)
        return "INVALID_STATE";
    if (status == ZMARSHALLINGERROR)
        return "MARSHALLING_ERROR";
    if (status == ZNOCHILDRENFOREPHEMERALS)
        return "NO_CHILDREN_FOR_EPHEMERALS";
}

void watcher(zhandle_t *zzh, int type, int state, const char *path,
             void* context)
{
    /* Be careful using zh here rather than zzh - as this may be mt code
     * the client lib may call the watcher before zookeeper_init returns */

    fprintf(stderr, "Watcher %s state = %s", type2String(type), state2String(state));
    if (path && strlen(path) > 0) {
      fprintf(stderr, " for path %s", path);
    }
    fprintf(stderr, "\n");

    if (type == ZOO_SESSION_EVENT) {
        if (state == ZOO_CONNECTED_STATE) {
            const clientid_t *id = zoo_client_id(zzh);
            if (myid.client_id == 0 || myid.client_id != id->client_id) {
                myid = *id;
                is_connected = 1;
                fprintf(stderr, "Got a new session id: 0x%llx\n",
                        myid.client_id);
            }
        }
    }
}


