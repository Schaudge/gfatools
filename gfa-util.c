#include <assert.h>
#include "gfa-priv.h"
#include "kvec.h"
#include "ksort.h"

#define generic_key(x) (x)
KRADIX_SORT_INIT(gfa64, uint64_t, generic_key, 8)

void gfa_sub(gfa_t *g, int n, char *const* seg, int step)
{
	int32_t i;
	int8_t *flag;
	kvec_t(uint64_t) stack = {0,0,0};
	if (n == 0) return;
	GFA_CALLOC(flag, g->n_seg * 2);
	for (i = 0; i < n; ++i) {
		int32_t s;
		s = gfa_name2id(g, seg[i]);
		if (s >= 0) {
			kv_push(uint64_t, stack, (uint64_t)(s<<1|0)<<32);
			kv_push(uint64_t, stack, (uint64_t)(s<<1|1)<<32);
		}
	}
	for (i = 0; i < g->n_seg; ++i) // mark all segments to be deleted
		g->seg[i].del = 1;
	while (stack.n) {
		uint64_t x = kv_pop(stack);
		uint32_t v = x>>32, r = (uint32_t)x;
		if (flag[v]) continue; // already visited
		flag[v] = 1;
		g->seg[v>>1].del = 0;
		if (r < step) {
			uint32_t nv = gfa_arc_n(g, v);
			gfa_arc_t *av = gfa_arc_a(g, v);
			for (i = 0; i < nv; ++i)
				if (flag[av[i].w] == 0)
					kv_push(uint64_t, stack, (uint64_t)av[i].w<<32 | (r + 1));
		}
	}
	free(stack.a);
	free(flag);
	gfa_arc_rm(g);
}

static uint64_t find_join(const gfa_t *g, uint32_t v)
{
	gfa_seg_t *t, *s = &g->seg[v>>1];
	int32_t i, nv, n_low, n_r;
	uint32_t w;
	gfa_arc_t *av;
	if (s->rank == 0) return (uint64_t)-1;
	nv = gfa_arc_n(g, v);
	av = gfa_arc_a(g, v);
	for (i = 0, n_low = n_r = 0, w = 0; i < nv; ++i) {
		gfa_arc_t *q = &av[i];
		if (q->rank >= 0 && q->rank == s->rank) {
			++n_r, w = q->w;
		} else {
			t = &g->seg[q->w>>1];
			if (t->rank >= 0 && t->rank < s->rank)
				++n_low, w = q->w;
		}
	}
	if (n_r != 1 && gfa_verbose >= 2)
		fprintf(stderr, "[W] failed to find the associated arc for vertex %c%s[%d]: %d,%d\n", "><"[v&1], g->seg[v>>1].name, v, n_r, n_low);
	if (n_r != 1 && n_low != 1) return (uint64_t)-1;
	t = &g->seg[w>>1];
	return (uint64_t)t->snid<<32 | (uint32_t)(w&1? t->soff + t->len : t->soff) << 1 | (w&1);
}

gfa_sfa_t *gfa_gfa2sfa(const gfa_t *g, int32_t *n_sfa_, int32_t write_seq)
{
	extern void radix_sort_gfa64(uint64_t *st, uint64_t *en);
	int32_t i, j, k, *scnt, *soff, n_sfa;
	gfa_sfa_t *sfa = 0;
	uint64_t *a;

	*n_sfa_ = 0;
	if (g->n_sseq == 0) return 0;

	// precount
	GFA_CALLOC(scnt, g->n_sseq);
	for (i = 0; i < g->n_seg; ++i)
		if (g->seg[i].snid >= 0)
			++scnt[g->seg[i].snid];
	GFA_MALLOC(soff, g->n_sseq + 1);
	for (soff[0] = 0, i = 1; i <= g->n_sseq; ++i)
		soff[i] = soff[i - 1] + scnt[i - 1];

	// fill a[]
	GFA_BZERO(scnt, g->n_sseq);
	GFA_MALLOC(a, g->n_seg);
	for (i = 0; i < g->n_seg; ++i) {
		const gfa_seg_t *s = &g->seg[i];
		if (s->snid < 0) continue;
		a[soff[s->snid] + scnt[s->snid]] = (uint64_t)s->soff<<32 | i;
		++scnt[s->snid];
	}
	for (i = 0; i < g->n_sseq; ++i)
		if (scnt[i] > 1)
			radix_sort_gfa64(&a[soff[i]], &a[soff[i+1]]);
	free(scnt);

	// check
	n_sfa = g->n_sseq;
	for (i = 0; i < g->n_sseq; ++i) {
		const gfa_seg_t *s;
		if (soff[i] == soff[i+1]) --n_sfa;
		if (soff[i] == soff[i+1]) continue;
		s = &g->seg[(int32_t)a[soff[i]]];
		if (s->rank == 0 && s->soff != 0) {
			if (gfa_verbose >= 2)
				fprintf(stderr, "[W] rank-0 stable sequence \"%s\" not started with 0\n", g->sseq[s->snid].name);
			goto end_check;
		}
		for (j = soff[i] + 1; j < soff[i+1]; ++j) {
			const gfa_seg_t *s = &g->seg[(int32_t)a[j-1]];
			const gfa_seg_t *t = &g->seg[(int32_t)a[j]];
			if (s->soff + s->len > t->soff) {
				if (gfa_verbose >= 2)
					fprintf(stderr, "[W] overlap on stable sequence \"%s\"\n", g->sseq[s->snid].name);
				goto end_check;
			}
			if (s->rank == 0 && s->soff + s->len != t->soff) {
				if (gfa_verbose >= 2)
					fprintf(stderr, "[W] rank-0 stable sequence \"%s\" is not contiguous\n", g->sseq[s->snid].name);
				goto end_check;
			}
			if (s->rank != t->rank) {
				if (gfa_verbose >= 2)
					fprintf(stderr, "[W] stable sequence \"%s\" associated with different ranks\n", g->sseq[s->snid].name);
				goto end_check;
			}
			if (s->soff + s->len == t->soff) {
				int32_t k, nv;
				const gfa_arc_t *av;
				nv = gfa_arc_n(g, (uint32_t)a[j-1]<<1);
				av = gfa_arc_a(g, (uint32_t)a[j-1]<<1);
				for (k = 0; k < nv; ++k)
					if (av[k].w == (uint32_t)a[j]<<1)
						break;
				if (s->rank == 0 && k == nv) {
					if (gfa_verbose >= 2)
						fprintf(stderr, "[W] nearby segments on rank-0 stable sequence \"%s\" are not connected\n", g->sseq[s->snid].name);
					goto end_check;
				}
				if (k == nv) ++n_sfa;
			} else ++n_sfa;
		}
	}

	// fill sfa[]
	*n_sfa_ = n_sfa;
	GFA_CALLOC(sfa, n_sfa);
	for (i = 0, k = 0; i < g->n_sseq; ++i) {
		int32_t jst;
		if (soff[i] == soff[i+1]) continue;
		for (j = soff[i] + 1, jst = j - 1; j <= soff[i+1]; ++j) {
			int32_t is_cont = 0;
			if (j < soff[i+1]) {
				const gfa_seg_t *s = &g->seg[(int32_t)a[j-1]];
				const gfa_seg_t *t = &g->seg[(int32_t)a[j]];
				if (s->soff + s->len == t->soff) {
					int32_t k, nv;
					const gfa_arc_t *av;
					nv = gfa_arc_n(g, (uint32_t)a[j-1]<<1);
					av = gfa_arc_a(g, (uint32_t)a[j-1]<<1);
					for (k = 0; k < nv; ++k)
						if (av[k].w == (uint32_t)a[j]<<1)
							break;
					if (k < nv) is_cont = 1;
				}
			}
			if (!is_cont) {
				int32_t l;
				const gfa_seg_t *s = &g->seg[(int32_t)a[jst]];
				gfa_sfa_t *p = &sfa[k++];
				assert(jst < j);
				p->snid = s->snid, p->soff = s->soff, p->rank = s->rank;
				p->end[0] = find_join(g, (uint32_t)a[jst]<<1|1);
				if (p->end[0] != (uint64_t)-1) p->end[0] ^= 1;
				p->end[1] = find_join(g, (uint32_t)a[j-1]<<1);
				for (l = jst, p->len = 0; l < j; ++l)
					p->len += g->seg[(int32_t)a[l]].len;
				if (write_seq) {
					GFA_MALLOC(p->seq, p->len + 1);
					for (l = jst, p->len = 0; l < j; ++l) {
						s = &g->seg[(int32_t)a[l]];
						memcpy(&p->seq[p->len], s->seq, s->len);
						p->len += s->len;
					}
					p->seq[p->len] = 0;
				}
				jst = j;
			}
		}
	}
	assert(k == n_sfa);

end_check:
	free(soff);
	free(a);
	return sfa;
}

void gfa_blacklist_print(const gfa_t *g, FILE *fp) // FIXME: doesn't work with translocations
{
	uint32_t i, *vs;
	GFA_MALLOC(vs, g->n_sseq);
	for (i = 0; i < g->n_sseq; ++i)
		vs[i] = (uint32_t)-1;
	for (i = 0; i < g->n_seg; ++i) {
		const gfa_seg_t *s = &g->seg[i];
		if (s->rank != 0 || s->snid < 0) continue;
		if (s->soff == 0) vs[s->snid] = i<<1; // NB: assuming all rank-0 sseq start with 0
	}
	for (i = 0; i < g->n_sseq; ++i) {
		gfa_sub_t *sub;
		int32_t j, jst, max_a;
		if (vs[i] == (uint32_t)-1) continue;
		sub = gfa_sub_from(0, g, vs[i], 0);
		for (j = 0, jst = 0, max_a = -1; j < sub->n_v; ++j) {
			gfa_subv_t *t = &sub->v[j];
			int32_t k;
			if (j == max_a) {
				const gfa_seg_t *sst = &g->seg[sub->v[jst].v>>1];
				const gfa_seg_t *sen = &g->seg[t->v>>1];
				if (sst->snid == i && sen->snid == i) {
					fprintf(fp, "%s\t%d\t%d\t%d\t", g->sseq[i].name, sst->soff + sst->len, sen->soff, j - jst + 1);
					for (k = jst; k <= j; ++k) {
						if (k > jst) fputc(',', fp);
						fputs(g->seg[sub->v[k].v>>1].name, fp);
					}
					fputc('\n', fp);
				}
				max_a = -1, jst = j;
			}
			for (k = 0; k < t->n; ++k)
				if (sub->a[t->off + k] > max_a)
					max_a = sub->a[t->off + k];
		}
		gfa_sub_destroy(sub);
	}
	free(vs);
}
