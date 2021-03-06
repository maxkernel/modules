#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

#include <kernel.h>
#include <buffer.h>


#define NUM_BUFFERS		1

typedef struct
{
	void * start;
	size_t length;
} videobuffer_t;

typedef struct
{
	char * path;
	int fd;

	int width, height;

	unsigned int format;
	char * format_str;

	videobuffer_t * buffers;
	size_t n_buffers;
} webcam_t;

static unsigned int webcam_getformat(char * fmt)
{
	for (size_t i = 0; i < strlen(fmt); i++)
	{
		fmt[i] = tolower(fmt[i]);
	}

	if (strcmp(fmt, "yuv420") == 0)
	{
		return V4L2_PIX_FMT_YUV420;
	}
	else if (strcmp(fmt, "yuv422") == 0 || strcmp(fmt, "yuyv") == 0)
	{
		return V4L2_PIX_FMT_YUYV;
	}
	else if (strcmp(fmt, "mjpeg") == 0)
	{
		return V4L2_PIX_FMT_MJPEG;
	}

	return 0;
}

static int webcam_open(char * path)
{
	struct stat st;

	if (stat(path, &st) == -1) {
			LOG(LOG_ERR, "Webcam: Cannot identify '%s': %s", path, strerror(errno));
			return -1;
	}

	if (!S_ISCHR(st.st_mode)) {
			LOG(LOG_ERR, "Webcam: %s is no device", path);
			return -1;
	}

	int fd = open(path, O_RDWR | O_NONBLOCK, 0);
	if (fd == -1) {
			LOG(LOG_ERR, "Webcam: Cannot open '%s': %s", path, strerror(errno));
			return -1;
	}

	return fd;
}

static bool webcam_init(webcam_t * webcam)
{
	struct v4l2_capability cap;
	memset(&cap, 0, sizeof(struct v4l2_capability));

	if (ioctl(webcam->fd, VIDIOC_QUERYCAP, &cap) == -1) {
		if (errno == EINVAL) {
			LOG(LOG_ERR, "Webcam: %s is not a V4L2 device", webcam->path);
			return false;
		} else {
			LOG(LOG_ERR, "Webcam: Could not call VIDIOC_QUERYCAP on %s: %s", webcam->path, strerror(errno));
			return false;
		}
	}

	if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
		LOG(LOG_ERR, "Webcam: %s is not a video capture device", webcam->path);
		return false;
	}

	if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
		LOG(LOG_ERR, "Webcam: %s does not support streaming I/O", webcam->path);
		return false;
	}


	struct v4l2_format fmt;
	memset(&fmt, 0, sizeof(struct v4l2_format));
	fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width       = webcam->width;
	fmt.fmt.pix.height      = webcam->height;
	fmt.fmt.pix.pixelformat = webcam->format;
	fmt.fmt.pix.field       = V4L2_FIELD_ANY;

	if (ioctl(webcam->fd, VIDIOC_S_FMT, &fmt) == -1)
	{
		LOG(LOG_ERR, "Webcam: Could not set video parameters (%dx%d, %s) on %s: %s", webcam->width, webcam->height, webcam->format_str, webcam->path, strerror(errno));
		return false;
	}

	webcam->width = fmt.fmt.pix.width;
	webcam->height = fmt.fmt.pix.height;
	webcam->format = fmt.fmt.pix.pixelformat;


	return true;
}

static bool webcam_start(webcam_t * webcam)
{
	//Initialize memmap
	struct v4l2_requestbuffers req;
	memset(&req, 0, sizeof(struct v4l2_requestbuffers));
	req.count               = NUM_BUFFERS;
	req.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory              = V4L2_MEMORY_MMAP;

	if (ioctl(webcam->fd, VIDIOC_REQBUFS, &req) == -1) {
		if (errno == EINVAL) {
			LOG(LOG_ERR, "Webcam: %s does not support memory mapping", webcam->path);
			return false;
		} else {
			LOG(LOG_ERR, "Webcam: Could not call VIDIOC_REQBUFS on %s: %s", webcam->path, strerror(errno));
			return false;
		}
	}

	if (req.count != NUM_BUFFERS) {
		LOG(LOG_ERR, "Webcam: Insufficient buffer memory on %s. Requested %d, got %d", webcam->path, NUM_BUFFERS, req.count);
		return false;
	}

	webcam->buffers = malloc(sizeof(videobuffer_t) * req.count);
	memset(webcam->buffers, 0, sizeof(videobuffer_t) * req.count);
	if (!webcam->buffers) {
		LOG(LOG_ERR, "Webcam: Out of buffer memory");
		return false;
	}

	for (webcam->n_buffers = 0; webcam->n_buffers < req.count; webcam->n_buffers++)
	{
		struct v4l2_buffer buf;
		memset(&buf, 0, sizeof(struct v4l2_buffer));
		buf.type        = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory      = V4L2_MEMORY_MMAP;
		buf.index       = webcam->n_buffers;

		if (ioctl(webcam->fd, VIDIOC_QUERYBUF, &buf) == -1)
		{
			LOG(LOG_ERR, "Webcam: Could not call VIDIOC_QUERYBUF on %s: %s", webcam->path, strerror(errno));
			return false;
		}

		webcam->buffers[webcam->n_buffers].length = buf.length;
		webcam->buffers[webcam->n_buffers].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, webcam->fd, buf.m.offset);

		if (webcam->buffers[webcam->n_buffers].start == MAP_FAILED)
		{
			LOG(LOG_ERR, "Webcam: Could not mmap buffers on %s: %s", webcam->path, strerror(errno));
			webcam->buffers[webcam->n_buffers].start = 0;
			return false;
		}
	}

	for (size_t i = 0; i<webcam->n_buffers; i++)
	{
		struct v4l2_buffer buf;
		memset(&buf, 0, sizeof(struct v4l2_buffer));
		buf.type        = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory      = V4L2_MEMORY_MMAP;
		buf.index       = i;

		if (ioctl(webcam->fd, VIDIOC_QBUF, &buf) == -1)
		{
			LOG(LOG_ERR, "Webcam: Could not call VIDIOC_QBUF on %s: %s", webcam->path, strerror(errno));
			return false;
		}
	}

	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (ioctl(webcam->fd, VIDIOC_STREAMON, &type) == -1)
	{
		LOG(LOG_ERR, "Webcam: Could not call VIDIOC_STREAMON on %s: %s", webcam->path, strerror(errno));
		return false;
	}

	return true;
}

static bool webcam_stop(webcam_t * webcam)
{
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (ioctl(webcam->fd, VIDIOC_STREAMOFF, &type) == -1)
	{
		LOG(LOG_ERR, "Webcam: Could not call VIDIOC_STREAMOFF on %s: %s", webcam->path, strerror(errno));
		return false;
	}

	if (webcam->buffers != NULL)
	{
		for (size_t i = 0; i < webcam->n_buffers; i++)
		{
			if (munmap(webcam->buffers[i].start, webcam->buffers[i].length) == -1)
			{
				LOG(LOG_ERR, "Webcam: could not munmap buffer %zu on %s", i, webcam->path);
			}
		}
		free(webcam->buffers);
		webcam->buffers = NULL;
	}

	return true;
}

static bool webcam_readframe(webcam_t * webcam, buffer_t * frame)
{
	struct v4l2_buffer buf;
	memset(&buf, 0, sizeof(struct v4l2_buffer));
	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buf.memory = V4L2_MEMORY_MMAP;

	if (ioctl(webcam->fd, VIDIOC_DQBUF, &buf) == -1) {
		LOG1(LOG_ERR, "Webcam: Could not call VIDIOC_DQBUF (dequeue buffer) on %s: %s", webcam->path, strerror(errno));
		return false;
	}

	if (buf.index >= webcam->n_buffers)
	{
		LOG(LOG_ERR, "Webcam: Buffer overrun detected on capture buffers");
		return false;
	}

	bool success = true;
	size_t size = webcam->buffers[buf.index].length;
	if (buffer_write(frame, webcam->buffers[buf.index].start, 0, size) != size)
	{
		LOG1(LOG_WARN, "Could not write all webcam frame data to buffer");
		success = false;
	}

	if (ioctl(webcam->fd, VIDIOC_QBUF, &buf) == -1)
	{
		LOG1(LOG_ERR, "Webcam: Could not call VIDIOC_QBUF (enqueue buffer) on %s: %s", webcam->path, strerror(errno));
	}

	return success;
}

void * webcam_new(char * path, char * format, int width, int height)
{
	int fd = webcam_open(path);
	if (fd == -1)
	{
		LOG(LOG_ERR, "Failure to open webcam %s", path);
		return NULL;
	}

	webcam_t * webcam = malloc(sizeof(webcam_t));
	memset(webcam, 0, sizeof(webcam_t));
	webcam->fd = fd;
	webcam->path = strdup(path);
	webcam->format = webcam_getformat(format);
	webcam->format_str = strdup(format);
	webcam->width = width;
	webcam->height = height;

	if (format == 0)
	{
		LOG(LOG_ERR, "Unknown webcam format: %s. Cannot initialize", format);
		return NULL;
	}

	if (!webcam_init(webcam))
	{
		LOG(LOG_ERR, "Failure to initialize webcam %s", path);
		return NULL;
	}

	if (!webcam_start(webcam))
	{
		LOG(LOG_ERR, "Failure to start streaming webcam %s", path);
		return NULL;
	}

	LOG(LOG_DEBUG, "Webcam %s is up and running.", path);

	return webcam;
}

void webcam_destroy(void * object)
{
	webcam_t * webcam = object;
	if (webcam == NULL)
	{
		return;
	}

	if (!webcam_stop(webcam))
	{
		LOG(LOG_ERR, "Webcam: Could not stop streaming on %s", webcam->path);
	}

	close(webcam->fd);
	webcam->fd = -1;
}

void webcam_update(void * object)
{
	webcam_t * webcam = object;
	if (webcam == NULL)
	{
		return;
	}

	const int * width = input(width);
	const int * height = input(height);

	if (width != NULL && height != NULL && (*width != webcam->width || *height != webcam->height))
	{
		webcam->width = *width;
		webcam->height = *height;

		webcam_destroy(object);
		webcam->fd = webcam_open(webcam->path);
		if (webcam->fd == -1)
		{
			LOG(LOG_ERR, "Failure to reopen webcam %s on change of parameters", webcam->path);
			return;
		}

		if (!webcam_init(webcam))
		{
			LOG(LOG_ERR, "Webcam: Could not set video parameters on %s", webcam->path);
			return;
		}

		if (!webcam_start(webcam))
		{
			LOG(LOG_ERR, "Webcam: Could not start video streaming on %s", webcam->path);
			return;
		}
	}

	fd_set fds;
	struct timeval tv = {0};
	int r;

	FD_ZERO(&fds);
	FD_SET(webcam->fd, &fds);

	// TODO IMPORTANT - WTF, don't select here!
	if ((r = select(webcam->fd+1, &fds, NULL, NULL, &tv)) > 0)
	{
		buffer_t * frame = buffer_new();
		if (webcam_readframe(webcam, frame))
		{
			output(width, &webcam->width);
			output(height, &webcam->height);
			output(frame, &frame);

			//LOG(LOG_INFO, "FRAME");
		}
		buffer_free(frame);
	}
	else if (r < 0)
	{
		LOG(LOG_WARN, "Could not call select on camera %s: %s", webcam->path, strerror(errno));
	}
	else
	{
		//LOG(LOG_INFO, "NO FRAME");
	}
}


module_name("Webcam");
module_version(1,0,0);
module_author("Andrew Klofas - andrew@maxkernel.com");
module_description("Captures webcam frames (video4linux2) and outputs YUV format");

define_block(webcam, "A webcam block", webcam_new, "ssii", "(1) The device file [eg. /dev/video0] (2) The output format [eg. yuv422] (3) The initial capture width (4) The initial capture height");
block_input(		webcam, 	width, 		'i', 	"Requested frame capture width. Changing this may cause momentary image artifacts");
block_input(		webcam, 	height, 	'i', 	"Requested frame capture height. Changing this may cause momentary image artifacts");
block_output(		webcam, 	width, 		'i', 	"Actual frame capture width");
block_output(		webcam, 	height, 	'i', 	"Actual frame capture height");
block_output(		webcam, 	frame, 		'x', 	"The captured frame");
// TODO - add timestamp output
block_onupdate(		webcam, 	webcam_update);
block_ondestroy(	webcam, 	webcam_destroy);
