#include "lower.h"
#include "ssd.h"
#include "../tools/rte_ring/rte_ring.h"
#include <sys/mman.h>
#include <pthread.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

struct ssd ssd_lower;

void ssd_init(lower_info *li);
void ssd_stats_init(lower_info *li);
uint64_t ssd_write_ppa(uint32_t pgidx, uint64_t size, uint64_t stime);
uint64_t ssd_read_ppa(uint32_t ppa, uint64_t size, uint64_t stime);
uint64_t ssd_trim_block(uint32_t pgidx);
void ssd_destroy(lower_info *li);

lower_stats ssd_stats;

lower_info ssd_li = {
    .create = ssd_init,
    .destroy = ssd_destroy,
    .write = ssd_write_ppa,
    .read = ssd_read_ppa,
    .trim_block = ssd_trim_block,
    .stats = &ssd_stats,
};

struct ppa pgidx2ppa(struct ssd *ssd, uint32_t pgidx) {
    struct ssdparams *spp = &ssd->sp;

    struct ppa ppa = {
        .g.ch = pgidx % spp->nchs,
        .g.lun = (pgidx / spp->nchs) % spp->luns_per_ch,
        .g.pl = (pgidx / spp->nchs / spp->luns_per_ch) % spp->pls_per_lun,
        .g.blk = (pgidx / spp->nchs / spp->luns_per_ch / spp->pls_per_lun) % spp->blks_per_pl,
        .g.pg = (pgidx / spp->nchs / spp->luns_per_ch / spp->pls_per_lun / spp->blks_per_pl) % spp->pgs_per_blk,
    };

    return ppa;
}

uint64_t ppa2pgidx(struct ssd *ssd, struct ppa *ppa) {
    struct ssdparams *spp = &ssd->sp;
    uint64_t pgidx;

    pgidx = ppa->g.ch * spp->pgs_per_ch + ppa->g.lun * spp->pgs_per_lun +
            ppa->g.pl * spp->pgs_per_pl + ppa->g.blk * spp->pgs_per_blk +
            ppa->g.pg;

    ftl_assert(pgidx < spp->tt_pgs);

    return pgidx;
}

static inline void check_addr(int a, int max) { ftl_assert(a >= 0 && a < max); }

void ssd_stats_init(lower_info *li) {
    ftl_assert(li->stats);
    li->stats->nr_nand_erase = 0;
    li->stats->nr_nand_read = 0;
    li->stats->nr_nand_write = 0;
    li->stats->nr_nand_rd_lun = g_malloc0(sizeof(uint64_t) * ssd_lower.sp.tt_luns);
    li->stats->nr_nand_wr_lun = g_malloc0(sizeof(uint64_t) * ssd_lower.sp.tt_luns);
    li->stats->nr_nand_er_lun = g_malloc0(sizeof(uint64_t) * ssd_lower.sp.tt_luns);
}

static void ssd_init_params(struct ssdparams *spp) {
    spp->secsz = 512;        // 512
    spp->secs_per_pg = 8;    // 8
    spp->pgs_per_blk = 512; // 512 128MB SBLK
    spp->blks_per_pl = 56;  /* 564 70.5GB */
    spp->pls_per_lun = 1;    // 1
    spp->luns_per_ch = 8;    // 8
    spp->nchs = 8;           // 8

    ftl_log("ssd created! size: %lu MB\n", (uint64_t)spp->secsz * spp->secs_per_pg * spp->pgs_per_blk * spp->blks_per_pl * spp->pls_per_lun * spp->luns_per_ch * spp->nchs / 1024 / 1024);

    spp->pg_rd_lat = NAND_READ_LATENCY;
    spp->pg_wr_lat = NAND_WRITE_LATENCY;
    spp->blk_er_lat = NAND_ERASE_LATENCY;
    spp->ch_xfer_lat = CHANNEL_XFER_LATENCY;

    /* calculated values */
    spp->secs_per_blk = spp->secs_per_pg * spp->pgs_per_blk;
    spp->secs_per_pl = spp->secs_per_blk * spp->blks_per_pl;
    spp->secs_per_lun = spp->secs_per_pl * spp->pls_per_lun;
    spp->secs_per_ch = spp->secs_per_lun * spp->luns_per_ch;
    spp->tt_secs = spp->secs_per_ch * spp->nchs;

    spp->pgs_per_pl = spp->pgs_per_blk * spp->blks_per_pl;
    spp->pgs_per_lun = spp->pgs_per_pl * spp->pls_per_lun;
    spp->pgs_per_ch = spp->pgs_per_lun * spp->luns_per_ch;
    spp->tt_pgs = spp->pgs_per_ch * spp->nchs;

    spp->blks_per_lun = spp->blks_per_pl * spp->pls_per_lun;
    spp->blks_per_ch = spp->blks_per_lun * spp->luns_per_ch;
    spp->tt_blks = spp->blks_per_ch * spp->nchs;

    spp->pls_per_ch = spp->pls_per_lun * spp->luns_per_ch;
    spp->tt_pls = spp->pls_per_ch * spp->nchs;

    spp->tt_luns = spp->luns_per_ch * spp->nchs;

    /* line is special, put it at the end */
    spp->blks_per_line = spp->tt_luns; /* TODO: to fix under multiplanes */
    spp->pgs_per_line = spp->blks_per_line * spp->pgs_per_blk;
    spp->secs_per_line = spp->pgs_per_line * spp->secs_per_pg;
    spp->tt_lines = spp->blks_per_lun; /* TODO: to fix under multiplanes */

    // spp->gc_thres_pcent = n->bb_params.gc_thres_pcent/100.0;
    // spp->gc_thres_lines = (int)((1 - spp->gc_thres_pcent) * spp->tt_lines);
    // spp->gc_thres_pcent_high = n->bb_params.gc_thres_pcent_high/100.0;
    // spp->gc_thres_lines_high = (int)((1 - spp->gc_thres_pcent_high) *
    // spp->tt_lines);
    spp->enable_gc_delay = true;
}

static void ssd_init_ch(struct ssd_channel *ch, struct ssdparams *spp) {
    ch->nluns = spp->luns_per_ch;
    ch->lun = g_malloc0(sizeof(struct nand_lun) * ch->nluns);
    ch->next_ch_avail_time = 0;
    ch->busy = 0;
}

void ssd_init(lower_info *li) {
    struct ssd *ssd = &ssd_lower;
    struct ssdparams *spp = &ssd->sp;

    ftl_assert(ssd);
    ssd_init_params(spp);

    ssd->ppa_state = g_malloc0(sizeof(bool) * spp->tt_pgs);
    ssd->line_wp = g_malloc0(sizeof(uint32_t) * spp->tt_lines);


    /* initialize ssd internal layout architecture */
    ssd->ch = g_malloc0(sizeof(struct ssd_channel) * spp->nchs);
    for (int i = 0; i < spp->nchs; i++) {
        ssd_init_ch(&ssd->ch[i], spp);
    }

    ssd_stats_init(&ssd_li);
}

void ssd_destroy(lower_info *li) {
    struct ssd *ssd = &ssd_lower;
    struct ssdparams *spp = &ssd->sp;
    g_free(ssd->ppa_state);
    for (int i = 0; i < spp->nchs; i++) {
        g_free(&ssd->ch[i].lun);
    }
    g_free(ssd->ch);
}

static inline bool valid_ppa(struct ssd *ssd, struct ppa *ppa) {
    struct ssdparams *spp = &ssd->sp;
    int ch = ppa->g.ch;
    int lun = ppa->g.lun;
    int pl = ppa->g.pl;
    int blk = ppa->g.blk;
    int pg = ppa->g.pg;
    int sec = ppa->g.sec;

    if (ch >= 0 && ch < spp->nchs && lun >= 0 && lun < spp->luns_per_ch &&
        pl >= 0 && pl < spp->pls_per_lun && blk >= 0 &&
        blk < spp->blks_per_pl && pg >= 0 && pg < spp->pgs_per_blk &&
        sec >= 0 && sec < spp->secs_per_pg)
        return true;

    return false;
}

static inline struct ssd_channel *get_ch(struct ssd *ssd, struct ppa *ppa) {
    return &(ssd->ch[ppa->g.ch]);
}

static inline struct nand_lun *get_lun(struct ssd *ssd, struct ppa *ppa) {
    struct ssd_channel *ch = get_ch(ssd, ppa);
    return &(ch->lun[ppa->g.lun]);
}

static uint64_t ssd_advance_status(struct ssd *ssd, struct ppa *ppa,
                                   int ncmd, uint64_t stime) {
    int c = ncmd;
    uint64_t cmd_stime = (stime == 0) ? clock_get_ns() : stime;
    uint64_t nand_stime;
    struct ssdparams *spp = &ssd->sp;
    struct nand_lun *lun = get_lun(ssd, ppa);
    uint64_t lat = 0;

    switch (c) {
    case NAND_READ:
        /* read: perform NAND cmd first */
        nand_stime = (lun->next_lun_avail_time < cmd_stime)
                         ? cmd_stime
                         : lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->pg_rd_lat;
        lat = lun->next_lun_avail_time - cmd_stime;
#if 0
        lun->next_lun_avail_time = nand_stime + spp->pg_rd_lat;

        /* read: then data transfer through channel */
        chnl_stime = (ch->next_ch_avail_time < lun->next_lun_avail_time) ? \
            lun->next_lun_avail_time : ch->next_ch_avail_time;
        ch->next_ch_avail_time = chnl_stime + spp->ch_xfer_lat;

        lat = ch->next_ch_avail_time - cmd_stime;
#endif
        break;

    case NAND_WRITE:
        /* write: transfer data through channel first */
        nand_stime = (lun->next_lun_avail_time < cmd_stime)
                         ? cmd_stime
                         : lun->next_lun_avail_time;

        lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;
        lat = lun->next_lun_avail_time - cmd_stime;

#if 0
        chnl_stime = (ch->next_ch_avail_time < cmd_stime) ? cmd_stime : \
                     ch->next_ch_avail_time;
        ch->next_ch_avail_time = chnl_stime + spp->ch_xfer_lat;

        /* write: then do NAND program */
        nand_stime = (lun->next_lun_avail_time < ch->next_ch_avail_time) ? \
            ch->next_ch_avail_time : lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;

        lat = lun->next_lun_avail_time - cmd_stime;
#endif
        break;

    case NAND_ERASE:
        /* erase: only need to advance NAND status */
        nand_stime = (lun->next_lun_avail_time < cmd_stime)
                         ? cmd_stime
                         : lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->blk_er_lat;

        lat = lun->next_lun_avail_time - cmd_stime;
        break;

    default:
        ftl_err("Unsupported NAND command: 0x%x\n", c);
    }

    return lat;
}

uint64_t ssd_write_ppa(uint32_t pgidx, uint64_t size, uint64_t stime) {
    uint32_t tmp_pgidx = pgidx;
    uint64_t now = clock_get_ns();
    stime = stime > now ? stime : now;
    uint64_t max_etime = 0;
    ftl_assert(size >= PAGESIZE);
    for (; tmp_pgidx < pgidx + size / PAGESIZE; ++tmp_pgidx) {
        check_addr(tmp_pgidx, ssd_lower.sp.tt_pgs);
        if (unlikely(ssd_lower.ppa_state[tmp_pgidx])) {
            ftl_err("Overwrite PPA: %d\n", tmp_pgidx);
            abort();
        }
        if (unlikely(ssd_lower.line_wp[LINE_IDX(tmp_pgidx)] != tmp_pgidx - LINE_IDX(tmp_pgidx) * ssd_lower.sp.pgs_per_line)) {  // tmp_pgidx % ssd_lower.sp.pgs_per_line != 0 && !ssd_lower.ppa_state[tmp_pgidx-1]
            ftl_err("Write PPA out of line wp: %d\n", tmp_pgidx);
            abort();
        }
        ssd_lower.ppa_state[tmp_pgidx] = true;
        ssd_lower.line_wp[LINE_IDX(tmp_pgidx)]++;
        struct ppa ppa = pgidx2ppa(&ssd_lower, tmp_pgidx);
        ssd_li.stats->nr_nand_wr_lun[ppa.g.ch * ssd_lower.sp.luns_per_ch + ppa.g.lun]++;
        uint64_t etime = stime + ssd_advance_status(&ssd_lower, &ppa, NAND_WRITE, stime);
        max_etime = etime > max_etime ? etime : max_etime;
    }
    ftl_assert(max_etime >= stime);
    ssd_li.stats->nr_nand_write += (size / PAGESIZE);
    return max_etime - stime;
}

uint64_t ssd_read_ppa(uint32_t pgidx, uint64_t size, uint64_t stime) {
    uint32_t tmp_pgidx = pgidx;
    uint64_t now = clock_get_ns();
    stime = stime > now ? stime : now;
    uint64_t max_etime = 0;
    ftl_assert(size >= PAGESIZE);
    for (; tmp_pgidx < pgidx + size / PAGESIZE; ++tmp_pgidx) {
        check_addr(tmp_pgidx, ssd_lower.sp.tt_pgs);
        if (unlikely(!ssd_lower.ppa_state[tmp_pgidx])) {
            ftl_err("PPA not written: %d\n", tmp_pgidx);
            abort();
        }
        struct ppa ppa = pgidx2ppa(&ssd_lower, tmp_pgidx);
        ssd_li.stats->nr_nand_rd_lun[ppa.g.ch * ssd_lower.sp.luns_per_ch + ppa.g.lun]++;
        uint64_t etime = stime + ssd_advance_status(&ssd_lower, &ppa, NAND_READ, stime);
        max_etime = etime > max_etime ? etime : max_etime;
    }
    ftl_assert(max_etime >= stime);
    ssd_li.stats->nr_nand_read += (size / PAGESIZE);
    return max_etime - stime;
}

uint64_t ssd_trim_block(uint32_t pgidx) {
    check_addr(pgidx, ssd_lower.sp.tt_pgs);
    ftl_assert(pgidx % ssd_lower.sp.pgs_per_line == 0);
    uint32_t maxlat = 0;
    uint64_t stime = clock_get_ns();
    for (int i = 0; i < ssd_lower.sp.tt_luns; ++i) {
        struct ppa ppa = pgidx2ppa(&ssd_lower, pgidx + i);
        ssd_li.stats->nr_nand_er_lun[ppa.g.ch * ssd_lower.sp.luns_per_ch + ppa.g.lun]++;
        uint64_t lat = ssd_advance_status(&ssd_lower, &ppa, NAND_ERASE, stime);
        if (lat > maxlat) {
            maxlat = lat;
        }
    }
    uint64_t etime = stime + maxlat;
    memset(ssd_lower.ppa_state + pgidx, 0, sizeof(bool) * ssd_lower.sp.pgs_per_line);
    ssd_lower.line_wp[LINE_IDX(pgidx)] = 0;
    ssd_li.stats->nr_nand_erase += ssd_lower.sp.pgs_per_line;
    return etime - stime;
}
