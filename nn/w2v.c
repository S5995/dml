/* ========================================================
 *   Copyright (C) 2016 All rights reserved.
 *   
 *   filename : w2v.c
 *   author   : ***
 *   date     : 2016-12-14
 *   info     : 
 * ======================================================== */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "hash.h"
#include "str.h"
#include "repo.h"
#include "w2v.h"

static inline void w2v_st(W2V * w2v, double *st, int id, int k, int l, int r){
    int  c, i, j, vi;
    double *u;

    c = 0;

    for (i = l; i < r; i++) if (i != id){
        vi = w2v->ds->tokens[i];
        u  = w2v->u + vi * k;
        for (j = 0; j < k; j++){
            st[j] += u[j];
        }
        c += 1;
    }

    if (c > 1) for (j = 0; j < k; j++){
        st[j] /= c;
    }
}

static inline void w2v_ut(W2V * w2v, double * sg, int id, int k, int l, int r, double alpha){
    int i, j, vi;
    double * u;
    for (i = l; i < r; i++) if (i != id){
        vi = w2v->ds->tokens[i];
        u  = w2v->u + vi * k;
        for (j = 0; j < k; j++){
            u[j] += alpha * sg[j];
        }
    }
}

static Hash * w2v_load_model(W2V * w2v){
    char * outdir = w2v->wc->get_o(w2v->wc);
    char out[512] = {0};
    char buffer[10000] = {0};
    char *string, *token;
    FILE *fp = NULL;

    int i;
    Hash * vhs = hash_create(1 << 20, STRING);
    sprintf(out, "%s/vector", outdir);
    if (NULL == (fp = fopen(out, "r"))){
        hash_free(vhs);
        vhs = NULL;
        goto ret;
    }
    i = 0;
    while (NULL != fgets(buffer, 10000, fp)){
        string = trim(buffer, 3);
        token = strsep(&string, "\t");
        hash_add(vhs, token);
        while (NULL != (token = strsep(&string, "\t"))){
            w2v->u[i++] = atof(token);
        }
    }
    fclose(fp);

ret:
    return vhs;
}

static Hash * w2v_weight_init(W2V * w2v){
    int i, v, k, t;
    v = w2v->hsf->v;
    k = w2v->wc->get_k(w2v->wc);
    t = w2v->wc->get_t(w2v->wc);

    w2v->u = (double *)calloc(v * k, sizeof(double));

    i = v * k;
    while (i-- > 0){
        w2v->u[i] = ((rand() + 0.1) / (RAND_MAX + 0.1) - 0.5) / v;
    }

    if (t > 0) {
        return w2v_load_model(w2v);
    }
    return NULL;
}

W2V * w2v_create(int argc, char *argv[]){
    W2V * w2v = (W2V*)calloc(1, sizeof(W2V));

    w2v->wc = init_w2v_config();

    if (0 != w2v->wc->set(w2v->wc, argc, argv)){
        w2v->wc->help();
        free(w2v);
        w2v = NULL;
    }

    return w2v;
}

int w2v_init(W2V * w2v){
    int v, k, t;
    char *outdir;

    t = w2v->wc->get_t(w2v->wc);
    k = w2v->wc->get_k(w2v->wc);

    if (t > 0){
        outdir = w2v->wc->get_o(w2v->wc);
        if (0 != hsoft_load(&(w2v->hsf), outdir, k)){
            return -1;
        }
        Hash * vhs = w2v_weight_init(w2v);

        if (vhs){
            w2v->ds = tsd_load_v(w2v->wc->get_d(w2v->wc), vhs);
            hash_free(vhs);
            vhs = NULL;
            return 0;
        }
        return -1;
    }

    w2v->ds   = tsd_load(w2v->wc->get_d(w2v->wc));
    v = w2v->ds->v;

    hsoft_build (&(w2v->hsf), w2v->ds->fcnt, v, k);

    w2v_weight_init(w2v);

    return 0;
}


typedef struct _thread_arg{
    W2V * w2v;     // w2v struct
    int * dindex;  // doc index range
    int id;        // thread index from 0 ~ m - 1
    int *runCount; // global scan count, thread safe
    double *loss;  // loss, thread safe
} ThreadArg;


static void * thread_call_learn(void * arg){
    ThreadArg * thresD = (ThreadArg*)arg;
    W2V * w2v     = thresD->w2v;
    int * dindex  = thresD->dindex;
    int * rCnt    = thresD->runCount;
    double* tloss = thresD->loss;
    int w, n, k, t, d, id, iid, l, r, ds, de, totle;
    double *st, *sg;
    double alpha, loss, lr, prog;
    w     = w2v->wc->get_w(w2v->wc);
    n     = w2v->wc->get_n(w2v->wc);
    k     = w2v->wc->get_k(w2v->wc);
    t     = w2v->wc->get_t(w2v->wc);
    iid   = thresD->id;
    lr    = w2v->wc->get_alpha(w2v->wc);
    alpha = lr;
    totle = n * w2v->ds->t;
    st = (double *)calloc(k, sizeof(double));
    sg = (double *)calloc(k, sizeof(double));
    // scan data for train
    while(n-- > 0) for (d = dindex[iid]; d < dindex[iid + 1]; d++){
        loss = 0.0;
        ds = w2v->ds->doffs[d];
        de = w2v->ds->doffs[d + 1];
        l = ds; r = l + w;
        if (r > de) r = de;
        if (de - ds > 1) for (id = ds; id < de; id++){
            memset(st, 0, sizeof(double) * k);
            memset(sg, 0, sizeof(double) * k);
            w2v_st(w2v, st, id, k, l, r);
            if (t == 2){ // hidden layer fixed
                loss += hsoft_learn(w2v->hsf, st, sg, w2v->ds->tokens[id], 0.0);
            }
            else {
                loss += hsoft_learn(w2v->hsf, st, sg, w2v->ds->tokens[id], alpha);
            }
            w2v_ut(w2v, sg, id, k, l, r, alpha);
            if (id >= ds + (w>>1) && id < de - 1 - (w>>1)){
                l += 1;
                r += 1;
            }
        }
        *tloss += loss;
        *rCnt += de - ds;
        prog  = 1.0 - 1.0 * (*rCnt) / totle;
        alpha = lr * prog;
        if (iid == 0){
            progress(stderr, totle, *rCnt, *tloss, alpha);
        }
    }
    free(st); st = NULL;
    free(sg); sg = NULL;
    return NULL;
}

void w2v_learn(W2V * w2v){
    int i, d, l, m, rCnt = 0;
    double loss = 0.0;
    int dindex[100] = {0}; // max 99 threads
    ThreadArg args[100] = {{0}}; // thread args
    pthread_t thid[100] = {0}; // thread ids
    m = w2v->wc->get_m(w2v->wc);
    d = w2v->ds->d;
    l = d / m;
    for (i = 1; i < m + 1; i++){
        dindex[i] = l;
    }
    l = d % m;
    for (i = 1; i <= l; i++){
        dindex[i] += 1;
    }
    for (i = 1; i < m + 1; i++){
        dindex[i] += dindex[i - 1];
    }
    for (i = 0; i < m; i++){
        args[i].w2v = w2v;
        args[i].dindex = dindex;
        args[i].id = i;
        args[i].runCount = &rCnt;
        args[i].loss = &loss;
    }
    // call sub threads for train
    for (i = 0; i < m; i++){
        pthread_create(thid + i, NULL, thread_call_learn, args + i);
    }
    // wait all threads done
    for (i = 0; i < m; i++){
        pthread_join(thid[i], NULL);
    }
}

/*
void w2v_learn (W2V * w2v){
    int t, k, n, w, d, id, ds, de, l, r;
    double *st, *sg;
    double alpha, loss, tloss = 0.0, alpha_step = 0.0;

    w     = w2v->wc->get_w(w2v->wc);
    n     = w2v->wc->get_n(w2v->wc);
    k     = w2v->wc->get_k(w2v->wc);
    t     = w2v->wc->get_t(w2v->wc);
    alpha = w2v->wc->get_alpha(w2v->wc);
    alpha_step = alpha / (w2v->ds->t * n);

    st = (double *)calloc(k, sizeof(double));
    sg = (double *)calloc(k, sizeof(double));

    while (n-- > 0) {
        for (d = 0; d < w2v->ds->d; d++){
            loss = 0.0;
            ds = w2v->ds->doffs[d];
            de = w2v->ds->doffs[d + 1];
            if (de - ds > 1) {
                l = ds; r = l + w;
                if (r > de) r = de;
                for (id = ds; id < de; id ++){
                    memset(st, 0, sizeof(double) * k);
                    memset(sg, 0, sizeof(double) * k);

                    w2v_st(w2v, st, id, k, l, r);
                    if (t == 2){ // h fix
                        loss += hsoft_learn(w2v->hsf, st, sg, w2v->ds->tokens[id], 0.0);
                    }
                    else {
                        loss += hsoft_learn(w2v->hsf, st, sg, w2v->ds->tokens[id], alpha);
                    }
                    w2v_ut(w2v, sg, id, k, l, r, alpha);
                    alpha -= alpha_step;
                    if (id >= ds + (w>>1) && id < de - 1 - (w>>1)){
                        l += 1;
                        r += 1;
                    }
                }
                tloss += loss / (de - ds);
                progress(stderr, w2v->ds->d, d + 1, tloss, alpha);
            }
        }
        tloss = 0.0;
    }

    free(st); st = NULL;
    free(sg); sg = NULL;
}
*/

void w2v_save(W2V * w2v){
    char * outdir = w2v->wc->get_o(w2v->wc);
    char out[512];
    FILE * fp = NULL;
    int i, k, t;
    k = w2v->wc->get_k(w2v->wc);
    hsoft_save(w2v->hsf, outdir);

    sprintf(out, "%s/vector", outdir);
    if (NULL == (fp = fopen(out, "w"))){
        return;
    }
    for (i = 0; i < w2v->ds->v; i++){
        fprintf(fp, "%s", w2v->ds->idm[i]);
        for (t = 0; t < k; t++){
            fprintf(fp, "\t%.3f", w2v->u[i * k + t]);
        }
        fprintf(fp, "\n");
    }
    fclose(fp);
}

void w2v_free(W2V * w2v){
    if (w2v->ds){
        tsd_free(w2v->ds);
        w2v->ds = NULL;
    }
    if (w2v->hsf){
        hsoft_free(w2v->hsf);
        w2v->hsf = NULL;
    }
    if (w2v->u){
        free(w2v->u);
        w2v->u = NULL;
    }
    if (w2v->wc){
        w2v->wc->free(w2v->wc);
        w2v->wc = NULL;
    }
    free(w2v);
}

int w2v_dsize(W2V * w2v){
    return w2v->ds->d;
}

int w2v_vsize(W2V * w2v){
    return w2v->ds->v;
}

int w2v_tsize(W2V * w2v){
    return w2v->ds->t;
}
