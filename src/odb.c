/*
 * This file is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2,
 * as published by the Free Software Foundation.
 *
 * In addition to the permissions in the GNU General Public License,
 * the authors give you unlimited permission to link the compiled
 * version of this file into combinations with other programs,
 * and to distribute those combinations without any restriction
 * coming from the use of this file.  (The General Public License
 * restrictions do apply in other respects; for example, they cover
 * modification of the file, and distribution when not linked into
 * a combined executable.)
 *
 * This file is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "common.h"
#include "git/odb.h"
#include "git/zlib.h"
#include "fileops.h"
#include "hash.h"
#include "odb.h"

#define GIT_PACK_NAME_MAX (5 + 40 + 1)
struct git_pack {
	git_odb *db;
	git_lck lock;

	/** Functions to access idx_map. */
	int (*idx_search)(
		off_t *,
		struct git_pack *,
		const git_oid *);

	/** The .idx file, mapped into memory. */
	git_file idx_fd;
	git_map idx_map;
	uint32_t *im_fanout;
	unsigned char *im_oid;
	uint32_t *im_crc;
	uint32_t *im_offset32;
	uint32_t *im_offset64;

	/** Number of objects in this pack. */
	uint32_t obj_cnt;

	/** Number of git_packlist we appear in. */
	unsigned int refcnt;

	/** Number of active users of the idx_map data. */
	unsigned int idxcnt;
	unsigned
		invalid:1 /* the pack is unable to be read by libgit2 */
		;

	/** Name of the pack file(s), without extension ("pack-abc"). */
	char pack_name[GIT_PACK_NAME_MAX];

	/** The .pack file, mapped into memory. */
	git_file pack_fd;
	git_map pack_map;
};
typedef struct git_pack git_pack;

typedef struct {
	size_t n_packs;
	unsigned int refcnt;
	git_pack *packs[GIT_FLEX_ARRAY];
} git_packlist;

struct git_odb {
	git_lck lock;

	/** Path to the "objects" directory. */
	char *objects_dir;

	/** Known pack files from ${objects_dir}/packs. */
	git_packlist *packlist;

	/** Alternate databases to search. */
	git_odb **alternates;
	size_t n_alternates;

	/** loose object zlib compression level. */
	int object_zlib_level;
	/** loose object file fsync flag. */
	int fsync_object_files;
};

typedef struct {  /* object header data */
	git_otype type;         /* object type */
	size_t    size;         /* object size */
	off_t     base_offset;  /* delta base offset (GIT_OBJ_OFS_DELTA) */
	git_oid   base_name;    /* delta base name (GIT_OBJ_REF_DELTA) */
} obj_hdr;

static struct {
	const char *str;   /* type name string */
	int        loose;  /* valid loose object type flag */
} obj_type_table [] = {
	{ "",          0 },  /* 0 = GIT_OBJ__EXT1     */
	{ "commit",    1 },  /* 1 = GIT_OBJ_COMMIT    */
	{ "tree",      1 },  /* 2 = GIT_OBJ_TREE      */
	{ "blob",      1 },  /* 3 = GIT_OBJ_BLOB      */
	{ "tag",       1 },  /* 4 = GIT_OBJ_TAG       */
	{ "",          0 },  /* 5 = GIT_OBJ__EXT2     */
	{ "OFS_DELTA", 0 },  /* 6 = GIT_OBJ_OFS_DELTA */
	{ "REF_DELTA", 0 }   /* 7 = GIT_OBJ_REF_DELTA */
};

GIT_INLINE(uint32_t) decode32(void *b)
{
	return ntohl(*((uint32_t*)b));
}

GIT_INLINE(uint64_t) decode64(void *b)
{
	uint32_t *p = b;
	return (((uint64_t)ntohl(p[0])) << 32) | ntohl(p[1]);
}

const char *git_obj_type_to_string(git_otype type)
{
	if (type < 0 || type >= ARRAY_SIZE(obj_type_table))
		return "";
	return obj_type_table[type].str;
}

git_otype git_obj_string_to_type(const char *str)
{
	int i;

	if (!str || !*str)
		return GIT_OBJ_BAD;

	for (i = 0; i < ARRAY_SIZE(obj_type_table); i++)
		if (!strcmp(str, obj_type_table[i].str))
			return (git_otype) i;

	return GIT_OBJ_BAD;
}

int git_obj__loose_object_type(git_otype type)
{
	if (type < 0 || type >= ARRAY_SIZE(obj_type_table))
		return 0;
	return obj_type_table[type].loose;
}

static int format_object_header(char *hdr, size_t n, git_obj *obj)
{
	const char *type_str = git_obj_type_to_string(obj->type);
	int len = snprintf(hdr, n, "%s %"PRIuZ, type_str, obj->len);

	assert(len > 0);  /* otherwise snprintf() is broken */
	assert(len < n);  /* otherwise the caller is broken! */

	if (len < 0 || len >= n)
		return GIT_ERROR;
	return len+1;
}

static int hash_obj(git_oid *id, char *hdr, size_t n, int *len, git_obj *obj)
{
	git_buf_vec vec[2];
	int  hdrlen;

	assert(id && hdr && len && obj);

	if (!git_obj__loose_object_type(obj->type))
		return GIT_ERROR;

	if (!obj->data && obj->len != 0)
		return GIT_ERROR;

	if ((hdrlen = format_object_header(hdr, n, obj)) < 0)
		return GIT_ERROR;

	*len = hdrlen;

	vec[0].data = hdr;
	vec[0].len  = hdrlen;
	vec[1].data = obj->data;
	vec[1].len  = obj->len;

	git_hash_vec(id, vec, 2);

	return GIT_SUCCESS;
}

int git_obj_hash(git_oid *id, git_obj *obj)
{
	char hdr[64];
	int  hdrlen;

	assert(id && obj);

	return hash_obj(id, hdr, sizeof(hdr), &hdrlen, obj);
}

static size_t object_file_name(char *name, size_t n, char *dir, const git_oid *id)
{
	size_t len = strlen(dir);

	/* check length: 43 = 40 hex sha1 chars + 2 * '/' + '\0' */
	if (len+43 > n)
		return len+43;

	/* the object dir: eg $GIT_DIR/objects */
	strcpy(name, dir);
	if (name[len-1] != '/')
		name[len++] = '/';

	/* loose object filename: aa/aaa... (41 bytes) */
	git_oid_pathfmt(&name[len], id);
	name[len+41] = '\0';

	return 0;
}

static int is_zlib_compressed_data(unsigned char *data)
{
	unsigned int w;

	w = ((unsigned int)(data[0]) << 8) + data[1];
	return data[0] == 0x78 && !(w %31);
}

static size_t get_binary_object_header(obj_hdr *hdr, gitfo_buf *obj)
{
	unsigned char c;
	unsigned char *data = obj->data;
	size_t shift, size, used = 0;
	off_t base_offset;

	if (obj->len == 0)
		return 0;

	c = data[used++];
	hdr->type = (c >> 4) & 7;

	size = c & 15;
	shift = 4;
	while (c & 0x80) {
		if (obj->len <= used)
			return 0;
		if (sizeof(size_t) * 8 <= shift)
			return 0;
		c = data[used++];
		size += (c & 0x7f) << shift;
		shift += 7;
	}
	hdr->size = size;

	hdr->base_offset = 0;
	hdr->base_name.id[0] = '\0';

	if (hdr->type == GIT_OBJ_OFS_DELTA) {
		c = data[used++];
		base_offset = c & 127;
		while (c & 128) {
			base_offset++;
			if (!base_offset || MSB(base_offset, 7))
				return 0;  /* overflow */
			c = data[used++];
			base_offset = (base_offset << 7) + (c & 127);
		}
		assert(base_offset > 0);
		hdr->base_offset = base_offset;
	}
	else if (hdr->type == GIT_OBJ_REF_DELTA) {
		git_oid_mkraw(&hdr->base_name, data + used);
		used += 20;
	}

	return used;
}

static size_t get_object_header(obj_hdr *hdr, unsigned char *data)
{
	char c, typename[10];
	size_t size, used = 0;

	/*
	 * type name string followed by space.
	 */
	while ((c = data[used]) != ' ') {
		typename[used++] = c;
		if (used >= sizeof(typename))
			return 0;
	}
	typename[used] = 0;
	if (used == 0)
		return 0;
	hdr->type = git_obj_string_to_type(typename);
	used++;  /* consume the space */

	/*
	 * length follows immediately in decimal (without
	 * leading zeros).
	 */
	size = data[used++] - '0';
	if (size > 9)
		return 0;
	if (size) {
		while ((c = data[used]) != '\0') {
			size_t d = c - '0';
			if (d > 9)
				break;
			used++;
			size = size * 10 + d;
		}
	}
	hdr->size = size;

	/*
	 * the length must be followed by a zero byte
	 */
	if (data[used++] != '\0')
		return 0;

	return used;
}

static void init_stream(z_stream *s, void *out, size_t len)
{
	memset(s, 0, sizeof(*s));
	s->next_out  = out;
	s->avail_out = len;
}

static void set_stream_input(z_stream *s, void *in, size_t len)
{
	s->next_in  = in;
	s->avail_in = len;
}

static void set_stream_output(z_stream *s, void *out, size_t len)
{
	s->next_out  = out;
	s->avail_out = len;
}

static int start_inflate(z_stream *s, gitfo_buf *obj, void *out, size_t len)
{
	int status;

	init_stream(s, out, len);
	set_stream_input(s, obj->data, obj->len);

	if ((status = inflateInit(s)) < Z_OK)
		return status;

	return inflate(s, 0);
}

static int finish_inflate(z_stream *s)
{
	int status = Z_OK;

	while (status == Z_OK)
		status = inflate(s, Z_FINISH);

	inflateEnd(s);

	if ((status != Z_STREAM_END) || (s->avail_in != 0))
		return GIT_ERROR;

	return GIT_SUCCESS;
}

static void *inflate_tail(z_stream *s, void *hb, size_t used, obj_hdr *hdr)
{
	unsigned char *buf, *head = hb;
	size_t tail;

	/*
	 * allocate a buffer to hold the inflated data and copy the
	 * initial sequence of inflated data from the tail of the
	 * head buffer, if any.
	 */
	if ((buf = git__malloc(hdr->size + 1)) == NULL) {
		inflateEnd(s);
		return NULL;
	}
	tail = s->total_out - used;
	if (used > 0 && tail > 0) {
		if (tail > hdr->size)
			tail = hdr->size;
		memcpy(buf, head + used, tail);
	}
	used = tail;

	/*
	 * inflate the remainder of the object data, if any
	 */
	if (hdr->size < used)
		inflateEnd(s);
	else {
		set_stream_output(s, buf + used, hdr->size - used);
		if (finish_inflate(s)) {
			free(buf);
			return NULL;
		}
	}

	return buf;
}

static int inflate_buffer(void *in, size_t inlen, void *out, size_t outlen)
{
	z_stream zs;
	int status = Z_OK;

	init_stream(&zs, out, outlen);
	set_stream_input(&zs, in, inlen);

	if (inflateInit(&zs) < Z_OK)
		return GIT_ERROR;

	while (status == Z_OK)
		status = inflate(&zs, Z_FINISH);

	inflateEnd(&zs);

	if (status != Z_STREAM_END)
		return GIT_ERROR;

	if (zs.total_out != outlen)
		return GIT_ERROR;

	return GIT_SUCCESS;
}

/*
 * At one point, there was a loose object format that was intended to
 * mimic the format used in pack-files. This was to allow easy copying
 * of loose object data into packs. This format is no longer used, but
 * we must still read it.
 */
static int inflate_packlike_loose_disk_obj(git_obj *out, gitfo_buf *obj)
{
	unsigned char *in, *buf;
	obj_hdr hdr;
	size_t len, used;

	/*
	 * read the object header, which is an (uncompressed)
	 * binary encoding of the object type and size.
	 */
	if ((used = get_binary_object_header(&hdr, obj)) == 0)
		return GIT_ERROR;

	if (!git_obj__loose_object_type(hdr.type))
		return GIT_ERROR;

	/*
	 * allocate a buffer and inflate the data into it
	 */
	buf = git__malloc(hdr.size + 1);
	if (!buf)
		return GIT_ERROR;

	in  = ((unsigned char *)obj->data) + used;
	len = obj->len - used;
	if (inflate_buffer(in, len, buf, hdr.size)) {
		free(buf);
		return GIT_ERROR;
	}
	buf[hdr.size] = '\0';

	out->data = buf;
	out->len  = hdr.size;
	out->type = hdr.type;

	return GIT_SUCCESS;
}

static int inflate_disk_obj(git_obj *out, gitfo_buf *obj)
{
	unsigned char head[64], *buf;
	z_stream zs;
	int z_status;
	obj_hdr hdr;
	size_t used;

	/*
	 * check for a pack-like loose object
	 */
	if (!is_zlib_compressed_data(obj->data))
		return inflate_packlike_loose_disk_obj(out, obj);

	/*
	 * inflate the initial part of the io buffer in order
	 * to parse the object header (type and size).
	 */
	if ((z_status = start_inflate(&zs, obj, head, sizeof(head))) < Z_OK)
		return GIT_ERROR;

	if ((used = get_object_header(&hdr, head)) == 0)
		return GIT_ERROR;

	if (!git_obj__loose_object_type(hdr.type))
		return GIT_ERROR;

	/*
	 * allocate a buffer and inflate the object data into it
	 * (including the initial sequence in the head buffer).
	 */
	if ((buf = inflate_tail(&zs, head, used, &hdr)) == NULL)
		return GIT_ERROR;
	buf[hdr.size] = '\0';

	out->data = buf;
	out->len  = hdr.size;
	out->type = hdr.type;

	return GIT_SUCCESS;
}

static int make_temp_file(git_file *fd, char *tmp, size_t n, char *file)
{
	char *template = "/tmp_obj_XXXXXX";
	size_t tmplen = strlen(template);
	size_t dirlen;

	if ((dirlen = git__dirname(tmp, n, file)) < 0)
		return GIT_ERROR;

	if ((dirlen + tmplen) >= n)
		return GIT_ERROR;

	strcpy(tmp + dirlen, (dirlen) ? template : template + 1);

	*fd = gitfo_mkstemp(tmp);
	if (*fd < 0 && dirlen) {
		/* create directory if it doesn't exist */
		tmp[dirlen] = '\0';
		if ((gitfo_exists(tmp) < 0) && gitfo_mkdir(tmp, 0755))
			return GIT_ERROR;
		/* try again */
		strcpy(tmp + dirlen, template);
		*fd = gitfo_mkstemp(tmp);
	}
	if (*fd < 0)
		return GIT_ERROR;

	return GIT_SUCCESS;
}

static int deflate_buf(z_stream *s, void *in, size_t len, int flush)
{
	int status = Z_OK;

	set_stream_input(s, in, len);
	while (status == Z_OK) {
		status = deflate(s, flush);
		if (s->avail_in == 0)
			break;
	}
	return status;
}

static int deflate_obj(gitfo_buf *buf, char *hdr, int hdrlen, git_obj *obj, int level)
{
	z_stream zs;
	int status;
	size_t size;

	assert(buf && !buf->data && hdr && obj);
	assert(level == Z_DEFAULT_COMPRESSION || (level >= 0 && level <= 9));

	buf->data = NULL;
	buf->len  = 0;
	init_stream(&zs, NULL, 0);

	if (deflateInit(&zs, level) < Z_OK)
		return GIT_ERROR;

	size = deflateBound(&zs, hdrlen + obj->len);

	if ((buf->data = git__malloc(size)) == NULL) {
		deflateEnd(&zs);
		return GIT_ERROR;
	}

	set_stream_output(&zs, buf->data, size);

	/* compress the header */
	status = deflate_buf(&zs, hdr, hdrlen, Z_NO_FLUSH);

	/* if header compressed OK, compress the object */
	if (status == Z_OK)
		status = deflate_buf(&zs, obj->data, obj->len, Z_FINISH);

	if (status != Z_STREAM_END) {
		deflateEnd(&zs);
		free(buf->data);
		buf->data = NULL;
		return GIT_ERROR;
	}

	buf->len = zs.total_out;
	deflateEnd(&zs);

	return GIT_SUCCESS;
}

static int write_obj(gitfo_buf *buf, git_oid *id, git_odb *db)
{
	char file[GIT_PATH_MAX];
	char temp[GIT_PATH_MAX];
	git_file fd;

	if (object_file_name(file, sizeof(file), db->objects_dir, id))
		return GIT_ERROR;

	if (make_temp_file(&fd, temp, sizeof(temp), file) < 0)
		return GIT_ERROR;

	if (gitfo_write(fd, buf->data, buf->len) < 0) {
		gitfo_close(fd);
		gitfo_unlink(temp);
		return GIT_ERROR;
	}

	if (db->fsync_object_files)
		gitfo_fsync(fd);
	gitfo_close(fd);
	gitfo_chmod(temp, 0444);

	if (gitfo_move_file(temp, file) < 0) {
		gitfo_unlink(temp);
		return GIT_ERROR;
	}

	return GIT_SUCCESS;
}

static int open_alternates(git_odb *db)
{
	unsigned n = 0;

	gitlck_lock(&db->lock);
	if (db->alternates) {
		gitlck_unlock(&db->lock);
		return 1;
	}

	db->alternates = git__malloc(sizeof(*db->alternates) * (n + 1));
	if (!db->alternates) {
		gitlck_unlock(&db->lock);
		return -1;
	}

	db->alternates[n] = NULL;
	db->n_alternates = n;
	gitlck_unlock(&db->lock);
	return 0;
}

static int pack_openidx_map(git_pack *p)
{
	char pb[GIT_PATH_MAX];
	off_t len;

	if (git__fmt(pb, sizeof(pb), "%s/pack/%s.idx",
			p->db->objects_dir,
			p->pack_name) < 0)
		return GIT_ERROR;

	if ((p->idx_fd = gitfo_open(pb, O_RDONLY)) < 0)
		return GIT_ERROR;

	if ((len = gitfo_size(p->idx_fd)) < 0
		|| !git__is_sizet(len)
		|| gitfo_map_ro(&p->idx_map, p->idx_fd, 0, (size_t)len)) {
		gitfo_close(p->idx_fd);
		return GIT_ERROR;
	}

	return GIT_SUCCESS;
}

static int idxv1_search(off_t *out, git_pack *p, const git_oid *id)
{
	unsigned char *data = p->im_oid;
	size_t lo = id->id[0] ? p->im_fanout[id->id[0] - 1] : 0;
	size_t hi = p->im_fanout[id->id[0]];

	do {
		size_t mid = (lo + hi) >> 1;
		size_t pos = 24 * mid;
		int cmp = memcmp(id->id, data + pos + 4, 20);
		if (cmp < 0)
			hi = mid;
		else if (!cmp) {
			*out = decode32(data + pos);
			return GIT_SUCCESS;
		} else
			lo = mid + 1;
	} while (lo < hi);
	return GIT_ENOTFOUND;
}

static int pack_openidx_v1(git_pack *p)
{
	uint32_t *src_fanout = p->idx_map.data;
	uint32_t *im_fanout;
	size_t expsz;
	int j;

	if ((im_fanout = git__malloc(sizeof(*im_fanout) * 256)) == NULL)
		return GIT_ERROR;

	im_fanout[0] = decode32(&src_fanout[0]);
	for (j = 1; j < 256; j++) {
		im_fanout[j] = decode32(&src_fanout[j]);
		if (im_fanout[j] < im_fanout[j - 1]) {
			free(im_fanout);
			return GIT_ERROR;
		}
	}
	p->obj_cnt = im_fanout[255];

	expsz = 4 * 256 + 24 * p->obj_cnt + 2 * 20;
	if (expsz != p->idx_map.len) {
		free(im_fanout);
		return GIT_ERROR;
	}

	p->idx_search = idxv1_search;
	p->im_fanout = im_fanout;
	p->im_oid = (unsigned char*)(src_fanout + 256);
	return GIT_SUCCESS;
}

static int idxv2_search(off_t *out, git_pack *p, const git_oid *id)
{
	unsigned char *data = p->im_oid;
	size_t lo = id->id[0] ? p->im_fanout[id->id[0] - 1] : 0;
	size_t hi = p->im_fanout[id->id[0]];

	do {
		size_t mid = (lo + hi) >> 1;
		size_t pos = 20 * mid;
		int cmp = memcmp(id->id, data + pos, 20);
		if (cmp < 0)
			hi = mid;
		else if (!cmp) {
			uint32_t o32 = decode32(p->im_offset32 + mid);
			if (o32 & 0x80000000)
				*out = decode64(p->im_offset64 + 2*(o32 & ~0x80000000));
			else
				*out = o32;
			return GIT_SUCCESS;
		} else
			lo = mid + 1;
	} while (lo < hi);
	return GIT_ENOTFOUND;
}

static int pack_openidx_v2(git_pack *p)
{
	unsigned char *data = p->idx_map.data;
	uint32_t *src_fanout = (uint32_t*)(data + 8);
	uint32_t *im_fanout;
	int j;

	if ((im_fanout = git__malloc(sizeof(*im_fanout) * 256)) == NULL)
		return GIT_ERROR;

	im_fanout[0] = decode32(&src_fanout[0]);
	for (j = 1; j < 256; j++) {
		im_fanout[j] = decode32(&src_fanout[j]);
		if (im_fanout[j] < im_fanout[j - 1]) {
			free(im_fanout);
			return GIT_ERROR;
		}
	}
	p->obj_cnt = im_fanout[255];

	p->idx_search = idxv2_search;
	p->im_fanout = im_fanout;
	p->im_oid = (unsigned char*)(src_fanout + 256);
	p->im_crc = (uint32_t*)(p->im_oid + 20 * p->obj_cnt);
	p->im_offset32 = p->im_crc + p->obj_cnt;
	p->im_offset64 = p->im_offset32 + p->obj_cnt;
	return GIT_SUCCESS;
}

static int pack_openidx(git_pack *p)
{
	gitlck_lock(&p->lock);
	if (p->invalid)
		goto unlock_fail;
	if (++p->idxcnt == 1 && !p->idx_search) {
		uint32_t *data;

		if (pack_openidx_map(p))
			goto invalid_fail;
		data = p->idx_map.data;

		if (decode32(&data[0]) == PACK_TOC) {
			switch (decode32(&data[1])) {
			case 2:
				if (pack_openidx_v2(p))
					goto unmap_fail;
				break;
			default:
				goto unmap_fail;
			}
		} else if (pack_openidx_v1(p))
			goto unmap_fail;
	}
	gitlck_unlock(&p->lock);
	return GIT_SUCCESS;

unmap_fail:
	gitfo_free_map(&p->idx_map);

invalid_fail:
	p->invalid = 1;
	p->idxcnt--;

unlock_fail:
	gitlck_unlock(&p->lock);
	return GIT_ERROR;
}

static int pack_openpack_map(git_pack *p)
{
	char pb[GIT_PATH_MAX];
	off_t len;

	if (git__fmt(pb, sizeof(pb), "%s/pack/%s.pack",
			p->db->objects_dir,
			p->pack_name) < 0)
		return GIT_ERROR;

	if ((p->pack_fd = gitfo_open(pb, O_RDONLY)) < 0)
		return GIT_ERROR;

	if ((len = gitfo_size(p->pack_fd)) < 0
			|| !git__is_sizet(len)
			|| gitfo_map_ro(&p->pack_map, p->pack_fd, 0, (size_t)len)) {
		gitfo_close(p->pack_fd);
		return GIT_ERROR;
	}

	return GIT_SUCCESS;
}

static int pack_openpack(git_pack *p)
{
	gitlck_lock(&p->lock);
	if (p->invalid)
		goto unlock_fail;
	if (p->pack_fd < 0) {
		uint32_t *data;

		if (pack_openpack_map(p))
			goto invalid_fail;
		data = p->pack_map.data;

		if (decode32(&data[0]) != PACK_HDR)
			goto unmap_fail;
	}
	gitlck_unlock(&p->lock);
	return GIT_SUCCESS;

unmap_fail:
	gitfo_free_map(&p->pack_map);

invalid_fail:
	p->invalid = 1;
	p->pack_fd = -1;

unlock_fail:
	gitlck_unlock(&p->lock);
	return GIT_ERROR;
}

static void pack_decidx(git_pack *p)
{
	gitlck_lock(&p->lock);
	p->idxcnt--;
	gitlck_unlock(&p->lock);
}

static void pack_dec(git_pack *p)
{
	int need_free;

	gitlck_lock(&p->lock);
	need_free = !--p->refcnt;
	gitlck_unlock(&p->lock);

	if (need_free) {
		if (p->idx_search) {
			gitfo_free_map(&p->idx_map);
			gitfo_close(p->idx_fd);
			free(p->im_fanout);
		}
		if (p->pack_fd >= 0) {
			gitfo_free_map(&p->pack_map);
			gitfo_close(p->pack_fd);
			p->pack_fd = -1;
		}

		gitlck_free(&p->lock);
		free(p);
	}
}

static void packlist_dec(git_odb *db, git_packlist *pl)
{
	int need_free;

	gitlck_lock(&db->lock);
	need_free = !--pl->refcnt;
	gitlck_unlock(&db->lock);

	if (need_free) {
		size_t j;
		for (j = 0; j < pl->n_packs; j++)
			pack_dec(pl->packs[j]);
		free(pl);
	}
}

static git_pack *alloc_pack(const char *pack_name)
{
	git_pack *p = git__calloc(1, sizeof(*p));
	if (!p)
		return NULL;

	gitlck_init(&p->lock);
	strcpy(p->pack_name, pack_name);
	p->refcnt = 1;
	p->pack_fd = -1;
	return p;
}

struct scanned_pack {
	struct scanned_pack *next;
	git_pack *pack;
};

static int scan_one_pack(void *state, char *name)
{
	struct scanned_pack **ret = state, *r;
	char *s = strrchr(name, '/'), *d;

	if (git__prefixcmp(s + 1, "pack-")
	 || git__suffixcmp(s, ".pack")
	 || strlen(s + 1) != GIT_PACK_NAME_MAX + 4)
		return 0;

	d = strrchr(s + 1, '.');
	strcpy(d + 1, "idx");    /* "pack-abc.pack" -> "pack-abc.idx" */
	if (gitfo_exists(name))
		return 0;

	if ((r = git__malloc(sizeof(*r))) == NULL)
		return GIT_ERROR;

	*d = '\0';               /* "pack-abc.pack" -_> "pack-abc" */
	if ((r->pack = alloc_pack(s + 1)) == NULL) {
		free(r);
		return GIT_ERROR;
	}

	r->next = *ret;
	*ret = r;
	return GIT_SUCCESS;
}

static git_packlist* scan_packs(git_odb *db)
{
	char pb[GIT_PATH_MAX];
	struct scanned_pack *state = NULL, *c;
	size_t cnt;
	git_packlist *new_list;

	if (git__fmt(pb, sizeof(pb), "%s/pack", db->objects_dir) < 0)
		return NULL;
	gitfo_dirent(pb, sizeof(pb), scan_one_pack, &state);

	/* TODO - merge old entries into the new array */
	for (cnt = 0, c = state; c; c = c->next)
		cnt++;
	new_list = git__malloc(sizeof(*new_list)
		+ (sizeof(new_list->packs[0]) * cnt));
	if (!new_list)
		goto fail;

	for (cnt = 0, c = state; c; ) {
		struct scanned_pack *n = c->next;
		c->pack->db = db;
		new_list->packs[cnt++] = c->pack;
		free(c);
		c = n;
	}
	new_list->n_packs = cnt;
	new_list->refcnt = 2;
	db->packlist = new_list;
	return new_list;

fail:
	while (state) {
		struct scanned_pack *n = state->next;
		pack_dec(state->pack);
		free(state);
		state = n;
	}
	return NULL;
}

static git_packlist *packlist_get(git_odb *db)
{
	git_packlist *pl;

	gitlck_lock(&db->lock);
	if ((pl = db->packlist) != NULL)
		pl->refcnt++;
	else
		pl = scan_packs(db);
	gitlck_unlock(&db->lock);
	return pl;
}

static int search_packs(git_pack **p, off_t *offset, git_odb *db, const git_oid *id)
{
	git_packlist *pl = packlist_get(db);
	size_t j;

	if (!pl)
		return GIT_ENOTFOUND;

	for (j = 0; j < pl->n_packs; j++) {

		git_pack *pack = pl->packs[j];
		off_t pos;
		int res;

		if (pack_openidx(pack))
			continue;
		res = pack->idx_search(&pos, pack, id);
		pack_decidx(pack);

		if (!res) {
			packlist_dec(db,pl);
			if (p)
				*p = pack;
			if (offset)
				*offset = pos;
			return GIT_SUCCESS;
		}

	}

	packlist_dec(db,pl);
	return GIT_ENOTFOUND;
}

static int exists_packed(git_odb *db, const git_oid *id)
{
	return !search_packs(NULL, NULL, db, id);
}

static int exists_loose(git_odb *db, const git_oid *id)
{
	char file[GIT_PATH_MAX];

	if (object_file_name(file, sizeof(file), db->objects_dir, id))
		return 0;

	if (gitfo_exists(file) < 0)
		return 0;

	return 1;
}

int git_odb_exists(git_odb *db, const git_oid *id)
{
	/* TODO: extend to search alternate db's */
	if (exists_packed(db, id))
		return 1;
	return exists_loose(db, id);
}

int git_odb_open(git_odb **out, const char *objects_dir)
{
	git_odb *db = git__calloc(1, sizeof(*db));
	if (!db)
		return GIT_ERROR;

	db->objects_dir = git__strdup(objects_dir);
	if (!db->objects_dir) {
		free(db);
		return GIT_ERROR;
	}

	gitlck_init(&db->lock);

	db->object_zlib_level = Z_BEST_SPEED;
	db->fsync_object_files = 0;

	*out = db;
	return GIT_SUCCESS;
}

void git_odb_close(git_odb *db)
{
	git_packlist *pl;

	if (!db)
		return;

	gitlck_lock(&db->lock);

	pl = db->packlist;
	db->packlist = NULL;

	if (db->alternates) {
		git_odb **alt;
		for (alt = db->alternates; *alt; alt++)
			git_odb_close(*alt);
		free(db->alternates);
	}

	free(db->objects_dir);

	gitlck_unlock(&db->lock);
	if (pl)
		packlist_dec(db, pl);
	gitlck_free(&db->lock);
	free(db);
}

int git_odb_read(
	git_obj *out,
	git_odb *db,
	const git_oid *id)
{
attempt:
	if (!git_odb__read_packed(out, db, id))
		return GIT_SUCCESS;
	if (!git_odb__read_loose(out, db, id))
		return GIT_SUCCESS;
	if (!open_alternates(db))
		goto attempt;

	out->data = NULL;
	return GIT_ENOTFOUND;
}

int git_odb__read_loose(git_obj *out, git_odb *db, const git_oid *id)
{
	char file[GIT_PATH_MAX];
	gitfo_buf obj = GITFO_BUF_INIT;

	assert(out && db && id);

	out->data = NULL;
	out->len  = 0;
	out->type = GIT_OBJ_BAD;

	if (object_file_name(file, sizeof(file), db->objects_dir, id))
		return GIT_ENOTFOUND;  /* TODO: error handling */

	if (gitfo_read_file(&obj, file))
		return GIT_ENOTFOUND;  /* TODO: error handling */

	if (inflate_disk_obj(out, &obj)) {
		gitfo_free_buf(&obj);
		return GIT_ENOTFOUND;  /* TODO: error handling */
	}

	gitfo_free_buf(&obj);

	return GIT_SUCCESS;
}

static int read_packed(git_obj *out, git_pack *p, const git_oid *id)
{
	off_t pos;
	int res;

	if (pack_openidx(p))
		return GIT_ERROR;
	res = p->idx_search(&pos, p, id);
	pack_decidx(p);

	if (!res) {
		/* TODO unpack object at pos */
		res = GIT_ERROR;
	}

	return res;
}

int git_odb__read_packed(git_obj *out, git_odb *db, const git_oid *id)
{
	git_packlist *pl = packlist_get(db);
	size_t j;

	if (!pl)
		return GIT_ENOTFOUND;

	for (j = 0; j < pl->n_packs; j++) {
		if (!read_packed(out, pl->packs[j], id)) {
			packlist_dec(db, pl);
			return GIT_SUCCESS;
		}
	}

	packlist_dec(db, pl);
	return GIT_ENOTFOUND;
}

int git_odb_write(git_oid *id, git_odb *db, git_obj *obj)
{
	char hdr[64];
	int  hdrlen;
	gitfo_buf buf = GITFO_BUF_INIT;

	assert(id && db && obj);

	if (hash_obj(id, hdr, sizeof(hdr), &hdrlen, obj) < 0)
		return GIT_ERROR;

	if (git_odb_exists(db, id))
		return GIT_SUCCESS;

	if (deflate_obj(&buf, hdr, hdrlen, obj, db->object_zlib_level) < 0)
		return GIT_ERROR;

	if (write_obj(&buf, id, db) < 0) {
		gitfo_free_buf(&buf);
		return GIT_ERROR;
	}

	gitfo_free_buf(&buf);

	return GIT_SUCCESS;
}

