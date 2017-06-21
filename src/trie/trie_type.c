#include "../dep/libnu/libnu.h"
#include "../rmutil/strings.h"
#include "../rmutil/util.h"
#include "../util/heap.h"
#include "rune_util.h"

#include "trie_type.h"
#include "../commands.h"
#include <math.h>
#include <sys/param.h>
#include <time.h>
#include <string.h>
#include <limits.h>

Trie *NewTrie() {
  Trie *tree = RedisModule_Alloc(sizeof(Trie));
  rune *rs = strToRunes("", 0);
  rune *payload = strToRunes("", 0);
  tree->root = __newTrieNode(rs, 0, 0, payload, 0, 0, 0, 0);
  tree->size = 0;
  free(rs);
  free(payload);
  return tree;
}

int Trie_Insert(Trie *t, RedisModuleString *s, double score, int incr, const char *payload,
                size_t payloadSize) {
  size_t len;
  char *str = (char *)RedisModule_StringPtrLen(s, &len);
  int ret = Trie_InsertStringBuffer(t, str, len, score, incr, payload, payloadSize);
  return ret;
}

int Trie_InsertStringBuffer(Trie *t, char *s, size_t len, double score, int incr,
                            const char *payload, size_t payloadSize) {
  rune *runes = strToRunes(s, &len);
  rune *runesPayload = payload == NULL ? NULL : strToRunes(payload, &payloadSize);
  if (len && len < MAX_STRING_LEN && payloadSize < MAX_STRING_LEN) {
    int rc = TrieNode_Add(&t->root, runes, len, runesPayload, payloadSize, (float)score,
                          incr ? ADD_INCR : ADD_REPLACE);
    free(runes);
    if (runesPayload != NULL) free(runesPayload);
    t->size += rc;
    return rc;
  } else {
    if (runesPayload != NULL) free(runesPayload);
  }
  return 0;
}

int Trie_Delete(Trie *t, char *s, size_t len) {

  rune *runes = strToRunes(s, &len);
  int rc = TrieNode_Delete(t->root, runes, len);
  t->size -= rc;
  free(runes);
  return rc;
}

void TrieSearchResult_Free(TrieSearchResult *e) {
  if (e->str) {
    free(e->str);
    e->str = NULL;
  }
  if (e->payload) {
    free(e->payload);
    e->payload = NULL;
  }
  free(e);
}

static int cmpEntries(const void *p1, const void *p2, const void *udata) {
  const TrieSearchResult *e1 = p1, *e2 = p2;

  if (e1->score < e2->score) {
    return 1;
  } else if (e1->score > e2->score) {
    return -1;
  }
  return 0;
}

TrieIterator *Trie_IteratePrefix(Trie *t, char *prefix, size_t len, int maxDist) {
  size_t rlen;
  rune *runes = strToFoldedRunes(prefix, &rlen);
  DFAFilter *fc = malloc(sizeof(*fc));
  *fc = NewDFAFilter(runes, rlen, maxDist, 1);

  TrieIterator *it = TrieNode_Iterate(t->root, FilterFunc, StackPop, fc);
  free(runes);
  return it;
}

Vector *Trie_Search(Trie *tree, char *s, size_t len, size_t num, int maxDist, int prefixMode,
                    int trim, int optimize) {
  heap_t *pq = malloc(heap_sizeof(num));
  heap_init(pq, cmpEntries, NULL, num);

  size_t rlen;
  rune *runes = strToFoldedRunes(s, &rlen);
  DFAFilter fc = NewDFAFilter(runes, rlen, maxDist, prefixMode);

  TrieIterator *it = TrieNode_Iterate(tree->root, FilterFunc, StackPop, &fc);
  // TrieIterator *it = TrieNode_Iterate(tree->root,NULL, NULL, NULL);
  rune *rstr;
  t_len slen;
  float score;
  rune *rpayload;
  t_len rpayloadSize;

  TrieSearchResult *pooledEntry = NULL;
  int dist = maxDist + 1;
  while (TrieIterator_Next(it, &rstr, &slen, &rpayload, &rpayloadSize, &score, &dist)) {
    if (pooledEntry == NULL) {
      pooledEntry = malloc(sizeof(TrieSearchResult));
      pooledEntry->str = NULL;
      pooledEntry->payload = NULL;
      pooledEntry->plen = 0;
    }
    TrieSearchResult *ent = pooledEntry;

    ent->score = slen > 0 && slen == rlen && memcmp(runes, rstr, slen) == 0 ? INT_MAX : score;

    if (maxDist > 0) {
      // factor the distance into the score
      ent->score *= exp((double)-(2 * dist));
    }
    // in prefix mode we also factor in the total length of the suffix
    if (prefixMode) {
      ent->score /= sqrt(1 + (slen >= len ? slen - len : len - slen));
    }

    if (heap_count(pq) < heap_size(pq)) {
      ent->str = runesToStr(rstr, slen, &ent->len);
      ent->payload = rpayload == NULL ? NULL : runesToStr(rpayload, rpayloadSize, &ent->plen);
      heap_offerx(pq, ent);
      pooledEntry = NULL;

      if (heap_count(pq) == heap_size(pq)) {
        TrieSearchResult *qe = heap_peek(pq);
        it->minScore = qe->score;
      }

    } else {
      if (ent->score >= it->minScore) {
        pooledEntry = heap_poll(pq);
        free(pooledEntry->str);
        if (pooledEntry->payload != NULL) free(pooledEntry->payload);
        pooledEntry->str = NULL;
        pooledEntry->payload = NULL;
        pooledEntry->plen = 0;
        ent->str = runesToStr(rstr, slen, &ent->len);
        ent->payload = rpayload == NULL ? NULL : runesToStr(rpayload, rpayloadSize, &ent->plen);
        heap_offerx(pq, ent);

        // get the new minimal score
        TrieSearchResult *qe = heap_peek(pq);
        if (qe->score > it->minScore) {
          it->minScore = qe->score;
        }

      } else {
        pooledEntry = ent;
      }
    }

    // dist = maxDist + 3;
  }

  if (pooledEntry) {
    TrieSearchResult_Free(pooledEntry);
  }

  // printf("Nodes consumed: %d/%d (%.02f%%)\n", it->nodesConsumed,
  //        it->nodesConsumed + it->nodesSkipped,
  //        100.0 * (float)(it->nodesConsumed) / (float)(it->nodesConsumed +
  //        it->nodesSkipped));

  // put the results from the heap on a vector to return
  size_t n = MIN(heap_count(pq), num);
  Vector *ret = NewVector(TrieSearchResult *, n);
  for (int i = 0; i < n; ++i) {
    TrieSearchResult *h = heap_poll(pq);
    Vector_Put(ret, n - i - 1, h);
  }

  // trim the results to remove irrelevant results
  if (trim) {
    float maxScore = 0;
    int i;
    for (i = 0; i < n; ++i) {
      TrieSearchResult *h;
      Vector_Get(ret, i, &h);

      if (maxScore && h->score < maxScore / SCORE_TRIM_FACTOR) {
        // TODO: Fix trimming the vector
        ret->top = i;
        break;
      }
      maxScore = MAX(maxScore, h->score);
    }

    for (; i < n; ++i) {
      TrieSearchResult *h;
      Vector_Get(ret, i, &h);
      TrieSearchResult_Free(h);
    }
  }

  free(runes);
  TrieIterator_Free(it);
  DFAFilter_Free(&fc);
  heap_free(pq);

  return ret;
}

/***************************************************************
*
*                       Trie type methods
*
***************************************************************/

/* declaration of the type for redis registration. */
RedisModuleType *TrieType;

void *TrieType_GenericLoad(RedisModuleIO *rdb) {

  uint64_t elements = RedisModule_LoadUnsigned(rdb);
  Trie *tree = NewTrie();

  while (elements--) {
    size_t len;
    size_t payloadSize;
    char *str = RedisModule_LoadStringBuffer(rdb, &len);
    double score = RedisModule_LoadDouble(rdb);
    char *payload = RedisModule_LoadStringBuffer(rdb, &payloadSize);
    payloadSize--;
    Trie_InsertStringBuffer(tree, str, len - 1, score, 0, payload, payloadSize);
    RedisModule_Free(str);
    if (payload != NULL) RedisModule_Free(payload);
  }
  // TrieNode_Print(tree->root, 0, 0);
  return tree;
}

void *TrieType_RdbLoad(RedisModuleIO *rdb, int encver) {
  if (encver != 0) {
    return NULL;
  }
  return TrieType_GenericLoad(rdb);
}

void TrieType_RdbSave(RedisModuleIO *rdb, void *value) {
  Trie *tree = (Trie *)value;
  RedisModule_SaveUnsigned(rdb, tree->size);
  RedisModuleCtx *ctx = RedisModule_GetContextFromIO(rdb);
  RedisModule_Log(ctx, "notice", "Trie: saving %zd nodes.", tree->size);
  int count = 0;
  if (tree->root) {
    TrieIterator *it = TrieNode_Iterate(tree->root, NULL, NULL, NULL);
    rune *rstr;
    t_len len;
    float score;
    rune *rpayload;
    t_len rpayloadSize;

    while (TrieIterator_Next(it, &rstr, &len, &rpayload, &rpayloadSize, &score, NULL)) {
      size_t slen = 0;
      size_t splen = 0;
      char *s = runesToStr(rstr, len, &slen);
      char *sp = rpayload == NULL ? NULL : runesToStr(rpayload, rpayloadSize, &splen);
      RedisModule_SaveStringBuffer(rdb, s, slen + 1);
      RedisModule_SaveDouble(rdb, (double)score);
      if (sp != NULL) RedisModule_SaveStringBuffer(rdb, sp, splen + 1);
      free(s);
      if (sp != NULL) free(sp);
      count++;
    }
    if (count != tree->size) {
      RedisModule_Log(ctx, "warning", "Trie: saving %zd nodes actually iterated only %zd nodes",
                      tree->size, count);
    }
    TrieIterator_Free(it);
  }
}

void TrieType_AofRewrite(RedisModuleIO *aof, RedisModuleString *key, void *value) {
  Trie *tree = (Trie *)value;

  if (tree->root) {
    TrieIterator *it = TrieNode_Iterate(tree->root, NULL, NULL, NULL);
    rune *rstr;
    t_len len;
    float score;
    rune *rpayload;
    t_len rpayloadSize;

    while (TrieIterator_Next(it, &rstr, &len, &rpayload, &rpayloadSize, &score, NULL)) {
      size_t slen = 0;
      size_t splen = 0;
      char *s = runesToStr(rstr, len, &slen);
      char *sp = rpayload == NULL ? NULL : runesToStr(rpayload, rpayloadSize, &splen);
      RedisModule_EmitAOF(aof, TRIE_ADD_AOF_CMD, "sbdb", key, s, slen, (double)score, sp, splen);
      free(s);
      if (sp != NULL) free(sp);
    }

    TrieIterator_Free(it);
  }
}

void TrieType_Digest(RedisModuleDigest *digest, void *value) {
  /* TODO: The DIGEST module interface is yet not implemented. */
}

void TrieType_Free(void *value) {
  Trie *tree = value;
  if (tree->root) {

    TrieNode_Free(tree->root);
  }

  RedisModule_Free(tree);
}

int TrieType_Register(RedisModuleCtx *ctx) {

  RedisModuleTypeMethods tm = {.version = REDISMODULE_TYPE_METHOD_VERSION,
                               .rdb_load = TrieType_RdbLoad,
                               .rdb_save = TrieType_RdbSave,
                               .aof_rewrite = TrieType_AofRewrite,
                               .free = TrieType_Free};

  TrieType = RedisModule_CreateDataType(ctx, "trietype0", 0, &tm);
  if (TrieType == NULL) {
    return REDISMODULE_ERR;
  }

  return REDISMODULE_OK;
}
