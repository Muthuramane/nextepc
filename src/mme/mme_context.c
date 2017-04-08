#define TRACE_MODULE _mme_context

#include "core_debug.h"
#include "core_pool.h"

#include "gtp_path.h"
#include "s1ap_message.h"

#include "mme_context.h"

#define MAX_CELL_PER_ENB            8
#define MAX_ESM_PER_UE              4

#define MAX_NUM_OF_SGW              8
#define MAX_NUM_OF_ESM              (MAX_NUM_OF_UE * MAX_ESM_PER_UE)

#define S1AP_SCTP_PORT              36412

static mme_context_t self;

pool_declare(mme_sgw_pool, mme_sgw_t, MAX_NUM_OF_SGW);
pool_declare(mme_enb_pool, mme_enb_t, MAX_NUM_OF_ENB);
pool_declare(mme_ue_pool, mme_ue_t, MAX_NUM_OF_UE);
pool_declare(mme_esm_pool, mme_esm_t, MAX_NUM_OF_ESM);

static int context_initialized = 0;

status_t mme_context_init()
{
    d_assert(context_initialized == 0, return CORE_ERROR,
            "MME context already has been context_initialized");

    /* Initialize MME context */
    memset(&self, 0, sizeof(mme_context_t));

    pool_init(&mme_sgw_pool, MAX_NUM_OF_SGW);
    pool_init(&mme_enb_pool, MAX_NUM_OF_ENB);
    pool_init(&mme_ue_pool, MAX_NUM_OF_UE);
    pool_init(&mme_esm_pool, MAX_NUM_OF_ESM);

    list_init(&self.sgw_list);
    list_init(&self.enb_list);

    self.mme_ue_s1ap_id_hash = hash_make();

    self.s1ap_addr = inet_addr("127.0.0.1");
    self.s1ap_port = S1AP_SCTP_PORT;

    mme_sgw_t *sgw = mme_sgw_add();
    d_assert(sgw, return CORE_ERROR, "Can't add SGW context");

    self.s11_addr = inet_addr("127.0.0.1");
    self.s11_port = GTPV2_C_UDP_PORT;

    sgw->gnode.addr = inet_addr("127.0.0.1");
    sgw->gnode.port = GTPV2_C_UDP_PORT+1;

    /* MCC : 001, MNC : 01 */
    plmn_id_build(&self.plmn_id, 1, 1, 2); 
    self.tracking_area_code = 12345;
    self.default_paging_drx = S1ap_PagingDRX_v64;
    self.relative_capacity = 0xff;

    self.srvd_gummei.num_of_plmn_id = 1;
    /* MCC : 001, MNC : 01 */
    plmn_id_build(&self.srvd_gummei.plmn_id[0], 1, 1, 2); 

    self.srvd_gummei.num_of_mme_gid = 1;
    self.srvd_gummei.mme_gid[0] = 2;
    self.srvd_gummei.num_of_mme_code = 1;
    self.srvd_gummei.mme_code[0] = 1;

    self.selected_enc_algorithm = NAS_SECURITY_ALGORITHMS_EEA0;
    self.selected_int_algorithm = NAS_SECURITY_ALGORITHMS_128_EIA1;

    context_initialized = 1;

    return CORE_OK;
}

status_t mme_context_final()
{
    d_assert(context_initialized == 1, return CORE_ERROR,
            "HyperCell context already has been finalized");

    mme_sgw_remove_all();
    mme_enb_remove_all();

    d_assert(self.mme_ue_s1ap_id_hash, , "Null param");
    hash_destroy(self.mme_ue_s1ap_id_hash);

    pool_final(&mme_sgw_pool);
    pool_final(&mme_enb_pool);
    pool_final(&mme_ue_pool);

    context_initialized = 0;

    return CORE_OK;
}

mme_context_t* mme_self()
{
    return &self;
}

mme_sgw_t* mme_sgw_add()
{
    mme_sgw_t *sgw = NULL;

    pool_alloc_node(&mme_sgw_pool, &sgw);
    d_assert(sgw, return NULL, "Null param");

    memset(sgw, 0, sizeof(mme_sgw_t));

    list_init(&sgw->gnode.local_list);
    list_init(&sgw->gnode.remote_list);

    list_append(&self.sgw_list, sgw);
    
    return sgw;
}

status_t mme_sgw_remove(mme_sgw_t *sgw)
{
    d_assert(sgw, return CORE_ERROR, "Null param");

    list_remove(&self.sgw_list, sgw);
    pool_free_node(&mme_sgw_pool, sgw);

    return CORE_OK;
}

status_t mme_sgw_remove_all()
{
    mme_sgw_t *sgw = NULL, *next_sgw = NULL;
    
    sgw = mme_sgw_first();
    while (sgw)
    {
        next_sgw = mme_sgw_next(sgw);

        mme_sgw_remove(sgw);

        sgw = next_sgw;
    }

    return CORE_OK;
}

mme_sgw_t* mme_sgw_find_by_node(gtp_node_t *gnode)
{
    mme_sgw_t *sgw = NULL;
    
    sgw = mme_sgw_first();
    while (sgw)
    {
        if (GTP_COMPARE_NODE(&sgw->gnode, gnode))
            break;

        sgw = mme_sgw_next(sgw);
    }

    return sgw;
}

mme_sgw_t* mme_sgw_first()
{
    return list_first(&self.sgw_list);
}

mme_sgw_t* mme_sgw_next(mme_sgw_t *sgw)
{
    return list_next(sgw);
}

mme_enb_t* mme_enb_add()
{
    mme_enb_t *enb = NULL;

    pool_alloc_node(&mme_enb_pool, &enb);
    d_assert(enb, return NULL, "Null param");

    memset(enb, 0, sizeof(mme_enb_t));

    list_init(&enb->ue_list);

    list_append(&self.enb_list, enb);
    
    return enb;
}

status_t mme_enb_remove(mme_enb_t *enb)
{
    d_assert(enb, return CORE_ERROR, "Null param");

    mme_ue_remove_in_enb(enb);

    if (FSM_STATE(&enb->s1ap_sm))
    {
        fsm_final((fsm_t*)&enb->s1ap_sm, 0);
        fsm_clear((fsm_t*)&enb->s1ap_sm);
    }
    else
        d_assert(0,, "Null param");

    net_unregister_sock(enb->s1ap_sock);
    net_close(enb->s1ap_sock);

    list_remove(&self.enb_list, enb);
    pool_free_node(&mme_enb_pool, enb);

    return CORE_OK;
}

status_t mme_enb_remove_all()
{
    mme_enb_t *enb = NULL, *next_enb = NULL;
    
    enb = mme_enb_first();
    while (enb)
    {
        next_enb = mme_enb_next(enb);

        mme_enb_remove(enb);

        enb = next_enb;
    }

    return CORE_OK;
}

mme_enb_t* mme_enb_find_by_sock(net_sock_t *sock)
{
    mme_enb_t *enb = NULL;
    
    enb = mme_enb_first();
    while (enb)
    {
        if (sock == enb->s1ap_sock)
            break;

        enb = mme_enb_next(enb);
    }

    return enb;
}

mme_enb_t* mme_enb_find_by_enb_id(c_uint32_t enb_id)
{
    mme_enb_t *enb = NULL;
    
    enb = list_first(&self.enb_list);
    while (enb)
    {
        if (enb_id == enb->enb_id)
            break;

        enb = list_next(enb);
    }

    return enb;
}

mme_enb_t* mme_enb_first()
{
    return list_first(&self.enb_list);
}

mme_enb_t* mme_enb_next(mme_enb_t *enb)
{
    return list_next(enb);
}

mme_ue_t* mme_ue_add(mme_enb_t *enb)
{
    mme_ue_t *ue = NULL;

    d_assert(self.mme_ue_s1ap_id_hash, return NULL, "Null param");
    d_assert(enb, return NULL, "Null param");

    pool_alloc_node(&mme_ue_pool, &ue);
    d_assert(ue, return NULL, "Null param");

    memset(ue, 0, sizeof(mme_ue_t));

    ue->enb = enb;

    ue->mme_ue_s1ap_id = NEXT_ID(self.mme_ue_s1ap_id, 0xffffffff);
    hash_set(self.mme_ue_s1ap_id_hash, &ue->mme_ue_s1ap_id, 
            sizeof(ue->mme_ue_s1ap_id), ue);

    list_init(&ue->esm_list);

    list_append(&enb->ue_list, ue);
    
    return ue;
}

status_t mme_ue_remove(mme_ue_t *ue)
{
    d_assert(self.mme_ue_s1ap_id_hash, return CORE_ERROR, "Null param");
    d_assert(ue, return CORE_ERROR, "Null param");
    d_assert(ue->enb, return CORE_ERROR, "Null param");

    if (FSM_STATE(&ue->emm_sm))
    {
        fsm_final((fsm_t*)&ue->emm_sm, 0);
        fsm_clear((fsm_t*)&ue->emm_sm);
    }

    list_remove(&ue->enb->ue_list, ue);
    hash_set(self.mme_ue_s1ap_id_hash, &ue->mme_ue_s1ap_id, 
            sizeof(ue->mme_ue_s1ap_id), NULL);

    mme_esm_remove_all(ue);

    pool_free_node(&mme_ue_pool, ue);

    return CORE_OK;
}

status_t mme_ue_remove_all()
{
    hash_index_t *hi = NULL;
    mme_ue_t *ue = NULL;

    for (hi = mme_ue_first(); hi; hi = mme_ue_next(hi))
    {
        ue = mme_ue_this(hi);
        mme_ue_remove(ue);
    }

    return CORE_OK;
}

mme_ue_t* mme_ue_find_by_mme_ue_s1ap_id(c_uint32_t mme_ue_s1ap_id)
{
    d_assert(self.mme_ue_s1ap_id_hash, return NULL, "Null param");
    return hash_get(self.mme_ue_s1ap_id_hash, 
            &mme_ue_s1ap_id, sizeof(mme_ue_s1ap_id));
}

hash_index_t *mme_ue_first()
{
    d_assert(self.mme_ue_s1ap_id_hash, return NULL, "Null param");
    return hash_first(self.mme_ue_s1ap_id_hash);
}

hash_index_t *mme_ue_next(hash_index_t *hi)
{
    return hash_next(hi);
}

mme_ue_t *mme_ue_this(hash_index_t *hi)
{
    d_assert(hi, return NULL, "Null param");
    return hash_this_val(hi);
}

unsigned int mme_ue_count()
{
    d_assert(self.mme_ue_s1ap_id_hash, return 0, "Null param");
    return hash_count(self.mme_ue_s1ap_id_hash);
}

status_t mme_ue_remove_in_enb(mme_enb_t *enb)
{
    mme_ue_t *ue = NULL, *next_ue = NULL;
    
    ue = mme_ue_first_in_enb(enb);
    while (ue)
    {
        next_ue = mme_ue_next_in_enb(ue);

        mme_ue_remove(ue);

        ue = next_ue;
    }

    return CORE_OK;
}

mme_ue_t* mme_ue_find_by_enb_ue_s1ap_id(
        mme_enb_t *enb, c_uint32_t enb_ue_s1ap_id)
{
    mme_ue_t *ue = NULL;
    
    ue = mme_ue_first_in_enb(enb);
    while (ue)
    {
        if (enb_ue_s1ap_id == ue->enb_ue_s1ap_id)
            break;

        ue = mme_ue_next_in_enb(ue);
    }

    return ue;
}

mme_ue_t* mme_ue_first_in_enb(mme_enb_t *enb)
{
    return list_first(&enb->ue_list);
}

mme_ue_t* mme_ue_next_in_enb(mme_ue_t *ue)
{
    return list_next(ue);
}

mme_esm_t* mme_esm_add(mme_ue_t *ue)
{
    mme_esm_t *esm = NULL;

    d_assert(ue, return NULL, "Null param");

    pool_alloc_node(&mme_esm_pool, &esm);
    d_assert(esm, return NULL, "Null param");

    memset(esm, 0, sizeof(mme_esm_t));

    esm->ue = ue;

    list_append(&ue->esm_list, esm);
    
    return esm;
}

status_t mme_esm_remove(mme_esm_t *esm)
{
    d_assert(esm, return CORE_ERROR, "Null param");
    d_assert(esm->ue, return CORE_ERROR, "Null param");
    
    if (FSM_STATE(&esm->sm))
    {
        fsm_final((fsm_t*)&esm->sm, 0);
        fsm_clear((fsm_t*)&esm->sm);
    }
    else
        d_assert(0, , "Null param");

    list_remove(&esm->ue->esm_list, esm);
    pool_free_node(&mme_esm_pool, esm);

    return CORE_OK;
}

status_t mme_esm_remove_all(mme_ue_t *ue)
{
    mme_esm_t *esm = NULL, *next_esm = NULL;

    d_assert(ue, return CORE_ERROR, "Null param");
    
    esm = mme_esm_first(ue);
    while (esm)
    {
        next_esm = mme_esm_next(esm);

        mme_esm_remove(esm);

        esm = next_esm;
    }

    return CORE_OK;
}

mme_esm_t* mme_esm_find_by_pti(mme_ue_t *ue, c_uint8_t pti)
{
    mme_esm_t *esm = NULL;

    d_assert(ue, return NULL, "Null param");
    
    esm = mme_esm_first(ue);
    while (esm)
    {
        if (pti == esm->pti)
            break;

        esm = mme_esm_next(esm);
    }

    return esm;
}

mme_esm_t* mme_esm_first(mme_ue_t *ue)
{
    d_assert(ue, return NULL, "Null param");

    return list_first(&ue->esm_list);
}

mme_esm_t* mme_esm_next(mme_esm_t *esm)
{
    return list_next(esm);
}
