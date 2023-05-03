#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/likely.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/file.h"
#include "spdk/nvme.h"
#include "spdk/vmd.h"
#include "spdk/nvme_zns.h"
#include "spdk/log.h"
#include "../lib/trace_io.h"

struct ctrlr_entry {
	struct spdk_nvme_ctrlr		*ctrlr;
	TAILQ_ENTRY(ctrlr_entry)	link;
	char				        name[1024];
};

struct ns_entry {
	struct spdk_nvme_ctrlr	    *ctrlr;
	struct spdk_nvme_ns	        *ns;
	TAILQ_ENTRY(ns_entry)	    link;
	struct spdk_nvme_qpair	    *qpair;
};

static TAILQ_HEAD(, ctrlr_entry) g_controllers = TAILQ_HEAD_INITIALIZER(g_controllers);
static TAILQ_HEAD(, ns_entry) g_namespaces = TAILQ_HEAD_INITIALIZER(g_namespaces);
static struct spdk_nvme_transport_id g_trid = {};
static bool g_input_file = false;
static bool g_report_zone = false;
static uint64_t g_zone_report_limit = 0;
static int outstanding_commands;

/* Underline a "line" with the given marker, e.g. print_uline("=", printf(...)); */
static void
print_uline(char marker, int line_len)
{
    for (int i = 1; i < line_len; ++i) {
        putchar(marker);
    }   
    putchar('\n');
}

static void
register_ns(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_ns *ns)
{
    struct ns_entry *entry;

    if (!spdk_nvme_ns_is_active(ns)) {
        return;
    }   

    entry = (struct ns_entry *)malloc(sizeof(struct ns_entry));
    if (entry == NULL) {
        perror("ns_entry malloc");
        exit(1);
    }   

    entry->ctrlr = ctrlr;
    entry->ns = ns; 
    TAILQ_INSERT_TAIL(&g_namespaces, entry, link);

    printf("  Namespace ID: %d size: %juGB\n", spdk_nvme_ns_get_id(ns),
            spdk_nvme_ns_get_size(ns) / 1000000000);
}

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
     struct spdk_nvme_ctrlr_opts *opts)
{
    printf("Attaching to %s\n", trid->traddr);

    return true;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
    int nsid;
    struct ctrlr_entry *entry;
    struct spdk_nvme_ns *ns;
    const struct spdk_nvme_ctrlr_data *cdata;

    entry = (struct ctrlr_entry *)malloc(sizeof(struct ctrlr_entry));
    if (entry == NULL) {
        perror("ctrlr_entry malloc");
        exit(1);
    }

    printf("Attached to %s\n", trid->traddr);

    /*
     * spdk_nvme_ctrlr is the logical abstraction in SPDK for an NVMe
     * controller.  During initialization, the IDENTIFY data for the
     * controller is read using an NVMe admin command, and that data
     * can be retrieved using spdk_nvme_ctrlr_get_data() to get
     * detailed information on the controller.  Refer to the NVMe
     * specification for more details on IDENTIFY for NVMe controllers.
     */
    cdata = spdk_nvme_ctrlr_get_data(ctrlr);

    snprintf(entry->name, sizeof(entry->name), "%-20.20s (%-20.20s)", cdata->mn, cdata->sn);

    entry->ctrlr = ctrlr;
    TAILQ_INSERT_TAIL(&g_controllers, entry, link);

    /*
     * Each controller has one or more namespaces.
     * Note that in NVMe, namespace IDs start at 1, not 0.
     */
    for (nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr); nsid != 0;
        nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, nsid)) {
        ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
        if (ns == NULL) {
            continue;
        }
        register_ns(ctrlr, ns);
    }
}

static void
cleanup(void)
{
    struct ns_entry *ns_entry, *tmp_ns_entry;
    struct ctrlr_entry *ctrlr_entry, *tmp_ctrlr_entry;
    struct spdk_nvme_detach_ctx *detach_ctx = NULL;

    TAILQ_FOREACH_SAFE(ns_entry, &g_namespaces, link, tmp_ns_entry) {
        TAILQ_REMOVE(&g_namespaces, ns_entry, link);
        free(ns_entry);
    }

    TAILQ_FOREACH_SAFE(ctrlr_entry, &g_controllers, link, tmp_ctrlr_entry) {
        TAILQ_REMOVE(&g_controllers, ctrlr_entry, link);
        spdk_nvme_detach_async(ctrlr_entry->ctrlr, &detach_ctx);
        free(ctrlr_entry);
    }

    if (detach_ctx) {
        spdk_nvme_detach_poll(detach_ctx);
    }
}

/* report zone start */
static void
identify_zns_info(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_ns *ns)
{
    uint64_t num_zones = spdk_nvme_zns_ns_get_num_zones(ns);
    uint64_t zone_size = spdk_nvme_zns_ns_get_zone_size(ns);
    uint32_t zone_append_size_limit = spdk_nvme_zns_ctrlr_get_max_zone_append_size(ctrlr);
    const struct spdk_nvme_ns_data *ref_ns_data = spdk_nvme_ns_get_data(ns);
    const struct spdk_nvme_zns_ns_data *ref_ns_zns_data = spdk_nvme_zns_ns_get_data(ns);
    
    print_uline('=', printf("\nNVMe ZNS Zone Information\n"));    
    printf("number of zone: %lu\n", num_zones);
    printf("size of zone: %lu (%lu * %lu)\n", zone_size, ref_ns_zns_data->lbafe->zsze, ref_ns_data->nsze);
    printf("size of lBA: %lu\n", ref_ns_data->nsze);
    printf("max zone append size: %u\n", zone_append_size_limit);
    printf("\n");
}

static void
get_zns_zone_report_completion(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
    if (spdk_nvme_cpl_is_error(cpl)) {
        printf("get zns zone report failed\n");
    }

    outstanding_commands--;
}

static void print_zns_zone(uint8_t *report, uint32_t index, uint32_t zdes)
{
    uint32_t zrs = sizeof(struct spdk_nvme_zns_zone_report);
    uint32_t zds = sizeof(struct spdk_nvme_zns_zone_desc);
    uint32_t zd_index = zrs + index * (zds + zdes);

    struct spdk_nvme_zns_zone_desc *desc = (struct spdk_nvme_zns_zone_desc *)(report + zd_index);

    printf("ZSLBA: 0x%-18" PRIx64 " ZCAP: 0x%-18" PRIx64 " WP: 0x%-18" PRIx64 " ZS: ", desc->zslba,
           desc->zcap, desc->wp);
    switch (desc->zs)
    {
    case SPDK_NVME_ZONE_STATE_EMPTY:
        printf("%-20s", "Empty");
        break;
    case SPDK_NVME_ZONE_STATE_IOPEN:
        printf("%-20s", "Implicit open");
        break;
    case SPDK_NVME_ZONE_STATE_EOPEN:
        printf("%-20s", "Explicit open");
        break;
    case SPDK_NVME_ZONE_STATE_CLOSED:
        printf("%-20s", "Closed");
        break;
    case SPDK_NVME_ZONE_STATE_RONLY:
        printf("%-20s", "Read only");
        break;
    case SPDK_NVME_ZONE_STATE_FULL:
        printf("%-20s", "Full");
        break;
    case SPDK_NVME_ZONE_STATE_OFFLINE:
        printf("%-20s", "Offline");
        break;
    default:
        printf("%-20s", "Reserved");
    }
    printf(" ZT: %-20s ZA: 0x%-18x\n", (desc->zt == SPDK_NVME_ZONE_TYPE_SEQWR) ? "SWR" : "Reserved",
           desc->za.raw);

    if (!desc->za.bits.zdev)
    {
        return;
    }

    for (int i = 0; i < (int)zdes; i += 8)
    {
        printf("zone_desc_ext[%d] : 0x%" PRIx64 "\n", i,
               *(uint64_t *)(report + zd_index + zds + i));
    }
}

static void report_zone(void)
{
    int rc = 0;
   
    struct ns_entry *ns_entry = TAILQ_FIRST(&g_namespaces);
    
    if (spdk_nvme_ns_get_csi(ns_entry->ns) != SPDK_NVME_CSI_ZNS) {
        return;
    }

    identify_zns_info(ns_entry->ctrlr, ns_entry->ns);
    uint64_t zone_size_lba = spdk_nvme_zns_ns_get_zone_size_sectors(ns_entry->ns);
    uint64_t total_zones = spdk_nvme_zns_ns_get_num_zones(ns_entry->ns);

    /* specify namespace and allocate io qpair for the namespace */
    ns_entry->qpair = spdk_nvme_ctrlr_alloc_io_qpair(ns_entry->ctrlr, NULL, 0);
    if (ns_entry->qpair == NULL) {
        printf("ERROR: spdk_nvme_ctrlr_alloc_io_qpair() failed\n");
        return;
    }

    const struct spdk_nvme_ns_data *nsdata = spdk_nvme_ns_get_data(ns_entry->ns);
    const struct spdk_nvme_zns_ns_data *nsdata_zns = spdk_nvme_zns_ns_get_data(ns_entry->ns);

    //uint32_t zrs = sizeof(struct spdk_nvme_zns_zone_report);
    //uint32_t zds = sizeof(struct spdk_nvme_zns_zone_desc);

    uint32_t format_index = spdk_nvme_ns_get_format_index(nsdata);
    size_t zdes = nsdata_zns->lbafe[format_index].zdes * 64;

    size_t report_bufsize = spdk_nvme_ns_get_max_io_xfer_size(ns_entry->ns);
    uint8_t *report_buf = calloc(1, report_bufsize);
    if (!report_buf) {
        printf("Zone report allocation failed!\n");
        exit(1);
    }

    uint64_t zones_to_print = g_zone_report_limit ? spdk_min(total_zones, (uint64_t)g_zone_report_limit) : \
	                total_zones;
    
    print_uline('=', printf("NVMe ZNS Zone Report (first %zu of %zu)\n", zones_to_print, total_zones));

    outstanding_commands = 0;
    uint64_t handled_zones = 0;
    uint64_t slba = 0;
    while (handled_zones < zones_to_print) {
        memset(report_buf, 0, report_bufsize);

        if (zdes) {
            //uint64_t max_zones_per_buf = (report_bufsize - zrs) / (zds + zdes);
            rc = spdk_nvme_zns_ext_report_zones(ns_entry->ns, ns_entry->qpair, report_buf, report_bufsize,
                            slba, SPDK_NVME_ZRA_LIST_ALL, true,
                            get_zns_zone_report_completion, NULL);
        } else {
            //uint64_t max_zones_per_buf = (report_bufsize - zrs) / zds;
            rc = spdk_nvme_zns_report_zones(ns_entry->ns, ns_entry->qpair, report_buf, report_bufsize,
                            slba, SPDK_NVME_ZRA_LIST_ALL, true,
                            get_zns_zone_report_completion, NULL);
        }

        if (rc) {
            fprintf(stderr, "Report zones failed\n");
            exit(1);
        } else {
            outstanding_commands++;
        }

        while (outstanding_commands) {
            spdk_nvme_qpair_process_completions(ns_entry->qpair, 0);
        }

        for (int i = 0; handled_zones < zones_to_print; i++) {
            print_zns_zone(report_buf, i, zdes);
            slba += zone_size_lba;
            handled_zones++;
        }
        printf("\n");
    }

    free(report_buf);
    spdk_nvme_ctrlr_free_io_qpair(ns_entry->qpair);
}
/* report zone end */

struct io_seq
{
    struct ns_entry *ns_entry;
    char *buf;
    unsigned using_cmb_io;
    int is_completed;
};

static int
process_replay(struct bin_file_data *b)
{
    struct ns_entry *ns_entry;
    struct io_seq seq;
    int rc;
    size_t sz;
    
    /* specify namespace and allocate io qpair for the namespace */
    ns_entry = TAILQ_FIRST(&g_namespaces);
    ns_entry->qpair = spdk_nvme_ctrlr_alloc_io_qpair(ns_entry->ctrlr, NULL, 0);
    if (ns_entry->qpair == NULL) {
        printf("ERROR: spdk_nvme_ctrlr_alloc_io_qpair() failed\n");
        rc = -1;
        return rc;
    }

    /*
     * allocate a 4KB zeroed buffer which is required for data buffers 
     * used for SPDK NVMe I/O operations
     */
    seq.using_cmb_io = 1;
    seq.buf = spdk_nvme_ctrlr_map_cmb(ns_entry->ctrlr, &sz);
    if (seq.buf == NULL || sz < 0x1000) {
        seq.using_cmb_io = 0;
        seq.buf = spdk_zmalloc(0x1000, 0x1000, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
    }
    if (seq.buf == NULL) {
        printf("ERROR: write buffer allocation failed\n");
        rc = -1;
        return rc;
    }
    if (seq.using_cmb_io) {
        printf("INFO: using controller memory buffer for IO\n");
    } else {
        printf("INFO: using host memory buffer for IO\n");
    }
    seq.is_completed = 0;
    seq.ns_entry = ns_entry;

    /* reset zone before write */
    if (spdk_nvme_ns_get_csi(ns_entry->ns) == SPDK_NVME_CSI_ZNS) {
        //reset_zone_and_wait_for_completion(&seq);
        printf("I will delete this later...\n");
    }
    
    // to do replay....

    spdk_free(seq.buf);
    spdk_nvme_ctrlr_free_io_qpair(ns_entry->qpair);
    return 0;
}

static void
usage(const char *program_name)
{
    printf("usage:\n");
    printf("   %s <options>\n", program_name);
    printf("\n");
    printf("         '-f' specify the input file which generated by trace_io_record\n");
    printf("         '-z' to display zone\n");
    printf("         '-n' to specify the number of displayed zone\n");
    printf("              (-n must be used with -z)\n");
}

static int
parse_args(int argc, char **argv, char *file_name, size_t file_name_size)
{
    int op;

    while ((op = getopt(argc, argv, "f:zn:")) != -1) {
        switch (op) {
        case 'f':
            g_input_file = true;
            snprintf(file_name, file_name_size, "%s", optarg);
            break;
        case 'z':
            g_report_zone = true;
            break;
        case 'n':
            g_zone_report_limit = atoi(optarg);
            break;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    return 0;
}

int
main(int argc, char **argv)
{
    struct spdk_env_opts env_opts;
    int rc, i;
    uint64_t start_tsc, end_tsc, tsc_diff;
    char input_file_name[68];
    FILE *fptr;
    int file_size;
    int entry_cnt;
    size_t read_val;
    
    spdk_nvme_trid_populate_transport(&g_trid, SPDK_NVME_TRANSPORT_PCIE);
    snprintf(g_trid.subnqn, sizeof(g_trid.subnqn), "%s", SPDK_NVMF_DISCOVERY_NQN);
    
    /* Get the input file name */
    rc = parse_args(argc, argv, input_file_name, sizeof(input_file_name));
    if (rc != 0) {
        return rc;
    }

    if (!g_report_zone && g_zone_report_limit) {
        fprintf(stderr, "-n must be used with -z \n");
        exit(1);
    }

    if (input_file_name == NULL || !g_input_file) {
        fprintf(stderr, "-f input file must be specified\n");
        exit(1);
    }

    fptr = fopen(input_file_name, "rb");
    if (fptr == NULL) {
        fprintf(stderr, "Failed to open input file %s\n", input_file_name);
        return -1; 
    }

    fseek(fptr, 0, SEEK_END);
    file_size = ftell(fptr);
    rewind(fptr);
    entry_cnt = file_size / sizeof(struct bin_file_data);

    struct bin_file_data buffer[entry_cnt];    
 
    read_val = fread(&buffer, sizeof(struct bin_file_data), entry_cnt, fptr);
    if (read_val != (size_t)entry_cnt)
        fprintf(stderr, "Fail to read input file\n");
    
    fclose(fptr);

    spdk_env_opts_init(&env_opts);
    env_opts.name = "trace_io_replay";
    if (spdk_env_init(&env_opts) < 0) {
        fprintf(stderr, "Unable to initialize SPDK env\n");
        return 1;
    }
    printf("Initializing NVMe Controllers\n");
    
    rc = spdk_nvme_probe(&g_trid, NULL, probe_cb, attach_cb, NULL);
    if (rc != 0) {
        fprintf(stderr, "spdk_nvme_probe() failed\n");
        rc = 1;
        goto exit;
    }
    
    if (TAILQ_EMPTY(&g_controllers)) {
        fprintf(stderr, "no NVMe controllers found\n");
        rc = 1;
        goto exit;
	}
    printf("Initialization complete.\n");
    
    start_tsc = spdk_get_ticks();

    for (i = 0; i < entry_cnt; i++) {
        rc = process_replay(&buffer[i]);
        if (rc != 0) {
            fprintf(stderr, "process_replay() failed\n");
            rc = 1;
            goto exit;
        }
    }

    end_tsc = spdk_get_ticks();
    tsc_diff = end_tsc - start_tsc;
    printf("Total time: %ju\n", tsc_diff);
   
    if (g_report_zone) {
        report_zone();
    }
 
    exit:
    cleanup();
    spdk_env_fini();
    return rc;
}

