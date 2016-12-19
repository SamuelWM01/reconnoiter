/*
 * Copyright (c) 2016, Circonus, Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name Circonus, Inc. nor the names of its contributors
 *       may be used to endorse or promote products derived from this
 *       software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <mtev_defines.h>
#include <mtev_conf.h>
#include <mtev_fq.h>
#include <mtev_hash.h>
#include <mtev_str.h>
#include <mtev_log.h>
#include <ck_fifo.h>
#include <mtev_fq.h>

#include <openssl/md5.h>

#include <noit_metric_director.h>
#include <noit_check_log_helpers.h>
#include <noit_message_decoder.h>

struct fq_conn_s;
struct fq_msg;

struct hash_and_time {
  mtev_hash_table hash;
  uint32_t last_touched_s;
};

static __thread struct {
  int id;
  ck_fifo_spsc_t *fifo;
} my_lane;

static mtev_atomic64_t number_of_messages_received = 0;
static mtev_atomic64_t number_of_messages_distributed = 0;

static int nthreads;
static volatile void **thread_queues;
static mtev_hash_table id_level;
static mtev_hash_table dedupe_hashes;
typedef unsigned short caql_cnt_t;
static caql_cnt_t *check_interests;
static mtev_boolean dedupe = mtev_true;

static void noit_metric_director_free_message(noit_metric_message_t* message) {
  if(message->original_message) {
    free(message->original_message);
    message->original_message = NULL;
  }
  free(message);
}

void
noit_metric_director_message_ref(void *m) {
  noit_metric_message_t *message = (noit_metric_message_t *)m;
  mtev_atomic_inc32(&message->refcnt);
}

void
noit_metric_director_message_deref(void *m) {
  noit_metric_message_t *message = (noit_metric_message_t *)m;
  if (mtev_atomic_dec32(&message->refcnt) == 0) {
    noit_metric_director_free_message(message);
  }
}

static int
get_my_lane() {
  if(my_lane.fifo == NULL) {
    int new_thread;
    my_lane.fifo = calloc(1, sizeof(ck_fifo_spsc_t));
    ck_fifo_spsc_init(my_lane.fifo, malloc(sizeof(ck_fifo_spsc_entry_t)));
    for(new_thread=0;new_thread<nthreads;new_thread++) {
      if(mtev_atomic_casptr(&thread_queues[new_thread], my_lane.fifo, NULL) == NULL) break;
    }
    mtevAssert(new_thread<nthreads);
    my_lane.id = new_thread;
    mtevL(mtev_debug, "Assigning thread(%p) to %d\n", (void*)(uintptr_t)pthread_self(), my_lane.id);
  }
  return my_lane.id;
}

void
noit_adjust_checks_interest(short cnt) {
  int thread_id, icnt;

  thread_id = get_my_lane();

  icnt = check_interests[thread_id];
  icnt += cnt;
  if(icnt < 0) icnt = 0;
  mtevAssert(icnt <= 0xffff);
  check_interests[thread_id] = icnt;
}

void
noit_adjust_metric_interest(uuid_t id, const char *metric, short cnt) {
  int thread_id, icnt;
  void *vhash, *vinterests;
  mtev_hash_table *level2;
  caql_cnt_t *interests;

  thread_id = get_my_lane();

  /* Get us our UUID->HASH(METRICS) mapping */
  if(!mtev_hash_retrieve(&id_level, (const char *)id, UUID_SIZE, &vhash)) {
    int found;
    uuid_t *copy = malloc(UUID_SIZE);
    level2 = calloc(1,sizeof(*level2));
    mtev_hash_init_locks(level2, MTEV_HASH_DEFAULT_SIZE, MTEV_HASH_LOCK_MODE_MUTEX);
    uuid_copy(*copy, id);
    if(!mtev_hash_store(&id_level, (const char *)copy, UUID_SIZE, level2)) {
      free(copy);
      free(level2);
    }
    found = mtev_hash_retrieve(&id_level, (const char *)id, UUID_SIZE, &vhash);
    mtevAssert(found);
  }
  level2 = vhash;

  if(!mtev_hash_retrieve(level2, metric, strlen(metric), &vinterests)) {
    int found;
    char *metric_copy;
    metric_copy = strdup(metric);
    vinterests = calloc(nthreads, sizeof(*interests));
    if(!mtev_hash_store(level2, metric_copy, strlen(metric_copy), vinterests)) {
      free(metric_copy);
      free(vinterests);
    }
    found = mtev_hash_retrieve(level2, metric, strlen(metric), &vinterests);
    mtevAssert(found);
  }
  interests = vinterests;
  /* This is fine because thread_id is only ours */
  icnt = interests[thread_id];
  icnt += cnt;
  if(icnt < 0) icnt = 0;
  mtevAssert(icnt <= 0xffff);
  interests[thread_id] = icnt;
}

static void
distribute_message_with_interests(caql_cnt_t *interests, noit_metric_message_t *message) {
  mtev_atomic_inc64(&number_of_messages_received);

  int i;
  for(i = 0; i < nthreads; i++) {
    if(interests[i] > 0) {
      ck_fifo_spsc_t *fifo = (ck_fifo_spsc_t *) thread_queues[i];
      ck_fifo_spsc_entry_t *fifo_entry;
      ck_fifo_spsc_enqueue_lock(fifo);
      fifo_entry = ck_fifo_spsc_recycle(fifo);
      if(!fifo_entry) fifo_entry = malloc(sizeof(ck_fifo_spsc_entry_t));
      noit_metric_director_message_ref(message);
      ck_fifo_spsc_enqueue(fifo, fifo_entry, message);
      ck_fifo_spsc_enqueue_unlock(fifo);

      mtev_atomic_inc64(&number_of_messages_distributed);
    }
  }
}

static void
distribute_metric(noit_metric_message_t *message) {
  void *vhash, *vinterests;
  char uuid_str[UUID_STR_LEN + 1];
  uuid_unparse_lower(message->id.id, uuid_str);
  if(mtev_hash_retrieve(&id_level, (const char *) &message->id.id, UUID_SIZE,
      &vhash)) {
    if(mtev_hash_retrieve((mtev_hash_table *) vhash, message->id.name,
        message->id.name_len, &vinterests)) {
      caql_cnt_t *interests = vinterests;
      distribute_message_with_interests(interests, message);
    }
  }
}

static void
distribute_check(noit_metric_message_t *message) {
  distribute_message_with_interests(check_interests, message);
}

static mtev_hash_table *
get_dedupe_hash(uint64_t whence)
{
  struct hash_and_time *hash_with_time;
  mtev_hash_table *hash;

  if (mtev_hash_retrieve(&dedupe_hashes, (const char *)&whence, sizeof(whence), (void **)&hash_with_time) == 1) {
    hash = &hash_with_time->hash;
  } else {
    hash_with_time = calloc(1, sizeof(struct hash_and_time));

    mtev_hash_init_locks(&hash_with_time->hash, MTEV_HASH_DEFAULT_SIZE, MTEV_HASH_LOCK_MODE_MUTEX);
    uint64_t *stored_ts = calloc(1, sizeof(uint64_t));
    *stored_ts = whence;
    if (mtev_hash_store(&dedupe_hashes, (const char *)stored_ts, sizeof(*stored_ts), hash_with_time) == 0) {
      /* ugh, someone beat us */
      free(stored_ts);
      mtev_hash_destroy(&hash_with_time->hash, NULL, NULL);
      free(hash_with_time);
      if (mtev_hash_retrieve(&dedupe_hashes, (const char *)&whence, sizeof(whence), (void **)&hash_with_time) == 0) {
        return NULL;
      }
    }
    hash = &hash_with_time->hash;
  }

  hash_with_time->last_touched_s = mtev_gethrtime() / 1000000000;
  return hash;
}

static int
prune_old_dedupe_hashes(eventer_t e, int mask, void *unused,
    struct timeval *now) {

  mtev_hash_iter iter = MTEV_HASH_ITER_ZERO;
  uint64_t now_hrtime = mtev_gethrtime() / 1000000000;
  const char *k;
  int klen;
  void *data;
  struct hash_and_time *hash_with_time;

  struct removable_hashes {
    uint64_t key;
    struct hash_and_time *data;
    struct removable_hashes *next;
  };

  struct removable_hashes *head = NULL;
  struct removable_hashes *tail = NULL;

  /* build a list of expirable items */
  while(mtev_hash_next(&dedupe_hashes, &iter, &k, &klen, &data)) {
    hash_with_time = data;
    if (now_hrtime > hash_with_time->last_touched_s && now_hrtime - hash_with_time->last_touched_s > 10) {
      struct removable_hashes *h = calloc(1, sizeof(struct removable_hashes));
      h->key = *(uint64_t *)k;
      h->data = hash_with_time;
      if (tail != NULL) {
        tail->next = h;
      }
      tail = h;

      if (head == NULL) {
        head = tail;
      }
    }
  }

  /* expire them */
  while (head != NULL) {
    mtev_hash_delete(&dedupe_hashes, (const char *)&head->key, sizeof(head->key), free, NULL);
    mtev_hash_destroy(&head->data->hash, free, NULL);
    free(head->data);
    struct removable_hashes *prev = head;
    head = head->next;
    free(prev);
  }

  e->whence.tv_sec = now->tv_sec + 5;
  return 1;
}

static void
distribute_message(noit_metric_message_t *message) {
  if(message->type == 'H' || message->type == 'M') {
    distribute_metric(message);
  } else {
    distribute_check(message);
  }
}

noit_metric_message_t *noit_metric_director_lane_next() {
  noit_metric_message_t *msg = NULL;
  if(my_lane.fifo == NULL)
    return NULL;
  ck_fifo_spsc_dequeue_lock(my_lane.fifo);
  if(ck_fifo_spsc_dequeue(my_lane.fifo, &msg) == false) {
    msg = NULL;
  }
  ck_fifo_spsc_dequeue_unlock(my_lane.fifo);
  return msg;
}

static void
handle_metric_buffer(const char *payload, int payload_len,
    int has_noit) {

  if (payload_len <= 0) {
    return;
  }

  switch (payload[0]) {
    case 'C':
    case 'D':
    case 'S':
    case 'H':
    case 'M':
      {
        // mtev_fq will free the fq_msg -> copy the payload
        char *copy = mtev__strndup(payload, payload_len);
        noit_metric_message_t *message = calloc(1, sizeof(noit_metric_message_t));

        message->type = copy[0];
        message->original_message = copy;
        message->original_message_len = payload_len;
        noit_metric_director_message_ref(message);

        int rv = noit_message_decoder_parse_line(copy, payload_len,
            &message->id.id, &message->id.name,
            &message->id.name_len, &message->value, has_noit);

        if(rv == 1) {
          distribute_message(message);
        }

        noit_metric_director_message_deref(message);
      }
      break;
    case 'B':
      {
        int n_metrics, i;
        char **metrics = NULL;
        n_metrics = noit_check_log_b_to_sm((const char *)payload, payload_len,
            &metrics, has_noit);
        for(i = 0; i < n_metrics; i++) {
          handle_metric_buffer(metrics[i], strlen(metrics[i]), false);
          free(metrics[i]);
        }
        free(metrics);
      }
      break;
    default: ;
      /* ignored */
  }
}

static uint64_t
get_message_time(const char* msg, int msg_length) {
  const int minimum_bytes_before_second_tab = sizeof("M\t1.2.3.4");
  if (msg_length <= minimum_bytes_before_second_tab) {
    mtevL(mtev_error, "Unable to retrieve timestamp from message: %s\n", msg);
    return 0;
  }

  const char* time_str = (char*) memchr(msg + minimum_bytes_before_second_tab,
      '\t', msg_length - minimum_bytes_before_second_tab);
  if (time_str) {
    ++time_str;
  }

  if (time_str == NULL) {
    mtevL(mtev_error, "Unable to retrieve timestamp from message: %s\n", msg);
    return 0;
  }

  return atol(time_str);
}

static mtev_boolean
check_duplicate(char *payload, size_t payload_len) {
  if (dedupe) {
    unsigned char *digest = malloc(MD5_DIGEST_LENGTH);
    MD5((unsigned char*)payload, payload_len, digest);

    uint64_t whence = get_message_time(payload, payload_len);
    if(whence > 0) {
      mtev_hash_table *hash = get_dedupe_hash(whence);
      if (hash) {
        int x = mtev_hash_store(hash, (const char *)digest, MD5_DIGEST_LENGTH, (void *)0x1);
        if (x == 0) {
          /* this is a dupe */
          free(digest);
          return mtev_true;
        }
      } else {
        free(digest);
      }
    } else {
      free(digest);
    }
  }
  return mtev_false;
}

static mtev_hook_return_t
handle_fq_message(void *closure, struct fq_conn_s *client, int idx, struct fq_msg *m,
                  void *payload, size_t payload_len) {
  if(check_duplicate(payload, payload_len) == mtev_false) {
    handle_metric_buffer(payload, payload_len, 1);
  }
  return MTEV_HOOK_CONTINUE;
}
static mtev_hook_return_t
handle_log_line(void *closure, mtev_log_stream_t ls, const struct timeval *whence,
                const char *timebuf, int timebuflen,
                const char *debugbuf, int debugbuflen,
                const char *line, size_t len) {
  if(!ls) return MTEV_HOOK_CONTINUE;
  const char *name = mtev_log_stream_get_name(ls);
  if(!name ||
     (strcmp(name,"metrics") && strcmp(name,"bundle") &&
      strcmp(name,"check") && strcmp(name,"status")))
    return MTEV_HOOK_CONTINUE;
  handle_metric_buffer(line, len, 0);
  return MTEV_HOOK_CONTINUE;
}

void
noit_metric_director_dedupe(mtev_boolean d) 
{
  dedupe = d;
}

void noit_metric_director_init() {
  nthreads = eventer_loop_concurrency();
  mtevAssert(nthreads > 0);
  thread_queues = calloc(sizeof(*thread_queues),nthreads);
  check_interests = calloc(sizeof(*check_interests),nthreads);
  if(mtev_fq_handle_message_hook_register_available())
    mtev_fq_handle_message_hook_register("metric-director", handle_fq_message, NULL);
  mtev_log_line_hook_register("metric-director", handle_log_line, NULL);

  eventer_t e = eventer_alloc();
  e->mask = EVENTER_TIMER;
  e->callback = prune_old_dedupe_hashes;
  mtev_gettimeofday(&e->whence, NULL);
  e->whence.tv_sec += 2;
  eventer_add_timed(e);
}

void noit_metric_director_init_globals(void) {
  mtev_hash_init_locks(&id_level, MTEV_HASH_DEFAULT_SIZE, MTEV_HASH_LOCK_MODE_MUTEX);
  mtev_hash_init_locks(&dedupe_hashes, MTEV_HASH_DEFAULT_SIZE, MTEV_HASH_LOCK_MODE_MUTEX);
}

int64_t
noit_metric_director_get_messages_received() {
  return number_of_messages_received;
}

int64_t
noit_metric_director_get_messages_distributed() {
 return number_of_messages_distributed;
}
