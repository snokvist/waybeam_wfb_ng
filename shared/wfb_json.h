/* wfb_json.h — minimal jsmn-shaped JSON tokenizer (header-only).
 *
 * Single source of truth for the small read-only JSON parsing used across the
 * project. Extracted verbatim from ground/gs_supervisor.c's in-file parser so
 * config-env (multicall) and any other consumer can share it without a second
 * implementation. Functions are `static inline` so multiple translation units
 * can include it with no link conflicts; gs_supervisor.c still carries its own
 * (identical) copy for now — folding it onto this header is a trivial cleanup.
 *
 * One-shot tokenizer: a flat array of tokens with parent links; traversal
 * helpers walk the array. Zero allocations, no recursion in the parser.
 * Strings are NOT unescaped — plain ASCII keys/values only.
 */
#ifndef WFB_JSON_H
#define WFB_JSON_H

#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { JT_NONE = 0, JT_OBJ, JT_ARR, JT_STR, JT_PRIM } JTokType;

typedef struct {
	JTokType type;
	int      start;
	int      end;
	int      size;
	int      parent;
} JTok;

#define JSON_MAX_TOKS 512

typedef struct {
	const char *js;
	int         len;
	int         pos;
	JTok       *toks;
	int         tok_max;
	int         tok_count;
	int         super;   /* index of current container */
} JParser;

static inline JTok *wfb_json_alloc(JParser *p)
{
	if (p->tok_count >= p->tok_max) return NULL;
	JTok *t = &p->toks[p->tok_count++];
	t->type = JT_NONE; t->start = -1; t->end = -1; t->size = 0; t->parent = -1;
	return t;
}

static inline int wfb_json_parse_prim(JParser *p)
{
	int start = p->pos;
	for (; p->pos < p->len; p->pos++) {
		char c = p->js[p->pos];
		if (c == ',' || c == '}' || c == ']' ||
		    c == ' ' || c == '\t' || c == '\n' || c == '\r')
			break;
	}
	JTok *t = wfb_json_alloc(p);
	if (!t) return -1;
	t->type = JT_PRIM; t->start = start; t->end = p->pos; t->parent = p->super;
	if (p->super >= 0) p->toks[p->super].size++;
	p->pos--;
	return 0;
}

static inline int wfb_json_parse_str(JParser *p)
{
	int start = ++p->pos;  /* skip opening quote */
	for (; p->pos < p->len; p->pos++) {
		char c = p->js[p->pos];
		if (c == '"') {
			JTok *t = wfb_json_alloc(p);
			if (!t) return -1;
			t->type = JT_STR; t->start = start; t->end = p->pos; t->parent = p->super;
			if (p->super >= 0) p->toks[p->super].size++;
			return 0;
		}
		if (c == '\\' && p->pos + 1 < p->len) p->pos++;  /* skip escape */
	}
	return -1;
}

static inline int json_parse(const char *js, int len, JTok *toks, int max)
{
	JParser p = { js, len, 0, toks, max, 0, -1 };
	for (; p.pos < p.len; p.pos++) {
		char c = p.js[p.pos];
		if (c == '{' || c == '[') {
			JTok *t = wfb_json_alloc(&p);
			if (!t) return -1;
			t->type = (c == '{') ? JT_OBJ : JT_ARR;
			t->start = p.pos; t->end = -1; t->parent = p.super;
			if (p.super >= 0) p.toks[p.super].size++;
			p.super = p.tok_count - 1;
		} else if (c == '}' || c == ']') {
			JTokType expect = (c == '}') ? JT_OBJ : JT_ARR;
			if (p.super < 0) return -1;
			if (p.toks[p.super].type != expect) return -1;
			p.toks[p.super].end = p.pos + 1;
			int s = p.super;
			p.super = p.toks[s].parent;
			while (p.super >= 0 && p.toks[p.super].end != -1)
				p.super = p.toks[p.super].parent;
		} else if (c == '"') {
			if (wfb_json_parse_str(&p) < 0) return -1;
		} else if (c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
		           c == ':' || c == ',') {
			/* skip */
		} else {
			if (wfb_json_parse_prim(&p) < 0) return -1;
		}
	}
	if (p.super != -1) return -1;
	return p.tok_count;
}

static inline bool jeq(const char *js, const JTok *t, const char *s)
{
	if (t->type != JT_STR) return false;
	int slen = (int)strlen(s);
	return (t->end - t->start) == slen && strncmp(js + t->start, s, (size_t)slen) == 0;
}

/* Advance past a token + all its descendants. Returns next index. */
static inline int jskip(const JTok *toks, int n, int i)
{
	if (i >= n) return n;
	if (toks[i].type == JT_OBJ) {
		int pairs = toks[i].size / 2;
		int j = i + 1;
		while (pairs-- > 0) {
			j = jskip(toks, n, j);     /* key */
			j = jskip(toks, n, j);     /* value */
		}
		return j;
	}
	if (toks[i].type == JT_ARR) {
		int kids = toks[i].size;
		int j = i + 1;
		while (kids-- > 0) j = jskip(toks, n, j);
		return j;
	}
	return i + 1;
}

/* Find a child of object `obj_idx` by key. Returns idx of value, or -1. */
static inline int jfind(const char *js, const JTok *toks, int n, int obj_idx, const char *key)
{
	if (obj_idx < 0 || obj_idx >= n || toks[obj_idx].type != JT_OBJ) return -1;
	int pairs = toks[obj_idx].size / 2;
	int i = obj_idx + 1;
	for (int k = 0; k < pairs; k++) {
		if (jeq(js, &toks[i], key)) return i + 1;
		i = jskip(toks, n, i);     /* key */
		i = jskip(toks, n, i);     /* value */
	}
	return -1;
}

static inline int jstr(const char *js, const JTok *t, char *out, size_t cap)
{
	if (!t || t->type != JT_STR) { out[0] = 0; return -1; }
	int slen = t->end - t->start;
	if (slen < 0 || (size_t)slen >= cap) slen = (int)cap - 1;
	memcpy(out, js + t->start, (size_t)slen);
	out[slen] = 0;
	return slen;
}

static inline int jint(const char *js, const JTok *t, long *out)
{
	if (!t || t->type != JT_PRIM) return -1;
	char buf[32];
	int slen = t->end - t->start;
	if (slen <= 0 || slen >= (int)sizeof(buf)) return -1;
	memcpy(buf, js + t->start, (size_t)slen);
	buf[slen] = 0;
	char *end = NULL;
	long v = strtol(buf, &end, 10);
	if (!end || *end) return -1;
	*out = v;
	return 0;
}

static inline int jbool(const char *js, const JTok *t, bool *out)
{
	if (!t || t->type != JT_PRIM) return -1;
	int slen = t->end - t->start;
	if (slen == 4 && !strncmp(js + t->start, "true",  4)) { *out = true;  return 0; }
	if (slen == 5 && !strncmp(js + t->start, "false", 5)) { *out = false; return 0; }
	return -1;
}

#ifdef __cplusplus
}
#endif

#endif /* WFB_JSON_H */
