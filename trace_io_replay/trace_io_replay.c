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

#define UINT8BIT_MASK 0xFF
#define UINT16BIT_MASK 0xFFFF
#define UINT32BIT_MASK 0xFFFFFFFF

struct ctrlr_entry {
	struct spdk_nvme_ctrlr		*ctrlr;
	TAILQ_ENTRY(ctrlr_entry)	link;
	char				name[1024];
};

struct ns_entry {
	struct spdk_nvme_ctrlr	*ctrlr;
	struct spdk_nvme_ns	*ns;
	TAILQ_ENTRY(ns_entry)	link;
	struct spdk_nvme_qpair	*qpair;
};

static TAILQ_HEAD(, ctrlr_entry) g_controllers = TAILQ_HEAD_INITIALIZER(g_controllers);
static TAILQ_HEAD(, ns_entry) g_namespaces = TAILQ_HEAD_INITIALIZER(g_namespaces);
static struct spdk_nvme_transport_id g_trid = {};

struct bin_file_data {
    uint32_t lcore;
    uint64_t tsc_rate;
    uint64_t tsc_timestamp;
    uint32_t obj_idx;
    uint64_t obj_id;
    uint64_t tsc_sc_time; /* object from submit to complete (us) */
    char     tpoint_name[32];       
    uint16_t opc;
    uint16_t cid;
    uint32_t nsid;
    uint32_t cpl;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
};

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
	 *  controller.  During initialization, the IDENTIFY data for the
	 *  controller is read using an NVMe admin command, and that data
	 *  can be retrieved using spdk_nvme_ctrlr_get_data() to get
	 *  detailed information on the controller.  Refer to the NVMe
	 *  specification for more details on IDENTIFY for NVMe controllers.
	 */
	cdata = spdk_nvme_ctrlr_get_data(ctrlr);

	snprintf(entry->name, sizeof(entry->name), "%-20.20s (%-20.20s)", cdata->mn, cdata->sn);

	entry->ctrlr = ctrlr;
	TAILQ_INSERT_TAIL(&g_controllers, entry, link);

	/*
	 * Each controller has one or more namespaces.  An NVMe namespace is basically
	 *  equivalent to a SCSI LUN.  The controller's IDENTIFY data tells us how
	 *  many namespaces exist on the controller.  For Intel(R) P3X00 controllers,
	 *  it will just be one namespace.
	 *
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

static void
usage(const char *program_name)
{
    printf("usage:\n");
    printf("   %s <options>\n", program_name);
    printf("\n");
    printf("         '-i' The input file which generated by trace_io_parser\n");
}

static int
parse_args(int argc, char **argv, char *file_name, size_t file_name_size)
{
    int op;

    spdk_nvme_trid_populate_transport(&g_trid, SPDK_NVME_TRANSPORT_PCIE);
    snprintf(g_trid.subnqn, sizeof(g_trid.subnqn), "%s", SPDK_NVMF_DISCOVERY_NQN);

    while ((op = getopt(argc, argv, "f:")) != -1) {
        switch (op) {
        case 'f':
            snprintf(file_name, file_name_size, "%s", optarg);
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
    
    /*
     * Get the input file name.
     */
    
    rc = parse_args(argc, argv, input_file_name, sizeof(input_file_name));
    if (rc != 0) {
        return rc;
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
    /********************** process input file **************************/
    for (i = 0; i < entry_cnt; i++) {
        printf("entry: %d  ", i);
        printf("lcore: %d  ", buffer[i].lcore);
        printf("tsc_rate: %ld  ", buffer[i].tsc_rate);
        printf("tsc_timestamp: %ld  ", buffer[i].tsc_timestamp);
        printf("obj_idx: %d  ", buffer[i].obj_idx);
        printf("obj_id: %ld  ", buffer[i].obj_id);
        printf("tsc_sc_time: %ld  ", buffer[i].tsc_sc_time);
        printf("tpoint_name: %s  ", buffer[i].tpoint_name);
        printf("opc: %d  ", buffer[i].opc);
        printf("cid: %d  ", buffer[i].cid);
        printf("nsid: %d  ", buffer[i].nsid);
        printf("cpl: %d  ", buffer[i].cpl);
        printf("cdw10: %d  ", buffer[i].cdw10);
        printf("cdw11: %d  ", buffer[i].cdw11);
        printf("cdw12: %d  ", buffer[i].cdw12);
        printf("cdw13: %d  ", buffer[i].cdw13);
        printf("\n");
    }   
    /********************** process input file **************************/

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

    // process_replay();

    end_tsc = spdk_get_ticks();
    tsc_diff = end_tsc - start_tsc;
    printf("Total time: %ju\n", tsc_diff);
    
    exit:
    cleanup();
    spdk_env_fini();
    return rc;
}
