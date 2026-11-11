/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
/* Command-processor packet encoding and status page layout. */
#ifndef APEX_CP_PACKET_H
#define APEX_CP_PACKET_H
#include <linux/types.h>

#define APEX_CP_NOP 0x00
#define APEX_CP_INDIRECT 0x01
#define APEX_CP_SET_STATE 0x02
#define APEX_CP_DISPATCH 0x10
#define APEX_CP_DISPATCH_INDIRECT 0x11
#define APEX_CP_DRAW 0x20
#define APEX_CP_DRAW_INDEXED 0x21
#define APEX_CP_DRAW_INDIRECT 0x22
#define APEX_CP_DRAW_INDEXED_INDIRECT 0x23
#define APEX_CP_BEGIN_PASS 0x28
#define APEX_CP_END_PASS 0x29
#define APEX_CP_COPY 0x30
#define APEX_CP_FILL 0x31
#define APEX_CP_COPY_RECT 0x32
#define APEX_CP_BARRIER 0x40
#define APEX_CP_WAIT 0x50
#define APEX_CP_SIGNAL 0x51
#define APEX_CP_KFENCE 0x52
#define APEX_CP_WRITE 0x53
#define APEX_CP_TIMESTAMP 0x60
#define APEX_CP_QUERY_BEGIN 0x61
#define APEX_CP_QUERY_END 0x62
#define APEX_CP_TLB_INVALIDATE 0x80
#define APEX_CP_VM_DRAIN 0x81

/* Header: opcode 7:0, payload dword count 23:8, bits 31:24 zero. */
static inline __u32 apex_cp_header(__u32 opcode, __u32 count)
{
	return (opcode & 0xff) | ((count & 0xffff) << 8);
}

/* VMID 0 addresses LOCAL physically; bit 52 selects a SYSTEM IOVA. */
#define APEX_CP_SYSTEM_ADDRESS (1ull << 52)

/* Kernel-ring packets. Each writes its dwords at p and returns the count. */
static inline unsigned int apex_cp_kfence(__u32 *p, __u64 value)
{
	p[0] = apex_cp_header(APEX_CP_KFENCE, 2);
	p[1] = (__u32)value;
	p[2] = (__u32)(value >> 32);
	return 3;
}

static inline unsigned int apex_cp_vmid_packet(__u32 *p, __u32 opcode, __u32 vmid)
{
	p[0] = apex_cp_header(opcode, 1);
	p[1] = vmid;
	return 2;
}

static inline unsigned int apex_cp_fill(__u32 *p, __u64 destination, __u64 bytes,
					__u32 value)
{
	p[0] = apex_cp_header(APEX_CP_FILL, 5);
	p[1] = (__u32)destination;
	p[2] = (__u32)(destination >> 32);
	p[3] = (__u32)bytes;
	p[4] = (__u32)(bytes >> 32);
	p[5] = value;
	return 6;
}

/* Status page (host memory, 4 KiB). The CP writes offsets 0-15 and 24-47. */
#define APEX_CP_STATUS_RPTR 0
#define APEX_CP_STATUS_STATE 4
#define APEX_CP_STATUS_KFENCE 8
#define APEX_CP_STATUS_FAULT_STATUS 24
#define APEX_CP_STATUS_FAULT_UNIT 28
#define APEX_CP_STATUS_FAULT_ADDRESS 32
#define APEX_CP_STATUS_FAULT_PACKET 40
#define APEX_CP_STATE_ENABLED (1u << 0)
#define APEX_CP_STATE_WAITING (1u << 1)
#define APEX_CP_STATE_FAULTED (1u << 2)
#define APEX_CP_STATE_RESET (1u << 3)

/* Doorbell page offsets (BAR4 page q for queue q). */
#define APEX_CP_DOORBELL_WPTR 0
#define APEX_CP_DOORBELL_RECHECK 4

#endif
