#ifndef TRACE_IO_H
#define TRACE_IO_H

#define UINT8BIT_MASK 0xFF
#define UINT16BIT_MASK 0xFFFF
#define UINT32BIT_MASK 0xFFFFFFFF

struct bin_file_data {
    uint32_t lcore;
    uint64_t tsc_rate;
    uint64_t tsc_timestamp;
    uint64_t obj_id;
    uint64_t obj_start; /* object submit start time */
    uint64_t tsc_sc_time; /* object from submit to complete */
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

/* in spdk/nvme_spec.h

// NVM command set opcodes

enum spdk_nvme_nvm_opcode {
    SPDK_NVME_OPC_FLUSH                 = 0x00,
    SPDK_NVME_OPC_WRITE                 = 0x01,
    SPDK_NVME_OPC_READ                  = 0x02,
    SPDK_NVME_OPC_WRITE_UNCORRECTABLE   = 0x04,
    SPDK_NVME_OPC_COMPARE               = 0x05,
    SPDK_NVME_OPC_WRITE_ZEROES          = 0x08,
    SPDK_NVME_OPC_DATASET_MANAGEMENT    = 0x09,
    SPDK_NVME_OPC_VERIFY                = 0x0c,
    SPDK_NVME_OPC_RESERVATION_REGISTER  = 0x0d,
    SPDK_NVME_OPC_RESERVATION_REPORT    = 0x0e,
    SPDK_NVME_OPC_RESERVATION_ACQUIRE   = 0x11,
    SPDK_NVME_OPC_IO_MANAGEMENT_RECEIVE = 0x12,
    SPDK_NVME_OPC_RESERVATION_RELEASE   = 0x15,
    SPDK_NVME_OPC_COPY                  = 0x19,
    SPDK_NVME_OPC_IO_MANAGEMENT_SEND    = 0x1D,
};

// Zoned Namespace command set opcodes

enum spdk_nvme_zns_opcode {
    SPDK_NVME_OPC_ZONE_MGMT_SEND    = 0x79,
    SPDK_NVME_OPC_ZONE_MGMT_RECV    = 0x7a,
    SPDK_NVME_OPC_ZONE_APPEND       = 0x7d,
};

// ZNS Zone Send Action (ZSA) cdw13 

enum spdk_nvme_zns_zone_send_action {
    SPDK_NVME_ZONE_CLOSE   = 0x1,
    SPDK_NVME_ZONE_FINISH  = 0x2,
    SPDK_NVME_ZONE_OPEN	   = 0x3,
    SPDK_NVME_ZONE_RESET   = 0x4,
    SPDK_NVME_ZONE_OFFLINE = 0x5,
    SPDK_NVME_ZONE_SET_ZDE = 0x10,
};

// ZNS Zone Receive Action (ZRA) cdw13

enum spdk_nvme_zns_zone_receive_action {
    SPDK_NVME_ZONE_REPORT		= 0x0,
    SPDK_NVME_ZONE_EXTENDED_REPORT	= 0x1,
};

enum spdk_nvme_zns_zra_report_opts {
    SPDK_NVME_ZRA_LIST_ALL  = 0x0,
    SPDK_NVME_ZRA_LIST_ZSE  = 0x1,
    SPDK_NVME_ZRA_LIST_ZSIO = 0x2,
    SPDK_NVME_ZRA_LIST_ZSEO = 0x3,
    SPDK_NVME_ZRA_LIST_ZSC  = 0x4,
    SPDK_NVME_ZRA_LIST_ZSF  = 0x5,
    SPDK_NVME_ZRA_LIST_ZSRO = 0x6,
    SPDK_NVME_ZRA_LIST_ZSO  = 0x7,
};

enum spdk_nvme_zns_zone_type {
    SPDK_NVME_ZONE_TYPE_SEQWR = 0x2,
};

enum spdk_nvme_zns_zone_state {
    SPDK_NVME_ZONE_STATE_EMPTY  = 0x1,
    SPDK_NVME_ZONE_STATE_IOPEN  = 0x2,
    SPDK_NVME_ZONE_STATE_EOPEN  = 0x3,
    SPDK_NVME_ZONE_STATE_CLOSED = 0x4,
    SPDK_NVME_ZONE_STATE_RONLY  = 0xD,
    SPDK_NVME_ZONE_STATE_FULL   = 0xE,
    SPDK_NVME_ZONE_STATE_OFFLINE    = 0xF,
};

struct spdk_nvme_zns_zone_desc {
    uint8_t zt      : 4; // Zone Type
    uint8_t rsvd0   : 4;
    uint8_t rsvd1   : 4;
    uint8_t zs      : 4; // Zone State
    union { // Zone Attributes
        uint8_t raw;
        struct {
            uint8_t zfc: 1; // Zone Finished by controller
            uint8_t fzr: 1; // Finish Zone Recommended
            uint8_t rzr: 1; // Reset Zone Recommended
            uint8_t rsvd3 : 4;
            uint8_t zdev: 1; // Zone Descriptor Extension Valid
        } bits;
    } za;
    uint8_t reserved[5];
    uint64_t zcap; // Zone Capacity (in number of LBAs)
    uint64_t zslba; // Zone Start LBA
    uint64_t wp; // Write Pointer (LBA)
    uint8_t reserved32[32];
};

struct spdk_nvme_zns_zone_report {
    uint64_t nr_zones;
    uint8_t reserved8[56];
    struct spdk_nvme_zns_zone_desc descs[];
};

*/

#endif
