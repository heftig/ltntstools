
#include "nic_monitor.h"

uint16_t hash_index_cal_hash(uint32_t addr, uint16_t port)
{
	 /*
	  * AB.CD.EF.GH:IJKL
	  *
	  * Hash: FGHL
	  */
	return ((addr << 4) & 0xfff0) | (port & 0x000f);
}

static uint16_t _compute_stream_hash(struct iphdr *iphdr, struct udphdr *udphdr)
{
	/* Compute the destination hash for faster lookup */
#ifdef __APPLE__
	uint32_t dstaddr = ntohl(iphdr->ip_dst.s_addr);
#endif
#ifdef __linux__
	uint32_t dstaddr = ntohl(iphdr->daddr);
#endif
	uint16_t dstport = ntohs(udphdr->uh_dport);
	return hash_index_cal_hash(dstaddr, dstport);
}

static const char *payloadTypes[] = {
	"???",
	"UDP",
	"RTP",
	"STL",
	"UNK",
	"21V",
	"21A",
	"21D",
};

const char *payloadTypeDesc(enum payload_type_e pt)
{
	if (pt >= PAYLOAD_MAX)
		return payloadTypes[0];

	return payloadTypes[pt];
}

void discovered_item_free(struct discovered_item_s *di)
{
	if (di->pcapRecorder) {
		ltntstools_segmentwriter_free(di->pcapRecorder);
		di->pcapRecorder = NULL;
	}

	if (di->packetIntervals) {
		ltn_histogram_free(di->packetIntervals);
		di->packetIntervals = NULL;
	}

	if (di->streamModel) {
		ltntstools_streammodel_free(di->streamModel);
		di->streamModel = NULL;
	}

	if (di->LTNLatencyProbe) {
		ltntstools_probe_ltnencoder_free(di->LTNLatencyProbe);
		di->LTNLatencyProbe = NULL;
	}
	free(di);
}

struct discovered_item_s *discovered_item_alloc(struct ether_header *ethhdr, struct iphdr *iphdr, struct udphdr *udphdr)
{
	struct discovered_item_s *di = calloc(1, sizeof(*di));
	if (di) {
		time(&di->firstSeen);
		di->lastUpdated = di->firstSeen;
		memcpy(&di->ethhdr, ethhdr, sizeof(*ethhdr));
		memcpy(&di->iphdr, iphdr, sizeof(*iphdr));
		memcpy(&di->udphdr, udphdr, sizeof(*udphdr));

		struct in_addr dstaddr, srcaddr;
#ifdef __linux__
		srcaddr.s_addr = di->iphdr.saddr;
		dstaddr.s_addr = di->iphdr.daddr;
#endif
#ifdef __APPLE__
		srcaddr.s_addr = di->iphdr.ip_src.s_addr;
		dstaddr.s_addr = di->iphdr.ip_dst.s_addr;
#endif

		sprintf(di->srcaddr, "%s:%d", inet_ntoa(srcaddr), ntohs(di->udphdr.uh_sport));
		sprintf(di->dstaddr, "%s:%d", inet_ntoa(dstaddr), ntohs(di->udphdr.uh_dport));

		di->iat_lwm_us = 50000000;
		di->iat_hwm_us = -1;
		di->iat_cur_us = 0;

		ltn_histogram_alloc_video_defaults(&di->packetIntervals, "IAT Intervals");

		/* Stream Model */
		if (ltntstools_streammodel_alloc(&di->streamModel) < 0) {
			fprintf(stderr, "\nUnable to allocate streammodel object, it's safe to continue.\n\n");
		}

		/* LTN Latency Estimator Probe - we'll only use this if we detect the LTN encoder */
		if (ltntstools_probe_ltnencoder_alloc(&di->LTNLatencyProbe) < 0) {
			fprintf(stderr, "\nUnable to allocate ltn encoder latency probe, it's safe to continue.\n\n");
		}
	}

	return di;
}

/* This function is take with the ctx->list held by the caller. */
static void discovered_item_insert(struct tool_context_s *ctx, struct discovered_item_s *di)
{
	struct discovered_item_s *e = NULL;

	/* Maintain a sorted list of objects, based on dst ip address and port. */
	xorg_list_for_each_entry(e, &ctx->list, list) {
#ifdef __linux__
		uint64_t a = (uint64_t)ntohl(e->iphdr.daddr) << 16;
		a |= (e->udphdr.uh_dport);

		uint64_t b = (uint64_t)ntohl(di->iphdr.daddr) << 16;
		b |= (di->udphdr.uh_dport);
#endif
#ifdef __APPLE__
		uint64_t a = (uint64_t)ntohl(e->iphdr.ip_dst.s_addr) << 16;
		a |= (e->udphdr.uh_dport);

		uint64_t b = (uint64_t)ntohl(di->iphdr.ip_dst.s_addr) << 16;
		b |= (di->udphdr.uh_dport);
#endif
		if (a < b)
			continue;

		if (a == b) {
			discovered_item_state_set(di, DI_STATE_DST_DUPLICATE);
			discovered_item_state_set(e, DI_STATE_DST_DUPLICATE);
		}
		xorg_list_add(&di->list, e->list.prev);
		return;
	}

	xorg_list_append(&di->list, &ctx->list);
}

/* Before August 2021, di object lookup takes an excessive amount of CPU with large numbers of streams.
   To investigate this, in a test case, were I had 99 streams all going to ports
   4001-4099, and I put a static fixed array in play with a fast direct lookup,
   I saved 50% CPU in the pcap thread and 75% CPU in the stats thread.
   So, optimization is worthwhile but a more flexible approach was needed.

   Instead:
   Build a hashing function that makes our streams 'fairly' unique,
   with room for some overflow hashes.
   Put this into a static array of 65536 addresses, with pointers to a new
   struct {
      // Contains ideally the only DI object associated with the hash
      // But could contain multiple di objects matching this hash
      struct discovered_item_s *array[];
      int arrlen;
   }
   when asked to search for a di object.
   1. compute the hash as a uint16_t hash = X.
   2. check globalhashtable[ hash ], if not set, create a new object general object.
   3.  if set, look manually at all the entries in hash entry array, looking for the specific record.
       if not found, create a new object.
       if found, optimization achieved.

  The result of this optimzation, in the following configuration:
   DC60 older hardware with a 10Gb card, playing out 99 x 20Mbps streams
   With a total output capacity of 2Gb, running tstools on the same
   system.

   Performance:     NoCache   Cache
      pcap-thread       65%     33%
     stats-thread       35%      5%
*/

struct discovered_item_s *discovered_item_findcreate(struct tool_context_s *ctx,
	struct ether_header *ethhdr, struct iphdr *iphdr, struct udphdr *udphdr)
{
	struct discovered_item_s *found = NULL;

	/* Compute the src/dst ip/udp address/ports hash for faster lookup */
	uint16_t hash = _compute_stream_hash(iphdr, udphdr);

	if (ctx->verbose > 2) {
		char *str = network_stream_ascii(iphdr, udphdr);
		printf("cache srch on %s\n", str);
		free(str);
		if (ctx->verbose > 3) {
			hash_index_print(ctx->hashIndex, hash);
		}
	}

	pthread_mutex_lock(&ctx->lock);

	/* With the hash, lookup the di objects in the cachelist. */
	if (hash_index_get_count(ctx->hashIndex, hash) >= 1) {
		/* One or more items in the cache for the same hash,
		 * we have to enum and locate our exact item.
		 * The hash has reasonable selectivity, but overflows can occur.
		 */
		struct discovered_item_s *item = NULL;
		int enumerator = 0;
		int ret = 0;
		while (ret == 0) {
			ret = hash_index_get_enum(ctx->hashIndex, hash, &enumerator, (void **)&item);
			if (ret == 0 && item) {
				/* Do a 100% perfect match on the ip and udp headers */
				if (network_addr_compare(iphdr, udphdr, &item->iphdr, &item->udphdr) == 1) {
					/* Found the perfect match in the cache */
					found = item;
					break;
				}
			}
		}
	}

	if (!found) {
		ctx->cacheMiss++;

		if (ctx->verbose > 3) {
			char *str = network_stream_ascii(iphdr, udphdr);
			printf("cache miss on %s\n", str);
			free(str);
		}

	} else {
		ctx->cacheHit++;

		if (ctx->verbose > 3) {
			char *str = network_stream_ascii(iphdr, udphdr);
			printf("cache  hit on %s\n", str);
			free(str);
		}

	}
	ctx->cacheHitRatio = 100.0 - (((double)ctx->cacheMiss / (double)ctx->cacheHit) * 100.0);

#if 0
	/* A refactored older mechanism, look through the entire array
	 * which gets super expensive as the number of streams increases.
	 */
	if (!found) {
                struct discovered_item_s *e = NULL;
		/* Enumerate the di array for each input packet.
		 * It works well for 1-2 dozen streams, but doesn't scale well beyond this.
		 * We never really want to do this, this can go away.
		 */
		xorg_list_for_each_entry(e, &ctx->list, list) {
			if (network_addr_compare(iphdr, udphdr, &e->iphdr, &e->udphdr) == 1) {
				found = e;
				break;
			}
		}
	}
#endif

	if (!found) {
		found = discovered_item_alloc(ethhdr, iphdr, udphdr);
		if (found) {
			discovered_item_insert(ctx, found);
			hash_index_set(ctx->hashIndex, hash, found);

			if (ctx->automaticallyRecordStreams) {
				discovered_item_state_set(found, DI_STATE_PCAP_RECORD_START);
			}
		}
	}
	pthread_mutex_unlock(&ctx->lock);

	return found;
}

void discovered_item_fd_summary(struct tool_context_s *ctx, struct discovered_item_s *di, int fd)
{
	char stream[128];
	sprintf(stream, "%s", di->srcaddr);
	sprintf(stream + strlen(stream), " -> %s", di->dstaddr);

	dprintf(fd, "   PID   PID     PacketCount     CCErrors    TEIErrors @ %6.2f : %s (%s)\n",
		ltntstools_pid_stats_stream_get_mbps(&di->stats), stream,
		payloadTypeDesc(di->payloadType));
	dprintf(fd, "<---------------------------  ----------- ------------ ---Mb/ps------------------------------------------------>\n");
	for (int i = 0; i < MAX_PID; i++) {
		if (di->stats.pids[i].enabled) {
			dprintf(fd, "0x%04x (%4d) %14" PRIu64 " %12" PRIu64 " %12" PRIu64 "   %6.2f\n", i, i,
				di->stats.pids[i].packetCount,
				di->stats.pids[i].ccErrors,
				di->stats.pids[i].teiErrors,
				ltntstools_pid_stats_pid_get_mbps(&di->stats, i));
		}
	}
	ltn_histogram_interval_print(fd, di->packetIntervals, 0);
	dprintf(fd, "\n");
}

void discovered_items_console_summary(struct tool_context_s *ctx)
{
	struct discovered_item_s *e = NULL;

	pthread_mutex_lock(&ctx->lock);
	xorg_list_for_each_entry(e, &ctx->list, list) {
		discovered_item_fd_summary(ctx, e, STDOUT_FILENO);
	}
	pthread_mutex_unlock(&ctx->lock);
}

/* For a given item, open a detailed stats file on disk, append the current stats, close it. */
void discovered_item_detailed_file_summary(struct tool_context_s *ctx, struct discovered_item_s *di)
{
	if (di->detailed_filename[0] == 0) {
		if (ctx->detailed_file_prefix)
			sprintf(di->detailed_filename, "%s", ctx->detailed_file_prefix);

		sprintf(di->detailed_filename + strlen(di->detailed_filename), "%s", di->dstaddr);
	}

	int fd = open(di->detailed_filename, O_CREAT | O_RDWR | O_APPEND, 0644);
	if (fd < 0) {
		fprintf(stderr, "Failed to open %s\n", di->detailed_filename);
		return;
	}

	/* If we're a super user, obtain any SUDO uid and change file ownership to it - if possible. */
	if (getuid() == 0 && getenv("SUDO_UID") && getenv("SUDO_GID")) {
		uid_t o_uid = atoi(getenv("SUDO_UID"));
		gid_t o_gid = atoi(getenv("SUDO_GID"));

		if (fchown(fd, o_uid, o_gid) != 0) {
			/* Error */
			fprintf(stderr, "Error changing %s ownership to uid %d gid %d, ignoring\n",
				di->detailed_filename, o_uid, o_gid);
		}
	}

	struct tm tm;
	time_t now;
	time(&now);
	localtime_r(&now, &tm);

	char line[256];
	char ts[24];
        sprintf(ts, "%04d%02d%02d-%02d%02d%02d",
                tm.tm_year + 1900,
                tm.tm_mon  + 1,
                tm.tm_mday,
                tm.tm_hour,
                tm.tm_min,
                tm.tm_sec);

	uint32_t bps = 0;
	double mbps = 0;
	if ((di->payloadType == PAYLOAD_UDP_TS) || (di->payloadType == PAYLOAD_RTP_TS)) {
		mbps = ltntstools_pid_stats_stream_get_mbps(&di->stats);
		bps = ltntstools_pid_stats_stream_get_bps(&di->stats);
	} else
	if (di->payloadType == PAYLOAD_SMPTE2110_20_VIDEO) {
		mbps = ltntstools_ctp_stats_stream_get_mbps(&di->stats);
		bps = ltntstools_ctp_stats_stream_get_bps(&di->stats);
	} else
	if (di->payloadType == PAYLOAD_SMPTE2110_30_AUDIO) {
		mbps = ltntstools_ctp_stats_stream_get_mbps(&di->stats);
		bps = ltntstools_ctp_stats_stream_get_bps(&di->stats);
	} else
	if (di->payloadType == PAYLOAD_A324_CTP) {
		mbps = ltntstools_ctp_stats_stream_get_mbps(&di->stats);
		bps = ltntstools_ctp_stats_stream_get_bps(&di->stats);
	} else {
		mbps = ltntstools_bytestream_stats_stream_get_mbps(&di->stats);
		bps = ltntstools_bytestream_stats_stream_get_bps(&di->stats);
	}
	sprintf(line, "time=%s,nic=%s,bps=%d,mbps=%.2f,tspacketcount=%" PRIu64 ",ccerrors=%" PRIu64 "%s,src=%s,dst=%s,dropped=%d/%d\n",
		ts,
		ctx->ifname,
		bps,
		mbps,
		di->stats.packetCount,
		di->stats.ccErrors,
		di->stats.ccErrors != di->statsToFile.ccErrors ? "!" : "",
		di->srcaddr,
		di->dstaddr,
		ctx->pcap_stats.ps_drop,
		ctx->pcap_stats.ps_ifdrop);

	write(fd, line, strlen(line));

	discovered_item_fd_summary(ctx, di, fd);

	close(fd);
}

/* For a given item, open a stats file on disk, append the current stats, close it. */
void discovered_item_file_summary(struct tool_context_s *ctx, struct discovered_item_s *di)
{
	if (di->filename[0] == 0) {
		if (ctx->file_prefix)
			sprintf(di->filename, "%s", ctx->file_prefix);

		sprintf(di->filename + strlen(di->filename), "%s", di->dstaddr);
	}

	if (di->detailed_filename[0] == 0) {
		if (ctx->detailed_file_prefix)
			sprintf(di->detailed_filename, "%s", ctx->detailed_file_prefix);

		sprintf(di->detailed_filename + strlen(di->detailed_filename), "%s", di->dstaddr);
	}

	int fd = open(di->filename, O_CREAT | O_RDWR | O_APPEND, 0644);
	if (fd < 0) {
		fprintf(stderr, "Failed to open %s\n", di->filename);
		return;
	}

	/* If we're a super user, obtain any SUDO uid and change file ownership to it - if possible. */
	if (getuid() == 0 && getenv("SUDO_UID") && getenv("SUDO_GID")) {
		uid_t o_uid = atoi(getenv("SUDO_UID"));
		gid_t o_gid = atoi(getenv("SUDO_GID"));

		if (fchown(fd, o_uid, o_gid) != 0) {
			/* Error */
			fprintf(stderr, "Error changing %s ownership to uid %d gid %d, ignoring\n",
				di->filename, o_uid, o_gid);
		}
	}

	struct tm tm;
	time_t now;
	time(&now);
	localtime_r(&now, &tm);

	char line[256];
	char ts[24];
        sprintf(ts, "%04d%02d%02d-%02d%02d%02d",
                tm.tm_year + 1900,
                tm.tm_mon  + 1,
                tm.tm_mday,
                tm.tm_hour,
                tm.tm_min,
                tm.tm_sec);

	uint32_t bps = 0;
	double mbps = 0;
	if ((di->payloadType == PAYLOAD_UDP_TS) || (di->payloadType == PAYLOAD_RTP_TS)) {
		mbps = ltntstools_pid_stats_stream_get_mbps(&di->stats);
		bps = ltntstools_pid_stats_stream_get_bps(&di->stats);
	} else
	if (di->payloadType == PAYLOAD_A324_CTP) {
		mbps = ltntstools_ctp_stats_stream_get_mbps(&di->stats);
		bps = ltntstools_ctp_stats_stream_get_bps(&di->stats);
	} else
	if (di->payloadType == PAYLOAD_SMPTE2110_20_VIDEO) {
		mbps = ltntstools_ctp_stats_stream_get_mbps(&di->stats);
		bps = ltntstools_ctp_stats_stream_get_bps(&di->stats);
	} else
	if (di->payloadType == PAYLOAD_SMPTE2110_30_AUDIO) {
		mbps = ltntstools_ctp_stats_stream_get_mbps(&di->stats);
		bps = ltntstools_ctp_stats_stream_get_bps(&di->stats);
	} else {
		mbps = ltntstools_bytestream_stats_stream_get_mbps(&di->stats);
		bps = ltntstools_bytestream_stats_stream_get_bps(&di->stats);
	}
	sprintf(line, "time=%s,nic=%s,bps=%d,mbps=%.2f,tspacketcount=%" PRIu64 ",ccerrors=%" PRIu64 "%s,src=%s,dst=%s,dropped=%d/%d\n",
		ts,
		ctx->ifname,
		bps,
		mbps,
		di->stats.packetCount,
		di->stats.ccErrors,
		di->stats.ccErrors != di->statsToFile.ccErrors ? "!" : "",
		di->srcaddr,
		di->dstaddr,
		ctx->pcap_stats.ps_drop,
		ctx->pcap_stats.ps_ifdrop);

	write(fd, line, strlen(line));

	close(fd);
#if 0
	printf("   PID   PID     PacketCount     CCErrors    TEIErrors @ %6.2f : %s\n",
		di->stats.mbps, stream);
	printf("<---------------------------  ----------- ------------ ---Mb/ps------------------------------------------->\n");
	for (int i = 0; i < MAX_PID; i++) {
		if (di->stats.pids[i].enabled) {
			printf("0x%04x (%4d) %14" PRIu64 " %12" PRIu64 " %12" PRIu64 "   %6.2f\n", i, i,
				di->stats.pids[i].packetCount,
				di->stats.pids[i].ccErrors,
				di->stats.pids[i].teiErrors,
				di->stats.pids[i].mbps);
		}
	}
#endif
}

void discovered_items_file_summary(struct tool_context_s *ctx)
{
	struct discovered_item_s *e = NULL;

	pthread_mutex_lock(&ctx->lock);
	xorg_list_for_each_entry(e, &ctx->list, list) {
		discovered_item_file_summary(ctx, e);
		discovered_item_detailed_file_summary(ctx, e);

		/* Implied memcpy of struct */
		/* Cache the current stats. When we prepare
		 * file records, of the CC counts have changed, we
		 * do something significant in the file records.
		 */
		e->statsToFile = e->stats;
	}
	pthread_mutex_unlock(&ctx->lock);
}

void discovered_items_stats_reset(struct tool_context_s *ctx)
{
	struct discovered_item_s *e = NULL;

	pthread_mutex_lock(&ctx->lock);
	xorg_list_for_each_entry(e, &ctx->list, list) {
		ltntstools_pid_stats_reset(&e->stats);
		e->iat_lwm_us = 5000000;
		e->iat_hwm_us = -1;
		ltn_histogram_reset(e->packetIntervals);
	}
	pthread_mutex_unlock(&ctx->lock);
}

void discovered_item_state_set(struct discovered_item_s *di, unsigned int state)
{
	di->state |= state;
}

void discovered_item_state_clr(struct discovered_item_s *di, unsigned int state)
{
	di->state &= ~(state);
}

unsigned int discovered_item_state_get(struct discovered_item_s *di, unsigned int state)
{
	return di->state & state;
}

void discovered_items_select_first(struct tool_context_s *ctx)
{
	struct discovered_item_s *e = NULL;

	pthread_mutex_lock(&ctx->lock);
	xorg_list_for_each_entry(e, &ctx->list, list) {
		discovered_item_state_set(e, DI_STATE_SELECTED);
		break;
	}
	pthread_mutex_unlock(&ctx->lock);
}

void discovered_items_select_next(struct tool_context_s *ctx)
{
	struct discovered_item_s *e = NULL;

	int doSelect = 0;
	pthread_mutex_lock(&ctx->lock);
	xorg_list_for_each_entry(e, &ctx->list, list) {
		if (discovered_item_state_get(e, DI_STATE_HIDDEN))
			continue;
		if (discovered_item_state_get(e, DI_STATE_SELECTED)) {

			/* Only clear the current entry, if it's NOT the last entry in the list */
			if (e->list.next != &ctx->list)
				discovered_item_state_clr(e, DI_STATE_SELECTED);
			doSelect = 1;
		} else
		if (doSelect) {
			discovered_item_state_set(e, DI_STATE_SELECTED);
			break;
		}
	}
	pthread_mutex_unlock(&ctx->lock);

#if 0
	if (!doSelect)
		discovered_items_select_first(ctx);
#endif
}

void discovered_items_select_prev(struct tool_context_s *ctx)
{
	struct discovered_item_s *e = NULL;
	struct discovered_item_s *p = NULL;

	pthread_mutex_lock(&ctx->lock);
	xorg_list_for_each_entry(e, &ctx->list, list) {
		if (discovered_item_state_get(e, DI_STATE_HIDDEN))
			continue;
		if (discovered_item_state_get(e, DI_STATE_SELECTED) && p) {
			discovered_item_state_clr(e, DI_STATE_SELECTED);
			discovered_item_state_set(p, DI_STATE_SELECTED);
			break;
		}
		p = e;
	}
	pthread_mutex_unlock(&ctx->lock);
}

void discovered_items_select_all(struct tool_context_s *ctx)
{
	struct discovered_item_s *e = NULL;

	pthread_mutex_lock(&ctx->lock);
	xorg_list_for_each_entry(e, &ctx->list, list) {
		discovered_item_state_set(e, DI_STATE_SELECTED);
	}
	pthread_mutex_unlock(&ctx->lock);
}

void discovered_items_select_none(struct tool_context_s *ctx)
{
	struct discovered_item_s *e = NULL;

	pthread_mutex_lock(&ctx->lock);
	xorg_list_for_each_entry(e, &ctx->list, list) {
		discovered_item_state_clr(e, DI_STATE_SELECTED);
	}
	pthread_mutex_unlock(&ctx->lock);
}

void discovered_items_select_record_toggle(struct tool_context_s *ctx)
{
	struct discovered_item_s *e = NULL;

	pthread_mutex_lock(&ctx->lock);
	xorg_list_for_each_entry(e, &ctx->list, list) {
		if (discovered_item_state_get(e, DI_STATE_SELECTED) == 0)
			continue;

		if (discovered_item_state_get(e, DI_STATE_PCAP_RECORDING) || discovered_item_state_get(e, DI_STATE_PCAP_RECORD_START)) {
			discovered_item_state_set(e, DI_STATE_PCAP_RECORD_STOP);
		} else {
			discovered_item_state_set(e, DI_STATE_PCAP_RECORD_START);
		}
	}
	pthread_mutex_unlock(&ctx->lock);
}

void discovered_items_record_abort(struct tool_context_s *ctx)
{
	struct discovered_item_s *e = NULL;

	pthread_mutex_lock(&ctx->lock);
	xorg_list_for_each_entry(e, &ctx->list, list) {
		if (discovered_item_state_get(e, DI_STATE_PCAP_RECORDING) || discovered_item_state_get(e, DI_STATE_PCAP_RECORD_START)) {
			discovered_item_state_set(e, DI_STATE_PCAP_RECORD_STOP);
		}
	}
	pthread_mutex_unlock(&ctx->lock);
}

void discovered_items_select_show_pids_toggle(struct tool_context_s *ctx)
{
	struct discovered_item_s *e = NULL;

	pthread_mutex_lock(&ctx->lock);
	xorg_list_for_each_entry(e, &ctx->list, list) {
		if (discovered_item_state_get(e, DI_STATE_SELECTED) == 0)
			continue;

		if (discovered_item_state_get(e, DI_STATE_SHOW_PIDS)) {
			discovered_item_state_clr(e, DI_STATE_SHOW_PIDS);
		} else {
			discovered_item_state_set(e, DI_STATE_SHOW_PIDS);
		}
	}
	pthread_mutex_unlock(&ctx->lock);
}

void discovered_items_select_show_tr101290_toggle(struct tool_context_s *ctx)
{
	struct discovered_item_s *e = NULL;

	pthread_mutex_lock(&ctx->lock);
	xorg_list_for_each_entry(e, &ctx->list, list) {
		if (discovered_item_state_get(e, DI_STATE_SELECTED) == 0)
			continue;

		if (discovered_item_state_get(e, DI_STATE_SHOW_TR101290)) {
			discovered_item_state_clr(e, DI_STATE_SHOW_TR101290);
		} else {
			discovered_item_state_set(e, DI_STATE_SHOW_TR101290);
		}
	}
	pthread_mutex_unlock(&ctx->lock);
}

void discovered_items_select_show_iats_toggle(struct tool_context_s *ctx)
{
	struct discovered_item_s *e = NULL;

	pthread_mutex_lock(&ctx->lock);
	xorg_list_for_each_entry(e, &ctx->list, list) {
		if (discovered_item_state_get(e, DI_STATE_SELECTED) == 0)
			continue;

		if (discovered_item_state_get(e, DI_STATE_SHOW_IAT_HISTOGRAM)) {
			discovered_item_state_clr(e, DI_STATE_SHOW_IAT_HISTOGRAM);
		} else {
			discovered_item_state_set(e, DI_STATE_SHOW_IAT_HISTOGRAM);
		}
	}
	pthread_mutex_unlock(&ctx->lock);
}

void discovered_items_select_hide(struct tool_context_s *ctx)
{
	struct discovered_item_s *e = NULL;

	pthread_mutex_lock(&ctx->lock);
	xorg_list_for_each_entry(e, &ctx->list, list) {
		if (discovered_item_state_get(e, DI_STATE_SELECTED) == 0)
			continue;

		/* No hiding if recording */
		if (discovered_item_state_get(e, DI_STATE_PCAP_RECORDING))
			continue;

		discovered_item_state_set(e, DI_STATE_HIDDEN);
	}
	pthread_mutex_unlock(&ctx->lock);
}

void discovered_items_unhide_all(struct tool_context_s *ctx)
{
	struct discovered_item_s *e = NULL;

	pthread_mutex_lock(&ctx->lock);
	xorg_list_for_each_entry(e, &ctx->list, list) {
		discovered_item_state_clr(e, DI_STATE_HIDDEN);
	}
	pthread_mutex_unlock(&ctx->lock);
}

void discovered_items_free(struct tool_context_s *ctx)
{
	struct discovered_item_s *di = NULL;

	pthread_mutex_lock(&ctx->lock);
        while (!xorg_list_is_empty(&ctx->list)) {
		di = xorg_list_first_entry(&ctx->list, struct discovered_item_s, list);
		xorg_list_del(&di->list);
		discovered_item_free(di);
	}
	pthread_mutex_unlock(&ctx->lock);
}

void discovered_items_select_show_streammodel_toggle(struct tool_context_s *ctx)
{
	struct discovered_item_s *e = NULL;

	pthread_mutex_lock(&ctx->lock);
	xorg_list_for_each_entry(e, &ctx->list, list) {
		if (discovered_item_state_get(e, DI_STATE_SELECTED) == 0)
			continue;

		if (discovered_item_state_get(e, DI_STATE_SHOW_STREAMMODEL)) {
			discovered_item_state_clr(e, DI_STATE_SHOW_STREAMMODEL);
		} else {
			discovered_item_state_set(e, DI_STATE_SHOW_STREAMMODEL);
		}
	}
	pthread_mutex_unlock(&ctx->lock);
}

