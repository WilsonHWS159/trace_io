#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/likely.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/file.h"

#include <map>

#define UINT8BIT_MASK 0xFF
#define UINT16BIT_MASK 0xFFFF
#define UINT32BIT_MASK 0xFFFFFFFF

extern "C" {
#include "spdk/trace_parser.h"
#include "spdk/util.h"
}

static struct spdk_trace_parser *g_parser;
static const struct spdk_trace_flags *g_flags;
static char *g_exe_name;

/* This is a bit ugly, but we don't want to include env_dpdk in the app, while spdk_util, which we
 * do need, uses some of the functions implemented there.  We're not actually using the functions
 * that depend on those, so just define them as no-ops to allow the app to link.
 */
extern "C" {
    void *
    spdk_realloc(void *buf, size_t size, size_t align)
    {
        assert(false);

        return NULL;
    }

    void
    spdk_free(void *buf)
    {
        assert(false);
    }

    uint64_t
	spdk_get_ticks(void)
    {
        return 0;
    }
} /* extern "C" */

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
process_output_file(struct spdk_trace_parser_entry *entry, uint64_t tsc_rate, uint64_t tsc_base, FILE *fptr)
{
    struct spdk_trace_entry *e = entry->entry;
    const struct spdk_trace_tpoint  *d;
    size_t	i;
    struct bin_file_data buffer;

    d = &g_flags->tpoint[e->tpoint_id];
    
    buffer.lcore = entry->lcore;
    buffer.tsc_rate = tsc_rate;
    buffer.tsc_timestamp = e->tsc - tsc_base;    
    buffer.obj_id = e->object_id;
    
    if (entry->object_index < UINT64_MAX) {
        buffer.obj_idx = entry->object_index;
    } else {
        buffer.obj_idx = 0;
    }

    if (!d->new_object && d->object_type != OBJECT_NONE) {
        buffer.tsc_sc_time = e->tsc - entry->object_start;
    } else {
        buffer.tsc_sc_time = 0;
    }

    strcpy(buffer.tpoint_name, d->name);
    
    if (strcmp(buffer.tpoint_name, "NVME_IO_SUBMIT") == 0) {
        for (i = 1; i < d->num_args; ++i) {
            if (strcmp(d->args[i].name, "opc") == 0) {
                buffer.opc = (uint16_t)(entry->args[i].integer & UINT8BIT_MASK);
            } else if (strcmp(d->args[i].name, "cid") == 0) {
                buffer.cid = (uint16_t)entry->args[i].integer;
            } else if (strcmp(d->args[i].name, "nsid") == 0) { 
                buffer.nsid = (uint32_t)entry->args[i].integer;
            } else if (strcmp(d->args[i].name, "cdw10") == 0) { 
                buffer.cdw10 = (uint32_t)entry->args[i].integer;
            } else if (strcmp(d->args[i].name, "cdw11") == 0) { 
                buffer.cdw11 = (uint32_t)entry->args[i].integer;
            } else if (strcmp(d->args[i].name, "cdw12") == 0) { 
                buffer.cdw12 = (uint32_t)entry->args[i].integer;
            } else if (strcmp(d->args[i].name, "cdw13") == 0) { 
                buffer.cdw13 = (uint32_t)entry->args[i].integer;
            } else {
                continue;
            }
        }
    } else if (strcmp(buffer.tpoint_name, "NVME_IO_COMPLETE") == 0) {
        for (i = 1; i < d->num_args; ++i) {
            if (strcmp(d->args[i].name, "cid") == 0) {
                buffer.cid = entry->args[i].integer;
            } else if (strcmp(d->args[i].name, "cpl") == 0) {
                buffer.cpl = entry->args[i].integer;
            } else {
                continue;
            }
        }
    }
    
    fwrite(&buffer, sizeof(struct bin_file_data), 1, fptr);
}

static void
usage(void)
{
    fprintf(stderr, "usage:\n");
    fprintf(stderr, "   %s <option> <lcore#>\n", g_exe_name);
    fprintf(stderr, "   '-c' to display single lcore history\n");
    fprintf(stderr, "   '-s' to specify spdk_trace shm name for a currently running process\n");                
    fprintf(stderr, "   '-i' to specify the shared memory ID\n");
    fprintf(stderr, "   '-p' to specify the trace PID\n");
    fprintf(stderr, "        If -s is specified, then one of\n");
    fprintf(stderr, "        -i or -p must be specified)\n");
    fprintf(stderr, "   '-f' to specify a tracepoint file name\n");
    fprintf(stderr, "        (-s and -f are mutually exclusive)\n");
    fprintf(stderr, "   '-o' to produce output file and specify output file name.\n");
}

int
main(int argc, char **argv)
{
    struct spdk_trace_parser_opts	opts;
    struct spdk_trace_parser_entry	entry;
    int				    lcore = SPDK_TRACE_MAX_LCORE;
    uint64_t			tsc_base, entry_count;
    const char			*app_name = NULL;
    const char			*file_name = NULL;
    int				    op, i;
    FILE                *fptr;
    char                output_file_name[68];
    char				shm_name[64];
    int				    shm_id = -1, shm_pid = -1;
    const struct spdk_trace_tpoint	*d;

    g_exe_name = argv[0];
    while ((op = getopt(argc, argv, "c:f:i:p:s:t")) != -1) {
        switch (op) {
        case 'c':
            lcore = atoi(optarg);
            if (lcore > SPDK_TRACE_MAX_LCORE) {
                fprintf(stderr, "Selected lcore: %d "
                    "exceeds maximum %d\n", lcore,
                    SPDK_TRACE_MAX_LCORE);
                exit(1);
            }
            break;
        case 'i':
            shm_id = atoi(optarg);
            break;
        case 'p':
            shm_pid = atoi(optarg);
            break;
        case 's':
            app_name = optarg;
            break;
        case 'f':
            file_name = optarg;
            break;
        default:
            usage();
            exit(1);
        }
    }

    if (file_name != NULL && app_name != NULL) {
        fprintf(stderr, "-f and -s are mutually exclusive\n");
        usage();
        exit(1);
    }

    if (file_name == NULL && app_name == NULL) {
        fprintf(stderr, "One of -f and -s must be specified\n");
        usage();
        exit(1);
    }

    /* 
     * output file name in ./ 
     */                                                                                                                                                                           
    if (!file_name) {
        if (shm_id >= 0)
            snprintf(output_file_name, sizeof(output_file_name), "%s_%d.bin", app_name, shm_id);
        else
            snprintf(output_file_name, sizeof(output_file_name), "%s_pid%d.bin", app_name, shm_pid);
    } else
        snprintf(output_file_name, sizeof(output_file_name), "%s.bin", file_name);
    /* 
     * file name in /dev/shm/ 
     */
    if (!file_name) {
        if (shm_id >= 0) {
            snprintf(shm_name, sizeof(shm_name), "/%s_trace.%d", app_name, shm_id);
        } else {
            snprintf(shm_name, sizeof(shm_name), "/%s_trace.pid%d", app_name, shm_pid);
        }
        file_name = shm_name;
    }
    
    if (output_file_name) {
        fptr = fopen(output_file_name, "wb");
        if (fptr == NULL) {
            fprintf(stderr, "Failed to open output file %s\n", output_file_name);
            return -1; 
        }
        printf("Output .bin file: %s\n", output_file_name);
    }   

    opts.filename = file_name;
    opts.lcore = lcore;
    opts.mode = app_name == NULL ? SPDK_TRACE_PARSER_MODE_FILE : SPDK_TRACE_PARSER_MODE_SHM;
    g_parser = spdk_trace_parser_init(&opts);
    if (g_parser == NULL) {
        fprintf(stderr, "Failed to initialize trace parser\n");
        exit(1);
    }

    g_flags = spdk_trace_parser_get_flags(g_parser);
    printf("TSC Rate: %ju\n", g_flags->tsc_rate);

    for (i = 0; i < SPDK_TRACE_MAX_LCORE; ++i) {
        if (lcore == SPDK_TRACE_MAX_LCORE || i == lcore) {
            entry_count = spdk_trace_parser_get_entry_count(g_parser, i);
            if (entry_count > 0) {
                printf("Trace Size of lcore (%d): %ju\n", i, entry_count);
            }
        }
    }

    tsc_base = 0;
    while (spdk_trace_parser_next_entry(g_parser, &entry)) {
        d = &g_flags->tpoint[entry.entry->tpoint_id];
    
        if (strcmp(d->name, "NVME_IO_SUBMIT") != 0 && strcmp(d->name, "NVME_IO_COMPLETE") != 0) {
            continue;
        } else if ((strcmp(d->name, "NVME_IO_SUBMIT") == 0 || strcmp(d->name, "NVME_IO_COMPLETE") == 0)
                    && entry.args[0].integer) { 
            continue;   
        }
        /* tsc_base = tsc of first io cmd entry */
        if (!tsc_base) {
            tsc_base = entry.entry->tsc;
        }

        /* process output file */
        if (output_file_name)
            process_output_file(&entry, g_flags->tsc_rate, tsc_base, fptr);
    }
    fclose(fptr);

    /*
    size_t entry_cnt;
    size_t file_size;
    size_t read_val;
    
    fptr = fopen(output_file_name, "rb");
    if (fptr == NULL) {
        fprintf(stderr, "Failed to open output file %s\n", output_file_name);
        return -1; 
    }   

    fseek(fptr, 0, SEEK_END);
    file_size = ftell(fptr);
    rewind(fptr);
    entry_cnt = file_size / sizeof(struct bin_file_data);
    
    struct bin_file_data buffer[entry_cnt];
    
    read_val = fread(&buffer, sizeof(struct bin_file_data), entry_cnt, fptr);
    if (read_val != (size_t)entry_cnt)
        fprintf(stderr, "Fail to read output file\n");

    for (i = 0; i < (int)entry_cnt; i++) {
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
    fclose(fptr);
    */

    spdk_trace_parser_cleanup(g_parser);

    return (0);
}
