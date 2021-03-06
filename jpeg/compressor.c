#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <setjmp.h>
#include <ctype.h>
#include <jpeglib.h>
#include <jerror.h>

#include <kernel.h>
#include <buffer.h>


#define MAX_ROWSIZE			1600
#define BUFFER_SIZE			(20 * 1024)		// 20 KB

// -------------------- JPEG STRUCT & MANAGE FUNCS -------------
static void jc_yuv422(size_t row, JDIMENSION width, JDIMENSION height, const buffer_t * from, JSAMPLE * to);
static bool js_yuv422(JDIMENSION width, JDIMENSION height, size_t buffersize);

typedef void (*convert_f)(size_t row, JDIMENSION width, JDIMENSION height, const buffer_t * from, JSAMPLE * to);
typedef bool (*sizecheck_f)(JDIMENSION width, JDIMENSION height, size_t buffersize);

typedef struct
{
	int width;
	int height;
	int quality;

	bool isvalid;							// Bool: Are the following libjpeg structs valid and initialized?
	struct jpeg_compress_struct cinfo;
	struct jpeg_error_mgr jerr;
	struct jpeg_destination_mgr dest;

	JSAMPLE row[MAX_ROWSIZE * 3];
	JOCTET buffer[BUFFER_SIZE];

	jmp_buf jmp;
	convert_f converter;
	sizecheck_f sizechecker;

	buffer_t * out;
	size_t out_size;
} jpeg_t;

static bool jpeg_getfmtinfo(char * fmt, convert_f * converter, sizecheck_f * sizechecker)
{
	for (size_t i = 0; i < strlen(fmt); i++)
	{
		fmt[i] = (char)tolower(fmt[i]);
	}

	if (strcmp(fmt, "yuyv") == 0 || strcmp(fmt, "yuv422") == 0)
	{
		*converter = jc_yuv422;
		*sizechecker = js_yuv422;
		return true;
	}

	return false;
}

static void jpeg_freecompress(jpeg_t * jpeg)
{
	if (jpeg->isvalid)
	{
		jpeg_abort_compress(&jpeg->cinfo);
		jpeg_destroy_compress(&jpeg->cinfo);
		jpeg->isvalid = false;
	}

	if (jpeg->out != NULL)
	{
		buffer_free(jpeg->out);
		jpeg->out = NULL;
	}
}

// --------------------------- LIBJPEG ERROR HANDLERS ---------------------
static void jpeg_error_exit(j_common_ptr cinfo)
{
	jpeg_t * jpeg = cinfo->client_data;
	cinfo->err->output_message(cinfo);
	longjmp(jpeg->jmp, 1);
}

static void jpeg_output_message(j_common_ptr cinfo)
{
	LOG(LOG_ERR, "libJPEG Error: %s", cinfo->err->jpeg_message_table[cinfo->err->msg_code]);
}

// -------------------------- LIBJPEG DESTINATION MANAGER HANDLERS --------------
void jpeg_destinit(j_compress_ptr cinfo)
{
	jpeg_t * jpeg = cinfo->client_data;

	cinfo->dest->next_output_byte = jpeg->buffer;
	cinfo->dest->free_in_buffer = BUFFER_SIZE;
}

void jpeg_destterm(j_compress_ptr cinfo)
{
	jpeg_t * jpeg = cinfo->client_data;

	size_t length = jpeg->dest.next_output_byte - jpeg->buffer;
	buffer_write(jpeg->out, jpeg->buffer, jpeg->out_size, length);
	jpeg->out_size += length;
}

boolean jpeg_destempty(j_compress_ptr cinfo)
{
	jpeg_t * jpeg = cinfo->client_data;

	size_t length = BUFFER_SIZE;	// Despite what the internal structures say, this is the length! (BAD libjpeg!)
	buffer_write(jpeg->out, jpeg->buffer, jpeg->out_size, length);
	jpeg->out_size += length;

	jpeg->dest.next_output_byte = jpeg->buffer;
	jpeg->dest.free_in_buffer = BUFFER_SIZE;

	return true;
}

// -------------------------- THE BEEF OF THE MODULE ------------------------
// TODO - add restrict keywords to these functions!
static void jc_yuv422(size_t row, JDIMENSION width, JDIMENSION height, const buffer_t * from, JSAMPLE * to)
{
	// Algorithm developed to use same memory buffer for converting y1,u,y2,v -> y1,u,v,y2,u,v
	// It does, however, corrupt the first y2 value, so we must save and restore

	buffer_read(from, to, row * width * 2, width * 2);

	size_t from_tail = width * 2, to_tail = width * 3;
	char y2 = to[2];
	do
	{
		from_tail -= 4;
		to_tail -= 6;

		to[to_tail + 5] = to[to_tail + 2] = to[from_tail + 3];
		to[to_tail + 4] = to[to_tail + 1] = to[from_tail + 1];
		to[to_tail + 3] = to[from_tail + 2];
		to[to_tail + 0] = to[from_tail + 0];

	} while (from_tail > 0);

	to[3] = y2;	// Restore y2 value
}

static bool js_yuv422(JDIMENSION width, JDIMENSION height, size_t buffersize)
{
	size_t expected = width * height * 2;
	return buffersize == expected;
}

static void compressor_update(void * object)
{
	// Sanity check
	{
		if unlikely(object == NULL)
		{
			return;
		}
	}

	const int * width = input(width);
	const int * height = input(height);
	buffer_t * const * frame = input(frame);

	if (object == NULL || width == NULL || height == NULL || frame == NULL)
	{
		//required input parameters not there!
		return;
	}

	if (*width == 0 || *height == 0)
	{
		//invalid input dimensions
		return;
	}

	if (*width > MAX_ROWSIZE)
	{
		// Rowsize too big!
		LOG1(LOG_ERR, "Could not compress JPEG with width greater than %d", MAX_ROWSIZE);
		return;
	}

	jpeg_t * jpeg = object;
	struct jpeg_compress_struct * cinfo = &jpeg->cinfo;
	struct jpeg_error_mgr * jerr = &jpeg->jerr;
	struct jpeg_destination_mgr * dest = &jpeg->dest;

	if (*width != jpeg->width || *height != jpeg->height)
	{
		jpeg_freecompress(jpeg);
		jpeg->width = *width;
		jpeg->height = *height;
	}

	if (!jpeg->isvalid)
	{
		// Init the compression struct
		cinfo->client_data = jpeg;

		cinfo->err = jpeg_std_error(jerr);
		jerr->error_exit = jpeg_error_exit;
		jerr->output_message = jpeg_output_message;

		jpeg_create_compress(cinfo);

		cinfo->dest = dest;
		dest->init_destination = jpeg_destinit;
		dest->term_destination = jpeg_destterm;
		dest->empty_output_buffer = jpeg_destempty;

		cinfo->image_width = jpeg->width;
		cinfo->image_height = jpeg->height;
		cinfo->input_components = 3;
		cinfo->in_color_space = JCS_YCbCr;

		jpeg_set_defaults(cinfo);
		jpeg_set_quality(cinfo, jpeg->quality, true);
	}

	if (setjmp(jpeg->jmp))
	{
		// An error happened!
		LOG(LOG_WARN, "An exception occurred during JPEG compression, aborting frame");

		// Reset all struct variables
		jpeg_freecompress(jpeg);
		return;
	}
	else
	{
		if (!jpeg->sizechecker(jpeg->width, jpeg->height, buffer_size(*frame)))
		{
			LOG(LOG_WARN, "Invalid frame size given to input of jpeg compressor (width=%d, height=%d): %zu", jpeg->width, jpeg->height, buffer_size(*frame));
			return;
		}

		// Start the compression
		jpeg->out = buffer_new();
		jpeg->out_size = 0;

		jpeg_start_compress(cinfo, true);
		JSAMPROW rowdata[1] = {jpeg->row};

		for (int rownum = 0; rownum < jpeg->height; rownum++)
		{
			jpeg->converter(rownum, jpeg->width, jpeg->height, *frame, jpeg->row);
			jpeg_write_scanlines(cinfo, rowdata, 1);
		}

		jpeg_finish_compress(cinfo);

		output(frame, &jpeg->out);
		buffer_free(jpeg->out);
		jpeg->out = NULL;
	}
}

void * compressor_new(char * format, int quality)
{
	convert_f converter;
	sizecheck_f sizechecker;


	if (!jpeg_getfmtinfo(format, &converter, &sizechecker))
	{
		LOG(LOG_ERR, "libJPEG error: Invalid input format: %s", format);
		return NULL;
	}

	jpeg_t * jpeg = malloc(sizeof(jpeg_t));
	memset(jpeg, 0, sizeof(jpeg_t));
	jpeg->isvalid = false;
	jpeg->quality = quality;
	jpeg->converter = converter;
	jpeg->sizechecker = sizechecker;
	jpeg->out = NULL;

	return jpeg;
}

static void compressor_destroy(void * object)
{
	// Sanity check
	{
		if unlikely(object == NULL)
		{
			return;
		}
	}

	jpeg_t * jpeg = object;
	jpeg_freecompress(jpeg);
	free(jpeg);
}


define_block(	compressor,	"JPEG compressor block", compressor_new, "si", "(1) The input format [currently only YUV422 is supported] (2) The lossy compress quality [1-100]");
block_onupdate(	compressor,	compressor_update);
block_ondestroy(compressor,	compressor_destroy);
block_input(	compressor,	width,	'i',	"The width in pixels of the raw input image");
block_input(	compressor,	height,	'i',	"The hight in pixels of the raw input image");
block_input(	compressor,	frame,	'x',	"The raw frame to compress");
block_output(	compressor,	frame,	'x',	"The compressed JPEG image frame");
