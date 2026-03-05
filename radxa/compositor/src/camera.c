#include "camera.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

static int xioctl(int fd, unsigned long request, void *arg)
{
    int r;
    do {
        r = ioctl(fd, request, arg);
    } while (r == -1 && errno == EINTR);
    return r;
}

int camera_init(camera_t *cam, const char *device)
{
    memset(cam, 0, sizeof(*cam));
    cam->zoom_level = 1.0f;

    cam->fd = open(device, O_RDWR | O_NONBLOCK);
    if (cam->fd < 0) {
        perror("camera: open");
        return -1;
    }

    /* Set format: NV12, 1280x720 */
    struct v4l2_format fmt = {0};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = CAM_OUTPUT_W;
    fmt.fmt.pix.height = CAM_OUTPUT_H;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;

    if (xioctl(cam->fd, VIDIOC_S_FMT, &fmt) < 0) {
        perror("camera: VIDIOC_S_FMT");
        close(cam->fd);
        return -1;
    }

    cam->width = fmt.fmt.pix.width;
    cam->height = fmt.fmt.pix.height;
    printf("camera: format set to %dx%d NV12\n", cam->width, cam->height);

    /* Set framerate */
    struct v4l2_streamparm parm = {0};
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = CAM_FPS;
    xioctl(cam->fd, VIDIOC_S_PARM, &parm);

    /* Request buffers */
    struct v4l2_requestbuffers req = {0};
    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (xioctl(cam->fd, VIDIOC_REQBUFS, &req) < 0) {
        perror("camera: VIDIOC_REQBUFS");
        close(cam->fd);
        return -1;
    }
    cam->num_buffers = req.count;

    /* Map buffers */
    for (int i = 0; i < cam->num_buffers; i++) {
        struct v4l2_buffer buf = {0};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (xioctl(cam->fd, VIDIOC_QUERYBUF, &buf) < 0) {
            perror("camera: VIDIOC_QUERYBUF");
            close(cam->fd);
            return -1;
        }

        cam->buffer_sizes[i] = buf.length;
        cam->buffers[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                                MAP_SHARED, cam->fd, buf.m.offset);
        if (cam->buffers[i] == MAP_FAILED) {
            perror("camera: mmap");
            close(cam->fd);
            return -1;
        }
    }

    printf("camera: %d buffers mapped\n", cam->num_buffers);
    return 0;
}

int camera_start(camera_t *cam)
{
    /* Queue all buffers */
    for (int i = 0; i < cam->num_buffers; i++) {
        struct v4l2_buffer buf = {0};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (xioctl(cam->fd, VIDIOC_QBUF, &buf) < 0) {
            perror("camera: VIDIOC_QBUF");
            return -1;
        }
    }

    /* Start streaming */
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(cam->fd, VIDIOC_STREAMON, &type) < 0) {
        perror("camera: VIDIOC_STREAMON");
        return -1;
    }

    printf("camera: streaming started\n");
    return 0;
}

int camera_dequeue(camera_t *cam, uint8_t **frame_data, size_t *frame_len)
{
    struct v4l2_buffer buf = {0};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    if (xioctl(cam->fd, VIDIOC_DQBUF, &buf) < 0) {
        if (errno == EAGAIN)
            return -1; /* no frame ready */
        perror("camera: VIDIOC_DQBUF");
        return -1;
    }

    *frame_data = cam->buffers[buf.index];
    *frame_len = buf.bytesused;
    return buf.index;
}

int camera_enqueue(camera_t *cam, int buf_idx)
{
    struct v4l2_buffer buf = {0};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = buf_idx;

    if (xioctl(cam->fd, VIDIOC_QBUF, &buf) < 0) {
        perror("camera: VIDIOC_QBUF");
        return -1;
    }
    return 0;
}

int camera_set_zoom(camera_t *cam, float zoom)
{
    if (zoom < 1.0f) zoom = 1.0f;
    if (zoom > CAM_MAX_ZOOM) zoom = CAM_MAX_ZOOM;

    cam->zoom_level = zoom;

    /* Calculate crop region centered on sensor */
    int crop_w = (int)(CAM_SENSOR_W / zoom);
    int crop_h = (int)(CAM_SENSOR_H / zoom);
    int crop_x = (CAM_SENSOR_W - crop_w) / 2;
    int crop_y = (CAM_SENSOR_H - crop_h) / 2;

    struct v4l2_selection sel = {0};
    sel.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    sel.target = V4L2_SEL_TGT_CROP;
    sel.r.left = crop_x;
    sel.r.top = crop_y;
    sel.r.width = crop_w;
    sel.r.height = crop_h;

    if (xioctl(cam->fd, VIDIOC_S_SELECTION, &sel) < 0) {
        /* Fallback: try VIDIOC_S_CROP for older drivers */
        struct v4l2_crop crop = {0};
        crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        crop.c.left = crop_x;
        crop.c.top = crop_y;
        crop.c.width = crop_w;
        crop.c.height = crop_h;
        if (xioctl(cam->fd, VIDIOC_S_CROP, &crop) < 0) {
            perror("camera: set zoom (crop)");
            return -1;
        }
    }

    printf("camera: zoom=%.1fx crop=%dx%d+%d+%d\n",
           zoom, crop_w, crop_h, crop_x, crop_y);
    return 0;
}

void camera_close(camera_t *cam)
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    xioctl(cam->fd, VIDIOC_STREAMOFF, &type);

    for (int i = 0; i < cam->num_buffers; i++) {
        if (cam->buffers[i] && cam->buffers[i] != MAP_FAILED) {
            munmap(cam->buffers[i], cam->buffer_sizes[i]);
        }
    }

    close(cam->fd);
    printf("camera: closed\n");
}
