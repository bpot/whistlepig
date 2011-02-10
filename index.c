#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include "whistlepig.h"

#define PATH_BUF_SIZE 4096

int wp_index_exists(const char* pathname_base) {
  char buf[PATH_BUF_SIZE];
  snprintf(buf, PATH_BUF_SIZE, "%s0", pathname_base);
  return wp_segment_exists(buf);
}

wp_error* wp_index_create(wp_index** indexptr, const char* pathname_base) {
  char buf[PATH_BUF_SIZE];

  snprintf(buf, PATH_BUF_SIZE, "%s0", pathname_base);
  if(wp_segment_exists(buf)) RAISE_ERROR("index with base path '%s' already exists", pathname_base);

  wp_index* index = *indexptr = malloc(sizeof(wp_index));
  index->pathname_base = pathname_base;
  index->num_segments = 1;
  index->sizeof_segments = 1;
  index->open = 1;
  index->segments = malloc(sizeof(wp_segment));
  index->docid_offsets = malloc(sizeof(uint64_t));

  RELAY_ERROR(wp_segment_create(&index->segments[0], buf));
  index->docid_offsets[0] = 0;

  return NO_ERROR;
}

RAISING_STATIC(ensure_num_segments(wp_index* index)) {
  if(index->num_segments >= index->sizeof_segments) {
    index->sizeof_segments *= 2;
    index->segments = realloc(index->segments, sizeof(wp_segment) * index->sizeof_segments);
    index->docid_offsets = realloc(index->docid_offsets, sizeof(uint64_t) * index->sizeof_segments);
    if(index->segments == NULL) RAISE_ERROR("oom");
  }

  return NO_ERROR;
}

wp_error* wp_index_load(wp_index** indexptr, const char* pathname_base) {
  char buf[PATH_BUF_SIZE];
  snprintf(buf, PATH_BUF_SIZE, "%s0", pathname_base);
  if(!wp_segment_exists(buf)) RAISE_ERROR("index with base path '%s' does not exist", pathname_base);

  wp_index* index = *indexptr = malloc(sizeof(wp_index));

  index->pathname_base = pathname_base;
  index->num_segments = 0;
  index->sizeof_segments = 1;
  index->open = 1;
  index->segments = malloc(sizeof(wp_segment));
  index->docid_offsets = malloc(sizeof(uint64_t));

  // load all the segments we can
  while(index->num_segments < WP_MAX_SEGMENTS) {
    snprintf(buf, PATH_BUF_SIZE, "%s%d", pathname_base, index->num_segments);
    if(!wp_segment_exists(buf)) break;

    RELAY_ERROR(ensure_num_segments(index));
    DEBUG("loading segment %s", buf);
    RELAY_ERROR(wp_segment_load(&index->segments[index->num_segments], buf));
    if(index->num_segments == 0)
      index->docid_offsets[index->num_segments] = 0;
    else {
      // segments return docids 1 through N, so the num_docs in a segment is
      // also the max document id
      postings_region* prevpr = MMAP_OBJ(index->segments[index->num_segments - 1].postings, postings_region);
      index->docid_offsets[index->num_segments] = prevpr->num_docs + index->docid_offsets[index->num_segments - 1];
    }

    index->num_segments++;
  }

  return NO_ERROR;
}

// we have two special values at our disposal to mark where we are in
// the sequence of segments
#define SEGMENT_UNINITIALIZED WP_MAX_SEGMENTS
#define SEGMENT_DONE (WP_MAX_SEGMENTS + 1)

wp_error* wp_index_setup_query(wp_index* index, wp_query* query) {
  (void)index;
  query->segment_idx = SEGMENT_UNINITIALIZED;

  return NO_ERROR;
}

// can be called multiple times to resume
wp_error* wp_index_run_query(wp_index* index, wp_query* query, uint32_t max_num_results, uint32_t* num_results, uint64_t* results) {
  *num_results = 0;
  if(index->num_segments == 0) return NO_ERROR;

  if(query->segment_idx == SEGMENT_UNINITIALIZED) {
    query->segment_idx = index->num_segments - 1;
    DEBUG("setting up segment %u", query->segment_idx);
    RELAY_ERROR(wp_search_init_search_state(query, &index->segments[query->segment_idx]));
  }

  // at this point, we assume we're initialized and query->segment_idx is the index
  // of the segment we're searching against
  while((*num_results < max_num_results) && (query->segment_idx != SEGMENT_DONE)) {
    uint32_t want_num_results = max_num_results - *num_results;
    uint32_t got_num_results = 0;
    search_result* segment_results = malloc(sizeof(search_result) * want_num_results);

    DEBUG("searching segment %d", query->segment_idx);
    RELAY_ERROR(wp_search_run_query_on_segment(query, &index->segments[query->segment_idx], want_num_results, &got_num_results, segment_results));
    DEBUG("asked segment %d for %d results, got %d", query->segment_idx, want_num_results, got_num_results);

    // extract the per-segment docids from the search results and adjust by
    // each segment's docid offset to form global docids
    for(uint32_t i = 0; i < got_num_results; i++) {
      results[*num_results + i] = index->docid_offsets[query->segment_idx] + segment_results[i].doc_id;
      wp_search_result_free(&segment_results[i]);
    }
    free(segment_results);
    *num_results += got_num_results;

    if(got_num_results < want_num_results) { // this segment is finished; move to the next one
      DEBUG("releasing index %d", query->segment_idx);
      RELAY_ERROR(wp_search_release_search_state(query));
      if(query->segment_idx > 0) {
        query->segment_idx--;
        DEBUG("setting up index %d", query->segment_idx);
        RELAY_ERROR(wp_search_init_search_state(query, &index->segments[query->segment_idx]));
      }
      else query->segment_idx = SEGMENT_DONE;
    }
  }

  return NO_ERROR;
}

#define RESULT_BUF_SIZE 1024
// count the results by just running the query until it stops. slow!
wp_error* wp_index_count_results(wp_index* index, wp_query* query, uint32_t* num_results) {
  uint64_t results[RESULT_BUF_SIZE];

  *num_results = 0;
  RELAY_ERROR(wp_index_setup_query(index, query));
  while(1) {
    uint32_t this_num_results;
    RELAY_ERROR(wp_index_run_query(index, query, RESULT_BUF_SIZE, &this_num_results, results));
    *num_results += this_num_results;
    if(this_num_results < RESULT_BUF_SIZE) break; // done
  }

  RELAY_ERROR(wp_index_teardown_query(index, query));

  return NO_ERROR;
}

wp_error* wp_index_teardown_query(wp_index* index, wp_query* query) {
  (void)index;
  if((query->segment_idx != SEGMENT_UNINITIALIZED) && (query->segment_idx != SEGMENT_DONE)) {
    RELAY_ERROR(wp_search_release_search_state(query));
  }
  query->segment_idx = SEGMENT_UNINITIALIZED;

  return NO_ERROR;
}

wp_error* wp_index_add_entry(wp_index* index, wp_entry* entry, uint64_t* doc_id) {
  int success;
  wp_segment* seg = &index->segments[index->num_segments - 1];

  // first, ensure we have enough space in the current segment
  uint32_t postings_bytes;
  RELAY_ERROR(wp_entry_sizeof_postings_region(entry, seg, &postings_bytes));
  RELAY_ERROR(wp_segment_ensure_fit(seg, postings_bytes, 0, &success));

  // if not, we need to open a new one
  if(!success) {
    DEBUG("segment %d is full, loading a new one", index->num_segments - 1);
    char buf[PATH_BUF_SIZE];
    snprintf(buf, PATH_BUF_SIZE, "%s%d", index->pathname_base, index->num_segments);
    RELAY_ERROR(ensure_num_segments(index));
    RELAY_ERROR(wp_segment_create(&index->segments[index->num_segments], buf));
    index->num_segments++;

    // set the docid_offset
    postings_region* prevpr = MMAP_OBJ(index->segments[index->num_segments - 2].postings, postings_region);
    index->docid_offsets[index->num_segments - 1] = prevpr->num_docs + index->docid_offsets[index->num_segments - 2];

    seg = &index->segments[index->num_segments - 1];
    DEBUG("loaded new segment %d at %p", index->num_segments - 1, &index->segments[index->num_segments - 1]);

    RELAY_ERROR(wp_entry_sizeof_postings_region(entry, seg, &postings_bytes));
    RELAY_ERROR(wp_segment_ensure_fit(seg, postings_bytes, 0, &success));
    if(!success) RAISE_ERROR("can't fit new entry into fresh segment. that's crazy");
  }

  docid_t seg_doc_id;
  RELAY_ERROR(wp_segment_grab_docid(seg, &seg_doc_id));
  RELAY_ERROR(wp_entry_write_to_segment(entry, seg, seg_doc_id));
  *doc_id = seg_doc_id + index->docid_offsets[index->num_segments - 1];

  return NO_ERROR;
}

wp_error* wp_index_unload(wp_index* index) {
  for(uint16_t i = 0; i < index->num_segments; i++) RELAY_ERROR(wp_segment_unload(&index->segments[i]));
  index->open = 0;

  return NO_ERROR;
}

wp_error* wp_index_free(wp_index* index) {
  if(index->open) RELAY_ERROR(wp_index_unload(index));
  free(index->segments);
  free(index->docid_offsets);
  free(index);

  return NO_ERROR;
}

wp_error* wp_index_dumpinfo(wp_index* index, FILE* stream) {
  fprintf(stream, "index has %d segments\n", index->num_segments);
  for(int i = 0; i < index->num_segments; i++) {
    fprintf(stream, "\nsegment %d:\n", i);
    RELAY_ERROR(wp_segment_dumpinfo(&index->segments[i], stream));
  }

  return NO_ERROR;
}

wp_error* wp_index_delete(const char* pathname_base) {
  char buf[PATH_BUF_SIZE];

  int i = 0;
  while(1) {
    snprintf(buf, PATH_BUF_SIZE, "%s%d", pathname_base, i);
    if(wp_segment_exists(buf)) {
      DEBUG("deleting segment %s", buf);
      RELAY_ERROR(wp_segment_delete(buf));
      i++;
    }
    else break;
  }

  return NO_ERROR;
}

wp_error* wp_index_add_label(wp_index* index, const char* label, uint64_t doc_id) {
  int found = 0;

  for(uint32_t i = index->num_segments; i > 0; i--) {
    if(doc_id > index->docid_offsets[i - 1]) {
      DEBUG("found doc %llu in segment %u", doc_id, i - 1);
      RELAY_ERROR(wp_segment_add_label(&index->segments[i - 1], label, (docid_t)(doc_id - index->docid_offsets[i - 1])));
      found = 1;
      break;
    }
    else DEBUG("did not find doc %llu in segment %u", doc_id, i - 1);
  }

  if(!found) RAISE_ERROR("couldn't find doc id %llu", doc_id);

  return NO_ERROR;
}

wp_error* wp_index_remove_label(wp_index* index, const char* label, uint64_t doc_id) {
  int found = 0;

  for(uint32_t i = index->num_segments; i > 0; i--) {
    if(doc_id > index->docid_offsets[i - 1]) {
      DEBUG("found doc %llu in segment %u", doc_id, i - 1);
      RELAY_ERROR(wp_segment_remove_label(&index->segments[i - 1], label, (docid_t)(doc_id - index->docid_offsets[i - 1])));
      found = 1;
      break;
    }
    else DEBUG("did not find doc %llu in segment %u", doc_id, i - 1);
  }

  if(!found) RAISE_ERROR("couldn't find doc id %llu", doc_id);

  return NO_ERROR;
}

uint64_t wp_index_num_docs(wp_index* index) {
  uint64_t ret = 0;

  // TODO check for overflow or some shit
  for(uint32_t i = index->num_segments; i > 0; i--) ret += wp_segment_num_docs(&index->segments[i - 1]);

  return ret;
}

// insane. but i'm putting this here. not defined in c99. don't want to make a
// "utils.c" or "compat.c" or whatever just yet.
char* strdup(const char* old) {
  size_t len = strlen(old) + 1;
  char *new = malloc(len * sizeof(char));
  return memcpy(new, old, len);
}
