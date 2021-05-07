// Copyright (c) Veeam Software Group GmbH

#include "stdafx.h"
#include "container_spinlocking.h"
#include "tracker_queue.h"
#include "blk_util.h"

#define SECTION "tracker   "

container_sl_t tracker_disk_container;

int tracker_disk_init(void)
{
    container_sl_init(&tracker_disk_container, sizeof(tracker_disk_t));
    return SUCCESS;
}

int tracker_disk_done(void)
{
    int result = container_sl_done(&tracker_disk_container);
    if (SUCCESS != result)
        log_err("Failed to free up tracker queue container");
    return result;
}

#ifdef HAVE_BLK_INTERPOSER

bool tracking_submit_bio(struct bio *bio);

static void submit_bio_interposer_fn(struct bio *bio)
{
    if (tracking_submit_bio(bio))
        return;

    /* add bio to bio_list then bio was not intercepted */
    BUG_ON(!current->bio_list);
    bio_list_add(&current->bio_list[0], bio);
}

// find or create new tracker queue
int tracker_disk_ref(struct gendisk *disk, tracker_disk_t** ptracker_disk)
{
    int ret = SUCCESS;
    tracker_disk_t* tr_disk = NULL;

    ret = tracker_disk_find(disk, &tr_disk);
    if (SUCCESS == ret) {
        log_tr("Tracker queue already exists");

        *ptracker_disk = tr_disk;
        atomic_inc(&tr_disk->atomic_ref_count);

        return ret;
    }

    if (-ENODATA != ret) {
        log_err_d("Cannot to find tracker queue. errno=", ret);
        return ret;
    }

    log_tr("New tracker queue create");

    tr_disk = (tracker_disk_t*)content_sl_new(&tracker_disk_container);
    if (NULL == tr_disk)
        return -ENOMEM;

    atomic_set(&tr_disk->atomic_ref_count, 0);
    tr_disk->disk = disk;

    *ptracker_disk = tr_disk;
    atomic_inc(&tr_disk->atomic_ref_count);

    blk_disk_freeze(disk);
    {
        ret = blk_interposer_attach(disk, &tr_disk->interposer, submit_bio_interposer_fn);
        if (unlikely(ret))
            log_err_d("Failed to attack blk_interposer. errno=", ret);
        else
            container_sl_push_back(&tracker_disk_container, &tr_disk->content);
    }
    blk_disk_unfreeze(disk);

    if (ret) {
        log_err("Failed to attach interposer to disk");
        content_sl_free(&tr_disk->content);
        return ret;
    }

    log_tr("New tracker queue was created");
    return SUCCESS;
}

int tracker_disk_find(struct gendisk *disk, tracker_disk_t** ptracker_disk)
{
    int result = -ENODATA;
    content_sl_t* pContent = NULL;
    tracker_disk_t* tr_disk = NULL;

    CONTAINER_SL_FOREACH_BEGIN(tracker_disk_container, pContent)
    {
        tr_disk = (tracker_disk_t*)pContent;
        if ((tr_disk->disk == disk)) {
            *ptracker_disk = tr_disk;

            result = SUCCESS;    //don`t continue
            break;
        }
    }CONTAINER_SL_FOREACH_END(tracker_disk_container);

    return result;
}

#else //HAVE_BLK_INTERPOSER

#if LINUX_VERSION_CODE < KERNEL_VERSION(4,4,0)

#ifdef HAVE_MAKE_REQUEST_INT
int tracking_make_request(struct request_queue *q, struct bio *bio);
#else
void tracking_make_request(struct request_queue *q, struct bio *bio);
#endif

#elif  defined(VEEAMSNAP_DISK_SUBMIT_BIO)
blk_qc_t tracking_make_request(struct bio *bio );
#else
blk_qc_t tracking_make_request( struct request_queue *q, struct bio *bio );
#endif

// find or create new tracker queue
int tracker_disk_ref(struct gendisk *disk, tracker_disk_t** ptracker_disk)
{
    int res = SUCCESS;
    tracker_disk_t* tr_disk = NULL;

    res = tracker_disk_find(disk, &tr_disk);
    if (SUCCESS == res){
        log_tr("Tracker disk already exists");

        *ptracker_disk = tr_disk;
        atomic_inc( &tr_disk->atomic_ref_count );

        return res;
    }

    if (-ENODATA != res){
        log_err_d( "Cannot to find tracker disk. errno=", res );
        return res;
    }

    log_tr("New tracker disk create" );

    tr_disk = (tracker_disk_t*)container_sl_new(&tracker_disk_container);
    if (NULL==tr_disk)
        return -ENOMEM;

    atomic_set( &tr_disk->atomic_ref_count, 0 );

#ifdef VEEAMSNAP_DISK_SUBMIT_BIO
    {
        unsigned long cr0;

        preempt_disable();
        cr0 = disable_page_protection();

        tr_disk->original_make_request_fn = disk->fops->submit_bio;
        ((struct block_device_operations *)disk->fops)->submit_bio = tracking_make_request;

        reenable_page_protection(cr0);
        preempt_enable();
    }
#else
    tr_disk->original_make_request_fn = disk->queue->make_request_fn;
    disk->queue->make_request_fn = tracking_make_request;
#endif

    tr_disk->disk = disk;

    *ptracker_disk = tr_disk;
    atomic_inc( &tr_disk->atomic_ref_count );

    log_tr("New tracker disk was created");

    return SUCCESS;
}

int tracker_disk_find( struct gendisk *disk, tracker_disk_t** ptracker_disk )
{
    int result = -ENODATA;
    content_sl_t* pContent = NULL;
    tracker_disk_t* tr_disk = NULL;

    CONTAINER_SL_FOREACH_BEGIN( tracker_disk_container, pContent )
    {
        tr_disk = (tracker_disk_t*)pContent;
        if (tr_disk->disk == disk){
            *ptracker_disk = tr_disk;

            result = SUCCESS;    //don`t continue
            break;
        }
    }CONTAINER_SL_FOREACH_END( tracker_disk_container );

    return result;
}
#endif //HAVE_BLK_INTERPOSER

void tracker_disk_unref(tracker_disk_t* tr_disk)
{
    if (atomic_dec_and_test(&tr_disk->atomic_ref_count)) {
#ifdef HAVE_BLK_INTERPOSER
        struct gendisk *disk = tr_disk->disk;

        blk_disk_freeze(disk);
        {
            blk_interposer_detach(&tr_disk->interposer, submit_bio_interposer_fn);
            tr_disk->disk = NULL;
        }
        blk_disk_unfreeze(disk);
#else

#ifdef VEEAMSNAP_DISK_SUBMIT_BIO
        struct gendisk *disk = tr_disk->disk;

        blk_disk_freeze(disk);
        {
            unsigned long cr0;

            preempt_disable();
            cr0 = disable_page_protection();

            ((struct block_device_operations *)disk->fops)->submit_bio = tr_disk->original_make_request_fn;

            reenable_page_protection(cr0);
            preempt_enable();
        }
        blk_disk_unfreeze(disk);
#else
        tr_disk->disk->queue->make_request_fn = tr_disk->original_make_request_fn;
#endif

#endif
        container_sl_free(&tr_disk->content);

        log_tr("Tracker disk freed");
    }
    else
        log_tr("Tracker disk is in use");
}
