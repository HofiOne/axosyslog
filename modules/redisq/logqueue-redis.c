/*
 * Copyright (c) 2018 Balabit
 * Copyright (c) 2018 Mehul Prajapati <mehulprajapati2802@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * As an additional exemption you are allowed to compile & link against the
 * OpenSSL libraries as published by the OpenSSL project. See the file
 * COPYING for details.
 *
 */

#include "logqueue-redis.h"
#include "logpipe.h"
#include "messages.h"
#include "serialize.h"
#include "logmsg/logmsg-serialize.h"
#include "stats/stats-registry.h"
#include "reloc.h"

#include <sys/types.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <sys/mman.h>
#endif
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

#define ITEMS_PER_MESSAGE 2

QueueType log_queue_redis_type = "FIFO";

static inline guint
_get_len_from_queue(GQueue *queue)
{
  return queue->length / ITEMS_PER_MESSAGE;
}

static gboolean
_send_redis_command(RedisServer *self, const char *format, ...)
{
  va_list ap;
  va_start(ap, format);

  g_mutex_lock(self->redis_thread_mutex);
  redisReply *reply = redisvCommand(self->ctx, format, ap);
  va_end(ap);
  g_mutex_unlock(self->redis_thread_mutex);

  msg_debug("redisq: send redis command");

  gboolean retval = reply && (reply->type != REDIS_REPLY_ERROR);
  if (reply)
    freeReplyObject(reply);
  return retval;
}

static redisReply *
_get_redis_reply(RedisServer *self, const char *format, ...)
{
  va_list ap;
  va_start(ap, format);

  g_mutex_lock(self->redis_thread_mutex);
  redisReply *reply = redisvCommand(self->ctx, format, ap);
  va_end(ap);
  g_mutex_unlock(self->redis_thread_mutex);

  msg_debug("redisq: get redis reply");

  gboolean retval = reply && (reply->type != REDIS_REPLY_ERROR);

  if (!retval)
    return NULL;

  return reply;
}

static gboolean
_check_ping_to_redis(RedisServer *self)
{
  return self->send_cmd(self, "ping");
}

static gboolean
_check_connection_to_redis(RedisServer *self)
{
  return self->is_conn(self);
}

static gboolean
_authenticate_to_redis(RedisServer *self, const gchar *password)
{
  return self->send_cmd(self, "AUTH %s", password);
}

static gboolean
_redis_connect(RedisServer *self)
{
  struct timeval timeout = {0, 0};

  timeout.tv_sec = self->redis_options->conn_timeout;

  msg_debug("redisq: Connecting to redis server");

  self->ctx = redisConnectWithTimeout(self->redis_options->host, self->redis_options->port, timeout);

  if (self->ctx->err)
    {
      msg_error("redisq: redis server error, suspending", evt_tag_str("error", self->ctx->errstr));
      return FALSE;
    }

  if (self->redis_options->auth)
    {
      if (!_authenticate_to_redis(self, self->redis_options->auth))
        {
          msg_error("redisq: failed to authenticate with redis server");
          return FALSE;
        }
    }

  if (!_check_ping_to_redis(self))
    {
      msg_error("redisq: ping to redis server failed");
      return FALSE;
    }

  msg_debug("redisq: Connection to redis server succeeded");

  return TRUE;
}

static gboolean
_redis_reconnect(RedisServer *self)
{
  msg_debug("redisq: reconnecting to redis server");

  if (self->ctx != NULL)
    {
      redisFree(self->ctx);
      self->ctx = NULL;
    }

  return _redis_connect(self);
}

static void
_redis_disconnect(RedisServer *self)
{
  msg_debug("redisq: redis disconnect");

  if (self->ctx)
    redisFree(self->ctx);

  self->ctx = NULL;
}

static gboolean
_is_redis_connection_alive(RedisServer *self)
{
  if (!_check_ping_to_redis(self))
    {
      if (!self->reconnect(self))
        {
          msg_error("redisq: Message was dropped, There is no redis connection");
          return FALSE;
        }
    }

  return TRUE;
}


static gint64
_get_length(LogQueue *s)
{
  LogQueueRedis *self = (LogQueueRedis *) s;
  redisReply *reply = NULL;
  glong qlen = 0;

  if (_check_connection_to_redis(self->redis_server))
    {
      reply = _get_redis_reply(self->redis_server, "LLEN %s", self->redis_queue_name);

      if (reply)
        {
          if (reply->type == REDIS_REPLY_INTEGER)
            qlen = reply->integer;

          freeReplyObject(reply);
        }
      else
        msg_error("redisq: get length redis reply failed");
    }

  msg_debug("redisq: get length", evt_tag_int("size", qlen));

  return qlen;
}

static void
_empty_queue(GQueue *q)
{
  while (q && (q->length) > 0)
    {
      LogMessage *msg;
      LogPathOptions path_options = LOG_PATH_OPTIONS_INIT;

      msg = g_queue_pop_head(q);
      POINTER_TO_LOG_PATH_OPTIONS(g_queue_pop_head(q), &path_options);

      log_msg_drop(msg, &path_options, AT_PROCESSED);
    }
}

static void
_push_tail(LogQueue *s, LogMessage *msg, const LogPathOptions *path_options)
{
  LogQueueRedis *self = (LogQueueRedis *) s;

  msg_debug("redisq: Pushing msg to tail");

  if (self->write_message(self, msg, path_options))
    {
      g_static_mutex_lock(&self->super.lock);
      log_queue_push_notify (&self->super);
      g_static_mutex_unlock(&self->super.lock);

      log_msg_ack (msg, path_options, AT_PROCESSED);
      return;
    }

  stats_counter_inc (self->super.dropped_messages);
  msg_error("redisq: write msg to redis server failed");
}

static LogMessage *
_pop_head(LogQueue *s, LogPathOptions *path_options)
{
  LogQueueRedis *self = (LogQueueRedis *) s;
  LogMessage *msg = NULL;

  msg_debug("redisq: Pop msg from head");

  msg = self->read_message(self, path_options);

  if (msg != NULL)
    {
      if (self->super.use_backlog)
        {
          log_msg_ref (msg);
          g_queue_push_tail (self->qbacklog, msg);
          g_queue_push_tail (self->qbacklog, LOG_PATH_OPTIONS_TO_POINTER (path_options));

          stats_counter_inc(self->super.queued_messages);
          stats_counter_add(self->super.memory_usage, log_msg_get_size(msg));

          if (!self->delete_message(self))
            msg_error("redisq: delete msg from redis server failed");
        }

      path_options->ack_needed = FALSE;
      log_msg_ack (msg, path_options, AT_PROCESSED);
    }

  return msg;
}

static void
_ack_backlog(LogQueue *s, gint num_msg_to_ack)
{
  LogQueueRedis *self = (LogQueueRedis *) s;
  LogMessage *msg;
  LogPathOptions path_options = LOG_PATH_OPTIONS_INIT;
  guint i;

  msg_debug("redisq: ack backlog");

  for (i = 0; i < num_msg_to_ack; i++)
    {
      if (self->qbacklog->length < ITEMS_PER_MESSAGE)
        return;

      msg = g_queue_pop_head (self->qbacklog);
      POINTER_TO_LOG_PATH_OPTIONS (g_queue_pop_head (self->qbacklog), &path_options);

      stats_counter_dec(self->super.queued_messages);
      stats_counter_sub(self->super.memory_usage, log_msg_get_size(msg));
      log_msg_unref(msg);
    }
}

static void
_rewind_backlog(LogQueue *s, guint rewind_count)
{
  LogQueueRedis *self = (LogQueueRedis *) s;
  LogPathOptions path_options = LOG_PATH_OPTIONS_INIT;
  LogMessage *msg = NULL;
  guint i;

  msg_debug("redisq: rewind backlog msg");

  rewind_count = MIN(rewind_count, _get_len_from_queue(self->qbacklog));

  for (i = 0; i < rewind_count; i++)
    {
      if (self->qbacklog->length > 0)
        {
          msg = g_queue_pop_head (self->qbacklog);
          POINTER_TO_LOG_PATH_OPTIONS (g_queue_pop_head (self->qbacklog), &path_options);

          stats_counter_dec(self->super.queued_messages);
          stats_counter_sub(self->super.memory_usage, log_msg_get_size(msg));

          if (!self->write_message(self, msg, &path_options))
            msg_error("redisq: Pushing backlog msg to redis server failed");
        }
    }
}

void
_backlog_all(LogQueue *s)
{
  msg_debug("redisq: backlog all");

  _rewind_backlog(s, -1);
}

static void
_free(LogQueue *s)
{
  LogQueueRedis *self = (LogQueueRedis *) s;

  msg_debug("redisq: free up");

  _empty_queue(self->qbacklog);
  g_queue_free(self->qbacklog);
  self->qbacklog = NULL;

  g_free(self->redis_queue_name);
  self->redis_queue_name = NULL;

  g_static_mutex_free(&self->super.lock);
  g_free(self->super.persist_name);
}

static LogMessage *
_read_message(LogQueueRedis *self, LogPathOptions *path_options)
{
  LogMessage *msg = NULL;
  GString *serialized;
  SerializeArchive *sa;
  redisReply *reply = NULL;

  msg_debug("redisq: read message from redis");

  if (!_check_connection_to_redis(self->redis_server))
    return NULL;

  reply = _get_redis_reply(self->redis_server, "LRANGE %s 0 0", self->redis_queue_name);

  if (reply)
    {
      if ((reply->elements == 1) && (reply->type == REDIS_REPLY_ARRAY))
        {
          msg_debug("redisq: got msg from redis server");

          serialized = g_string_new_len(reply->element[0]->str, reply->element[0]->len);
          g_string_set_size(serialized, reply->element[0]->len);
          sa = serialize_string_archive_new(serialized);

          msg = log_msg_new_empty();

          if (!log_msg_deserialize(msg, sa))
            msg_error("redisq: failed to deserialize a log message");

          serialize_archive_free(sa);
          g_string_free(serialized, TRUE);
        }

      freeReplyObject(reply);
    }

  return msg;
}

static gboolean
_write_message(LogQueueRedis *self, LogMessage *msg, const LogPathOptions *path_options)
{
  GString *serialized;
  SerializeArchive *sa;
  gboolean consumed = FALSE;

  if (_check_connection_to_redis(self->redis_server))
    {
      msg_debug("redisq: writing msg to redis queue");
      serialized = g_string_sized_new(4096);
      sa = serialize_string_archive_new(serialized);
      log_msg_serialize(msg, sa);

      msg_debug("redisq: serialized msg", evt_tag_str("list", self->redis_queue_name),
                evt_tag_str("msg", serialized->str), evt_tag_int("len", serialized->len));

      consumed = self->redis_server->send_cmd(self->redis_server, "RPUSH %s %b", self->redis_queue_name, serialized->str,
                                              serialized->len);

      serialize_archive_free(sa);
      g_string_free(serialized, TRUE);
    }

  return consumed;
}

static gboolean
_delete_message(LogQueueRedis *self)
{
  gboolean removed = FALSE;

  if (_check_connection_to_redis(self->redis_server))
    {
      msg_debug("redisq: removing msg from redis queue");

      removed = self->redis_server->send_cmd(self->redis_server, "LPOP %s", self->redis_queue_name);
    }

  return removed;
}

static void
_redis_server_init(RedisServer *self, RedisQueueOptions *options)
{
  self->redis_options = options;
  self->connect(self);
}

static void
_redis_server_deinit(RedisServer *self)
{
  self->redis_options = NULL;
  self->disconnect(self);
}

static void
_set_redis_queue_name(LogQueueRedis *self, gchar *keyprefix, const gchar *persist_name)
{
  gchar qname[1024] = "";

  g_snprintf(qname, sizeof(qname), "%s_%s", keyprefix, persist_name);
  self->redis_queue_name = g_strdup(qname);
}

static RedisServer *
_redis_server_new(RedisQueueOptions *options)
{
  RedisServer *self = g_new0(RedisServer, 1);

  msg_debug("redisq: redis server new");

  self->redis_thread_mutex = g_mutex_new();

  self->is_conn = _is_redis_connection_alive;
  self->connect = _redis_connect;
  self->reconnect = _redis_reconnect;
  self->disconnect = _redis_disconnect;
  self->send_cmd = _send_redis_command;

  _redis_server_init(self, options);

  return self;
}

static void
_redis_server_free(RedisServer *self)
{
  msg_debug("redisq: redis server free");

  g_mutex_free(self->redis_thread_mutex);
  _redis_server_deinit(self);
  g_free(self);
}

static void
_set_virtual_functions(LogQueueRedis *self)
{
  self->super.get_length = _get_length;
  self->super.push_tail = _push_tail;
  self->super.pop_head = _pop_head;
  self->super.ack_backlog = _ack_backlog;
  self->super.rewind_backlog = _rewind_backlog;
  self->super.rewind_backlog_all = _backlog_all;
  self->super.free_fn = _free;

  self->read_message = _read_message;
  self->write_message = _write_message;
  self->delete_message = _delete_message;
}

LogQueueRedis *
log_queue_redis_new_instance(RedisQueueOptions *options)
{
  LogQueueRedis *self = g_new0(LogQueueRedis, 1);

  msg_debug("redisq: log queue redis new instance");

  self->redis_server = _redis_server_new(options);
  return self;
}

LogQueue *
log_queue_redis_new(LogQueueRedis *self, const gchar *persist_name)
{
  msg_debug("redisq: log queue new");

  if (!_check_connection_to_redis(self->redis_server))
    return NULL;

  log_queue_init_instance(&self->super, persist_name);
  self->super.type = log_queue_redis_type;
  self->qbacklog = g_queue_new();
  _set_redis_queue_name(self, self->redis_server->redis_options->keyprefix, persist_name);

  _set_virtual_functions(self);
  return &self->super;
}

void
log_queue_redis_free(LogQueueRedis *self)
{
  msg_debug("redisq: log queue redis free");

  if (self)
    {
      _redis_server_free(self->redis_server);
      g_free(self);
    }
}
