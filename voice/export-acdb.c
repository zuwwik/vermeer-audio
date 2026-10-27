// SPDX-License-Identifier: GPL-2.0-only
/* Read-only ACDB export using AudioReach's public acdb_ioctl API.
 * No firmware upload or access to an audio device is performed here.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "acdb.h"

#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error ACDB export currently requires a little-endian host
#endif

#define MAX_BLOB (4U * 1024 * 1024)
#define MAX_SUBGRAPHS 64

struct profile {
	const char *name;
	AcdbGraphKeyVector gkv;
};

/* Numeric metadata from Vermeer's Android PAL/AGM call log. Do not use ACDB
 * enumeration indices: those change when another database adds a graph.
 */
static AcdbKeyValuePair handset_rx[] = {
	{0xa1000000, 0xa1000010}, {0xa2000000, 0xa2000004},
	{0xac000000, 0xac000005}, {0xaf000000, 0xaf000001},
	{0xb3000000, 0xb3000001},
};
static AcdbKeyValuePair speaker_rx[] = {
	{0xa1000000, 0xa1000010}, {0xa2000000, 0xa2000001},
	{0xac000000, 0xac000005}, {0xaf000000, 0xaf000001},
	{0xb3000000, 0xb3000001},
};
static AcdbKeyValuePair handset_tx[] = {
	{0xa3000000, 0xa3000004}, {0xad000000, 0xad000009},
	{0xb1000000, 0xb1000007}, {0xb3000000, 0xb3000001},
};
static AcdbKeyValuePair speaker_tx[] = {
	{0xa3000000, 0xa3000001}, {0xad000000, 0xad000009},
	{0xb1000000, 0xb1000007}, {0xb3000000, 0xb3000001},
};
#define PROFILE(n, v) {n, {sizeof(v) / sizeof(v[0]), v}}
static struct profile profiles[] = {
	PROFILE("handset-rx", handset_rx),
	PROFILE("handset-tx", handset_tx),
	PROFILE("speaker-rx", speaker_rx),
	PROFILE("speaker-tx", speaker_tx),
};

static void check(int rc, const char *what)
{
	if (rc) {
		fprintf(stderr, "%s: ACDB error %d\n", what, rc);
		exit(EXIT_FAILURE);
	}
}

static void *blob_alloc(uint32_t size)
{
	void *p;

	if (size > MAX_BLOB) {
		fprintf(stderr, "ACDB response exceeds %u bytes\n", MAX_BLOB);
		exit(EXIT_FAILURE);
	}
	p = calloc(1, size ? size : 1);
	if (!p) {
		perror("calloc");
		exit(EXIT_FAILURE);
	}
	return p;
}

static void save(const char *dir, const char *profile, const char *suffix,
		 const void *data, size_t size, const void *tail, size_t tail_size)
{
	char path[4096];
	FILE *f;
	int len = snprintf(path, sizeof(path), "%s/%s.%s.bin", dir, profile, suffix);

	if (len < 0 || (size_t)len >= sizeof(path)) {
		fprintf(stderr, "Output path is too long\n");
		exit(EXIT_FAILURE);
	}
	/* Refuse to overwrite a previous capture, including through a symlink. */
	f = fopen(path, "wbx");
	if (!f) {
		perror(path);
		exit(EXIT_FAILURE);
	}
	if ((size && fwrite(data, 1, size, f) != size) ||
	    (tail_size && fwrite(tail, 1, tail_size, f) != tail_size) || fclose(f)) {
		fprintf(stderr, "Cannot save %s\n", path);
		exit(EXIT_FAILURE);
	}
}

static void export_profile(const char *dir, struct profile *p)
{
	AcdbGetGraphRsp graph = {0};
	AcdbGetSubgraphDataRsp data = {0};
	AcdbSgIdGraphKeyVector request = {0};
	AcdbSubGraphList connection_request = {0};
	AcdbBlob links = {0}, cal = {0};
	AcdbSgIdPersistCalData persistent = {0};
	AcdbKeyValuePair ckv = {0xa9000000, 2};
	AcdbSgIdCalKeyVector cal_request = {0};
	uint32_t ids[MAX_SUBGRAPHS], id, count, i, j;
	size_t off = 0;

	check(acdb_ioctl(ACDB_CMD_GET_GRAPH, &p->gkv, sizeof(p->gkv),
			 &graph, sizeof(graph)), "graph size");
	if (!graph.num_subgraphs || graph.num_subgraphs > MAX_SUBGRAPHS)
		check(-1, "subgraph count");
	graph.subgraphs = blob_alloc(graph.size);
	check(acdb_ioctl(ACDB_CMD_GET_GRAPH, &p->gkv, sizeof(p->gkv),
			 &graph, sizeof(graph)), "graph data");
	for (i = 0; i < graph.num_subgraphs; i++) {
		if (off + 8 > graph.size)
			check(-1, "truncated subgraph list");
		memcpy(&id, (char *)graph.subgraphs + off, 4);
		memcpy(&count, (char *)graph.subgraphs + off + 4, 4);
		if (count > (graph.size - off - 8) / 4)
			check(-1, "truncated subgraph connections");
		for (j = 0; j < i; j++)
			if (ids[j] == id)
				check(-1, "duplicate subgraph");
		ids[i] = id;
		off += 8 + (size_t)count * 4;
	}
	if (off != graph.size)
		check(-1, "unexpected subgraph list trailer");
	request.num_sgid = graph.num_subgraphs;
	request.sg_ids = ids;
	request.graph_key_vector = p->gkv;
	check(acdb_ioctl(ACDB_CMD_GET_SUBGRAPH_DATA, &request, sizeof(request),
			 &data, sizeof(data)), "subgraph data size");
	data.spf_blob.buf = blob_alloc(data.spf_blob.buf_size);
	data.driver_prop.sub_graph_prop_data = blob_alloc(data.driver_prop.size);
	check(acdb_ioctl(ACDB_CMD_GET_SUBGRAPH_DATA, &request, sizeof(request),
			 &data, sizeof(data)), "subgraph data");
	connection_request.num_subgraphs = graph.num_subgraphs;
	connection_request.subgraphs = graph.subgraphs;
	check(acdb_ioctl(ACDB_CMD_GET_SUBGRAPH_CONNECTIONS, &connection_request,
			 sizeof(connection_request), &links, sizeof(links)), "connection size");
	links.buf = blob_alloc(links.buf_size);
	check(acdb_ioctl(ACDB_CMD_GET_SUBGRAPH_CONNECTIONS, &connection_request,
			 sizeof(connection_request), &links, sizeof(links)), "connections");
	cal_request.num_sg_ids = graph.num_subgraphs;
	cal_request.sg_ids = ids;
	cal_request.cal_key_vector_new.num_keys = 1;
	cal_request.cal_key_vector_new.graph_key_vector = &ckv;
	check(acdb_ioctl(ACDB_CMD_GET_SUBGRAPH_CALIBRATION_DATA_NONPERSIST,
			 &cal_request, sizeof(cal_request), &cal, sizeof(cal)), "calibration size");
	cal.buf = blob_alloc(cal.buf_size);
	check(acdb_ioctl(ACDB_CMD_GET_SUBGRAPH_CALIBRATION_DATA_NONPERSIST,
			 &cal_request, sizeof(cal_request), &cal, sizeof(cal)), "calibration");
	check(acdb_ioctl(ACDB_CMD_GET_SUBGRAPH_CALIBRATION_DATA_PERSIST,
			 &cal_request, sizeof(cal_request), &persistent, sizeof(persistent)),
	      "persistent calibration size");
	persistent.cal_data = blob_alloc(persistent.cal_data_size);
	check(acdb_ioctl(ACDB_CMD_GET_SUBGRAPH_CALIBRATION_DATA_PERSIST,
			 &cal_request, sizeof(cal_request), &persistent, sizeof(persistent)),
	      "persistent calibration");

	/* GSL appends the connection blob to the subgraph blob in GRAPH_OPEN. */
	save(dir, p->name, "open", data.spf_blob.buf, data.spf_blob.buf_size,
	     links.buf, links.buf_size);
	save(dir, p->name, "cal", cal.buf, cal.buf_size, NULL, 0);
	save(dir, p->name, "persist", persistent.cal_data, persistent.cal_data_size, NULL, 0);
	save(dir, p->name, "sgids", ids, graph.num_subgraphs * sizeof(ids[0]), NULL, 0);
	save(dir, p->name, "driver", data.driver_prop.sub_graph_prop_data,
	     data.driver_prop.size, NULL, 0);
	save(dir, p->name, "gkv", p->gkv.graph_key_vector,
	     p->gkv.num_keys * sizeof(AcdbKeyValuePair), NULL, 0);
	save(dir, p->name, "ckv", &ckv, sizeof(ckv), NULL, 0);
	printf("%s: %u subgraphs, open %u, calibration %u, persistent %u bytes\n",
	       p->name, graph.num_subgraphs, data.spf_blob.buf_size + links.buf_size,
	       cal.buf_size, persistent.cal_data_size);
	free(persistent.cal_data);
	free(cal.buf);
	free(links.buf);
	free(data.driver_prop.sub_graph_prop_data);
	free(data.spf_blob.buf);
	free(graph.subgraphs);
}

int main(int argc, char **argv)
{
	AcdbDataFiles files = {.num_files = 1};
	size_t i, len;

	if (argc != 3) {
		fprintf(stderr, "Usage: %s ACDB_FILE OUTPUT_DIRECTORY\n", argv[0]);
		return 2;
	}
	umask(077);
	if (mkdir(argv[2], 0700) && errno != EEXIST) {
		perror(argv[2]);
		return 1;
	}
	len = strlen(argv[1]);
	if (len >= sizeof(files.acdbFiles[0].fileName))
		return 2;
	memcpy(files.acdbFiles[0].fileName, argv[1], len + 1);
	files.acdbFiles[0].fileNameLen = len;
	check(acdb_init(&files, NULL), "acdb_init");
	for (i = 0; i < sizeof(profiles) / sizeof(profiles[0]); i++)
		export_profile(argv[2], &profiles[i]);
	check(acdb_deinit(), "acdb_deinit");
	return 0;
}
