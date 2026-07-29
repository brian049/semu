/* virtio-gpu 3D backend backed by virglrenderer. */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/uio.h>

#include <virgl/virglrenderer.h>

#include "virtio-gpu.h"

#define PRIV(x) ((virtio_gpu_data_t *) (x)->priv)

#define VGPU_VIRGL_MAX_BACKING_ENTRIES 16384

/* virglrenderer signals fence completion here. */
static void virgl_write_fence_cb(void *cookie, uint32_t fence)
{
    (void) cookie;
    (void) fence;
}

static struct virgl_renderer_callbacks virgl_cbs = {
    .version = 1,
    .write_fence = virgl_write_fence_cb,
};

static bool virgl_initialized;

bool virtio_gpu_virgl_init(virtio_gpu_state_t *vgpu)
{
    if (virgl_initialized)
        return true;

    int flags = VIRGL_RENDERER_USE_EGL | VIRGL_RENDERER_USE_SURFACELESS;

    int ret = virgl_renderer_init(vgpu, flags, &virgl_cbs);
    if (ret) {
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX
                "virgl_renderer_init() failed: %d. 3D disabled.\n",
                ret);
        return false;
    }

    virgl_initialized = true;
    fprintf(stderr, VIRTIO_GPU_LOG_PREFIX "virglrenderer initialized.\n");
    return true;
}

void virtio_gpu_virgl_cleanup(virtio_gpu_state_t *vgpu)
{
    if (!virgl_initialized)
        return;

    virgl_renderer_cleanup(vgpu);
    virgl_initialized = false;
}

/* Report the real VIRGL capset advertised by virglrenderer, replacing the
 * hardcoded placeholder used during negotiation bring-up.
 */
void virtio_gpu_virgl_get_capset_info_handler(virtio_gpu_state_t *vgpu,
                                              struct virtq_desc *vq_desc,
                                              uint32_t *plen)
{
    const struct virtq_desc *response_desc = virtio_gpu_get_response_desc(
        vq_desc, sizeof(struct virtio_gpu_resp_capset_info));
    if (!response_desc) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    struct virtio_gpu_get_capset_info *request = virtio_gpu_get_request(
        vgpu, vq_desc, sizeof(struct virtio_gpu_get_capset_info));
    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    /* Only index 0 is advertised ('num_capsets == 1'). */
    if (request->capset_index != 0) {
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX "%s(): invalid capset index %u\n",
                __func__, request->capset_index);
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &request->hdr, response_desc,
            VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
        if (!*plen)
            virtio_gpu_set_fail(vgpu);
        return;
    }

    struct virtio_gpu_resp_capset_info *response =
        virtio_gpu_mem_guest_to_host(vgpu, response_desc->addr,
                                     sizeof(*response));
    if (!response) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    uint32_t max_ver = 0, max_size = 0;
    virgl_renderer_get_cap_set(VIRTIO_GPU_CAPSET_VIRGL2, &max_ver, &max_size);

    memset(response, 0, sizeof(*response));
    response->hdr.type = VIRTIO_GPU_RESP_OK_CAPSET_INFO;
    response->capset_id = VIRTIO_GPU_CAPSET_VIRGL2;
    response->capset_max_version = max_ver;
    response->capset_max_size = max_size;

    if (request->hdr.flags & VIRTIO_GPU_FLAG_FENCE) {
        response->hdr.flags = VIRTIO_GPU_FLAG_FENCE;
        response->hdr.fence_id = request->hdr.fence_id;
    }

    *plen = sizeof(*response);
}

void virtio_gpu_virgl_get_capset_handler(virtio_gpu_state_t *vgpu,
                                         struct virtq_desc *vq_desc,
                                         uint32_t *plen)
{
    struct virtio_gpu_get_capset *request = virtio_gpu_get_request(
        vgpu, vq_desc, sizeof(struct virtio_gpu_get_capset));
    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    uint32_t max_ver = 0, max_size = 0;
    virgl_renderer_get_cap_set(request->capset_id, &max_ver, &max_size);
    if (!max_size) {
        fprintf(stderr, VIRTIO_GPU_LOG_PREFIX "%s(): unknown capset id %u\n",
                __func__, request->capset_id);
        const struct virtq_desc *err_desc = virtio_gpu_get_response_desc(
            vq_desc, sizeof(struct virtio_gpu_ctrl_hdr));
        if (!err_desc) {
            virtio_gpu_set_fail(vgpu);
            *plen = 0;
            return;
        }
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &request->hdr, err_desc,
            VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
        if (!*plen)
            virtio_gpu_set_fail(vgpu);
        return;
    }

    size_t resp_size = sizeof(struct virtio_gpu_resp_capset) + max_size;
    const struct virtq_desc *response_desc =
        virtio_gpu_get_response_desc(vq_desc, resp_size);
    if (!response_desc) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    struct virtio_gpu_resp_capset *response =
        virtio_gpu_mem_guest_to_host(vgpu, response_desc->addr, resp_size);
    if (!response) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    memset(response, 0, resp_size);
    response->hdr.type = VIRTIO_GPU_RESP_OK_CAPSET;

    /* Let virglrenderer fill the capset body directly into guest memory. */
    virgl_renderer_fill_caps(request->capset_id, request->capset_version,
                             response->capset_data);

    if (request->hdr.flags & VIRTIO_GPU_FLAG_FENCE) {
        response->hdr.flags = VIRTIO_GPU_FLAG_FENCE;
        response->hdr.fence_id = request->hdr.fence_id;
    }

    *plen = (uint32_t) resp_size;
}

void virtio_gpu_virgl_ctx_create_handler(virtio_gpu_state_t *vgpu,
                                         struct virtq_desc *vq_desc,
                                         uint32_t *plen)
{
    const struct virtq_desc *response_desc = virtio_gpu_get_response_desc(
        vq_desc, sizeof(struct virtio_gpu_ctrl_hdr));
    if (!response_desc) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    struct virtio_gpu_ctx_create *request = virtio_gpu_get_request(
        vgpu, vq_desc, sizeof(struct virtio_gpu_ctx_create));
    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    uint32_t ctx_id = request->hdr.ctx_id;

    /* Control guest-claimed nlen into the fixed debug_name buffer,
     * if not doing this virgl would copies exactly nlen bytes then
     * an oversized value may read past the struct.
     */
    uint32_t name_len = request->nlen;
    if (name_len > sizeof(request->debug_name))
        name_len = sizeof(request->debug_name);
    const char *name = (name_len > 0) ? request->debug_name : NULL;

    int ret = virgl_renderer_context_create(ctx_id, name_len, name);
    if (ret) {
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX
                "%s(): virgl_renderer_context_create(ctx_id=%u) failed: %d\n",
                __func__, ctx_id, ret);
        
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &request->hdr, response_desc, VIRTIO_GPU_RESP_ERR_UNSPEC);
        if (!*plen)
            virtio_gpu_set_fail(vgpu);
        return;
    }

    *plen = virtio_gpu_write_ctrl_response(vgpu, &request->hdr, response_desc,
                                           VIRTIO_GPU_RESP_OK_NODATA);
    if (!*plen)
        virtio_gpu_set_fail(vgpu);
}

void virtio_gpu_virgl_ctx_destroy_handler(virtio_gpu_state_t *vgpu,
                                          struct virtq_desc *vq_desc,
                                          uint32_t *plen)
{
    const struct virtq_desc *response_desc = virtio_gpu_get_response_desc(
        vq_desc, sizeof(struct virtio_gpu_ctrl_hdr));
    if (!response_desc) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    struct virtio_gpu_ctx_destroy *request = virtio_gpu_get_request(
        vgpu, vq_desc, sizeof(struct virtio_gpu_ctx_destroy));
    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    virgl_renderer_context_destroy(request->hdr.ctx_id);

    *plen = virtio_gpu_write_ctrl_response(vgpu, &request->hdr, response_desc,
                                           VIRTIO_GPU_RESP_OK_NODATA);
    if (!*plen)
        virtio_gpu_set_fail(vgpu);
}

/* Shared body for attach/detach, both take "ctx_id from hdr, resource_id
 * from body" and forward to the matching virgl call. Divide by "attach".
 */
static void virtio_gpu_virgl_ctx_resource_op(virtio_gpu_state_t *vgpu,
                                             struct virtq_desc *vq_desc,
                                             uint32_t *plen,
                                             bool attach)
{
    const struct virtq_desc *response_desc = virtio_gpu_get_response_desc(
        vq_desc, sizeof(struct virtio_gpu_ctrl_hdr));
    if (!response_desc) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    struct virtio_gpu_ctx_resource *request = virtio_gpu_get_request(
        vgpu, vq_desc, sizeof(struct virtio_gpu_ctx_resource));
    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    int ctx_id = (int) request->hdr.ctx_id;
    int res_handle = (int) request->resource_id;

    if (attach)
        virgl_renderer_ctx_attach_resource(ctx_id, res_handle);
    else
        virgl_renderer_ctx_detach_resource(ctx_id, res_handle);

    *plen = virtio_gpu_write_ctrl_response(vgpu, &request->hdr, response_desc,
                                           VIRTIO_GPU_RESP_OK_NODATA);
    if (!*plen)
        virtio_gpu_set_fail(vgpu);
}

void virtio_gpu_virgl_ctx_attach_resource_handler(virtio_gpu_state_t *vgpu,
                                                  struct virtq_desc *vq_desc,
                                                  uint32_t *plen)
{
    virtio_gpu_virgl_ctx_resource_op(vgpu, vq_desc, plen, true);
}

void virtio_gpu_virgl_ctx_detach_resource_handler(virtio_gpu_state_t *vgpu,
                                                  struct virtq_desc *vq_desc,
                                                  uint32_t *plen)
{
    virtio_gpu_virgl_ctx_resource_op(vgpu, vq_desc, plen, false);
}


/* Semu needs to track metadata per 3D resource for differ 3D or 2D dispatch,
 * w/h/format for readback, and backing iovecs.
 */
struct vgpu_virgl_res_3d {
    uint32_t resource_id;
    uint32_t target;
    uint32_t format; /* virgl/gallium format */
    uint32_t width, height;

    /* Guest backing pages. */
    struct iovec *backing_iov;
    uint32_t backing_iov_cnt;

    uint32_t *readback;

    struct vgpu_virgl_res_3d *next;
};

static struct vgpu_virgl_res_3d *g_virgl_res_3d_list;

static struct vgpu_virgl_res_3d *virgl_res_3d_find(uint32_t resource_id)
{
    for (struct vgpu_virgl_res_3d *r = g_virgl_res_3d_list; r; r = r->next) {
        if (r->resource_id == resource_id)
            return r;
    }
    return NULL;
}

/* Unlink and free a tracking node. */
static void virgl_res_3d_remove(uint32_t resource_id)
{
    struct vgpu_virgl_res_3d **pp = &g_virgl_res_3d_list;
    while (*pp) {
        if ((*pp)->resource_id == resource_id) {
            struct vgpu_virgl_res_3d *victim = *pp;
            *pp = victim->next;
            free(victim);
            return;
        }
        pp = &(*pp)->next;
    }
}

void virtio_gpu_virgl_resource_create_3d_handler(virtio_gpu_state_t *vgpu,
                                                 struct virtq_desc *vq_desc,
                                                 uint32_t *plen)
{
    const struct virtq_desc *response_desc = virtio_gpu_get_response_desc(
        vq_desc, sizeof(struct virtio_gpu_ctrl_hdr));
    if (!response_desc) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    struct virtio_gpu_resource_create_3d *request = virtio_gpu_get_request(
        vgpu, vq_desc, sizeof(struct virtio_gpu_resource_create_3d));
    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    /* resource_id 0 is reserved by the spec as the "no resource" value,
     * creating a resource with id 0 would make that sentinel ambiguous.
     * A duplicate id would also leave the tracking list and virgl's
     *  table out of sync.
     */
    if (!request->resource_id || virgl_res_3d_find(request->resource_id)) {
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX "%s(): invalid/duplicate resource id %u\n",
                __func__, request->resource_id);
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &request->hdr, response_desc,
            VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
        if (!*plen)
            virtio_gpu_set_fail(vgpu);
        return;
    }

    /* The virtio-gpu 3D creation parameters are defined to match
     * 'virgl_renderer_resource_create_args', "handle" is the
     * resource id and no translation is needed. 
     * "format" here is a virgl/gallium format code coming from
     * guest Mesa, distinct from the codes used by the 2D path.
     */
    struct virgl_renderer_resource_create_args args = {
        .handle = request->resource_id,
        .target = request->target,
        .format = request->format,
        .bind = request->bind,
        .width = request->width,
        .height = request->height,
        .depth = request->depth,
        .array_size = request->array_size,
        .last_level = request->last_level,
        .nr_samples = request->nr_samples,
        .flags = request->flags,
    };

    int ret = virgl_renderer_resource_create(&args, NULL, 0);
    if (ret) {
        /* virgl rejected the parameters like unsupported format/target on the
         * host GL, or zero-sized dims. Report INVALID_PARAMETER so guest
         * Mesa can fall back instead of assuming the resource exists.
         */
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX
                "%s(): virgl_renderer_resource_create(id=%u, fmt=%u) "
                "failed: %d\n",
                __func__, request->resource_id, request->format, ret);
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &request->hdr, response_desc,
            VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
        if (!*plen)
            virtio_gpu_set_fail(vgpu);
        return;
    }

    struct vgpu_virgl_res_3d *node = calloc(1, sizeof(*node));
    if (!node) {
        /* Keep virgl and the tracking list consistent by backing out the
         * virgl resource rather than leaving an untracked one alive.
         */
        virgl_renderer_resource_unref(request->resource_id);
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &request->hdr, response_desc,
            VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY);
        if (!*plen)
            virtio_gpu_set_fail(vgpu);
        return;
    }
    node->resource_id = request->resource_id;
    node->target = request->target;
    node->format = request->format;
    node->width = request->width;
    node->height = request->height;
    node->next = g_virgl_res_3d_list;
    g_virgl_res_3d_list = node;

    *plen = virtio_gpu_write_ctrl_response(vgpu, &request->hdr, response_desc,
                                           VIRTIO_GPU_RESP_OK_NODATA);
    if (!*plen)
        virtio_gpu_set_fail(vgpu);
}


void virtio_gpu_virgl_resource_unref_handler(virtio_gpu_state_t *vgpu,
                                             struct virtq_desc *vq_desc,
                                             uint32_t *plen)
{
    struct virtio_gpu_res_unref *request = virtio_gpu_get_request(
        vgpu, vq_desc, sizeof(struct virtio_gpu_res_unref));
    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    struct vgpu_virgl_res_3d *res = virgl_res_3d_find(request->resource_id);
    if (!res) {
        /* 2D path */
        vgpu_sw_cmd_resource_unref_handler(vgpu, vq_desc, plen);
        return;
    }

    const struct virtq_desc *response_desc = virtio_gpu_get_response_desc(
        vq_desc, sizeof(struct virtio_gpu_ctrl_hdr));
    if (!response_desc) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    /* Detach guest backing before unref so virgl stops referencing the iovs
     * then free them.
     */
    if (res->backing_iov) {
        struct iovec *iov = NULL;
        int iov_cnt = 0;
        virgl_renderer_resource_detach_iov(request->resource_id, &iov,
                                           &iov_cnt);
        free(res->backing_iov);
    }

    virgl_renderer_resource_unref(request->resource_id);
    free(res->readback);
    virgl_res_3d_remove(request->resource_id);

    *plen = virtio_gpu_write_ctrl_response(vgpu, &request->hdr, response_desc,
                                           VIRTIO_GPU_RESP_OK_NODATA);
    if (!*plen)
        virtio_gpu_set_fail(vgpu);
}

void virtio_gpu_virgl_resource_attach_backing_handler(
    virtio_gpu_state_t *vgpu,
    struct virtq_desc *vq_desc,
    uint32_t *plen)
{
    struct virtio_gpu_res_attach_backing *request = virtio_gpu_get_request(
        vgpu, vq_desc, sizeof(struct virtio_gpu_res_attach_backing));
    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    struct vgpu_virgl_res_3d *res = virgl_res_3d_find(request->resource_id);
    if (!res) {
        /* 2D path */
        vgpu_sw_cmd_resource_attach_backing_handler(vgpu, vq_desc, plen);
        return;
    }

    const struct virtq_desc *response_desc = virtio_gpu_get_response_desc(
        vq_desc, sizeof(struct virtio_gpu_ctrl_hdr));
    if (!response_desc) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    /* Mirror the 2D handler's validation of the descriptor. */
    if (vq_desc[1].flags & VIRTIO_DESC_F_WRITE) {
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX
                "%s(): backing entries descriptor is writable\n",
                __func__);
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    if (request->nr_entries == 0 ||
        request->nr_entries > VGPU_VIRGL_MAX_BACKING_ENTRIES) {
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX "%s(): invalid backing entry count %u\n",
                __func__, request->nr_entries);
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &request->hdr, response_desc,
            VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
        if (!*plen)
            virtio_gpu_set_fail(vgpu);
        return;
    }

    if (res->backing_iov) {
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX
                "%s(): backing already attached for resource %u\n",
                __func__, request->resource_id);
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &request->hdr, response_desc,
            VIRTIO_GPU_RESP_ERR_UNSPEC);
        if (!*plen)
            virtio_gpu_set_fail(vgpu);
        return;
    }

    size_t entries_size =
        sizeof(struct virtio_gpu_mem_entry) * request->nr_entries;
    if (vq_desc[1].len < entries_size) {
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX
                "%s(): backing entries descriptor too small\n",
                __func__);
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &request->hdr, response_desc,
            VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
        if (!*plen)
            virtio_gpu_set_fail(vgpu);
        return;
    }

    struct virtio_gpu_mem_entry *entries = virtio_gpu_mem_guest_to_host(
        vgpu, vq_desc[1].addr, (uint32_t) entries_size);
    if (!entries) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    struct iovec *iov = calloc(request->nr_entries, sizeof(struct iovec));
    if (!iov) {
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &request->hdr, response_desc,
            VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY);
        if (!*plen)
            virtio_gpu_set_fail(vgpu);
        return;
    }

    /* Translate each guest-physical extent. */
    for (uint32_t i = 0; i < request->nr_entries; i++) {
        void *host = virtio_gpu_mem_guest_to_host(vgpu, entries[i].addr,
                                                  entries[i].length);
        if (!host) {
            fprintf(stderr,
                    VIRTIO_GPU_LOG_PREFIX
                    "%s(): bad backing entry %u (addr=0x%llx len=%u)\n",
                    __func__, i, (unsigned long long) entries[i].addr,
                    entries[i].length);
            free(iov);
            *plen = virtio_gpu_write_ctrl_response(
                vgpu, &request->hdr, response_desc,
                VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
            if (!*plen)
                virtio_gpu_set_fail(vgpu);
            return;
        }
        iov[i].iov_base = host;
        iov[i].iov_len = entries[i].length;
    }

    int ret = virgl_renderer_resource_attach_iov(
        (int) request->resource_id, iov, (int) request->nr_entries);
    if (ret) {
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX
                "%s(): virgl attach_iov(id=%u, n=%u) failed: %d\n",
                __func__, request->resource_id, request->nr_entries, ret);
        free(iov);
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &request->hdr, response_desc, VIRTIO_GPU_RESP_ERR_UNSPEC);
        if (!*plen)
            virtio_gpu_set_fail(vgpu);
        return;
    }

    res->backing_iov = iov;
    res->backing_iov_cnt = request->nr_entries;

    *plen = virtio_gpu_write_ctrl_response(vgpu, &request->hdr, response_desc,
                                           VIRTIO_GPU_RESP_OK_NODATA);
    if (!*plen)
        virtio_gpu_set_fail(vgpu);
}

void virtio_gpu_virgl_resource_detach_backing_handler(
    virtio_gpu_state_t *vgpu,
    struct virtq_desc *vq_desc,
    uint32_t *plen)
{
    struct virtio_gpu_res_detach_backing *request = virtio_gpu_get_request(
        vgpu, vq_desc, sizeof(struct virtio_gpu_res_detach_backing));
    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    struct vgpu_virgl_res_3d *res = virgl_res_3d_find(request->resource_id);
    if (!res) {
        vgpu_sw_cmd_resource_detach_backing_handler(vgpu, vq_desc, plen);
        return;
    }

    const struct virtq_desc *response_desc = virtio_gpu_get_response_desc(
        vq_desc, sizeof(struct virtio_gpu_ctrl_hdr));
    if (!response_desc) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    if (res->backing_iov) {
        struct iovec *iov = NULL;
        int iov_cnt = 0;
        /* Returns the pointer which is handed to attach_iov and free the
         * copy after virgl has dropped its reference.
         */
        virgl_renderer_resource_detach_iov((int) request->resource_id, &iov,
                                           &iov_cnt);
        free(res->backing_iov);
        res->backing_iov = NULL;
        res->backing_iov_cnt = 0;
    }

    *plen = virtio_gpu_write_ctrl_response(vgpu, &request->hdr, response_desc,
                                           VIRTIO_GPU_RESP_OK_NODATA);
    if (!*plen)
        virtio_gpu_set_fail(vgpu);
}

/* Shared body for both 3D transfer directions. Divide by "to_host" */
static void virtio_gpu_virgl_transfer_3d_op(virtio_gpu_state_t *vgpu,
                                            struct virtq_desc *vq_desc,
                                            uint32_t *plen,
                                            bool to_host)
{
    const struct virtq_desc *response_desc = virtio_gpu_get_response_desc(
        vq_desc, sizeof(struct virtio_gpu_ctrl_hdr));
    if (!response_desc) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    struct virtio_gpu_transfer_host_3d *request = virtio_gpu_get_request(
        vgpu, vq_desc, sizeof(struct virtio_gpu_transfer_host_3d));
    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    if (!virgl_res_3d_find(request->resource_id)) {
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX "%s(): unknown 3D resource id %u\n",
                __func__, request->resource_id);
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &request->hdr, response_desc,
            VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
        if (!*plen)
            virtio_gpu_set_fail(vgpu);
        return;
    }

    int ret;
    if (to_host) {
        ret = virgl_renderer_transfer_write_iov(
            request->resource_id, request->hdr.ctx_id, (int) request->level,
            request->stride, request->layer_stride,
            (struct virgl_box *) &request->box, request->offset, NULL, 0);
    } else {
        ret = virgl_renderer_transfer_read_iov(
            request->resource_id, request->hdr.ctx_id, request->level,
            request->stride, request->layer_stride,
            (struct virgl_box *) &request->box, request->offset, NULL, 0);
    }

    if (ret) {
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX
                "%s(): virgl transfer_%s_iov(id=%u) failed: %d\n",
                __func__, to_host ? "write" : "read", request->resource_id,
                ret);
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &request->hdr, response_desc, VIRTIO_GPU_RESP_ERR_UNSPEC);
        if (!*plen)
            virtio_gpu_set_fail(vgpu);
        return;
    }

    *plen = virtio_gpu_write_ctrl_response(vgpu, &request->hdr, response_desc,
                                           VIRTIO_GPU_RESP_OK_NODATA);
    if (!*plen)
        virtio_gpu_set_fail(vgpu);
}

void virtio_gpu_virgl_transfer_to_host_3d_handler(virtio_gpu_state_t *vgpu,
                                                  struct virtq_desc *vq_desc,
                                                  uint32_t *plen)
{
    virtio_gpu_virgl_transfer_3d_op(vgpu, vq_desc, plen, true);
}

void virtio_gpu_virgl_transfer_from_host_3d_handler(virtio_gpu_state_t *vgpu,
                                                    struct virtq_desc *vq_desc,
                                                    uint32_t *plen)
{
    virtio_gpu_virgl_transfer_3d_op(vgpu, vq_desc, plen, false);
}

/* SUBMIT_3D forwards guest Mesa's virgl command stream to virglrenderer
 * , semu does not interpret the payload.
 */
void virtio_gpu_virgl_submit_3d_handler(virtio_gpu_state_t *vgpu,
                                        struct virtq_desc *vq_desc,
                                        uint32_t *plen)
{
    const struct virtq_desc *response_desc = virtio_gpu_get_response_desc(
        vq_desc, sizeof(struct virtio_gpu_ctrl_hdr));
    if (!response_desc) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    struct virtio_gpu_cmd_submit *request = virtio_gpu_get_request(
        vgpu, vq_desc, sizeof(struct virtio_gpu_cmd_submit));
    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    /* Virgl streams are dword-based: size must be non-zero and 4-byte aligned,
     * Reject before virgl sees it.
     */
    if (request->size == 0 || (request->size & 3)) {
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX "%s(): bad submit size %u\n", __func__,
                request->size);
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &request->hdr, response_desc,
            VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
        if (!*plen)
            virtio_gpu_set_fail(vgpu);
        return;
    }

    /* Command stream is device-readable and must cover request->size. */
    if ((vq_desc[1].flags & VIRTIO_DESC_F_WRITE) ||
        vq_desc[1].len < request->size) {
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX
                "%s(): bad command-stream descriptor (len=%u, need=%u)\n",
                __func__, vq_desc[1].len, request->size);
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &request->hdr, response_desc,
            VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
        if (!*plen)
            virtio_gpu_set_fail(vgpu);
        return;
    }

    void *cmd_buf =
        virtio_gpu_mem_guest_to_host(vgpu, vq_desc[1].addr, request->size);
    if (!cmd_buf) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    int ret = virgl_renderer_submit_cmd(cmd_buf, (int) request->hdr.ctx_id,
                                        (int) (request->size / 4));
    if (ret) {
        /* Virgl rejected part of the stream when an unsupported command for
         * the negotiated capset causing decoding failure.
         */
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX
                "%s(): virgl_renderer_submit_cmd(ctx=%u, %u dw) failed: %d\n",
                __func__, request->hdr.ctx_id, request->size / 4, ret);
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &request->hdr, response_desc, VIRTIO_GPU_RESP_ERR_UNSPEC);
        if (!*plen)
            virtio_gpu_set_fail(vgpu);
        return;
    }

    *plen = virtio_gpu_write_ctrl_response(vgpu, &request->hdr, response_desc,
                                           VIRTIO_GPU_RESP_OK_NODATA);
    if (!*plen)
        virtio_gpu_set_fail(vgpu);
}

/* SET_SCANOUT with a 3D resource only records the binding.
 * The GPU->CPU copy per frame is measured to be improved in the future.
 */
void virtio_gpu_virgl_set_scanout_handler(virtio_gpu_state_t *vgpu,
                                          struct virtq_desc *vq_desc,
                                          uint32_t *plen)
{
    struct virtio_gpu_set_scanout *request = virtio_gpu_get_request(
        vgpu, vq_desc, sizeof(struct virtio_gpu_set_scanout));
    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    struct vgpu_virgl_res_3d *res = virgl_res_3d_find(request->resource_id);
    if (!res) {
        /* 2D path */
        vgpu_sw_cmd_set_scanout_handler(vgpu, vq_desc, plen);
        return;
    }

    const struct virtq_desc *response_desc = virtio_gpu_get_response_desc(
        vq_desc, sizeof(struct virtio_gpu_ctrl_hdr));
    if (!response_desc) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    if (request->scanout_id >= PRIV(vgpu)->num_scanouts) {
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX "%s(): invalid scanout id %u\n",
                __func__, request->scanout_id);
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &request->hdr, response_desc,
            VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID);
        if (!*plen)
            virtio_gpu_set_fail(vgpu);
        return;
    }

    /* The scanout view must lie inside the resource. */
    if (request->r.width == 0 || request->r.height == 0 ||
        request->r.x + request->r.width > res->width ||
        request->r.y + request->r.height > res->height) {
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX
                "%s(): rect %u,%u %ux%u outside resource %u (%ux%u)\n",
                __func__, request->r.x, request->r.y, request->r.width,
                request->r.height, request->resource_id, res->width,
                res->height);
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &request->hdr, response_desc,
            VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
        if (!*plen)
            virtio_gpu_set_fail(vgpu);
        return;
    }

    /* Record the binding in the shared per-scanout state, mirroring what the
     * 2D handler does, also the flush wrapper and the display bridge both
     * read it.
     */
    struct virtio_gpu_scanout_info *scanout =
        &PRIV(vgpu)->scanouts[request->scanout_id];
    scanout->enabled = 1;
    scanout->primary_resource_id = request->resource_id;
    scanout->src_x = request->r.x;
    scanout->src_y = request->r.y;
    scanout->src_w = request->r.width;
    scanout->src_h = request->r.height;

    *plen = virtio_gpu_write_ctrl_response(vgpu, &request->hdr, response_desc,
                                           VIRTIO_GPU_RESP_OK_NODATA);
    if (!*plen)
        virtio_gpu_set_fail(vgpu);
}

/* RESOURCE_FLUSH is the per-frame trigger by reading the rendered image
 * back from virgl's GPU-side resource into a CPU snapshot, then push it
 * through the existing display bridge.
 */
void virtio_gpu_virgl_resource_flush_handler(virtio_gpu_state_t *vgpu,
                                             struct virtq_desc *vq_desc,
                                             uint32_t *plen)
{
    struct virtio_gpu_res_flush *request = virtio_gpu_get_request(
        vgpu, vq_desc, sizeof(struct virtio_gpu_res_flush));
    if (!request) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    struct vgpu_virgl_res_3d *res = virgl_res_3d_find(request->resource_id);
    if (!res) {
        /* 2D path */
        vgpu_sw_cmd_resource_flush_handler(vgpu, vq_desc, plen);
        return;
    }

    const struct virtq_desc *response_desc = virtio_gpu_get_response_desc(
        vq_desc, sizeof(struct virtio_gpu_ctrl_hdr));
    if (!response_desc) {
        virtio_gpu_set_fail(vgpu);
        *plen = 0;
        return;
    }

    size_t snap_size = (size_t) res->width * res->height * 4;
    if (!res->readback) {
        res->readback = malloc(snap_size);
        if (!res->readback) {
            *plen = virtio_gpu_write_ctrl_response(
                vgpu, &request->hdr, response_desc,
                VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY);
            if (!*plen)
                virtio_gpu_set_fail(vgpu);
            return;
        }
    }

    /* Here reads the full resource back to the CPU. 
     * ctx 0 is virglrenderer's host-internal context, valid for transfers
     * on any resource. Passing an explicit iov (not NULL) redirects the
     * copy to res->readback instead of the resource's guest backing.
     * stride 0 means tightly packed.
     *
     * Assumes 32bpp, as used by Linux KMS (B8G8R8X8/A8) and the SDL path.
     */
    struct virtio_gpu_box box = {
        .x = 0, .y = 0, .z = 0, .w = res->width, .h = res->height, .d = 1,
    };
    struct iovec snap_iov = {
        .iov_base = res->readback,
        .iov_len = snap_size,
    };
    int ret = virgl_renderer_transfer_read_iov(
        request->resource_id, 0 /* ctx 0 */, 0 /* level */, 0 /* stride */,
        0 /* layer_stride */, (struct virgl_box *) &box, 0 /* offset */,
        &snap_iov, 1);
    if (ret) {
        fprintf(stderr,
                VIRTIO_GPU_LOG_PREFIX
                "%s(): virgl transfer_read_iov(id=%u) failed: %d\n",
                __func__, request->resource_id, ret);
        *plen = virtio_gpu_write_ctrl_response(
            vgpu, &request->hdr, response_desc, VIRTIO_GPU_RESP_ERR_UNSPEC);
        if (!*plen)
            virtio_gpu_set_fail(vgpu);
        return;
    }

    /* Publish to every scanout bound to this resource, same loop shape as the
     * 2D flush handler. A full display queue drops the frame rather than
     * stalling.
     */
    for (uint32_t i = 0; i < PRIV(vgpu)->num_scanouts; i++) {
        struct virtio_gpu_scanout_info *scanout = &PRIV(vgpu)->scanouts[i];
        if (!scanout->enabled ||
            scanout->primary_resource_id != request->resource_id)
            continue;
        vgpu_sw_publish_frame(i, scanout, res->readback, res->width,
                              res->height, res->format);
    }

    *plen = virtio_gpu_write_ctrl_response(vgpu, &request->hdr, response_desc,
                                           VIRTIO_GPU_RESP_OK_NODATA);
    if (!*plen)
        virtio_gpu_set_fail(vgpu);
}