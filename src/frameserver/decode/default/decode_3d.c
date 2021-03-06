#include <arcan_shmif.h>
#include <arcan_shmif_sub.h>

#include "decode.h"

#define TINYOBJ_LOADER_C_IMPLEMENTATION
#include "parsers/tinyobj_loader_c.h"

static int process_obj(struct arcan_shmif_cont* C, char* buf, size_t buf_sz)
{
	tinyobj_attrib_t attrib;
	tinyobj_shape_t* shapes = NULL;
	size_t num_shapes;
	tinyobj_material_t* materials = NULL;
	size_t num_materials;

	int rv = tinyobj_parse_obj(&attrib, &shapes, &num_shapes,
		&materials, &num_materials, buf, buf_sz, 1 /* triangulate */);

/* tinyobj_parse_mtl_file */
/* also don't have a way to communicate animations */

	free(buf);
	if (rv != TINYOBJ_SUCCESS)
		return EXIT_FAILURE;

	tinyobj_attrib_free(&attrib);
/* tinyobj_materials_free */
	return EXIT_SUCCESS;
}

int decode_3d(struct arcan_shmif_cont* cont, struct arg_arr* args)
{
	const char* file = NULL;
	size_t inbuf_sz = 0;
	char* inbuf = NULL;

/* two modes we want to support, 'single file' once mode and as a decoder
 * daemon that gets fed files repeatedly in order to save setup */
	if (arg_lookup(args, "file", 0, &file)){
		FILE* fpek = fopen(file, "r");
		if (!fpek){
			char buf[64];
			snprintf(buf, sizeof(buf), "couldn't open %s", file);
			return show_use(cont, buf);
		}

		fseek(fpek, 0, SEEK_END);
		long pos = ftell(fpek);
		fseek(fpek, 0, SEEK_SET);
		if (pos <= 0){
			char buf[64];
			snprintf(buf, sizeof(buf), "invalid length (%ld) in %s", pos, file);
			fclose(fpek);
			return show_use(cont, buf);
		}

/* read the whole thing in memory, the catch 22 is that we need to parse
 * to know the reasonable size so we can set the context to that, but we
 * want to drop privileges before then, so need to do it in stages */
		inbuf = malloc(pos);
		if (!inbuf){
			fclose(fpek);
			return show_use(cont, "out of memory");
		}

		if (1 != fread(inbuf, pos, 1, fpek)){
			fclose(fpek);
			free(inbuf);
			return show_use(cont, "couldn't load");
		}

		inbuf_sz = pos;
		fclose(fpek);
	}

	if (!file)
		return show_use(cont, "missing file argument");

	if (!inbuf_sz)
		return show_use(cont, "no data in file");

/* there are other considerations here as well that we don't treat right
 * for the time being, mainly material data (which can be unpacked into vbufs
 * and sent that way, i.e. color, bump, normal, specular). To get further with
 * this, consider only allowing the directory that file points to. */
	arcan_shmif_privsep(cont, "shmif", NULL, 0);

/* assume .obj for now */
	return process_obj(cont, inbuf, inbuf_sz);

/* now we know the real unpack size, so resize to that */
	return EXIT_SUCCESS;
}
