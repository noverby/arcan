/*
 * Copyright 2016, Björn Ståhl
 * License: 3-Clause BSD, see COPYING file in arcan source repository.
 * Reference: http://arcan-fe.com
 * Description: egl-dri specific render-node based backend support
 * library for setting up headless display, and passing handles
 * handling render-node transfer
 */
#define WANT_ARCAN_SHMIF_HELPER
#define AGP_ENABLE_UNPURE
#include "../arcan_shmif.h"
#include "../shmif_privext.h"
#include "video_platform.h"
#include "agp/glfun.h"

#define EGL_EGLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#define MESA_EGL_NO_X11_HEADERS
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <drm.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <gbm.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

/*
 * note: should be moved into the agp_fenv
 */
static PFNEGLCREATEIMAGEKHRPROC create_image;
static PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC query_image_format;
static PFNEGLEXPORTDMABUFIMAGEMESAPROC export_dmabuf;
static PFNEGLDESTROYIMAGEKHRPROC destroy_image;

struct shmif_ext_hidden_int {
	struct gbm_device* dev;
	struct agp_rendertarget* rtgt;

/* with the gbm- buffer passing, we pretty much need double-buf */
	struct storage_info_t buf_a, buf_b, (* current);
	struct agp_fenv fenv;
	bool nopass;
	int fd;

	int type;
	struct {
		bool managed;
		EGLContext context;
		EGLDisplay display;
		EGLSurface surface;
	};
};

/*
 * These are spilled over from AGP, and ideally, we should just
 * separate those references or linker-script erase them as they are
 * not needed here
 */
void* platform_video_gfxsym(const char* sym)
{
	return eglGetProcAddress(sym);
}

bool platform_video_map_handle(struct storage_info_t* store, int64_t handle)
{
	return false;
}

static bool check_functions(void*(*lookup)(void*, const char*), void* tag)
{
	create_image = (PFNEGLCREATEIMAGEKHRPROC)
		lookup(tag, "eglCreateImageKHR");
	destroy_image = (PFNEGLDESTROYIMAGEKHRPROC)
		lookup(tag, "eglDestroyImageKHR");
	query_image_format = (PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC)
		lookup(tag, "eglExportDMABUFImageQueryMESA");
	export_dmabuf = (PFNEGLEXPORTDMABUFIMAGEMESAPROC)
		lookup(tag, "eglExportDMABUFImageMESA");
	return create_image && destroy_image && query_image_format && export_dmabuf;
}

static void zap_vstore(struct storage_info_t* vstore)
{
	free(vstore->vinf.text.raw);
	vstore->vinf.text.raw = NULL;
	vstore->vinf.text.s_raw = 0;
}

static void gbm_drop(struct arcan_shmif_cont* con)
{
	if (!con->privext->internal)
		return;

	struct shmif_ext_hidden_int* in = con->privext->internal;

	if (in->dev){
/* this will actually free the gbm- resources as well */
		if (in->rtgt){
			agp_drop_rendertarget(in->rtgt);
			agp_drop_vstore(&in->buf_a);
			agp_drop_vstore(&in->buf_b);
			zap_vstore(&in->buf_a);
			zap_vstore(&in->buf_b);
		}
		if (in->managed){
			eglMakeCurrent(in->display,
				EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
			eglDestroyContext(in->display, in->context);
			eglTerminate(in->display);
		}
		con->privext->internal->dev = NULL;
	}

	if (-1 != con->privext->internal->fd)
		close(con->privext->internal->fd);

	free(con->privext->internal);
	con->privext->internal = NULL;
	con->privext->cleanup = NULL;
}

struct arcan_shmifext_setup arcan_shmifext_defaults(
	struct arcan_shmif_cont* con)
{
	int major = getenv("AGP_GL_MAJOR") ?
		strtoul(getenv("AGP_GL_MAJOR"), NULL, 10) : 2;

	int minor= getenv("AGP_GL_MINOR") ?
		strtoul(getenv("AGP_GL_MINOR"), NULL, 10) : 1;

	return (struct arcan_shmifext_setup){
		.red = 8, .green = 8, .blue = 8,
		.alpha = 1, .depth = 16,
		.api = API_OPENGL,
		.builtin_fbo = true,
		.major = 2, .minor = 1,
		.shared_context = (uint64_t) EGL_NO_CONTEXT
	};
}

static void* lookup(void* tag, const char* sym)
{
	return eglGetProcAddress(sym);
}

void* arcan_shmifext_lookup(
	struct arcan_shmif_cont* con, const char* fun)
{
	return eglGetProcAddress(fun);
}

static void* lookup_fenv(void* tag, const char* sym, bool req)
{
	return eglGetProcAddress(sym);
}

enum shmifext_setup_status arcan_shmifext_setup(
	struct arcan_shmif_cont* con,
	struct arcan_shmifext_setup arg)
{
	int type;
	struct shmif_ext_hidden_int* ctx = con->privext->internal;
	EGLint nc, cc;

	switch (arg.api){
	case API_OPENGL:
		if ((ctx && ctx->display) || eglBindAPI(EGL_OPENGL_API))
			type = EGL_OPENGL_BIT;
		else
			return SHMIFEXT_NO_API;
	break;
	case API_GLES:
		if ((ctx && ctx->display) || eglBindAPI(EGL_OPENGL_ES_API))
			type = EGL_OPENGL_ES2_BIT;
		else
			return SHMIFEXT_NO_API;
	break;
	case API_VHK:
	default:
/* won't have working code here for a while, first need a working AGP_
 * implementation that works with normal Arcan. Then there's the usual
 * problem with getting access to a handle, for EGLStreams it should
 * work, but with GBM? KRH VKCube has some intel- only hack */
		return SHMIFEXT_NO_API;
	break;
	};
	const EGLint attribs[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RENDERABLE_TYPE, type,
		EGL_RED_SIZE, arg.red,
		EGL_GREEN_SIZE, arg.green,
		EGL_BLUE_SIZE, arg.blue,
		EGL_ALPHA_SIZE, arg.alpha,
		EGL_DEPTH_SIZE, arg.depth,
		EGL_NONE
	};

	if (ctx && ctx->display){
		goto context_only;
	}

	void* display;
	if (!arcan_shmifext_egl(con, &display, lookup, NULL))
		return SHMIFEXT_NO_DISPLAY;

	ctx = con->privext->internal;
	ctx->display = eglGetDisplay((EGLNativeDisplayType) display);
	if (!ctx->display)
		return SHMIFEXT_NO_DISPLAY;

	if (!eglInitialize(ctx->display, NULL, NULL))
		return SHMIFEXT_NO_EGL;

	if (arg.no_context)
		return SHMIFEXT_OK;

context_only:
	if (!eglGetConfigs(ctx->display, NULL, 0, &nc))
		return SHMIFEXT_NO_CONFIG;

	if (0 == nc)
		return SHMIFEXT_NO_CONFIG;

	EGLConfig cfg;
	if (!eglChooseConfig(ctx->display, attribs, &cfg, 1, &nc) || nc < 1)
		return SHMIFEXT_NO_CONFIG;

	EGLint cas[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE,
		EGL_NONE, EGL_NONE, EGL_NONE, EGL_NONE, EGL_NONE, EGL_NONE,
		EGL_NONE, EGL_NONE, EGL_NONE};

	agp_glinit_fenv(&ctx->fenv, lookup_fenv, NULL);

	int ofs = 2;
	if (arg.major){
		cas[ofs++] = EGL_CONTEXT_MAJOR_VERSION_KHR;
		cas[ofs++] = arg.major;
		cas[ofs++] = EGL_CONTEXT_MINOR_VERSION_KHR;
		cas[ofs++] = arg.minor;
	}

	if (arg.mask){
		cas[ofs++] = EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR;
		cas[ofs++] = arg.mask;
	}

	if (arg.flags){
		cas[ofs++] = EGL_CONTEXT_FLAGS_KHR;
		cas[ofs++] = arg.flags;
	}

/* ignore if this function was called without destroying the old context */
	bool context_reuse = false;
	if (!ctx->context){
		ctx->context = eglCreateContext(ctx->display, cfg,
			(EGLContext) arg.shared_context, cas);
	}
	else
		context_reuse = true;

	if (!ctx->context){
		int errc = eglGetError();
		return SHMIFEXT_NO_CONTEXT;
	}

	ctx->surface = EGL_NO_SURFACE;
	ctx->managed = true;
	eglMakeCurrent(ctx->display, ctx->surface, ctx->surface, ctx->context);

	if (arg.builtin_fbo || arg.vidp_pack){
		if (context_reuse){
			agp_drop_vstore(&ctx->buf_a); zap_vstore(&ctx->buf_a);
			agp_drop_vstore(&ctx->buf_b); zap_vstore(&ctx->buf_b);

			if (arg.builtin_fbo)
				agp_drop_rendertarget(ctx->rtgt);
		}

		agp_empty_vstore(&ctx->buf_a, con->w, con->h);
		agp_empty_vstore(&ctx->buf_b, con->w, con->h);
		ctx->current = &ctx->buf_a;

		if (arg.builtin_fbo)
			ctx->rtgt = agp_setup_rendertarget(
				ctx->current, arg.depth > 0 ?
					RENDERTARGET_COLOR_DEPTH_STENCIL : RENDERTARGET_COLOR
			);

		if (arg.vidp_pack){
			ctx->buf_a.vinf.text.s_fmt =
				ctx->buf_b.vinf.text.s_fmt = arg.vidp_infmt;
		}
	}

	arcan_shmifext_make_current(con);
	return SHMIFEXT_OK;
}

bool arcan_shmifext_drop(struct arcan_shmif_cont* con)
{
	if (!con || !con->privext || !con->privext->internal ||
		!con->privext->internal->display)
		return false;

	gbm_drop(con);
	return true;
}

bool arcan_shmifext_drop_context(struct arcan_shmif_cont* con)
{
	if (!con || !con->privext || !con->privext->internal ||
		!con->privext->internal->display)
		return false;

	struct shmif_ext_hidden_int* ctx = con->privext->internal;
	agp_drop_vstore(&ctx->buf_a); zap_vstore(&ctx->buf_a);
	agp_drop_vstore(&ctx->buf_b); zap_vstore(&ctx->buf_b);

	if (ctx->context){
		eglMakeCurrent(ctx->display,
			EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		eglDestroyContext(ctx->display, ctx->context);
		ctx->context = EGL_NO_CONTEXT;
	}

	return true;
}

bool arcan_shmifext_gl_handles(struct arcan_shmif_cont* con,
	uintptr_t* frame, uintptr_t* color, uintptr_t* depth)
{
	if (!con || !con->privext || !con->privext->internal ||
		!con->privext->internal->display || !con->privext->internal->rtgt)
		return false;

	agp_rendertarget_ids(con->privext->internal->rtgt, frame, color, depth);
	return true;
}

bool arcan_shmifext_egl(struct arcan_shmif_cont* con,
	void** display, void*(*lookup)(void*, const char*), void* tag)
{
	if (!lookup || !con || !con->addr || !display)
		return false;

	int dfd = -1;

/* case for switching to another node, we're still missing a way to extract the
 * 'real' library paths to the GL implementation and to the EGL implementation
 * for dynamic- GPU switching */
	if (con->privext->pending_fd != -1){
		if (-1 != con->privext->active_fd){
			close(con->privext->active_fd);
			gbm_drop(con);
		}
		else
			dfd = con->privext->active_fd;
	}
/* or first setup without a pending_fd */
	else if (!con->privext->internal){
		const char* nodestr = getenv("ARCAN_RENDER_NODE") ?
			getenv("ARCAN_RENDER_NODE") : "/dev/dri/renderD128";
		dfd = open(nodestr, O_RDWR | O_CLOEXEC);
	}
/* mode-switch is no-op in init here, but we still may need
 * to update function pointers due to possible context changes */
	else
		return check_functions(lookup, tag);

	if (-1 == dfd)
		return false;

/* special cleanup to deal with gbm_device abstraction */
	con->privext->cleanup = gbm_drop;

/* finally open device */
	if (!con->privext->internal){
		con->privext->internal = malloc(sizeof(struct shmif_ext_hidden_int));
		if (!con->privext->internal){
			gbm_drop(con);
			return false;
		}

		memset(con->privext->internal, '\0', sizeof(struct shmif_ext_hidden_int));
		con->privext->internal->fd = -1;
		con->privext->internal->nopass = getenv("ARCAN_VIDEO_NO_FDPASS") ?
			true : false;
		if (NULL == (con->privext->internal->dev = gbm_create_device(dfd))){
			gbm_drop(con);
			return false;
		}
	}

	if (!check_functions(lookup, tag)){
		gbm_drop(con);
		return false;
	}
	*display = (void*) (con->privext->internal->dev);
	return true;
}

bool arcan_shmifext_egl_meta(struct arcan_shmif_cont* con,
	uintptr_t* display, uintptr_t* surface, uintptr_t* context)
{
	if (!con || !con->privext || !con->privext->internal ||
		!con->privext->internal->display)
		return false;

	struct shmif_ext_hidden_int* ctx = con->privext->internal;
	if (display)
		*display = (uintptr_t) ctx->display;
	if (surface)
		*surface = (uintptr_t) ctx->surface;
	if (context)
		*context = (uintptr_t) ctx->context;

	return true;
}

bool arcan_shmifext_make_current(struct arcan_shmif_cont* con)
{
	if (!con || !con->privext || !con->privext->internal ||
		!con->privext->internal->display)
		return false;

	struct shmif_ext_hidden_int* ctx = con->privext->internal;

	agp_setenv(&ctx->fenv);
	eglMakeCurrent(ctx->display, ctx->surface, ctx->surface, ctx->context);

/* need to resize both potential rendertarget destinations */
	if (ctx->rtgt){
		if (ctx->current->w != con->w || ctx->current->h != con->h){
			agp_activate_rendertarget(NULL);
			agp_resize_rendertarget(ctx->rtgt, con->w, con->h);
		}
		agp_activate_rendertarget(ctx->rtgt);
	}

	return true;
}

int arcan_shmifext_signal(struct arcan_shmif_cont* con,
	uintptr_t display, int mask, uintptr_t tex_id, ...)
{
	if (!con || !con->addr || !con->privext || !con->privext->internal)
		return -1;

	struct shmif_ext_hidden_int* ctx = con->privext->internal;

	EGLDisplay* dpy = display == 0 ?
		con->privext->internal->display : (EGLDisplay*) display;

	if (tex_id == SHMIFEXT_BUILTIN){
		if (!ctx->managed)
			return -1;

/* vidp- to texture streaming, rather than FBO indirection */
		if (!ctx->rtgt){
			enum stream_type type = STREAM_RAW_DIRECT_SYNCHRONOUS;
/*			!(mask & SHMIF_SIGBLK_NONE) ?
				STREAM_RAW_DIRECT_SYNCHRONOUS : STREAM_RAW_DIRECT; */

/* mark this so the backing GLID / PBOs gets reallocated */
			if (ctx->current->w != con->w || ctx->current->h != con->h)
				type = STREAM_EXT_RESYNCH;

/* bpp/format are set during the shmifext_setup */
			ctx->current->w = con->w;
			ctx->current->h = con->h;
			ctx->current->vinf.text.raw = con->vidp;
			ctx->current->vinf.text.s_raw = con->w * con->h * sizeof(shmif_pixel);

/* we ignore the dirty- updates here due to the double buffering */
			struct stream_meta stream = {.buf = NULL};
			stream.buf = con->vidp;
			stream = agp_stream_prepare(ctx->current, stream, type);
			agp_stream_commit(ctx->current, stream);
			ctx->current->vinf.text.raw = NULL;
			struct agp_fenv* env = agp_env();
			env->flush();
		}

/* swap out the color attachment so we don't render to the shared buffer */
		tex_id = ctx->current->vinf.text.glid;
		struct storage_info_t* prev = ctx->current;
		ctx->current = (ctx->current == &ctx->buf_a ? &ctx->buf_b : &ctx->buf_a);
		if (ctx->rtgt){
			struct storage_info_t* a = agp_rendertarget_swap(ctx->rtgt, ctx->current);
			if (a)
				*prev = *a;
		}
	}

	if (!dpy)
		return -1;

	if (con->privext->internal->nopass || !create_image)
		goto fallback;

	EGLImageKHR image = create_image(dpy, eglGetCurrentContext(),
		EGL_GL_TEXTURE_2D_KHR, (EGLClientBuffer)(tex_id), NULL
	);

	if (!image)
		goto fallback;

	int fourcc, nplanes;
	if (!query_image_format(dpy, image, &fourcc, &nplanes, NULL))
		goto fallback;

/* currently unsupported */
	if (nplanes != 1)
		goto fallback;

	EGLint stride;
	int fd;
	if (!export_dmabuf(dpy, image, &fd, &stride, NULL))
		goto fallback;

	unsigned res = arcan_shmif_signalhandle(con, mask, fd, stride, fourcc);
	destroy_image(dpy, image);

	if (con->privext->internal->fd != -1){
		close(con->privext->internal->fd);
	}
	con->privext->internal->fd = fd;
	return res > INT_MAX ? INT_MAX : res;

/*
 * this should really be switched to flipping PBOs, or even better,
 * somehow be able to mark/pin our output buffer for safe readback
 */
fallback:
	if (1){
	struct storage_info_t vstore = {
		.w = con->w,
		.h = con->h,
		.txmapped = TXSTATE_TEX2D,
		.vinf.text = {
			.glid = tex_id,
			.raw = (void*) con->vidp
		},
	};

	if (ctx->rtgt){
		agp_activate_rendertarget(NULL);
		agp_readback_synchronous(&vstore);
		agp_activate_rendertarget(ctx->rtgt);
	}
	else
		agp_readback_synchronous(&vstore);
	}
	res = arcan_shmif_signal(con, mask);
	return res > INT_MAX ? INT_MAX : res;
}
