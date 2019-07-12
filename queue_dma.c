#include "queue_dma.h"
#include <linux/vmalloc.h>
#include <linux/mm.h>

static int bce_alloc_scatterlist_from_vm(struct sg_table *tbl, void *data, size_t len);
static struct bce_segment_list_element_hostinfo *bce_map_segment_list(
        struct device *dev, struct scatterlist *pages, int pagen);
static void bce_unmap_segement_list(struct device *dev, struct bce_segment_list_element_hostinfo *list);

int bce_map_dma_buffer(struct device *dev, struct bce_dma_buffer *buf, struct sg_table scatterlist,
        enum dma_data_direction dir)
{
    int cnt;

    buf->direction = dir;
    buf->scatterlist = scatterlist;

    cnt = dma_map_sg(dev, buf->scatterlist.sgl, buf->scatterlist.nents, dir);
    if (cnt != buf->scatterlist.nents) {
        pr_err("bce: DMA scatter list mapping returned an unexpected count: %i\n", cnt);
        dma_unmap_sg(dev, buf->scatterlist.sgl, buf->scatterlist.nents, dir);
        return -EIO;
    }
    buf->seglist_hostinfo = bce_map_segment_list(dev, buf->scatterlist.sgl, buf->scatterlist.nents);
    if (!buf->seglist_hostinfo) {
        pr_err("bce: Creating segment list failed\n");
        dma_unmap_sg(dev, buf->scatterlist.sgl, buf->scatterlist.nents, dir);
        return -EIO;
    }
    return 0;
}

int bce_map_dma_buffer_vm(struct device *dev, struct bce_dma_buffer *buf, void *data, size_t len,
                          enum dma_data_direction dir)
{
    int status;
    struct sg_table scatterlist;
    if ((status = bce_alloc_scatterlist_from_vm(&scatterlist, data, len)))
        return status;
    if ((status = bce_map_dma_buffer(dev, buf, scatterlist, dir))) {
        sg_free_table(&scatterlist);
        return status;
    }
    return 0;
}

int bce_map_dma_buffer_km(struct device *dev, struct bce_dma_buffer *buf, void *data, size_t len,
                          enum dma_data_direction dir)
{
    /* Kernel memory is continuous which is great for us. */
    int status;
    struct sg_table scatterlist;
    if ((status = sg_alloc_table(&scatterlist, 1, GFP_KERNEL))) {
        sg_free_table(&scatterlist);
        return status;
    }
    if ((status = bce_map_dma_buffer(dev, buf, scatterlist, dir))) {
        sg_free_table(&scatterlist);
        return status;
    }
    return 0;
}

void bce_unmap_dma_buffer(struct device *dev, struct bce_dma_buffer *buf)
{
    dma_unmap_sg(dev, buf->scatterlist.sgl, buf->scatterlist.nents, buf->direction);
    bce_unmap_segement_list(dev, buf->seglist_hostinfo);
}


static int bce_alloc_scatterlist_from_vm(struct sg_table *tbl, void *data, size_t len)
{
    int status;
    size_t off, start_page, end_page;
    struct vm_struct *v = find_vm_area(data);
    if (!v)
        return -ENOENT;
    WARN_ON(((size_t) v->addr % PAGE_SIZE) != 0);
    off        = (size_t) data % PAGE_SIZE;
    start_page = (data - v->addr) / PAGE_SIZE;
    end_page   = (data + len - 1 - v->addr) / PAGE_SIZE;

    if ((status = sg_alloc_table_from_pages(tbl, &v->pages[start_page], end_page - start_page + 1,
                                            (unsigned int) off, len, GFP_KERNEL))) {
        sg_free_table(tbl);
    }
    return status;
}

#define BCE_ELEMENTS_PER_PAGE ((PAGE_SIZE - sizeof(struct bce_segment_list_header)) \
                               / sizeof(struct bce_segment_list_element))
#define BCE_ELEMENTS_PER_ADDITIONAL_PAGE (PAGE_SIZE / sizeof(struct bce_segment_list_element))

static struct bce_segment_list_element_hostinfo *bce_map_segment_list(
        struct device *dev, struct scatterlist *pages, int pagen)
{
    size_t ptr, pptr = 0;
    struct bce_segment_list_header theader; /* a temp header, to store the initial seg */
    struct bce_segment_list_header *header;
    struct bce_segment_list_element *el, *el_end;
    struct bce_segment_list_element_hostinfo *out, *pout, *out_root;
    struct scatterlist *sg;
    int i;
    header = &theader;
    out = out_root = NULL;
    el = el_end = NULL;
    for_each_sg(pages, sg, pagen, i) {
        if (el >= el_end) {
            /* allocate a new page, this will be also done for the first element */
            ptr = __get_free_page(GFP_KERNEL);
            if (pptr && ptr == pptr + PAGE_SIZE) {
                out->page_count++;
                header->element_count += BCE_ELEMENTS_PER_ADDITIONAL_PAGE;
                el_end += BCE_ELEMENTS_PER_ADDITIONAL_PAGE;
            } else {
                header = (void *) ptr;
                header->element_count = BCE_ELEMENTS_PER_PAGE;
                header->data_size = 0;
                header->next_segl_addr = 0;
                header->next_segl_length = 0;
                el = (void *) (header + 1);
                el_end = el + BCE_ELEMENTS_PER_PAGE;

                if (out) {
                    out->next = kmalloc(sizeof(struct bce_segment_list_element_hostinfo), GFP_KERNEL);
                    out = out->next;
                } else {
                    out_root = out = kmalloc(sizeof(struct bce_segment_list_element_hostinfo), GFP_KERNEL);
                }
                out->page_start = (void *) ptr;
                out->page_count = 1;
                out->dma_start = DMA_MAPPING_ERROR;
            }
            pptr = ptr;
        }
        el->addr = sg->dma_address;
        el->length = sg->length;
        header->data_size += el->length;
    }

    /* DMA map */
    out = out_root;
    pout = NULL;
    while (out) {
        out->dma_start = dma_map_single(dev, out->page_start, out->page_count * PAGE_SIZE, DMA_TO_DEVICE);
        if (dma_mapping_error(dev, out->dma_start))
            goto error;
        if (pout) {
            header = pout->page_start;
            header->next_segl_addr = out->dma_start;
            header->next_segl_length = out->page_count * PAGE_SIZE;
        }
        pout = out;
        out = out->next;
    }
    return out_root;

    error:
    bce_unmap_segement_list(dev, out_root);
    return NULL;
}

static void bce_unmap_segement_list(struct device *dev, struct bce_segment_list_element_hostinfo *list)
{
    struct bce_segment_list_element_hostinfo *next;
    while (list) {
        if (list->dma_start != DMA_MAPPING_ERROR)
            dma_unmap_single(dev, list->dma_start, list->page_count * PAGE_SIZE, DMA_TO_DEVICE);
        next = list->next;
        kfree(list);
        list = next;
    }
}
