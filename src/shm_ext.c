/**
 * @file shm_ext.c
 * @author Michal Vasko <mvasko@cesnet.cz>
 * @brief ext SHM routines
 *
 * @copyright
 * Copyright 2018 Deutsche Telekom AG.
 * Copyright 2018 - 2021 CESNET, z.s.p.o.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "common.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <libyang/libyang.h>

sr_error_info_t *
sr_shmext_conn_remap_lock(sr_conn_ctx_t *conn, sr_lock_mode_t mode, int ext_lock, const char *func)
{
    sr_error_info_t *err_info = NULL;
    size_t shm_file_size;

    /* EXT LOCK */
    if (ext_lock && (err_info = sr_mlock(&SR_CONN_MAIN_SHM(conn)->ext_lock, SR_EXT_LOCK_TIMEOUT, func))) {
        return err_info;
    }

    /* REMAP LOCK */
    if ((err_info = sr_rwlock(&conn->ext_remap_lock, SR_CONN_REMAP_LOCK_TIMEOUT, mode, conn->cid, func, NULL, NULL))) {
        goto error_ext_unlock;
    }

    /* remap ext SHM */
    if (mode == SR_LOCK_WRITE) {
        /* we have WRITE lock, it is safe */
        if ((err_info = sr_shm_remap(&conn->ext_shm, 0))) {
            goto error_ext_remap_unlock;
        }
    } else {
        if ((err_info = sr_file_get_size(conn->ext_shm.fd, &shm_file_size))) {
            goto error_ext_remap_unlock;
        }
        if (shm_file_size > conn->ext_shm.size) {
            /* ext SHM is larger now and we need to remap it */

            if (mode == SR_LOCK_READ_UPGR) {
                /* REMAP WRITE LOCK UPGRADE */
                if ((err_info = sr_rwrelock(&conn->ext_remap_lock, SR_CONN_REMAP_LOCK_TIMEOUT, SR_LOCK_WRITE, conn->cid,
                        func, NULL, NULL))) {
                    goto error_ext_remap_unlock;
                }
            } else {
                /* REMAP READ UNLOCK */
                sr_rwunlock(&conn->ext_remap_lock, SR_CONN_REMAP_LOCK_TIMEOUT, SR_LOCK_READ, conn->cid, func);
                /* REMAP WRITE LOCK */
                if ((err_info = sr_rwlock(&conn->ext_remap_lock, SR_CONN_REMAP_LOCK_TIMEOUT, SR_LOCK_WRITE, conn->cid,
                        func, NULL, NULL))) {
                    goto error_ext_unlock;
                }
            }

            if ((err_info = sr_shm_remap(&conn->ext_shm, shm_file_size))) {
                mode = SR_LOCK_WRITE;
                goto error_ext_remap_unlock;
            }

            if (mode == SR_LOCK_READ_UPGR) {
                /* REMAP READ UPGR LOCK DOWNGRADE */
                if ((err_info = sr_rwrelock(&conn->ext_remap_lock, SR_CONN_REMAP_LOCK_TIMEOUT, SR_LOCK_READ_UPGR,
                        conn->cid, func, NULL, NULL))) {
                    mode = SR_LOCK_WRITE;
                    goto error_ext_remap_unlock;
                }
            } else {
                /* REMAP WRITE UNLOCK */
                sr_rwunlock(&conn->ext_remap_lock, SR_CONN_REMAP_LOCK_TIMEOUT, SR_LOCK_WRITE, conn->cid, func);
                /* REMAP READ LOCK */
                if ((err_info = sr_rwlock(&conn->ext_remap_lock, SR_CONN_REMAP_LOCK_TIMEOUT, SR_LOCK_READ, conn->cid,
                        func, NULL, NULL))) {
                    goto error_ext_unlock;
                }
            }
        } /* else no remapping needed */
    }

    return NULL;

error_ext_remap_unlock:
    /* REMAP UNLOCK */
    sr_rwunlock(&conn->ext_remap_lock, SR_CONN_REMAP_LOCK_TIMEOUT, mode, conn->cid, func);

error_ext_unlock:
    /* EXT UNLOCK */
    if (ext_lock) {
        sr_munlock(&SR_CONN_MAIN_SHM(conn)->ext_lock);
    }
    return err_info;
}

void
sr_shmext_conn_remap_unlock(sr_conn_ctx_t *conn, sr_lock_mode_t mode, int ext_lock, const char *func)
{
    /* REMAP UNLOCK */
    sr_rwunlock(&conn->ext_remap_lock, SR_EXT_LOCK_TIMEOUT, mode, conn->cid, func);

    /* EXT UNLOCK */
    if (ext_lock) {
        sr_munlock(&SR_CONN_MAIN_SHM(conn)->ext_lock);
    }
}

/**
 * @brief Item holding information about a SHM object for debug printing.
 */
struct shm_item {
    off_t start;
    size_t size;
    char *name;
};

/**
 * @brief Comparator for SHM print item qsort.
 *
 * @param[in] ptr1 First value pointer.
 * @param[in] ptr2 Second value pointer.
 * @return Less than, equal to, or greater than 0 if the first value is found
 * to be less than, equal to, or greater to the second value.
 */
static int
sr_shmext_print_cmp(const void *ptr1, const void *ptr2)
{
    struct shm_item *item1, *item2;

    item1 = (struct shm_item *)ptr1;
    item2 = (struct shm_item *)ptr2;

    assert(item1->start != item2->start);
    assert((item1->start > item2->start) || (item1->start + item1->size <= (unsigned)item2->start));
    assert((item1->start < item2->start) || (item2->start + item2->size <= (unsigned)item1->start));

    if (item1->start < item2->start) {
        return -1;
    }
    return 1;
}

/**
 * @brief Debug print the contents of ext SHM.
 *
 * @param[in] main_shm Main SHM.
 * @param[in] ext_shm_addr Ext SHM mapping address.
 * @param[in] ext_shm_size Ext SHM mapping size.
 */
static void
sr_shmext_print(sr_main_shm_t *main_shm, char *ext_shm_addr, size_t ext_shm_size)
{
    sr_mod_t *shm_mod;
    off_t cur_off;
    sr_mod_change_sub_t *change_subs;
    sr_mod_oper_sub_t *oper_subs;
    sr_rpc_t *shm_rpc;
    sr_mod_rpc_sub_t *rpc_subs;
    struct shm_item *items;
    size_t idx, i, j, item_count, printed, wasted;
    sr_datastore_t ds;
    int msg_len = 0;
    char *msg;

    if ((stderr_ll < SR_LL_DBG) && (syslog_ll < SR_LL_DBG) && !log_cb) {
        /* nothing to print */
        return;
    }

    /* add wasted */
    item_count = 0;
    items = malloc(sizeof *items);
    items[item_count].start = 0;
    items[item_count].size = sizeof(sr_ext_shm_t);
    asprintf(&(items[item_count].name), "ext wasted %lu", ATOMIC_LOAD_RELAXED(((sr_ext_shm_t *)ext_shm_addr)->wasted));
    ++item_count;

    for (idx = 0; idx < main_shm->mod_count; ++idx) {
        shm_mod = SR_SHM_MOD_IDX(main_shm, idx);

        for (ds = 0; ds < SR_DS_COUNT; ++ds) {
            if (shm_mod->change_sub[ds].sub_count) {
                /* add change subscriptions */
                items = sr_realloc(items, (item_count + 1) * sizeof *items);
                items[item_count].start = shm_mod->change_sub[ds].subs;
                items[item_count].size = SR_SHM_SIZE(shm_mod->change_sub[ds].sub_count * sizeof *change_subs);
                asprintf(&(items[item_count].name), "%s change subs (%u, mod \"%s\")", sr_ds2str(ds),
                        shm_mod->change_sub[ds].sub_count, ((char *)main_shm) + shm_mod->name);
                ++item_count;

                /* add xpaths */
                change_subs = (sr_mod_change_sub_t *)(ext_shm_addr + shm_mod->change_sub[ds].subs);
                for (i = 0; i < shm_mod->change_sub[ds].sub_count; ++i) {
                    if (change_subs[i].xpath) {
                        items = sr_realloc(items, (item_count + 1) * sizeof *items);
                        items[item_count].start = change_subs[i].xpath;
                        items[item_count].size = sr_strshmlen(ext_shm_addr + change_subs[i].xpath);
                        asprintf(&(items[item_count].name), "%s change sub xpath (\"%s\", mod \"%s\")", sr_ds2str(ds),
                                ext_shm_addr + change_subs[i].xpath, ((char *)main_shm) + shm_mod->name);
                        ++item_count;
                    }
                }
            }
        }

        if (shm_mod->oper_sub_count) {
            /* add oper subscriptions */
            items = sr_realloc(items, (item_count + 1) * sizeof *items);
            items[item_count].start = shm_mod->oper_subs;
            items[item_count].size = SR_SHM_SIZE(shm_mod->oper_sub_count * sizeof *oper_subs);
            asprintf(&(items[item_count].name), "oper subs (%u, mod \"%s\")", shm_mod->oper_sub_count,
                    ((char *)main_shm) + shm_mod->name);
            ++item_count;

            /* add xpaths */
            oper_subs = (sr_mod_oper_sub_t *)(ext_shm_addr + shm_mod->oper_subs);
            for (i = 0; i < shm_mod->oper_sub_count; ++i) {
                items = sr_realloc(items, (item_count + 1) * sizeof *items);
                items[item_count].start = oper_subs[i].xpath;
                items[item_count].size = sr_strshmlen(ext_shm_addr + oper_subs[i].xpath);
                asprintf(&(items[item_count].name), "oper sub xpath (\"%s\", mod \"%s\")",
                        ext_shm_addr + oper_subs[i].xpath, ((char *)main_shm) + shm_mod->name);
                ++item_count;
            }
        }

        shm_rpc = (sr_rpc_t *)(((char *)main_shm) + shm_mod->rpcs);
        for (i = 0; i < shm_mod->rpc_count; ++i) {
            if (shm_rpc[i].sub_count) {
                /* add RPC subscriptions */
                items = sr_realloc(items, (item_count + 1) * sizeof *items);
                items[item_count].start = shm_rpc[i].subs;
                items[item_count].size = SR_SHM_SIZE(shm_rpc[i].sub_count * sizeof *rpc_subs);
                asprintf(&(items[item_count].name), "rpc subs (%u, path \"%s\")", shm_rpc[i].sub_count,
                        ((char *)main_shm) + shm_rpc[i].path);
                ++item_count;

                rpc_subs = (sr_mod_rpc_sub_t *)(ext_shm_addr + shm_rpc[i].subs);
                for (j = 0; j < shm_rpc[i].sub_count; ++j) {
                    /* add RPC subscription XPath */
                    items = sr_realloc(items, (item_count + 1) * sizeof *items);
                    items[item_count].start = rpc_subs[j].xpath;
                    items[item_count].size = sr_strshmlen(ext_shm_addr + rpc_subs[j].xpath);
                    asprintf(&(items[item_count].name), "rpc sub xpath (\"%s\", path \"%s\")",
                            ext_shm_addr + rpc_subs[j].xpath, ((char *)main_shm) + shm_rpc[i].path);
                    ++item_count;
                }
            }
        }

        if (shm_mod->notif_sub_count) {
            /* add notif subscriptions */
            items = sr_realloc(items, (item_count + 1) * sizeof *items);
            items[item_count].start = shm_mod->notif_subs;
            items[item_count].size = SR_SHM_SIZE(shm_mod->notif_sub_count * sizeof(sr_mod_notif_sub_t));
            asprintf(&(items[item_count].name), "notif subs (%u, mod \"%s\")", shm_mod->notif_sub_count,
                    ((char *)main_shm) + shm_mod->name);
            ++item_count;
        }
    }

    /* sort all items */
    qsort(items, item_count, sizeof *items, sr_shmext_print_cmp);

    /* print it */
    cur_off = 0;
    printed = 0;
    wasted = 0;
    for (i = 0; i < item_count; ++i) {
        /* check alignment */
        assert(items[i].size == SR_SHM_SIZE(items[i].size));
        assert((unsigned)items[i].start == SR_SHM_SIZE(items[i].start));

        if (items[i].start > cur_off) {
            printed += sr_sprintf(&msg, &msg_len, printed, "%06ld-%06ld [%6ld]: (wasted %ld)\n",
                    cur_off, items[i].start, items[i].start - cur_off, items[i].start - cur_off);
            wasted += items[i].start - cur_off;
            cur_off = items[i].start;
        }
        printed += sr_sprintf(&msg, &msg_len, printed, "%06ld-%06ld [%6lu]: %s\n",
                items[i].start, items[i].start + items[i].size, items[i].size, items[i].name);
        cur_off += items[i].size;

        free(items[i].name);
    }

    if ((unsigned)cur_off < ext_shm_size) {
        printed += sr_sprintf(&msg, &msg_len, printed, "%06ld-%06ld [%6lu]: (wasted %ld)\n",
                cur_off, ext_shm_size, ext_shm_size - cur_off, ext_shm_size - cur_off);
        wasted += ext_shm_size - cur_off;
    }
    free(items);

    /* print all the information about SHM */
    SR_LOG_DBG("#SHM:\n%s", msg);
    free(msg);

    /* check that no item exists after the mapped segment */
    assert((unsigned)cur_off <= ext_shm_size);
    /* check that wasted memory is correct */
    assert(ATOMIC_LOAD_RELAXED(((sr_ext_shm_t *)ext_shm_addr)->wasted) == wasted);
}

/**
 * @brief Copy an array from ext SHM to buffer to defragment it.
 *
 * @param[in] ext_shm_addr Ext SHM mapping address.
 * @param[in] array SHM offset of the array.
 * @param[in] size Array item size.
 * @param[in] count Array item count.
 * @param[in] ext_buf SHM ext buffer.
 * @param[in,out] ext_buf_cur Current SHM ext buffer position.
 * @return Buffer offset of the copy.
 */
static off_t
sr_shmext_defrag_copy_array_with_string(char *ext_shm_addr, off_t array, size_t size, uint16_t count, char *ext_buf,
        char **ext_buf_cur)
{
    off_t ret, *item;
    uint16_t i;

    if (!array && !count) {
        /* empty array */
        return 0;
    }
    assert(array && count);

    /* current offset */
    ret = *ext_buf_cur - ext_buf;

    /* copy whole array */
    item = (off_t *)(ext_buf + sr_shmcpy(ext_buf, ext_shm_addr + array, count * size, ext_buf_cur));

    /* copy string for each item */
    for (i = 0; i < count; ++i) {
        if (*item) {
            *item = sr_shmcpy(ext_buf, ext_shm_addr + *item, sr_strshmlen(ext_shm_addr + *item), ext_buf_cur);
        }

        /* next item */
        item = (off_t *)(((uintptr_t)item) + size);
    }

    return ret;
}

/**
 * @brief Defragment Ext SHM.
 *
 * @param[in] conn Connection to use.
 * @param[out] defrag_ext_buf Defragmented Ext SHM memory copy.
 * @return err_info, NULL on success.
 */
static sr_error_info_t *
sr_shmext_defrag(sr_conn_ctx_t *conn, char **defrag_ext_buf)
{
    sr_error_info_t *err_info = NULL;
    sr_mod_t *shm_mod;
    sr_rpc_t *shm_rpc;
    char *ext_buf, *ext_buf_cur;
    sr_main_shm_t *main_shm;
    sr_mod_notif_sub_t *notif_subs;
    sr_datastore_t ds;
    uint32_t i, rpc_i;

    *defrag_ext_buf = NULL;

    main_shm = SR_CONN_MAIN_SHM(conn);

    /* resulting defragmented size is known */
    ext_buf_cur = ext_buf = malloc(conn->ext_shm.size - ATOMIC_LOAD_RELAXED(SR_CONN_EXT_SHM(conn)->wasted));
    SR_CHECK_MEM_RET(!ext_buf, err_info);

    /* wasted ext number */
    ((sr_ext_shm_t *)ext_buf_cur)->wasted = 0;
    ext_buf_cur += sizeof(sr_ext_shm_t);

    for (i = 0; i < main_shm->mod_count; ++i) {
        shm_mod = SR_SHM_MOD_IDX(main_shm, i);

        /* copy change subscriptions */
        for (ds = 0; ds < SR_DS_COUNT; ++ds) {
            shm_mod->change_sub[ds].subs = sr_shmext_defrag_copy_array_with_string(conn->ext_shm.addr,
                    shm_mod->change_sub[ds].subs, sizeof(sr_mod_change_sub_t), shm_mod->change_sub[ds].sub_count,
                    ext_buf, &ext_buf_cur);
        }

        /* copy operational subscriptions */
        shm_mod->oper_subs = sr_shmext_defrag_copy_array_with_string(conn->ext_shm.addr, shm_mod->oper_subs,
                sizeof(sr_mod_oper_sub_t), shm_mod->oper_sub_count, ext_buf, &ext_buf_cur);

        /* copy RPC subscriptions */
        shm_rpc = (sr_rpc_t *)(conn->main_shm.addr + shm_mod->rpcs);
        for (rpc_i = 0; rpc_i < shm_mod->rpc_count; ++rpc_i) {
            shm_rpc[rpc_i].subs = sr_shmext_defrag_copy_array_with_string(conn->ext_shm.addr, shm_rpc[rpc_i].subs,
                    sizeof(sr_mod_rpc_sub_t), shm_rpc[rpc_i].sub_count, ext_buf, &ext_buf_cur);
        }

        /* copy notification subscriptions */
        notif_subs = (sr_mod_notif_sub_t *)(conn->ext_shm.addr + shm_mod->notif_subs);
        shm_mod->notif_subs = sr_shmcpy(ext_buf, notif_subs, SR_SHM_SIZE(shm_mod->notif_sub_count * sizeof *notif_subs),
                &ext_buf_cur);
    }

    /* check size */
    if ((unsigned)(ext_buf_cur - ext_buf) != conn->ext_shm.size - ATOMIC_LOAD_RELAXED(SR_CONN_EXT_SHM(conn)->wasted)) {
        SR_ERRINFO_INT(&err_info);
        free(ext_buf);
        return err_info;
    }

    *defrag_ext_buf = ext_buf;
    return NULL;
}

/**
 * @brief Lock all SHM module locks before ext SHM defragmentation.
 *
 * @param[in] conn Connection to use.
 * @return err_info, NULL on success.
 */
static sr_error_info_t *
sr_shmext_defrag_modules_lock(sr_conn_ctx_t *conn)
{
    sr_error_info_t *err_info = NULL;
    sr_main_shm_t *main_shm = SR_CONN_MAIN_SHM(conn);
    sr_mod_t *shm_mod;
    sr_rpc_t *shm_rpc;
    uint32_t idx, rpc_i, i;
    sr_datastore_t ds;

    for (idx = 0; idx < main_shm->mod_count; ++idx) {
        shm_mod = SR_SHM_MOD_IDX(main_shm, idx);
        for (ds = 0; ds < SR_DS_COUNT; ++ds) {
            /* CHANGE SUB WRITE LOCK */
            if ((err_info = sr_rwlock(&shm_mod->change_sub[ds].lock, SR_SUBS_LOCK_TIMEOUT, SR_LOCK_WRITE, conn->cid,
                    __func__, NULL, NULL))) {
                goto error_changesub_unlock;
            }
        }

        shm_rpc = (sr_rpc_t *)(conn->main_shm.addr + shm_mod->rpcs);
        for (rpc_i = 0; rpc_i < shm_mod->rpc_count; ++rpc_i) {
            /* RPC SUB WRITE LOCK */
            if ((err_info = sr_rwlock(&shm_rpc[rpc_i].lock, SR_SUBS_LOCK_TIMEOUT, SR_LOCK_WRITE, conn->cid, __func__,
                    NULL, NULL))) {
                goto error_changesub_rpcsub_unlock;
            }
        }

        /* OPER SUB WRITE LOCK */
        if ((err_info = sr_rwlock(&shm_mod->oper_lock, SR_SUBS_LOCK_TIMEOUT, SR_LOCK_WRITE, conn->cid, __func__, NULL,
                NULL))) {
            goto error_changesub_rpcsub_unlock;
        }

        /* NOTIF SUB WRITE LOCK */
        if ((err_info = sr_rwlock(&shm_mod->notif_lock, SR_SUBS_LOCK_TIMEOUT, SR_LOCK_WRITE, conn->cid, __func__, NULL,
                NULL))) {
            goto error_changesub_rpcsub_opersub_unlock;
        }
    }

    return NULL;

error_changesub_rpcsub_opersub_unlock:
    /* OPER SUB WRITE UNLOCK */
    sr_rwunlock(&shm_mod->oper_lock, 0, SR_LOCK_WRITE, conn->cid, __func__);

error_changesub_rpcsub_unlock:
    for (i = 0; i < rpc_i; ++i) {
        /* RPC SUB WRITE UNLOCK */
        sr_rwunlock(&shm_rpc[i].lock, 0, SR_LOCK_WRITE, conn->cid, __func__);
    }

error_changesub_unlock:
    for (i = 0; i < ds; ++i) {
        /* CHANGE SUB WRITE UNLOCK */
        sr_rwunlock(&shm_mod->change_sub[ds].lock, 0, SR_LOCK_WRITE, conn->cid, __func__);
    }

    for (i = 0; i < idx; ++i) {
        shm_mod = SR_SHM_MOD_IDX(main_shm, i);
        for (ds = 0; ds < SR_DS_COUNT; ++ds) {
            /* CHANGE SUB WRITE UNLOCK */
            sr_rwunlock(&shm_mod->change_sub[ds].lock, 0, SR_LOCK_WRITE, conn->cid, __func__);
        }

        /* OPER SUB WRITE UNLOCK */
        sr_rwunlock(&shm_mod->oper_lock, 0, SR_LOCK_WRITE, conn->cid, __func__);

        /* NOTIF SUB WRITE UNLOCK */
        sr_rwunlock(&shm_mod->notif_lock, 0, SR_LOCK_WRITE, conn->cid, __func__);
    }

    return err_info;
}

/**
 * @brief Unlock all SHM module locks after ext SHM defragmentation.
 *
 * @param[in] conn Connection to use.
 */
static void
sr_shmext_defrag_modules_unlock(sr_conn_ctx_t *conn)
{
    sr_main_shm_t *main_shm = SR_CONN_MAIN_SHM(conn);
    sr_mod_t *shm_mod;
    sr_rpc_t *shm_rpc;
    uint32_t idx, rpc_i;
    sr_datastore_t ds;

    for (idx = 0; idx < main_shm->mod_count; ++idx) {
        shm_mod = SR_SHM_MOD_IDX(main_shm, idx);
        for (ds = 0; ds < SR_DS_COUNT; ++ds) {
            /* CHANGE SUB WRITE UNLOCK */
            sr_rwunlock(&shm_mod->change_sub[ds].lock, 0, SR_LOCK_WRITE, conn->cid, __func__);
        }

        shm_rpc = (sr_rpc_t *)(conn->main_shm.addr + shm_mod->rpcs);
        for (rpc_i = 0; rpc_i < shm_mod->rpc_count; ++rpc_i) {
            /* RPC SUB WRITE UNLOCK */
            sr_rwunlock(&shm_rpc[rpc_i].lock, 0, SR_LOCK_WRITE, conn->cid, __func__);
        }

        /* OPER SUB WRITE UNLOCK */
        sr_rwunlock(&shm_mod->oper_lock, 0, SR_LOCK_WRITE, conn->cid, __func__);

        /* NOTIF SUB WRITE UNLOCK */
        sr_rwunlock(&shm_mod->notif_lock, 0, SR_LOCK_WRITE, conn->cid, __func__);
    }
}

/**
 * @brief Check whether ext SHM wasted memory did not reach the treshold and if so, defragment it.
 *
 * @param[in] conn Connection to use.
 */
static void
sr_shmext_defrag_check(sr_conn_ctx_t *conn)
{
    sr_error_info_t *err_info = NULL;
    char *buf;
    int defrag = 0;

    /* EXT READ LOCK */
    if ((err_info = sr_shmext_conn_remap_lock(conn, SR_LOCK_READ, 0, __func__))) {
        sr_errinfo_free(&err_info);
        return;
    }

    /* check whether ext SHM should be defragmented */
    if (ATOMIC_LOAD_RELAXED(SR_CONN_EXT_SHM(conn)->wasted) > SR_SHM_WASTED_MAX_MEM) {
        defrag = 1;
    }

    /* EXT READ UNLOCK */
    sr_shmext_conn_remap_unlock(conn, SR_LOCK_READ, 0, __func__);

    /* defragment ext SHM */
    if (defrag) {
        /* LOCK all SHM module locks in the right order */
        if ((err_info = sr_shmext_defrag_modules_lock(conn))) {
            sr_errinfo_free(&err_info);
            return;
        }

        /* EXT WRITE LOCK */
        if ((err_info = sr_shmext_conn_remap_lock(conn, SR_LOCK_WRITE, 1, __func__))) {
            sr_errinfo_free(&err_info);
            goto unlock_modules;
        }

        /* check wasted memory again, it could have been defragmented by now */
        if (ATOMIC_LOAD_RELAXED(SR_CONN_EXT_SHM(conn)->wasted) > SR_SHM_WASTED_MAX_MEM) {
            SR_LOG_DBG("#SHM before defrag");
            sr_shmext_print(SR_CONN_MAIN_SHM(conn), conn->ext_shm.addr, conn->ext_shm.size);

            /* defrag mem into a separate memory */
            if (!(err_info = sr_shmext_defrag(conn, &buf))) {
                /* remap ext SHM, it does not matter if it fails, will just be kept larger than needed */
                err_info = sr_shm_remap(&conn->ext_shm, conn->ext_shm.size -
                        ATOMIC_LOAD_RELAXED(SR_CONN_EXT_SHM(conn)->wasted));

                SR_LOG_INF("Ext SHM was defragmented and %u B were saved.",
                        ATOMIC_LOAD_RELAXED(SR_CONN_EXT_SHM(conn)->wasted));

                /* copy the defragmented memory into ext SHM (has wasted set to 0) */
                memcpy(conn->ext_shm.addr, buf, conn->ext_shm.size);
                free(buf);

                SR_LOG_DBG("#SHM after defrag");
                sr_shmext_print(SR_CONN_MAIN_SHM(conn), conn->ext_shm.addr, conn->ext_shm.size);
            }
            sr_errinfo_free(&err_info);
        }

        /* EXT WRITE UNLOCK */
        sr_shmext_conn_remap_unlock(conn, SR_LOCK_WRITE, 1, __func__);

unlock_modules:
        /* UNLOCK SHM modules */
        sr_shmext_defrag_modules_unlock(conn);
    }
}

sr_error_info_t *
sr_shmext_change_subscription_add(sr_conn_ctx_t *conn, sr_mod_t *shm_mod, const char *xpath, sr_datastore_t ds,
        uint32_t priority, int sub_opts, uint32_t evpipe_num)
{
    sr_error_info_t *err_info = NULL;
    off_t xpath_off;
    sr_mod_change_sub_t *shm_sub;
    uint16_t i;

    /* CHANGE SUB WRITE LOCK */
    if ((err_info = sr_rwlock(&shm_mod->change_sub[ds].lock, SR_SUBS_LOCK_TIMEOUT, SR_LOCK_WRITE, conn->cid, __func__,
            NULL, NULL))) {
        return err_info;
    }

    /* EXT WRITE LOCK */
    if ((err_info = sr_shmext_conn_remap_lock(conn, SR_LOCK_WRITE, 1, __func__))) {
        goto cleanup_changesub_unlock;
    }

    if (sub_opts & SR_SUBSCR_UPDATE) {
        /* check that there is not already an update subscription with the same priority */
        shm_sub = (sr_mod_change_sub_t *)(conn->ext_shm.addr + shm_mod->change_sub[ds].subs);
        for (i = 0; i < shm_mod->change_sub[ds].sub_count; ++i) {
            if ((shm_sub[i].opts & SR_SUBSCR_UPDATE) && (shm_sub[i].priority == priority)) {
                sr_errinfo_new(&err_info, SR_ERR_INVAL_ARG, NULL,
                        "There already is an \"update\" subscription on module \"%s\" with priority %u for %s DS.",
                        conn->main_shm.addr + shm_mod->name, priority, sr_ds2str(ds));
                goto cleanup_changesub_ext_unlock;
            }
        }
    }

    /* allocate new subscription and its xpath, if any */
    if ((err_info = sr_shmrealloc_add(&conn->ext_shm, &shm_mod->change_sub[ds].subs, &shm_mod->change_sub[ds].sub_count,
            0, sizeof *shm_sub, -1, (void **)&shm_sub, xpath ? sr_strshmlen(xpath) : 0, &xpath_off))) {
        goto cleanup_changesub_ext_unlock;
    }

    /* fill new subscription */
    if (xpath) {
        strcpy(conn->ext_shm.addr + xpath_off, xpath);
        shm_sub->xpath = xpath_off;
    } else {
        shm_sub->xpath = 0;
    }
    shm_sub->priority = priority;
    shm_sub->opts = sub_opts;
    shm_sub->evpipe_num = evpipe_num;
    shm_sub->cid = conn->cid;

    /* EXT WRITE UNLOCK */
    sr_shmext_conn_remap_unlock(conn, SR_LOCK_WRITE, 1, __func__);

    /* CHANGE SUB WRITE UNLOCK */
    sr_rwunlock(&shm_mod->change_sub[ds].lock, SR_SUBS_LOCK_TIMEOUT, SR_LOCK_WRITE, conn->cid, __func__);

    /* ext SHM defrag */
    sr_shmext_defrag_check(conn);

    if (ds == SR_DS_RUNNING) {
        /* technically, operational data may have changed */
        if ((err_info = sr_module_update_oper_diff(conn, conn->main_shm.addr + shm_mod->name))) {
            return err_info;
        }
    }

    return NULL;

cleanup_changesub_ext_unlock:
    /* EXT WRITE UNLOCK */
    sr_shmext_conn_remap_unlock(conn, SR_LOCK_WRITE, 1, __func__);

cleanup_changesub_unlock:
    /* CHANGE SUB WRITE UNLOCK */
    sr_rwunlock(&shm_mod->change_sub[ds].lock, SR_SUBS_LOCK_TIMEOUT, SR_LOCK_WRITE, conn->cid, __func__);

    return err_info;
}

sr_error_info_t *
sr_shmext_change_subscription_del(sr_conn_ctx_t *conn, sr_mod_t *shm_mod, sr_datastore_t ds, const char *xpath,
        uint32_t priority, int sub_opts, uint32_t evpipe_num, sr_cid_t cid, int *last_removed, uint32_t *evpipe_num_p,
        int *found)
{
    sr_error_info_t *err_info = NULL;
    sr_mod_change_sub_t *shm_sub;
    uint16_t i;

    if (last_removed) {
        *last_removed = 0;
    }

    /* CHANGE SUB WRITE LOCK */
    if ((err_info = sr_rwlock(&shm_mod->change_sub[ds].lock, SR_SUBS_LOCK_TIMEOUT, SR_LOCK_WRITE, conn->cid, __func__,
            NULL, NULL))) {
        return err_info;
    }

    /* EXT READ LOCK */
    if ((err_info = sr_shmext_conn_remap_lock(conn, SR_LOCK_READ, 0, __func__))) {
        goto cleanup_changesub_unlock;
    }

    /* find the subscription(s) */
    shm_sub = (sr_mod_change_sub_t *)(conn->ext_shm.addr + shm_mod->change_sub[ds].subs);
    for (i = 0; i < shm_mod->change_sub[ds].sub_count; ++i) {
        if (cid) {
            if (shm_sub[i].cid == cid) {
                break;
            }
        } else if ((!xpath && !shm_sub[i].xpath)
                    || (xpath && shm_sub[i].xpath && !strcmp(conn->ext_shm.addr + shm_sub[i].xpath, xpath))) {
            if ((shm_sub[i].priority == priority) && (shm_sub[i].opts == sub_opts) && (shm_sub[i].evpipe_num == evpipe_num)) {
                break;
            }
        }
    }
    if (i == shm_mod->change_sub[ds].sub_count) {
        /* subscription not found */
        if (found) {
            *found = 0;
        }
        goto cleanup_changesub_ext_unlock;
    }
    if (found) {
        *found = 1;
    }

    if (evpipe_num_p) {
        *evpipe_num_p = shm_sub[i].evpipe_num;
    }

    /* remove the subscription and its xpath, if any */
    sr_shmrealloc_del(conn->ext_shm.addr, &shm_mod->change_sub[ds].subs, &shm_mod->change_sub[ds].sub_count, sizeof *shm_sub,
            i, shm_sub[i].xpath ? sr_strshmlen(conn->ext_shm.addr + shm_sub[i].xpath) : 0);

    if (!shm_mod->change_sub[ds].subs && last_removed) {
        *last_removed = 1;
    }

cleanup_changesub_ext_unlock:
    /* EXT READ UNLOCK */
    sr_shmext_conn_remap_unlock(conn, SR_LOCK_READ, 0, __func__);

cleanup_changesub_unlock:
    /* CHANGE SUB WRITE UNLOCK */
    sr_rwunlock(&shm_mod->change_sub[ds].lock, SR_SUBS_LOCK_TIMEOUT, SR_LOCK_WRITE, conn->cid, __func__);

    /* ext SHM defrag */
    sr_shmext_defrag_check(conn);

    return err_info;
}

sr_error_info_t *
sr_shmext_change_subscription_stop(sr_conn_ctx_t *conn, sr_mod_t *shm_mod, sr_datastore_t ds, const char *xpath,
        uint32_t priority, int sub_opts, uint32_t evpipe_num, sr_cid_t cid, int del_evpipe)
{
    sr_error_info_t *err_info = NULL;
    char *path;
    const char *mod_name;
    int last_removed, found;
    uint32_t evpipe_num_p;

    mod_name = (char *)(conn->main_shm.addr + shm_mod->name);

    do {
        /* remove the subscription from the ext SHM */
        if ((err_info = sr_shmext_change_subscription_del(conn, shm_mod, ds, xpath, priority, sub_opts, evpipe_num, cid,
                &last_removed, &evpipe_num_p, &found))) {
            break;
        }
        if (!found) {
            if (!cid) {
                /* error in this case */
                SR_ERRINFO_INT(&err_info);
            }
            break;
        }

        if (ds == SR_DS_RUNNING) {
            /* technically, operational data changed */
            if ((err_info = sr_module_update_oper_diff(conn, mod_name))) {
                break;
            }
        }

        if (last_removed) {
            /* delete the SHM file itself so that there is no leftover event */
            if ((err_info = sr_path_sub_shm(mod_name, sr_ds2str(ds), -1, &path))) {
                break;
            }
            if (unlink(path) == -1) {
                SR_LOG_WRN("Failed to unlink SHM \"%s\" (%s).", path, strerror(errno));
            }
            free(path);
        }

        if (del_evpipe) {
            /* delete the evpipe file, it could have been already deleted */
            if ((err_info = sr_path_evpipe(evpipe_num_p, &path))) {
                break;
            }
            unlink(path);
            free(path);
        }
    } while (cid);

    return err_info;
}

sr_error_info_t *
sr_shmext_oper_subscription_add(sr_conn_ctx_t *conn, sr_mod_t *shm_mod, const char *xpath, sr_mod_oper_sub_type_t sub_type,
        int sub_opts, uint32_t evpipe_num)
{
    sr_error_info_t *err_info = NULL;
    off_t xpath_off;
    sr_mod_oper_sub_t *shm_sub;
    size_t new_len, cur_len;
    uint16_t i;

    assert(xpath && sub_type);

    /* OPER SUB WRITE LOCK */
    if ((err_info = sr_rwlock(&shm_mod->oper_lock, SR_SUBS_LOCK_TIMEOUT, SR_LOCK_WRITE, conn->cid, __func__,
            NULL, NULL))) {
        return err_info;
    }

    /* EXT WRITE LOCK */
    if ((err_info = sr_shmext_conn_remap_lock(conn, SR_LOCK_WRITE, 1, __func__))) {
        goto cleanup_opersub_unlock;
    }

    /* check that this exact subscription does not exist yet while finding its position */
    new_len = sr_xpath_len_no_predicates(xpath);
    shm_sub = (sr_mod_oper_sub_t *)(conn->ext_shm.addr + shm_mod->oper_subs);
    for (i = 0; i < shm_mod->oper_sub_count; ++i) {
        cur_len = sr_xpath_len_no_predicates(conn->ext_shm.addr + shm_sub[i].xpath);
        if (cur_len > new_len) {
            /* we can insert it at i-th position */
            break;
        }

        if ((cur_len == new_len) && !strcmp(conn->ext_shm.addr + shm_sub[i].xpath, xpath)) {
            sr_errinfo_new(&err_info, SR_ERR_INVAL_ARG, NULL,
                    "Data provider subscription for \"%s\" on \"%s\" already exists.",
                    conn->main_shm.addr + shm_mod->name, xpath);
            goto cleanup_opersub_ext_unlock;
        }
    }

    /* allocate new subscription and its xpath, if any */
    if ((err_info = sr_shmrealloc_add(&conn->ext_shm, &shm_mod->oper_subs, &shm_mod->oper_sub_count, 0, sizeof *shm_sub,
            i, (void **)&shm_sub, xpath ? sr_strshmlen(xpath) : 0, &xpath_off))) {
        goto cleanup_opersub_ext_unlock;
    }

    /* fill new subscription */
    if (xpath) {
        strcpy(conn->ext_shm.addr + xpath_off, xpath);
        shm_sub->xpath = xpath_off;
    } else {
        shm_sub->xpath = 0;
    }
    shm_sub->sub_type = sub_type;
    shm_sub->opts = sub_opts;
    shm_sub->evpipe_num = evpipe_num;
    shm_sub->cid = conn->cid;

cleanup_opersub_ext_unlock:
    /* EXT WRITE UNLOCK */
    sr_shmext_conn_remap_unlock(conn, SR_LOCK_WRITE, 1, __func__);

cleanup_opersub_unlock:
    /* OPER SUB WRITE UNLOCK */
    sr_rwunlock(&shm_mod->oper_lock, SR_SUBS_LOCK_TIMEOUT, SR_LOCK_WRITE, conn->cid, __func__);

    /* ext SHM defrag */
    sr_shmext_defrag_check(conn);

    return err_info;
}

sr_error_info_t *
sr_shmext_oper_subscription_del(sr_conn_ctx_t *conn, sr_mod_t *shm_mod, const char *xpath, uint32_t evpipe_num,
        sr_cid_t cid, uint32_t *evpipe_num_p, int *found)
{
    sr_error_info_t *err_info = NULL;
    sr_mod_oper_sub_t *shm_sub;
    uint16_t i;

    /* OPER SUB WRITE LOCK */
    if ((err_info = sr_rwlock(&shm_mod->oper_lock, SR_SUBS_LOCK_TIMEOUT, SR_LOCK_WRITE, conn->cid, __func__, NULL, NULL))) {
        return err_info;
    }

    /* EXT READ LOCK */
    if ((err_info = sr_shmext_conn_remap_lock(conn, SR_LOCK_READ, 0, __func__))) {
        goto cleanup_opersub_unlock;
    }

    /* find the subscription */
    shm_sub = (sr_mod_oper_sub_t *)(conn->ext_shm.addr + shm_mod->oper_subs);
    for (i = 0; i < shm_mod->oper_sub_count; ++i) {
        if (cid) {
            if (shm_sub[i].cid == cid) {
                break;
            }
        } else if (shm_sub[i].xpath && !strcmp(conn->ext_shm.addr + shm_sub[i].xpath, xpath)
                && (shm_sub[i].evpipe_num == evpipe_num)) {
            break;
        }
    }
    if (i == shm_mod->oper_sub_count) {
        /* no matching subscription found */
        if (found) {
            *found = 0;
        }
        goto cleanup_opersub_ext_unlock;
    }
    if (found) {
        *found = 1;
    }

    if (evpipe_num_p) {
        *evpipe_num_p = shm_sub[i].evpipe_num;
    }

    /* delete the subscription */
    sr_shmrealloc_del(conn->ext_shm.addr, &shm_mod->oper_subs, &shm_mod->oper_sub_count, sizeof *shm_sub, i,
            shm_sub[i].xpath ? sr_strshmlen(conn->ext_shm.addr + shm_sub[i].xpath) : 0);

cleanup_opersub_ext_unlock:
    /* EXT READ UNLOCK */
    sr_shmext_conn_remap_unlock(conn, SR_LOCK_READ, 0, __func__);

cleanup_opersub_unlock:
    /* OPER SUB WRITE UNLOCK */
    sr_rwunlock(&shm_mod->oper_lock, SR_SUBS_LOCK_TIMEOUT, SR_LOCK_WRITE, conn->cid, __func__);

    /* ext SHM defrag */
    sr_shmext_defrag_check(conn);

    return err_info;
}

sr_error_info_t *
sr_shmext_oper_subscription_stop(sr_conn_ctx_t *conn, sr_mod_t *shm_mod, const char *xpath, uint32_t evpipe_num,
        sr_cid_t cid, int del_evpipe)
{
    sr_error_info_t *err_info = NULL;
    char *path;
    const char *mod_name;
    uint32_t evpipe_num_p;
    int found;

    mod_name = (char *)(conn->main_shm.addr + shm_mod->name);

    do {
        /* remove the subscriptions from the ext SHM */
        if ((err_info = sr_shmext_oper_subscription_del(conn, shm_mod, xpath, evpipe_num, cid, &evpipe_num_p, &found))) {
            break;
        }
        if (!found) {
            if (!cid) {
                SR_ERRINFO_INT(&err_info);
            }
            break;
        }

        /* delete the SHM file itself so that there is no leftover event */
        if ((err_info = sr_path_sub_shm(mod_name, "oper", sr_str_hash(xpath), &path))) {
            break;
        }
        if (unlink(path) == -1) {
            SR_LOG_WRN("Failed to unlink SHM \"%s\" (%s).", path, strerror(errno));
        }
        free(path);

        if (del_evpipe) {
            /* delete the evpipe file, it could have been already deleted */
            if ((err_info = sr_path_evpipe(evpipe_num_p, &path))) {
                break;
            }
            unlink(path);
            free(path);
        }
    } while (cid);

    return err_info;
}

sr_error_info_t *
sr_shmext_rpc_subscription_add(sr_conn_ctx_t *conn, sr_rpc_t *shm_rpc, const char *xpath, uint32_t priority,
        int sub_opts, uint32_t evpipe_num)
{
    sr_error_info_t *err_info = NULL;
    off_t xpath_off;
    sr_mod_rpc_sub_t *shm_sub;
    uint32_t i;

    assert(xpath);

    /* RPC SUB WRITE LOCK */
    if ((err_info = sr_rwlock(&shm_rpc->lock, SR_SUBS_LOCK_TIMEOUT, SR_LOCK_WRITE, conn->cid, __func__, NULL, NULL))) {
        return err_info;
    }

    /* EXT WRITE LOCK */
    if ((err_info = sr_shmext_conn_remap_lock(conn, SR_LOCK_WRITE, 1, __func__))) {
        goto cleanup_rpcsub_unlock;
    }

    /* check that this exact subscription does not exist yet */
    shm_sub = (sr_mod_rpc_sub_t *)(conn->ext_shm.addr + shm_rpc->subs);
    for (i = 0; i < shm_rpc->sub_count; ++i) {
        if (shm_sub->priority == priority) {
            sr_errinfo_new(&err_info, SR_ERR_INVAL_ARG, NULL, "RPC subscription for \"%s\" with priority %u "
                    "already exists.", conn->main_shm.addr + shm_rpc->path, priority);
            goto cleanup_rpcsub_ext_unlock;
        }
    }

    /* add new subscription with its xpath */
    if ((err_info = sr_shmrealloc_add(&conn->ext_shm, &shm_rpc->subs, &shm_rpc->sub_count, 0, sizeof *shm_sub, -1,
            (void **)&shm_sub, sr_strshmlen(xpath), &xpath_off))) {
        goto cleanup_rpcsub_ext_unlock;
    }

    /* fill new subscription */
    strcpy(conn->ext_shm.addr + xpath_off, xpath);
    shm_sub->xpath = xpath_off;
    shm_sub->priority = priority;
    shm_sub->opts = sub_opts;
    shm_sub->evpipe_num = evpipe_num;
    shm_sub->cid = conn->cid;

cleanup_rpcsub_ext_unlock:
    /* EXT WRITE UNLOCK */
    sr_shmext_conn_remap_unlock(conn, SR_LOCK_WRITE, 1, __func__);

cleanup_rpcsub_unlock:
    /* RPC SUB WRITE UNLOCK */
    sr_rwunlock(&shm_rpc->lock, SR_SUBS_LOCK_TIMEOUT, SR_LOCK_WRITE, conn->cid, __func__);

    /* ext SHM defrag */
    sr_shmext_defrag_check(conn);

    return err_info;
}

sr_error_info_t *
sr_shmext_rpc_subscription_del(sr_conn_ctx_t *conn, sr_rpc_t *shm_rpc, const char *xpath, uint32_t priority,
        uint32_t evpipe_num, sr_cid_t cid, int *last_removed, uint32_t *evpipe_num_p, int *found)
{
    sr_error_info_t *err_info = NULL;
    sr_mod_rpc_sub_t *shm_sub;
    uint16_t i;

    if (last_removed) {
        *last_removed = 0;
    }

    /* RPC SUB WRITE LOCK */
    if ((err_info = sr_rwlock(&shm_rpc->lock, SR_SUBS_LOCK_TIMEOUT, SR_LOCK_WRITE, conn->cid, __func__, NULL, NULL))) {
        return err_info;
    }

    /* EXT READ LOCK */
    if ((err_info = sr_shmext_conn_remap_lock(conn, SR_LOCK_READ, 0, __func__))) {
        goto cleanup_rpcsub_unlock;
    }

    /* find the subscription */
    shm_sub = (sr_mod_rpc_sub_t *)(conn->ext_shm.addr + shm_rpc->subs);
    for (i = 0; i < shm_rpc->sub_count; ++i) {
        if (cid) {
            if (shm_sub[i].cid == cid) {
                break;
            }
        } else if (!strcmp(conn->ext_shm.addr + shm_sub[i].xpath, xpath) && (shm_sub[i].priority == priority)
                && (shm_sub[i].evpipe_num == evpipe_num)) {
            break;
        }
    }
    if (i == shm_rpc->sub_count) {
        /* no matching subscription found */
        if (found) {
            *found = 0;
        }
        goto cleanup_rpcsub_ext_unlock;
    }
    if (found) {
        *found = 1;
    }

    if (evpipe_num_p) {
        *evpipe_num_p = shm_sub[i].evpipe_num;
    }

    /* remove the subscription */
    sr_shmrealloc_del(conn->ext_shm.addr, &shm_rpc->subs, &shm_rpc->sub_count, sizeof *shm_sub, i,
            sr_strshmlen(conn->ext_shm.addr + shm_sub[i].xpath));

    if (!shm_rpc->subs && last_removed) {
        *last_removed = 1;
    }

cleanup_rpcsub_ext_unlock:
    /* EXT READ UNLOCK */
    sr_shmext_conn_remap_unlock(conn, SR_LOCK_READ, 0, __func__);

cleanup_rpcsub_unlock:
    /* RPC SUB WRITE UNLOCK */
    sr_rwunlock(&shm_rpc->lock, SR_SUBS_LOCK_TIMEOUT, SR_LOCK_WRITE, conn->cid, __func__);

    /* ext SHM defrag */
    sr_shmext_defrag_check(conn);

    return err_info;
}

sr_error_info_t *
sr_shmext_rpc_subscription_stop(sr_conn_ctx_t *conn, sr_rpc_t *shm_rpc, const char *xpath, uint32_t priority,
        uint32_t evpipe_num, sr_cid_t cid, int del_evpipe)
{
    sr_error_info_t *err_info = NULL;
    char *mod_name, *shmpath;
    const char *path;
    int last_removed, found;
    uint32_t evpipe_num_p;

    path = (char *)(conn->main_shm.addr + shm_rpc->path);

    do {
        /* remove the subscription from the main SHM */
        if ((err_info = sr_shmext_rpc_subscription_del(conn, shm_rpc, xpath, priority, evpipe_num, cid, &last_removed,
                &evpipe_num_p, &found))) {
            break;
        }
        if (!found) {
            if (!cid) {
                SR_ERRINFO_INT(&err_info);
            }
            break;
        }

        if (last_removed) {
            /* get module name */
            mod_name = sr_get_first_ns(path);

            /* delete the SHM file itself so that there is no leftover event */
            err_info = sr_path_sub_shm(mod_name, "rpc", sr_str_hash(path), &shmpath);
            free(mod_name);
            if (err_info) {
                break;
            }
            if (unlink(shmpath) == -1) {
                SR_LOG_WRN("Failed to unlink SHM \"%s\" (%s).", shmpath, strerror(errno));
            }
            free(shmpath);
        }

        if (del_evpipe) {
            /* delete the evpipe file, it could have been already deleted */
            if ((err_info = sr_path_evpipe(evpipe_num_p, &shmpath))) {
                break;
            }
            unlink(shmpath);
            free(shmpath);
        }
    } while (cid);

    return err_info;
}

sr_error_info_t *
sr_shmext_notif_subscription_add(sr_conn_ctx_t *conn, sr_mod_t *shm_mod, uint32_t sub_id, uint32_t evpipe_num)
{
    sr_error_info_t *err_info = NULL;
    sr_mod_notif_sub_t *shm_sub;

    /* NOTIF SUB WRITE LOCK */
    if ((err_info = sr_rwlock(&shm_mod->notif_lock, SR_SUBS_LOCK_TIMEOUT, SR_LOCK_WRITE, conn->cid, __func__,
            NULL, NULL))) {
        return err_info;
    }

    /* EXT WRITE LOCK */
    if ((err_info = sr_shmext_conn_remap_lock(conn, SR_LOCK_WRITE, 1, __func__))) {
        goto cleanup_notifsub_unlock;
    }

    /* add new item */
    if ((err_info = sr_shmrealloc_add(&conn->ext_shm, &shm_mod->notif_subs, &shm_mod->notif_sub_count, 0,
            sizeof *shm_sub, -1, (void **)&shm_sub, 0, NULL))) {
        goto cleanup_notifsub_ext_unlock;
    }

    /* fill new subscription */
    shm_sub->sub_id = sub_id;
    shm_sub->evpipe_num = evpipe_num;
    shm_sub->cid = conn->cid;

cleanup_notifsub_ext_unlock:
    /* EXT WRITE UNLOCK */
    sr_shmext_conn_remap_unlock(conn, SR_LOCK_WRITE, 1, __func__);

cleanup_notifsub_unlock:
    /* NOTIF SUB WRITE UNLOCK */
    sr_rwunlock(&shm_mod->notif_lock, SR_SUBS_LOCK_TIMEOUT, SR_LOCK_WRITE, conn->cid, __func__);

    /* ext SHM defrag */
    sr_shmext_defrag_check(conn);

    return err_info;
}

sr_error_info_t *
sr_shmext_notif_subscription_del(sr_conn_ctx_t *conn, sr_mod_t *shm_mod, uint32_t sub_id, uint32_t evpipe_num,
        sr_cid_t cid, int *last_removed, uint32_t *evpipe_num_p, int *found)
{
    sr_error_info_t *err_info = NULL;
    sr_mod_notif_sub_t *shm_sub;
    uint16_t i;

    if (last_removed) {
        *last_removed = 0;
    }

    /* NOTIF SUB WRITE LOCK */
    if ((err_info = sr_rwlock(&shm_mod->notif_lock, SR_SUBS_LOCK_TIMEOUT, SR_LOCK_WRITE, conn->cid, __func__, NULL, NULL))) {
        return err_info;
    }

    /* EXT READ LOCK */
    if ((err_info = sr_shmext_conn_remap_lock(conn, SR_LOCK_READ, 0, __func__))) {
        goto cleanup_notifsub_unlock;
    }

    /* find the subscription */
    shm_sub = (sr_mod_notif_sub_t *)(conn->ext_shm.addr + shm_mod->notif_subs);
    for (i = 0; i < shm_mod->notif_sub_count; ++i) {
        if (cid) {
            if (shm_sub[i].cid == cid) {
                break;
            }
        } else if ((shm_sub[i].sub_id == sub_id) && (shm_sub[i].evpipe_num == evpipe_num)) {
            break;
        }
    }
    if (i == shm_mod->notif_sub_count) {
        /* no matching subscription found */
        if (found) {
            *found = 0;
        }
        goto cleanup_notifsub_ext_unlock;
    }
    if (found) {
        *found = 1;
    }

    if (evpipe_num_p) {
        *evpipe_num_p = shm_sub[i].evpipe_num;
    }

    /* remove the subscription */
    sr_shmrealloc_del(conn->ext_shm.addr, &shm_mod->notif_subs, &shm_mod->notif_sub_count, sizeof *shm_sub, i, 0);

    if (!shm_mod->notif_subs && last_removed) {
        *last_removed = 1;
    }

cleanup_notifsub_ext_unlock:
    /* EXT READ UNLOCK */
    sr_shmext_conn_remap_unlock(conn, SR_LOCK_READ, 0, __func__);

cleanup_notifsub_unlock:
    /* NOTIF SUB WRITE UNLOCK */
    sr_rwunlock(&shm_mod->notif_lock, SR_SUBS_LOCK_TIMEOUT, SR_LOCK_WRITE, conn->cid, __func__);

    /* ext SHM defrag */
    sr_shmext_defrag_check(conn);

    return err_info;
}

sr_error_info_t *
sr_shmext_notif_subscription_stop(sr_conn_ctx_t *conn, sr_mod_t *shm_mod, uint32_t sub_id, uint32_t evpipe_num,
        sr_cid_t cid, int del_evpipe)
{
    sr_error_info_t *err_info = NULL;
    char *path;
    const char *mod_name;
    int last_removed, found;
    uint32_t evpipe_num_p;

    mod_name = (char *)(conn->main_shm.addr + shm_mod->name);

    do {
        /* remove the subscriptions from the main SHM */
        if ((err_info = sr_shmext_notif_subscription_del(conn, shm_mod, sub_id, evpipe_num, cid, &last_removed,
                &evpipe_num_p, &found))) {
            break;
        }
        if (!found) {
            if (!cid) {
                SR_ERRINFO_INT(&err_info);
            }
            break;
        }

        if (last_removed) {
            /* delete the SHM file itself so that there is no leftover event */
            if ((err_info = sr_path_sub_shm(mod_name, "notif", -1, &path))) {
                break;
            }
            if (unlink(path) == -1) {
                SR_LOG_WRN("Failed to unlink SHM \"%s\" (%s).", path, strerror(errno));
            }
            free(path);
        }

        if (del_evpipe) {
            /* delete the evpipe file, it could have been already deleted */
            if ((err_info = sr_path_evpipe(evpipe_num_p, &path))) {
                break;
            }
            unlink(path);
            free(path);
        }
    } while (cid);

    return err_info;
}
