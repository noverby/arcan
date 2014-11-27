#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <locale.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <libtsm.h>
#include <pwd.h>
#include <signal.h>
#include <poll.h>

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/signal.h>
#include <sys/wait.h>

#include "shl/shl-pty.h"

#include <arcan_shmif.h>
#include "frameserver.h"

#define ARCAN_TTF_SUPPORT

#include "graphing/net_graph.h"

#ifdef ARCAN_TTF_SUPPORT
#include "arcan_ttf.h"
static TTF_Font* font;
#endif

static struct {
	struct tsm_screen* screen;
	struct tsm_vte* vte;
	struct shl_pty* pty;

	pid_t child;
	int child_fd;

	int rows;
	int cols;
	int cell_w, cell_h;
	int screen_w, screen_h;
	struct graph_context* graphing;

	int last_fd;
	struct arcan_shmif_cont arc_conn;
} term = {
	.cell_w = 8,
	.cell_h = 8,
	.rows = 80,
	.cols = 25
};

static void tsm_log(void* data, const char* file, int line,
	const char* func, const char* subs, unsigned int sev,
	const char* fmt, va_list arg)
{
	fprintf(stderr, "[%d] %s:%d - %s, %s()\n", sev, file, line, subs, func);
	vfprintf(stderr, fmt, arg);
}

static void screen_size(int screenw, int screenh, int fontw, int fonth)
{
	int px_w = screenw * fontw;
	int px_h = screenh * fonth;

	if (!arcan_shmif_resize(&term.arc_conn, px_w, px_h)){
		LOG("arcan_shmif_resize(), couldn't set"
			"	requested dimensions (%d, %d)\n", px_w, px_h);
		exit(EXIT_FAILURE);
	}

/*
 * tsm_screen_resize if not first,
 * shl_pty_resize to propagate
 */

	if (term.graphing){
		graphing_destroy(term.graphing);
	}

	term.graphing = graphing_new(px_w, px_h, (uint32_t*) term.arc_conn.vidp);

	term.screen_w = screenw;
	term.screen_h = screenh;
	term.cell_w = fontw;
	term.cell_h = fonth;
}

struct unpack_col {
	union{
		struct {
			uint8_t r;
			uint8_t g;
			uint8_t b;
			uint8_t a;
		};

		uint32_t rgba;
	};
};

static int draw_cb(struct tsm_screen* screen, uint32_t id,
	const uint32_t* ch, size_t len, unsigned width, unsigned x, unsigned y,
	const struct tsm_screen_attr* attr, tsm_age_t age, void* data)
{
	uint8_t fgc[3], bgc[3];
	uint8_t* dfg = fgc, (* dbg) = bgc;
	int base_y = y * term.cell_h;
	int base_x = x * term.cell_w;

	if (attr->inverse){
		dfg = bgc;
		dbg = fgc;
	}

	dfg[0] = attr->fr;
	dfg[1] = attr->fg;
	dfg[2] = attr->fb;
	dbg[0] = attr->br;
	dbg[1] = attr->bg;
	dbg[2] = attr->bb;

	draw_box(term.graphing, base_x, base_y, term.cell_w + 1,
		term.cell_h + 1, RGBA(bgc[0], bgc[1], bgc[2], 0xff));

	size_t u8_sz = tsm_ucs4_get_width(*ch) + 1;
	uint8_t u8_ch[u8_sz];
	size_t nch = tsm_ucs4_to_utf8(*ch, (char*) u8_ch);

	if (nch == 0 || u8_ch[0] == 0)
		return 0;

	if (font == NULL){
/* without proper ttf support, we just go with '?' for unknowns */
		u8_ch[0] = u8_ch[0] <= 128 ? u8_ch[0] : '?';
		u8_ch[1] = '\0';

		draw_text(term.graphing, (const char*) u8_ch, base_x, base_y,
			RGBA(fgc[0], fgc[1], fgc[2], 0xff)
		);
		return 0;
	}

/* interesting toggle, using typeface embedded images vs.
 * non-bitmap for the same glyph */
	u8_ch[u8_sz-1] = '\0';
	TTF_Color fg = {.r = fgc[0], .g = fgc[1], .b = fgc[2]};
	TTF_Surface* surf = TTF_RenderUTF8(font, (char*) u8_ch, fg);
	if (!surf)
		return 0;

	size_t w = term.arc_conn.addr->w;
	uint32_t* dst = (uint32_t*) term.arc_conn.vidp;

/* alpha blending against background is the more tedious bits here */
	for (int row = 0; row < surf->height; row++)
		for (int col = 0; col < surf->width; col++){
			uint8_t* bgra = (uint8_t*) &surf->data[ row * surf->stride + (col * 4) ];
			float fact = (float)bgra[3] / 255.0;
			float ifact = 1.0 - fact;
			off_t ofs = (row + base_y) * w + col + base_x;

			struct unpack_col incl;
			incl.rgba = dst[ofs];
			dst[ofs] = RGBA(
				incl.r * ifact + fgc[0] * fact,
				incl.g * ifact + fgc[1] * fact,
				incl.b * ifact + fgc[2] * fact,
				0xff
			);
		}

	free(surf);

	return 0;
}

static void read_callback(struct shl_pty* pty,
	void* data, char* u8, size_t len)
{
	tsm_vte_input(term.vte, u8, len);
}

static void write_callback(struct tsm_vte* vte,
	const char* u8, size_t len, void* data)
{
	shl_pty_write(term.pty, u8, len);
}

static void setup_shell()
{
	char* shell = getenv("SHELL");
	const struct passwd* pass = getpwuid( getuid() );
	if (pass){
		setenv("LOGNAME", pass->pw_name, 1);
		setenv("USER", pass->pw_name, 1);
		setenv("SHELL", pass->pw_shell, 0);
		setenv("HOME", pass->pw_dir, 0);
	}

	unsetenv("COLUMNS");
	unsetenv("LINES");
	unsetenv("TERMCAP");

	int sigs[] = {
		SIGCHLD, SIGHUP, SIGINT, SIGQUIT, SIGTERM, SIGALRM
	};

	setenv("TERM", "xterm-256color", 1);
	for (int i = 0; i < sizeof(sigs); i++)
		signal(sigs[i], SIG_DFL);

	char* args[] = {shell, "-i", NULL};

	execvp(args[0], args);
	exit(EXIT_FAILURE);
}

/*
 * keyboard mapping is not correct,
 * and we should have an (even more) detailed version that also
 * carries 7-bit ascii
 * mouse-mapping should go to a local cursor that hides on no motion
 */
static void ioev_ctxtbl(arcan_ioevent* ioev, const char* label)
{
/* map mouse motion + button to select, etc. */
	if (label){
	}

/* keyboard input */
	if (ioev->datatype == EVENT_IDATATYPE_TRANSLATED){
		bool pressed = ioev->input.translated.active;
		if (!pressed)
			return;

		tsm_vte_handle_keyboard(term.vte,
			ioev->input.translated.keysym,
			ioev->input.translated.keysym,
			ioev->input.translated.modifiers,
			ioev->input.translated.subid
		);
	}
	else if (ioev->datatype == EVENT_IDATATYPE_DIGITAL){
	}
	else if (ioev->datatype == EVENT_IDATATYPE_ANALOG){
	}
}

static void targetev(arcan_event* tgtev)
{
/*	arcan_tgtevent* ev = &tgtev->data.target; */

	switch (tgtev->kind){
	case TARGET_COMMAND_FDTRANSFER:
		term.last_fd = frameserver_readhandle(tgtev);
	break;

/* switch palette? */
	case TARGET_COMMAND_GRAPHMODE:
	break;

/* sigsuspend */
	case TARGET_COMMAND_PAUSE:
	break;

/* sigresume */
	case TARGET_COMMAND_UNPAUSE:
	break;

	case TARGET_COMMAND_RESET:
		tsm_vte_reset(term.vte);
	break;

/* dump raw to child out, just read/iterate until EOF
 * and map through tsm_vte_input */
	case TARGET_COMMAND_STORE:
	break;

/* redirect raw to parent */
	case TARGET_COMMAND_RESTORE:
	break;

	case TARGET_COMMAND_EXIT:
		exit(EXIT_SUCCESS);
	break;

	default:
	break;
	}
}

/*
 * this is still a bit rough;
 * 1. find a way to select on both client data,
 *    parent events, with a periodic timeout
 *
 * 2. use timeout to implement cursor blink (if requested)
 */
static void main_loop()
{
	while(true){
		arcan_event ev;

		int rc = shl_pty_dispatch(term.pty);
		if (0 < rc){
			printf("pty dispatch fail\n");
		}

		while (arcan_event_poll(&term.arc_conn.inev, &ev) == 1){
			switch (ev.category){
			case EVENT_IO:
				ioev_ctxtbl(&(ev.data.io), ev.label);
			break;

			case EVENT_TARGET:
				targetev(&ev);
			break;

			default:
			break;
			}

/* map up: set_palette, reset, hard_reset, input, handle_keyboard,
 * move_to, move_up, move_down, move_left, move_right, move_line_end,
 * move_line_home, tab_right, tab_left, insert_lines, delete_lines,
 * erase_cursor, erase_chars, ... selection reset, selection start,
 * selection copy, ... */
		}

		tsm_screen_draw(term.screen, draw_cb, NULL /* draw_cb_data */);
		arcan_shmif_signal(&term.arc_conn, SHMIF_SIGVID);
	}
}

int arcan_frameserver_terminal_run(
	struct arcan_shmif_cont* con,
	struct arg_arr* args)
{
	const char* val;
	TTF_Init();

	if (arg_lookup(args, "rows", 0, &val))
		term.rows = strtoul(val, NULL, 10);

	if (arg_lookup(args, "cols", 0, &val))
		term.cols = strtoul(val, NULL, 10);

	if (arg_lookup(args, "cell_w", 0, &val))
		term.cell_w = strtoul(val, NULL, 10);

	if (arg_lookup(args, "cell_h", 0, &val))
		term.cell_h = strtoul(val, NULL, 10);

	if (arg_lookup(args, "font", 0, &val)){
		font = TTF_OpenFont(val, term.cell_h);
		if (!font)
			LOG("font %s could not be opened, forcing built-in fallback\n", val);
	}
	else
		LOG("no font argument specified, forcing built-in fallback.\n");

	if (font && arg_lookup(args, "font_hint", 0, &val)){
		if (strcmp(val, "light") == 0)
			TTF_SetFontHinting(font, TTF_HINTING_LIGHT);
		else if (strcmp(val, "mono") == 0)
			TTF_SetFontHinting(font, TTF_HINTING_MONO);
		else if (strcmp(val, "none") == 0)
			TTF_SetFontHinting(font, TTF_HINTING_NONE);
		else
			LOG("unknown font hinting requested, "
				"accepted values(light, mono, none)");
	}

	if (tsm_screen_new(&term.screen, tsm_log, 0) < 0){
		LOG("fatal, couldn't setup tsm screen\n");
		return EXIT_FAILURE;
	}

	if (tsm_vte_new(&term.vte, term.screen, write_callback,
		NULL /* write_cb_data */, tsm_log, NULL /* tsm_log_data */) < 0){
		LOG("fatal, couldn't setup vte\n");
		return EXIT_FAILURE;
	}

	term.arc_conn = *con;
	if (!term.arc_conn.addr){
		LOG("fatal, couldn't map shared memory from (%s)\n", keyfile);
	}

	screen_size(term.rows, term.cols, term.cell_w, term.cell_h);
	tsm_screen_set_max_sb(term.screen, 1000);

	setlocale(LC_CTYPE, "");

/* possible need to track this and run shl_pty_close */
	signal(SIGHUP, SIG_IGN);

	if ( (term.child = shl_pty_open(&term.pty,
		read_callback, NULL /* term data */, 80, 25)) == 0){
		setup_shell();
		exit(EXIT_FAILURE);
	}

	if (term.child < 0){
		LOG("couldn't spawn child terminal.\n");
		return EXIT_FAILURE;
	}

	arcan_event outev = {
		.kind = EVENT_EXTERNAL_REGISTER,
		.category = EVENT_EXTERNAL,
		.data.external.registr.title = "ArcTerm",
		.data.external.registr.kind = SEGID_SHELL
	};

	arcan_event_enqueue(&term.arc_conn.outev, &outev);
	main_loop();
	return EXIT_SUCCESS;
}