
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>

#include "ddb_bits.h"
#include "tdb_internal.h"
#include "tdb_encode_model.h"
#include "huffman.h"
#include "util.h"

#define EDGE_INCREMENT     1000000
#define GROUPBUF_INCREMENT 10000000
#define READ_BUFFER_SIZE  (1000000 * sizeof(tdb_cookie_event))
#define MAX_INVALID_RATIO  0.005

static int compare(const void *p1, const void *p2)
{
    const tdb_cookie_event *x = (tdb_cookie_event*)p1;
    const tdb_cookie_event *y = (tdb_cookie_event*)p2;

    if (x->timestamp > y->timestamp)
        return 1;
    else if (x->timestamp < y->timestamp)
        return -1;
    return 0;
}

static void group_events(FILE *grouped_w,
                         const char *path,
                         const tdb_event *events,
                         const uint64_t *cookie_pointers,
                         uint64_t num_cookies,
                         uint32_t base_timestamp,
                         uint32_t *max_timestamp_delta)
{
    uint64_t i;
    uint64_t idx = 0;
    uint64_t num_invalid = 0;
    tdb_cookie_event *buf = NULL;
    uint32_t buf_size = 0;

    *max_timestamp_delta = 0;

    for (i = 0; i < num_cookies; i++){
        /* find the last event belonging to this cookie */
        const tdb_event *ev = &events[cookie_pointers[i]];
        uint32_t j = 0;
        uint32_t num_events = 0;
        uint32_t prev_timestamp;

        /* loop through all events belonging to this cookie,
           following back-links */
        while (1){
            if (j >= buf_size){
                buf_size += GROUPBUF_INCREMENT;
                if (!(buf = realloc(buf, buf_size * sizeof(tdb_cookie_event))))
                    DIE("Couldn't realloc group buffer of %u items\n",
                        buf_size);
            }
            buf[j].cookie_id = i;
            buf[j].item_zero = ev->item_zero;
            buf[j].num_items = ev->num_items;
            buf[j].timestamp = ev->timestamp;
            ++j;
            if (ev->prev_event_idx)
                ev = &events[ev->prev_event_idx - 1];
            else
                break;
        }
        num_events = j;

        /* sort events of this cookie by time */
        qsort(buf, num_events, sizeof(tdb_cookie_event), compare);

        /* delta-encode timestamps */
        for (prev_timestamp = base_timestamp, j = 0; j < num_events; j++){
            uint32_t prev = buf[j].timestamp;
            buf[j].timestamp -= prev_timestamp;
            /* timestamps can be at most 2**24 seconds apart (194 days).
               It is not a problem since data should be partitioned by time */
            if (buf[j].timestamp < (1 << 24)){

                if (buf[j].timestamp > *max_timestamp_delta)
                    *max_timestamp_delta = buf[j].timestamp;

                buf[j].timestamp <<= 8;
                prev_timestamp = prev;
            }else{
                /* mark event as invalid if it is too far in the future,
                   most likely because of a corrupted timestamp */
                buf[j].timestamp = 1;
                ++num_invalid;
            }
        }

        SAFE_WRITE(buf,
                   num_events * sizeof(tdb_cookie_event),
                   path,
                   grouped_w);
    }

    if (num_invalid / (float)idx > MAX_INVALID_RATIO)
        DIE("Too many invalid timestamps (base timestamp: %u)\n",
            base_timestamp);

    free(buf);
}

uint32_t edge_encode_items(const tdb_item *items,
                           uint32_t **encoded,
                           uint32_t *encoded_size,
                           tdb_item *prev_items,
                           const tdb_cookie_event *ev)
{
    uint32_t n = 0;

    /* consider only valid timestamps (first byte = 0) */
    if ((ev->timestamp & 255) == 0){
        uint64_t j = ev->item_zero;

        /* edge encode items: keep only fields that are different from
           the previous event */
        for (; j < ev->item_zero + ev->num_items; j++){
            tdb_field field = tdb_item_field(items[j]);

            if (prev_items[field] != items[j]){
                if (n == *encoded_size){
                    *encoded_size += EDGE_INCREMENT;
                    if (!(*encoded = realloc(*encoded, *encoded_size * 4)))
                        DIE("Could not allocate encoding buffer of %u items\n",
                            *encoded_size);
                }
                (*encoded)[n++] = prev_items[field] = items[j];
            }
        }
    }
    return n;
}

static void timestamp_range(const tdb_event *events,
                            uint64_t num_events,
                            uint32_t *min_timestamp,
                            uint32_t *max_timestamp)
{
    uint64_t i;
    *min_timestamp = UINT32_MAX;
    *max_timestamp = 0;

    for (i = 0; i < num_events; i++){
        if (events[i].timestamp < *min_timestamp)
            *min_timestamp = events[i].timestamp;
        if (events[i].timestamp > *max_timestamp)
            *max_timestamp = events[i].timestamp;
    }
}

static void store_info(uint64_t num_events,
                       uint64_t num_cookies,
                       uint32_t min_timestamp,
                       uint32_t max_timestamp,
                       uint32_t max_timestamp_delta,
                       const char *path)
{
    FILE *out;

    if (!(out = fopen(path, "w")))
        DIE("Could not create info file: %s\n", path);

    SAFE_FPRINTF(out,
                 path,
                 "%llu %llu %u %u %u\n",
                 (long long unsigned int)num_cookies,
                 (long long unsigned int)num_events,
                 min_timestamp,
                 max_timestamp,
                 max_timestamp_delta);

    SAFE_CLOSE(out, path);
}

static void encode_trails(const tdb_item *items,
                          FILE *grouped,
                          uint64_t num_events,
                          uint64_t num_cookies,
                          uint32_t num_fields,
                          const Pvoid_t codemap,
                          const Pvoid_t gram_freqs,
                          const struct field_stats *fstats,
                          const char *path)
{
    uint64_t *grams = NULL;
    tdb_item *prev_items = NULL;
    uint32_t *encoded = NULL;
    uint32_t encoded_size = 0;
    uint64_t i = 0;
    char *buf;
    FILE *out;
    uint64_t file_offs = (num_cookies + 1) * 4;
    struct gram_bufs gbufs;
    tdb_cookie_event ev;

    init_gram_bufs(&gbufs, num_fields);

    if (file_offs >= UINT32_MAX)
        DIE("Trail file %s over 4GB!\n", path);

    if (!(out = fopen(path, "wx")))
        DIE("Could not create trail file: %s\n", path);

    /* reserve space for TOC */
    SAFE_SEEK(out, file_offs, path);

    /* huff_encode_grams guarantees that it writes fewer
       than UINT32_MAX bits per buffer, or it fails */
    if (!(buf = calloc(1, UINT32_MAX / 8 + 8)))
        DIE("Could not allocate 512MB in encode_trails\n");

    if (!(prev_items = malloc(num_fields * 4)))
        DIE("Could not allocate %u fields\n", num_fields);

    if (!(grams = malloc(num_fields * 8)))
        DIE("Could not allocate %u grams\n", num_fields);

    rewind(grouped);
    fread(&ev, sizeof(tdb_cookie_event), 1, grouped);

    while (i < num_events){
        /* encode trail for one cookie (multiple events) */

        /* reserve 3 bits in the head of the trail for a length residual:
           Length of a trail is measured in bytes but the last byte may
           be short. The residual indicates how many bits in the end we
           should ignore. */
        uint64_t offs = 3;
        uint64_t cookie_id = ev.cookie_id;
        uint64_t trail_size;

        /* write offset to TOC */
        SAFE_SEEK(out, cookie_id * 4, path);
        SAFE_WRITE(&file_offs, 4, path, out);

        memset(prev_items, 0, num_fields * 4);

        for (;i < num_events && ev.cookie_id == cookie_id; i++){

            /* 1) produce an edge-encoded set of items for this event */
            uint32_t n = edge_encode_items(items,
                                           &encoded,
                                           &encoded_size,
                                           prev_items,
                                           &ev);

            /* 2) cover the encoded set with a set of unigrams and bigrams */
            uint32_t m = choose_grams(encoded,
                                      n,
                                      gram_freqs,
                                      &gbufs,
                                      grams,
                                      &ev);

            /* 3) huffman-encode grams */
            huff_encode_grams(codemap,
                              grams,
                              m,
                              buf,
                              &offs,
                              fstats);

            fread(&ev, sizeof(tdb_cookie_event), 1, grouped);
        }

        /* write the length residual */
        if (offs & 7){
            trail_size = offs / 8 + 1;
            write_bits(buf, 0, 8 - (offs & 7));
        }else
            trail_size = offs / 8;

        /* append trail to the end of file */
        if (fseek(out, 0, SEEK_END) == -1)
            DIE("Seeking to the end of %s failed\n", path);

        SAFE_SEEK(out, file_offs, path);
        SAFE_WRITE(buf, trail_size, path, out);

        file_offs += trail_size;
        if (file_offs >= UINT32_MAX)
            DIE("Trail file %s over 4GB!\n", path);

        memset(buf, 0, trail_size);
    }
    /* write the redundant last offset in the TOC, so we can determine
       trail length with toc[i + 1] - toc[i]. */
    SAFE_SEEK(out, num_cookies * 4, path);
    SAFE_WRITE(&file_offs, 4, path, out);

    SAFE_CLOSE(out, path);

    free_gram_bufs(&gbufs);
    free(grams);
    free(encoded);
    free(prev_items);
}

static void store_codebook(const Pvoid_t codemap, const char *path)
{
    FILE *out;
    uint32_t size;
    struct huff_codebook *book = huff_create_codebook(codemap, &size);

    if (!(out = fopen(path, "w")))
        DIE("Could not create codebook file: %s\n", path);

    SAFE_WRITE(book, size, path, out);

    free(book);
    SAFE_CLOSE(out, path);
}

void tdb_encode(const uint64_t *cookie_pointers,
                uint64_t num_cookies,
                tdb_event *events,
                uint64_t num_events,
                const tdb_item *items,
                uint64_t num_items,
                uint32_t num_fields,
                const uint64_t *field_cardinalities,
                const char *root)
{
    char path[TDB_MAX_PATH_SIZE];
    char grouped_path[TDB_MAX_PATH_SIZE];
    struct field_stats *fstats;
    uint32_t min_timestamp, max_timestamp, max_timestamp_delta;
    Pvoid_t unigram_freqs;
    Pvoid_t gram_freqs;
    Pvoid_t codemap;
    Word_t tmp;
    FILE *grouped_w;
    FILE *grouped_r;
    char *read_buf;

    TDB_TIMER_DEF

    /* 1. find minimum timestamp (for delta-encoding) */
    TDB_TIMER_START
    timestamp_range(events, num_events, &min_timestamp, &max_timestamp);
    TDB_TIMER_END("trail/timestamp_range");

    /* 2. group events by cookie, sort events of each cookie by time,
          and delta-encode timestamps */
    TDB_TIMER_START

    tdb_path(grouped_path, "%s/tmp.grouped.%d", root, getpid());
    if (!(grouped_w = fopen(grouped_path, "w")))
        DIE("Could not open tmp file at %s\n", path);

    group_events(grouped_w,
                 grouped_path,
                 events,
                 cookie_pointers,
                 num_cookies,
                 min_timestamp,
                 &max_timestamp_delta);

    SAFE_CLOSE(grouped_w, grouped_path);
    if (!(grouped_r = fopen(grouped_path, "r")))
        DIE("Could not open tmp file at %s\n", path);
    if (!(read_buf = malloc(READ_BUFFER_SIZE)))
        DIE("Could not allocate read buffer of %lu bytes\n", READ_BUFFER_SIZE);
    setvbuf(grouped_r, read_buf, _IOFBF, READ_BUFFER_SIZE);

    TDB_TIMER_END("trail/group_events");

    /* not the most clean separation of ownership here, but events is huge
       so keeping it around unecessarily is expensive */
    free(events);

    /* 3. store metatadata */
    TDB_TIMER_START
    tdb_path(path, "%s/info", root);
    store_info(num_events,
               num_cookies,
               min_timestamp,
               max_timestamp,
               max_timestamp_delta,
               path);
    TDB_TIMER_END("trail/info");

    /* 4. collect value (unigram) frequencies, including delta-encoded
          timestamps */
    TDB_TIMER_START
    unigram_freqs = collect_unigrams(grouped_r, num_events, items, num_fields);
    TDB_TIMER_END("trail/collect_unigrams");

    /* 5. construct uni/bi-grams */
    TDB_TIMER_START
    gram_freqs = make_grams(grouped_r,
                            num_events,
                            items,
                            num_fields,
                            unigram_freqs);
    TDB_TIMER_END("trail/gram_freqs");

    /* 6. build a huffman codebook and stats struct for encoding grams */
    TDB_TIMER_START
    codemap = huff_create_codemap(gram_freqs);
    fstats = huff_field_stats(field_cardinalities,
                              num_fields,
                              max_timestamp_delta);
    TDB_TIMER_END("trail/huff_create_codemap");

    /* 7. encode and write trails to disk */
    TDB_TIMER_START
    tdb_path(path, "%s/trails.data", root);
    encode_trails(items,
                  grouped_r,
                  num_events,
                  num_cookies,
                  num_fields,
                  codemap,
                  gram_freqs,
                  fstats,
                  path);
    TDB_TIMER_END("trail/encode_trails");

    /* 8. write huffman codebook to disk */
    TDB_TIMER_START
    tdb_path(path, "%s/trails.codebook", root);
    store_codebook(codemap, path);
    TDB_TIMER_END("trail/store_codebook");

    JLFA(tmp, unigram_freqs);
    JLFA(tmp, gram_freqs);
    JLFA(tmp, codemap);

    fclose(grouped_r);
    unlink(grouped_path);

    free(read_buf);
    free(fstats);
}