/* -*- c-basic-offset: 4; indent-tabs-mode: nil -*- */
/* ====================================================================
 * Copyright (c) 1999-2004 Carnegie Mellon University.  All rights
 * reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer. 
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 *
 * THIS SOFTWARE IS PROVIDED BY CARNEGIE MELLON UNIVERSITY ``AS IS'' AND 
 * ANY EXPRESSED OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, 
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL CARNEGIE MELLON UNIVERSITY
 * NOR ITS EMPLOYEES BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT 
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, 
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY 
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT 
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE 
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * ====================================================================
 *
 */

/*
 * fsg_search2.c -- Search structures for FSM decoding.
 * 
 * **********************************************
 * CMU ARPA Speech Project
 *
 * Copyright (c) 2004 Carnegie Mellon University.
 * ALL RIGHTS RESERVED.
 * **********************************************
 * 
 * HISTORY
 *
 * 18-Feb-2004	M K Ravishankar (rkm@cs.cmu.edu) at Carnegie Mellon
 * 		Started.
 */

/* System headers. */
#include <stdio.h>
#include <string.h>
#include <assert.h>

/* SphinxBase headers. */
#include <err.h>
#include <ckd_alloc.h>
#include <cmd_ln.h>

/* Local headers. */
#include "pocketsphinx_internal.h"
#include "fsg_search2.h"
#include "fsg_history.h"
#include "fsg_lextree.h"

/* Turn this on for detailed debugging dump */
#define __FSG_DBG__		0
#define __FSG_DBG_CHAN__	0

static ps_seg_t *fsg_search2_seg_iter(ps_search_t *search, int32 *out_score);

static ps_searchfuncs_t fsg_funcs = {
    /* name: */   "fsg",
    /* start: */  fsg_search2_start,
    /* step: */   fsg_search2_step,
    /* finish: */ fsg_search2_finish,
    /* free: */   fsg_search2_free,
    /* hyp: */      fsg_search2_hyp,
    /* seg_iter: */ fsg_search2_seg_iter,
};

ps_search_t *
fsg_search2_init(cmd_ln_t *config,
                 acmod_t *acmod,
                 dict_t *dict)
{
    fsg_search2_t *fsgs;
    char const *path;

    fsgs = ckd_calloc(1, sizeof(*fsgs));
    ps_search_init(&fsgs->base, &fsg_funcs, config, acmod, dict);

    /* Initialize HMM context. */
    fsgs->hmmctx = hmm_context_init(bin_mdef_n_emit_state(acmod->mdef),
                                    acmod->tmat->tp, NULL, acmod->mdef->sseq);

    /* Intialize the search history object */
    fsgs->history = fsg_history_init(NULL, dict);
    fsgs->frame = -1;

    /* Initialize FSG table. */
    fsgs->fsgs = hash_table_new(5, FALSE);

    /* Get search pruning parameters */
    fsgs->beam_factor = 1.0f;
    fsgs->beam = fsgs->beam_orig
        = (int32) logmath_log(acmod->lmath, cmd_ln_float64_r(config, "-beam"));
    fsgs->pbeam = fsgs->pbeam_orig
        = (int32) logmath_log(acmod->lmath, cmd_ln_float64_r(config, "-pbeam"));
    fsgs->wbeam = fsgs->wbeam_orig
        = (int32) logmath_log(acmod->lmath, cmd_ln_float64_r(config, "-wbeam"));

    /* LM related weights/penalties */
    fsgs->lw = cmd_ln_float32_r(config, "-lw");
    fsgs->pip = (int32) (logmath_log(acmod->lmath, cmd_ln_float32_r(config, "-pip"))
                           * fsgs->lw);
    fsgs->wip = (int32) (logmath_log(acmod->lmath, cmd_ln_float32_r(config, "-wip"))
                           * fsgs->lw);

    E_INFO("FSG(beam: %d, pbeam: %d, wbeam: %d; wip: %d, pip: %d)\n",
           fsgs->beam_orig, fsgs->pbeam_orig, fsgs->wbeam_orig,
           fsgs->wip, fsgs->pip);

    /* Load an FSG if one was specified in config */
    if ((path = cmd_ln_str_r(config, "-fsg"))) {
        fsg_model_t *fsg;

        if ((fsg = fsg_model_readfile(path, acmod->lmath)) == NULL)
            goto error_out;
        if (fsg_search2_add(fsgs, fsg_model_name(fsg), fsg) != fsg) {
            fsg_model_free(fsg);
            goto error_out;
        }
        if (fsg_search2_select(fsgs, fsg_model_name(fsg)) == NULL) {
            fsg_model_free(fsg);
            goto error_out;
        }
    }

    return ps_search_base(fsgs);

error_out:
    fsg_search2_free(ps_search_base(fsgs));
    return NULL;
}

void
fsg_search2_free(ps_search_t *search)
{
    fsg_search2_t *fsgs = (fsg_search2_t *)search;
    hash_iter_t *itor;

    ps_search_deinit(search);
    hmm_context_free(fsgs->hmmctx);
    fsg_lextree_free(fsgs->lextree);
    fsg_history_reset(fsgs->history);
    fsg_history_set_fsg(fsgs->history, NULL, NULL);
    for (itor = hash_table_iter(fsgs->fsgs);
         itor; itor = hash_table_iter_next(itor)) {
        fsg_model_t *fsg = (fsg_model_t *) hash_entry_val(itor->ent);
        fsg_model_free(fsg);
    }
    hash_table_free(fsgs->fsgs);
    fsg_history_free(fsgs->history);
    ckd_free(fsgs);
}


fsg_model_t *
fsg_search2_get_fsg(fsg_search2_t *fsgs, const char *name)
{
    void *val;

    if (hash_table_lookup(fsgs->fsgs, name, &val) < 0)
        return NULL;
    return (fsg_model_t *)val;
}

int
fsg_search2_add_silences(fsg_search2_t *fsgs, fsg_model_t *fsg)
{
    dict_t *dict;
    int32 wid;
    int n_sil;

    dict = ps_search_dict(fsgs);
    /* Add <s> and </s> to the first and last states. */
    fsg_model_add_silence(fsg, "<s>", fsg_model_start_state(fsg),
                          cmd_ln_float32_r(ps_search_config(fsgs), "-silprob"));
    fsg_model_add_silence(fsg, "</s>", fsg_model_final_state(fsg),
                          cmd_ln_float32_r(ps_search_config(fsgs), "-silprob"));
    /* Add any other fillers to all states. */
    fsg_model_add_silence(fsg, "<sil>", -1,
                          cmd_ln_float32_r(ps_search_config(fsgs), "-silprob"));
    n_sil = 0;
    for (wid = dict_to_id(dict, "<sil>") + 1; wid < dict_n_words(dict); ++wid) {
        char const *word = dict_word_str(dict, wid);
        /* FIXME: Shouldn't happen?  Also we need a better way to mark fillers. */
        if (0 == strcmp(word, "<s>") || 0 == strcmp(word, "</s>")) {
            E_ERROR("WTF, %s=%d > <sil>=%d\n", word, wid, dict_to_id(dict, "<sil>"));
            continue;
        }
        fsg_model_add_silence(fsg, word, -1,
                              cmd_ln_float32_r(ps_search_config(fsgs), "-fillprob"));
        ++n_sil;
    }

    return n_sil;
}

int
fsg_search2_add_altpron(fsg_search2_t *fsgs, fsg_model_t *fsg)
{
    dict_t *dict;
    int n_alt;
    int i;

    dict = ps_search_dict(fsgs);
    /* Scan FSG's vocabulary for words that have alternate pronunciations. */
    n_alt = 0;
    for (i = 0; i < fsg_model_n_word(fsg); ++i) {
        char const *word;
        int32 wid;

        word = fsg_model_word_str(fsg, i);
        wid = dict_to_id(dict, word);
        while ((wid = dict_next_alt(dict, wid)) != NO_WORD) {
            fsg_model_add_alt(fsg, word, dict_word_str(dict, wid));
            ++n_alt;
        }
    }

    return n_alt;
}

fsg_model_t *
fsg_search2_add(fsg_search2_t *fsgs, char const *name, fsg_model_t *fsg)
{
    if (name == NULL)
        name = fsg_model_name(fsg);

    /* Add silence transitions and alternate words. */
    if (cmd_ln_boolean_r(ps_search_config(fsgs), "-fsgusefiller")
        && !fsg_model_has_sil(fsg))
        fsg_search2_add_silences(fsgs, fsg);
    if (cmd_ln_boolean_r(ps_search_config(fsgs), "-fsgusealtpron")
        && !fsg_model_has_alt(fsg))
        fsg_search2_add_altpron(fsgs, fsg);

    return (fsg_model_t *)hash_table_enter(fsgs->fsgs, name, fsg);
}


fsg_model_t *
fsg_search2_remove_byname(fsg_search2_t *fsgs, char const *key)
{
    fsg_model_t *oldfsg;
    void *val;

    /* Look for the matching FSG. */
    if (hash_table_lookup(fsgs->fsgs, key, &val) < 0) {
        E_ERROR("FSG `%s' to be deleted not found\n", key);
        return NULL;
    }
    oldfsg = val;

    /* Remove it from the FSG table. */
    hash_table_delete(fsgs->fsgs, key);
    /* If this was the currently active FSG, also delete other stuff */
    if (fsgs->fsg == oldfsg) {
        fsg_lextree_free(fsgs->lextree);
        fsgs->lextree = NULL;
        fsg_history_set_fsg(fsgs->history, NULL, NULL);
        fsgs->fsg = NULL;
    }
    return oldfsg;
}


fsg_model_t *
fsg_search2_remove(fsg_search2_t *fsgs, fsg_model_t *fsg)
{
    char const *key;
    hash_iter_t *itor;

    key = NULL;
    for (itor = hash_table_iter(fsgs->fsgs);
         itor; itor = hash_table_iter_next(itor)) {
        fsg_model_t *oldfsg;

        oldfsg = (fsg_model_t *) hash_entry_val(itor->ent);
        if (oldfsg == fsg) {
            key = hash_entry_key(itor->ent);
            hash_table_iter_free(itor);
            break;
        }
    }
    if (key == NULL) {
        E_WARN("FSG '%s' to be deleted not found\n", fsg_model_name(fsg));
        return NULL;
    }
    else
        return fsg_search2_remove_byname(fsgs, key);
}


fsg_model_t *
fsg_search2_select(fsg_search2_t *fsgs, const char *name)
{
    fsg_model_t *fsg;

    fsg = fsg_search2_get_fsg(fsgs, name);
    if (fsg == NULL) {
        E_ERROR("FSG '%s' not known; cannot make it current\n", name);
        return NULL;
    }

    /* Free the old lextree */
    if (fsgs->lextree)
        fsg_lextree_free(fsgs->lextree);

    /* Allocate new lextree for the given FSG */
    fsgs->lextree = fsg_lextree_init(fsg, ps_search_dict(fsgs),
                                     ps_search_acmod(fsgs)->mdef,
                                     fsgs->hmmctx, fsgs->wip, fsgs->pip);

    /* Inform the history module of the new fsg */
    fsg_history_set_fsg(fsgs->history, fsg, ps_search_dict(fsgs));
    fsgs->fsg = fsg;

    return fsg;
}

static void
fsg_search2_sen_active(fsg_search2_t *fsgs)
{
    gnode_t *gn;
    fsg_pnode_t *pnode;
    hmm_t *hmm;

    acmod_clear_active(ps_search_acmod(fsgs));

    for (gn = fsgs->pnode_active; gn; gn = gnode_next(gn)) {
        pnode = (fsg_pnode_t *) gnode_ptr(gn);
        hmm = fsg_pnode_hmmptr(pnode);
        assert(hmm_frame(hmm) == fsgs->frame);
        acmod_activate_hmm(ps_search_acmod(fsgs), hmm);
    }
}


/*
 * Evaluate all the active HMMs.
 * (Executed once per frame.)
 */
static void
fsg_search2_hmm_eval(fsg_search2_t *fsgs)
{
    gnode_t *gn;
    fsg_pnode_t *pnode;
    hmm_t *hmm;
    int32 bestscore;
    int32 n, maxhmmpf;

    bestscore = WORST_SCORE;

    if (!fsgs->pnode_active) {
        E_ERROR("Frame %d: No active HMM!!\n", fsgs->frame);
        return;
    }

    for (n = 0, gn = fsgs->pnode_active; gn; gn = gnode_next(gn), n++) {
        int32 score;

        pnode = (fsg_pnode_t *) gnode_ptr(gn);
        hmm = fsg_pnode_hmmptr(pnode);
        assert(hmm_frame(hmm) == fsgs->frame);

#if __FSG_DBG__
        E_INFO("pnode(%08x) active @frm %5d\n", (int32) pnode,
               fsgs->frame);
        hmm_dump(hmm, stdout);
#endif
        score = hmm_vit_eval(hmm);
#if __FSG_DBG_CHAN__
        E_INFO("pnode(%08x) after eval @frm %5d\n",
               (int32) pnode, fsgs->frame);
        hmm_dump(hmm, stdout);
#endif

        if (bestscore < score)
            bestscore = score;
    }

#if __FSG_DBG__
    E_INFO("[%5d] %6d HMM; bestscr: %11d\n", fsgs->frame, n, bestscore);
#endif
    fsgs->n_hmm_eval += n;

    /* Adjust beams if #active HMMs larger than absolute threshold */
    maxhmmpf = cmd_ln_int32_r(ps_search_config(fsgs), "-maxhmmpf");
    if (maxhmmpf != -1 && n > maxhmmpf) {
        /*
         * Too many HMMs active; reduce the beam factor applied to the default
         * beams, but not if the factor is already at a floor (0.1).
         */
        if (fsgs->beam_factor > 0.1) {        /* Hack!!  Hardwired constant 0.1 */
            fsgs->beam_factor *= 0.9f;        /* Hack!!  Hardwired constant 0.9 */
            fsgs->beam =
                (int32) (fsgs->beam_orig * fsgs->beam_factor);
            fsgs->pbeam =
                (int32) (fsgs->pbeam_orig * fsgs->beam_factor);
            fsgs->wbeam =
                (int32) (fsgs->wbeam_orig * fsgs->beam_factor);
        }
    }
    else {
        fsgs->beam_factor = 1.0f;
        fsgs->beam = fsgs->beam_orig;
        fsgs->pbeam = fsgs->pbeam_orig;
        fsgs->wbeam = fsgs->wbeam_orig;
    }

    if (n > fsg_lextree_n_pnode(fsgs->lextree))
        E_FATAL("PANIC! Frame %d: #HMM evaluated(%d) > #PNodes(%d)\n",
                fsgs->frame, n, fsg_lextree_n_pnode(fsgs->lextree));

    fsgs->bestscore = bestscore;
}


static void
fsg_search2_pnode_trans(fsg_search2_t *fsgs, fsg_pnode_t * pnode)
{
    fsg_pnode_t *child;
    hmm_t *hmm;
    int32 newscore, thresh, nf;

    assert(pnode);
    assert(!fsg_pnode_leaf(pnode));

    nf = fsgs->frame + 1;
    thresh = fsgs->bestscore + fsgs->beam;

    hmm = fsg_pnode_hmmptr(pnode);

    for (child = fsg_pnode_succ(pnode);
         child; child = fsg_pnode_sibling(child)) {
        newscore = hmm_out_score(hmm) + child->logs2prob;

        if ((newscore >= thresh) && (newscore > hmm_in_score(&child->hmm))) {
            /* Incoming score > pruning threshold and > target's existing score */
            if (hmm_frame(&child->hmm) < nf) {
                /* Child node not yet activated; do so */
                fsgs->pnode_active_next =
                    glist_add_ptr(fsgs->pnode_active_next,
                                  (void *) child);
            }

            hmm_enter(&child->hmm, newscore, hmm_out_history(hmm), nf);
        }
    }
}


static void
fsg_search2_pnode_exit(fsg_search2_t *fsgs, fsg_pnode_t * pnode)
{
    hmm_t *hmm;
    fsg_link_t *fl;
    int32 wid;
    fsg_pnode_ctxt_t ctxt;

    assert(pnode);
    assert(fsg_pnode_leaf(pnode));

    hmm = fsg_pnode_hmmptr(pnode);
    fl = fsg_pnode_fsglink(pnode);
    assert(fl);

    wid = fsg_link_wid(fl);
    assert(wid >= 0);

#if __FSG_DBG__
    E_INFO("[%5d] Exit(%08x) %10d(score) %5d(pred)\n",
           fsgs->frame, (int32) pnode,
           hmm_out_score(hmm), hmm_out_history(hmm));
#endif

    /*
     * Check if this is filler or single phone word; these do not model right
     * context (i.e., the exit score applies to all right contexts).
     */
    if (fsg_model_is_filler(fsgs->fsg, wid)
        /* FIXME: This might be slow due to repeated calls to dict_to_id(). */
        || (dict_pronlen(ps_search_dict(fsgs),
                         dict_to_id(ps_search_dict(fsgs),
                                    fsg_model_word_str(fsgs->fsg, wid))) == 1)) {
        /* Create a dummy context structure that applies to all right contexts */
        fsg_pnode_add_all_ctxt(&ctxt);

        /* Create history table entry for this word exit */
        fsg_history_entry_add(fsgs->history,
                              fl,
                              fsgs->frame,
                              hmm_out_score(hmm),
                              hmm_out_history(hmm),
                              pnode->ci_ext, ctxt);

    }
    else {
        /* Create history table entry for this word exit */
        fsg_history_entry_add(fsgs->history,
                              fl,
                              fsgs->frame,
                              hmm_out_score(hmm),
                              hmm_out_history(hmm),
                              pnode->ci_ext, pnode->ctxt);
    }
}


/*
 * (Beam) prune the just evaluated HMMs, determine which ones remain
 * active, which ones transition to successors, which ones exit and
 * terminate in their respective destination FSM states.
 * (Executed once per frame.)
 */
static void
fsg_search2_hmm_prune_prop(fsg_search2_t *fsgs)
{
    gnode_t *gn;
    fsg_pnode_t *pnode;
    hmm_t *hmm;
    int32 thresh, word_thresh, phone_thresh;

    assert(fsgs->pnode_active_next == NULL);

    thresh = fsgs->bestscore + fsgs->beam;
    phone_thresh = fsgs->bestscore + fsgs->pbeam;
    word_thresh = fsgs->bestscore + fsgs->wbeam;

    for (gn = fsgs->pnode_active; gn; gn = gnode_next(gn)) {
        pnode = (fsg_pnode_t *) gnode_ptr(gn);
        hmm = fsg_pnode_hmmptr(pnode);

        if (hmm_bestscore(hmm) >= thresh) {
            /* Keep this HMM active in the next frame */
            if (hmm_frame(hmm) == fsgs->frame) {
                hmm_frame(hmm) = fsgs->frame + 1;
                fsgs->pnode_active_next =
                    glist_add_ptr(fsgs->pnode_active_next,
                                  (void *) pnode);
            }
            else {
                assert(hmm_frame(hmm) == fsgs->frame + 1);
            }

            if (!fsg_pnode_leaf(pnode)) {
                if (hmm_out_score(hmm) >= phone_thresh) {
                    /* Transition out of this phone into its children */
                    fsg_search2_pnode_trans(fsgs, pnode);
                }
            }
            else {
                if (hmm_out_score(hmm) >= word_thresh) {
                    /* Transition out of leaf node into destination FSG state */
                    fsg_search2_pnode_exit(fsgs, pnode);
                }
            }
        }
    }
}


/*
 * Propagate newly created history entries through null transitions.
 */
static void
fsg_search2_null_prop(fsg_search2_t *fsgs)
{
    int32 bpidx, n_entries, thresh, newscore;
    fsg_hist_entry_t *hist_entry;
    fsg_link_t *l;
    int32 s, d;
    fsg_model_t *fsg;

    fsg = fsgs->fsg;
    thresh = fsgs->bestscore + fsgs->wbeam; /* Which beam really?? */

    n_entries = fsg_history_n_entries(fsgs->history);

    for (bpidx = fsgs->bpidx_start; bpidx < n_entries; bpidx++) {
        hist_entry = fsg_history_entry_get(fsgs->history, bpidx);

        l = fsg_hist_entry_fsglink(hist_entry);

        /* Destination FSG state for history entry */
        s = l ? fsg_link_to_state(l) : fsg_model_start_state(fsg);

        /*
         * Check null transitions from d to all other states.  (Only need to
         * propagate one step, since FSG contains transitive closure of null
         * transitions.)
         */
        for (d = 0; d < fsg_model_n_state(fsg); d++) {
            l = fsg_model_null_trans(fsg, s, d);

            if (l) {            /* Propagate history entry through this null transition */
                newscore =
                    fsg_hist_entry_score(hist_entry) +
                    fsg_link_logs2prob(l);

                if (newscore >= thresh) {
                    fsg_history_entry_add(fsgs->history, l,
                                          fsg_hist_entry_frame(hist_entry),
                                          newscore,
                                          bpidx,
                                          fsg_hist_entry_lc(hist_entry),
                                          fsg_hist_entry_rc(hist_entry));
                }
            }
        }
    }
}


/*
 * Perform cross-word transitions; propagate each history entry created in this
 * frame to lextree roots attached to the target FSG state for that entry.
 */
static void
fsg_search2_word_trans(fsg_search2_t *fsgs)
{
    int32 bpidx, n_entries;
    fsg_hist_entry_t *hist_entry;
    fsg_link_t *l;
    int32 score, newscore, thresh, nf, d;
    fsg_pnode_t *root;
    int32 lc, rc;

    n_entries = fsg_history_n_entries(fsgs->history);

    thresh = fsgs->bestscore + fsgs->beam;
    nf = fsgs->frame + 1;

    for (bpidx = fsgs->bpidx_start; bpidx < n_entries; bpidx++) {
        hist_entry = fsg_history_entry_get(fsgs->history, bpidx);
        assert(hist_entry);
        score = fsg_hist_entry_score(hist_entry);
        assert(fsgs->frame == fsg_hist_entry_frame(hist_entry));

        l = fsg_hist_entry_fsglink(hist_entry);

        /* Destination state for hist_entry */
        d = l ? fsg_link_to_state(l) : fsg_model_start_state(fsgs->
                                                                fsg);

        lc = fsg_hist_entry_lc(hist_entry);

        /* Transition to all root nodes attached to state d */
        for (root = fsg_lextree_root(fsgs->lextree, d);
             root; root = root->sibling) {
            rc = root->ci_ext;

            if ((root->ctxt.bv[lc >> 5] & (1 << (lc & 0x001f))) &&
                (hist_entry->rc.bv[rc >> 5] & (1 << (rc & 0x001f)))) {
                /*
                 * Last CIphone of history entry is in left-context list supported by
                 * target root node, and
                 * first CIphone of target root node is in right context list supported
                 * by history entry;
                 * So the transition can go ahead (if new score is good enough).
                 */
                newscore = score + root->logs2prob;

                if ((newscore >= thresh)
                    && (newscore > hmm_in_score(&root->hmm))) {
                    if (hmm_frame(&root->hmm) < nf) {
                        /* Newly activated node; add to active list */
                        fsgs->pnode_active_next =
                            glist_add_ptr(fsgs->pnode_active_next,
                                          (void *) root);
#if __FSG_DBG__
                        E_INFO
                            ("[%5d] WordTrans bpidx[%d] -> pnode[%08x] (activated)\n",
                             fsgs->frame, bpidx, (int32) root);
#endif
                    }
                    else {
#if __FSG_DBG__
                        E_INFO
                            ("[%5d] WordTrans bpidx[%d] -> pnode[%08x]\n",
                             fsgs->frame, bpidx, (int32) root);
#endif
                    }

                    hmm_enter(&root->hmm, newscore, bpidx, nf);
                }
            }
        }
    }
}


int
fsg_search2_step(ps_search_t *search)
{
    fsg_search2_t *fsgs = (fsg_search2_t *)search;
    int16 const *senscr;
    int frame_idx, best_senid;
    int16 best_senscr;
    acmod_t *acmod = search->acmod;
    gnode_t *gn;
    fsg_pnode_t *pnode;
    hmm_t *hmm;

    /* Determine if we actually have a frame to process. */
    if (acmod->n_feat_frame == 0)
        return 0;

    /* Activate our HMMs for the current frame if need be. */
    if (!acmod->compallsen)
        fsg_search2_sen_active(fsgs);
    /* Compute GMM scores for the current frame. */
    senscr = acmod_score(acmod, &frame_idx, &best_senscr, &best_senid);
    fsgs->n_sen_eval += acmod->n_senone_active;
    hmm_context_set_senscore(fsgs->hmmctx, senscr);

    /* Mark backpointer table for current frame. */
    fsgs->bpidx_start = fsg_history_n_entries(fsgs->history);

    /* Evaluate all active pnodes (HMMs) */
    fsg_search2_hmm_eval(fsgs);

    /*
     * Prune and propagate the HMMs evaluated; create history entries for
     * word exits.  The words exits are tentative, and may be pruned; make
     * the survivors permanent via fsg_history_end_frame().
     */
    fsg_search2_hmm_prune_prop(fsgs);
    fsg_history_end_frame(fsgs->history);

    /*
     * Propagate new history entries through any null transitions, creating
     * new history entries, and then make the survivors permanent.
     */
    fsg_search2_null_prop(fsgs);
    fsg_history_end_frame(fsgs->history);

    /*
     * Perform cross-word transitions; propagate each history entry across its
     * terminating state to the root nodes of the lextree attached to the state.
     */
    fsg_search2_word_trans(fsgs);

    /*
     * We've now come full circle, HMM and FSG states have been updated for
     * the next frame.
     * Update the active lists, deactivate any currently active HMMs that
     * did not survive into the next frame
     */
    for (gn = fsgs->pnode_active; gn; gn = gnode_next(gn)) {
        pnode = (fsg_pnode_t *) gnode_ptr(gn);
        hmm = fsg_pnode_hmmptr(pnode);

        if (hmm_frame(hmm) == fsgs->frame) {
            /* This HMM NOT activated for the next frame; reset it */
            fsg_psubtree_pnode_deactivate(pnode);
        }
        else {
            assert(hmm_frame(hmm) == (fsgs->frame + 1));
        }
    }

    /* Free the currently active list */
    glist_free(fsgs->pnode_active);

    /* Make the next-frame active list the current one */
    fsgs->pnode_active = fsgs->pnode_active_next;
    fsgs->pnode_active_next = NULL;

    /* End of this frame; ready for the next */
    ++fsgs->frame;

    return 1;
}


/*
 * Set all HMMs to inactive, clear active lists, initialize FSM start
 * state to be the only active node.
 * (Executed at the start of each utterance.)
 */
int
fsg_search2_start(ps_search_t *search)
{
    fsg_search2_t *fsgs = (fsg_search2_t *)search;
    int32 silcipid;
    fsg_pnode_ctxt_t ctxt;

    /* Reset dynamic adjustment factor for beams */
    fsgs->beam_factor = 1.0f;
    fsgs->beam = fsgs->beam_orig;
    fsgs->pbeam = fsgs->pbeam_orig;
    fsgs->wbeam = fsgs->wbeam_orig;

    silcipid = bin_mdef_ciphone_id(ps_search_acmod(fsgs)->mdef, "SIL");

    /* Initialize EVERYTHING to be inactive */
    assert(fsgs->pnode_active == NULL);
    assert(fsgs->pnode_active_next == NULL);

    fsg_history_reset(fsgs->history);
    fsg_history_utt_start(fsgs->history);
    fsgs->final = FALSE;

    /* Dummy context structure that allows all right contexts to use this entry */
    fsg_pnode_add_all_ctxt(&ctxt);

    /* Create dummy history entry leading to start state */
    fsgs->frame = -1;
    fsgs->bestscore = 0;
    fsg_history_entry_add(fsgs->history,
                          NULL, -1, 0, -1, silcipid, ctxt);
    fsgs->bpidx_start = 0;

    /* Propagate dummy history entry through NULL transitions from start state */
    fsg_search2_null_prop(fsgs);

    /* Perform word transitions from this dummy history entry */
    fsg_search2_word_trans(fsgs);

    /* Make the next-frame active list the current one */
    fsgs->pnode_active = fsgs->pnode_active_next;
    fsgs->pnode_active_next = NULL;

    ++fsgs->frame;

    fsgs->n_hmm_eval = 0;
    fsgs->n_sen_eval = 0;

    return 0;
}

/*
 * Cleanup at the end of each utterance.
 */
int
fsg_search2_finish(ps_search_t *search)
{
    fsg_search2_t *fsgs = (fsg_search2_t *)search;
    gnode_t *gn;
    fsg_pnode_t *pnode;
    int32 n_hist;

    /* Deactivate all nodes in the current and next-frame active lists */
    for (gn = fsgs->pnode_active; gn; gn = gnode_next(gn)) {
        pnode = (fsg_pnode_t *) gnode_ptr(gn);
        fsg_psubtree_pnode_deactivate(pnode);
    }
    for (gn = fsgs->pnode_active_next; gn; gn = gnode_next(gn)) {
        pnode = (fsg_pnode_t *) gnode_ptr(gn);
        fsg_psubtree_pnode_deactivate(pnode);
    }

    glist_free(fsgs->pnode_active);
    fsgs->pnode_active = NULL;
    glist_free(fsgs->pnode_active_next);
    fsgs->pnode_active_next = NULL;

    fsgs->final = TRUE;

    n_hist = fsg_history_n_entries(fsgs->history);
    E_INFO
        ("%d frames, %d HMMs (%d/fr), %d senones (%d/fr), %d history entries (%d/fr)\n\n",
         fsgs->frame, fsgs->n_hmm_eval,
         (fsgs->frame > 0) ? fsgs->n_hmm_eval / fsgs->frame : 0,
         fsgs->n_sen_eval,
         (fsgs->frame > 0) ? fsgs->n_sen_eval / fsgs->frame : 0,
         n_hist, (fsgs->frame > 0) ? n_hist / fsgs->frame : 0);

    /* Sanity check */
    if (fsgs->n_hmm_eval >
        fsg_lextree_n_pnode(fsgs->lextree) * fsgs->frame) {
        E_ERROR
            ("SANITY CHECK #HMMEval(%d) > %d (#HMMs(%d)*#frames(%d)) FAILED\n",
             fsgs->n_hmm_eval,
             fsg_lextree_n_pnode(fsgs->lextree) * fsgs->frame,
             fsg_lextree_n_pnode(fsgs->lextree), fsgs->frame);
    }

    return 0;
}

static int
fsg_search2_find_exit(fsg_search2_t *fsgs, int frame_idx, int final, int32 *out_score)
{
    fsg_hist_entry_t *hist_entry;
    fsg_model_t *fsg;
    int bpidx, frm, last_frm, besthist;
    int32 bestscore;

    if (frame_idx == -1)
        frame_idx = fsgs->frame - 1;
    last_frm = frm = frame_idx;

    /* Scan backwards to find a word exit in frame_idx. */
    bpidx = fsg_history_n_entries(fsgs->history) - 1;
    while (bpidx > 0) {
        hist_entry = fsg_history_entry_get(fsgs->history, bpidx);
        if (fsg_hist_entry_frame(hist_entry) <= frame_idx) {
            frm = last_frm = fsg_hist_entry_frame(hist_entry);
            break;
        }
    }

    /* No hypothesis (yet). */
    if (bpidx <= 0) 
        return bpidx;

    /* Now find best word exit in this frame. */
    bestscore = INT_MIN;
    besthist = -1;
    fsg = fsgs->fsg;
    while (frm == last_frm) {
        fsg_link_t *fl;
        int32 score;

        fl = fsg_hist_entry_fsglink(hist_entry);
        score = fsg_hist_entry_score(hist_entry);

        if (score > bestscore) {
            /* Only enforce the final state constraint if this is a final hypothesis. */
            if ((!final)
                || fsg_link_to_state(fl) == fsg_model_final_state(fsg)) {
                bestscore = score;
                besthist = bpidx;
            }
        }

        --bpidx;
        if (bpidx < 0)
            break;
        hist_entry = fsg_history_entry_get(fsgs->history, bpidx);
        frm = fsg_hist_entry_frame(hist_entry);
    }

    /* Final state not reached. */
    if (besthist == -1) {
        E_ERROR("Final state not reached in frame %d\n", frame_idx);
        return -1;
    }

    /* This here's the one we want. */
    *out_score = bestscore;
    return besthist;
}

char const *
fsg_search2_hyp(ps_search_t *search, int32 *out_score)
{
    fsg_search2_t *fsgs = (fsg_search2_t *)search;
    char *c;
    size_t len;
    int bp, bpidx;

    /* Get last backpointer table index. */
    bpidx = fsg_search2_find_exit(fsgs, fsgs->frame, fsgs->final, out_score);
    /* No hypothesis (yet). */
    if (bpidx <= 0)
        return NULL;

    bp = bpidx;
    len = 0;
    while (bp > 0) {
        fsg_hist_entry_t *hist_entry = fsg_history_entry_get(fsgs->history, bp);
        fsg_link_t *fl = fsg_hist_entry_fsglink(hist_entry);
        int32 wid;

        bp = fsg_hist_entry_pred(hist_entry);
        wid = fsg_link_wid(fl);
        if (wid < 0 || fsg_model_is_filler(fsgs->fsg, wid))
            continue;
        len += strlen(fsg_model_word_str(fsgs->fsg, wid)) + 1;
    }

    ckd_free(search->hyp_str);
    search->hyp_str = ckd_calloc(1, len);
    bp = bpidx;
    c = search->hyp_str + len - 1;
    while (bp > 0) {
        fsg_hist_entry_t *hist_entry = fsg_history_entry_get(fsgs->history, bp);
        fsg_link_t *fl = fsg_hist_entry_fsglink(hist_entry);
        int32 wid;

        bp = fsg_hist_entry_pred(hist_entry);
        wid = fsg_link_wid(fl);
        if (wid < 0 || fsg_model_is_filler(fsgs->fsg, wid))
            continue;
        len = strlen(fsg_model_word_str(fsgs->fsg, wid));
        c -= len;
        memcpy(c, fsg_model_word_str(fsgs->fsg, wid), len);
        if (c > search->hyp_str) {
            --c;
            *c = ' ';
        }
    }

    return search->hyp_str;
}

static ps_seg_t *
fsg_search2_seg_iter(ps_search_t *search, int32 *out_score)
{
    return NULL;
}